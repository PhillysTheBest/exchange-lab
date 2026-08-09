# Observability Dashboard — Stage 6, Part E/F (redesigned, Stage 6A)

`dashboard/` is a small Rust/Axum web service (`px-dashboard`) that polls the
JSON telemetry snapshot `telemetry/telemetry_publisher.hpp` writes and serves a
browser page over it. This document is the "why" behind its shape; the code
itself (`dashboard/src/main.rs`, `dashboard/static/index.html`) carries the
"how" in its own header comments, not duplicated here. For the exact field-by-
field inventory of what's real vs. derived vs. deliberately not built, see
[`docs/DASHBOARD_DATA_CONTRACT.md`](DASHBOARD_DATA_CONTRACT.md) — this document
covers the architecture, that one covers the data.

**Stage 6A note**: the original Part E/F build (a stat-card grid) was replaced
this pass with a hierarchical trading-workstation layout — system status,
performance (with a real session P&L chart), market state (order book depth +
trade tape), positions/orders, strategy activity, and risk — organized by what
an operator needs to know first, not by "one box per number." Getting there
required real backend additions (order-book depth, a public trade tape, P&L
history, and live risk-limit figures — see the data contract doc), not just a
CSS pass; nothing in the new layout is fabricated to fill space.

## The one design constraint everything else follows from

> "The dashboard should be allowed to become stale. The trading engine must
> never wait for the dashboard." — this stage's own framing, echoed in
> `pipeline/brain.hpp`'s file header: `Brain` is a single pinned thread with no
> lock; anything that could ever stall it is a correctness bug, not a UX one.

Concretely, that means:

- **No channel back into the C++ process** — not a socket, not shared memory,
  not a callback. `Brain`'s telemetry hooks (`telemetry::TelemetryState*`,
  Stage 6 Part G) are write-only from T2's side; nothing reads them from inside
  the trading engine's own process.
- **A plain file, written atomically, polled on a schedule** —
  `telemetry::publish_snapshot` (temp-file-then-rename) from
  `tools/kalshi_pipeline.cpp`'s existing 500ms stats-printing loop, no new
  thread. `px-dashboard` polls that file on ITS OWN schedule (fresh
  `tokio::fs::read_to_string` per `/api/snapshot` request, no caching — see
  that handler's own comment for why this is simple enough not to need one),
  and the browser polls `px-dashboard` on a THIRD independent schedule
  (`static/index.html`'s 1s `setInterval`). Three schedules, zero
  synchronization between them, by design: the trading engine can outlive the
  dashboard, the dashboard can outlive any given browser tab, and none of them
  can stall either of the other two.
- **Passthrough, not a schema mirror** — `/api/snapshot` re-serializes whatever
  valid JSON is currently in the file rather than deserializing into a
  hand-written Rust struct. A struct mirror is the more obvious design but a
  silent drift hazard: add a field to `telemetry_publisher.hpp` and forget the
  Rust side, and the dashboard just quietly never shows it. Passthrough means
  `static/index.html`'s JS is the only place that needs to know the schema, and
  a drift there fails loudly (a table cell renders `undefined`) instead of
  silently.

## What actually happens when something goes wrong

Not theorized — checked by hand while building this (see the reproduce steps
below): killing the C++ pipeline, deleting the snapshot file mid-poll, and
racing a reader against the writer's rename all produce the SAME visible
behavior, not three different failure modes:

- `px-dashboard`'s `/api/snapshot` returns `503` with `{"error": "..."}`, never
  a fabricated `200`.
- `static/index.html` shows an explicit "showing last known data below" banner
  with the actual error text, and a `freshness` indicator (`updated Ns ago`)
  that turns amber past 3s stale and red past 10s — it keeps the last-known
  table/card contents visible rather than blanking the page, because a
  disappearing dashboard mid-incident is worse than a stale one that says so.
- Found empirically, not hypothesized: a real Windows-specific race exists
  where the C++ writer's atomic rename can transiently fail if
  `px-dashboard`'s own read of the same file is in flight at that instant
  (Windows can refuse to replace a file over an open read handle, unlike
  POSIX — see `telemetry/telemetry_publisher.hpp`'s `publish_snapshot` header
  comment for the full explanation and why it's deliberately not "fixed" with
  Windows-specific sharing flags on the Rust side). It self-heals one publish
  cycle later (~500ms) — reproduced by hand while testing this dashboard
  against a live pipeline, never observed to compound or worsen.

## Running it

```bash
# from the repo root, so the two default relative paths agree:
mingw32-make live                          # builds build/release/kalshi_pipeline.exe
PATH="/c/msys64/ucrt64/bin:$PATH" \
  ./build/release/kalshi_pipeline.exe --replay tests/frames/smoke.jsonl KNYC-DEMO
  # (or a real run: ./build/release/kalshi_pipeline.exe TICKER — see that tool's
  # own --help; either way it publishes telemetry/snapshot.json every ~500ms)

cd dashboard
cargo build --release
./target/release/px-dashboard.exe          # http://127.0.0.1:7878/
```

Both binaries default their respective `--telemetry-out` / `--telemetry-path`
flags to the SAME relative path, `telemetry/snapshot.json` — chosen (not
`telemetry_snapshot.json` at the repo root, an earlier draft of this pass used
before finding the `.gitignore` entry a prior Stage 6 session had already
committed) to match `.gitignore`'s pre-existing `telemetry/snapshot.json` /
`telemetry/snapshot.json.tmp` entries. Run both from the repo root (or pass
matching explicit `--telemetry-out`/`--telemetry-path` values) so they agree on
which file they mean; `dashboard/target/` and `Cargo.lock` are gitignored too
(`dashboard/` is a Rust crate, its build artifacts aren't source).

`px-dashboard --host`/`--port` default to `127.0.0.1:7878` — not
externally reachable unless you explicitly pass `--host 0.0.0.0`, the same
"safe by default, opt into wider exposure" posture `tools/kalshi_pipeline.cpp`
already takes with `--demo`.

## What it shows

Top to bottom, in the order an operator actually needs it (see
`static/index.html`'s own header comment for the full reasoning):

1. **System status** (top bar) — connection, mode (dry-run/live), active market
   count, uptime, and snapshot freshness, always visible.
2. **Performance** — a headline Net P&L number, its realized/unrealized/fee
   breakdown, and a real session P&L chart (plain `<canvas>`, no charting
   library — drawn from `pnl_history[]`, a genuine time series `TelemetryState`
   samples on every fill and on throttled book updates, not interpolated or
   synthesized).
3. **Active Markets** + **Current Market** — a clickable market list next to a
   detail panel for whichever one is selected: YES/NO bid-ask (NO derived
   client-side from YES via this project's own documented normalization
   identity, not a second data source), a real order-book depth ladder, and
   a recent-trades tape for that market.
4. **Positions** and **Orders / Execution** — a position table (only markets
   with a nonzero position) and a dense, clickable order-event table; clicking
   an order groups every telemetry event sharing its `id` into that order's own
   real lifecycle (submit → ack → fill/cancel/reject), not a fabricated stepper
   with invented intermediate stages.
5. **Strategy Activity** — a chronological feed of real `ExecutionDecision`s,
   including whatever free-form `reason` string the strategy provided (often
   empty today — see the data contract's "known gap" on this).
6. **Risk** — per-market exposure utilization against the real configured cap
   (as a bar, correctly scoped to "worst single market," not a fabricated
   portfolio-wide number — see the data contract's own note on why), live
   rate-limit token headroom, the real fat-finger bounds, and both rejection
   counters (pre-send `risk_rejections` vs. post-send `orders_rejected` — kept
   visually and semantically distinct, matching how `Brain`/`RiskEngine`
   already keep them distinct internally).
7. **System Events** — a merged, severity-tagged log of real order-lifecycle
   events and reconnects. No synthesized "book update received"/"queue depth"
   entries — see the data contract for why those would require inventing data
   this pass didn't add.

Every number traces to a real `telemetry::TelemetryState` field or an
explicitly-documented client-side derivation — see
`docs/DASHBOARD_DATA_CONTRACT.md` for the complete source-of-truth table.
Status-code-to-name mapping (`static/index.html`'s `STATUS_NAMES`/
`STATUS_CLASS`) must stay in sync with `px::MarketStatus`'s ordinal order
(`include/px/types.hpp` — `PreOpen`=0, `Open`=1, `Halted`=2, `Resolved`=3).

## What this is not

Not a control surface — there is no button anywhere on this page that sends an
order, cancels one, or changes `Brain`'s behavior in any way. That's a
deliberate scope boundary, not a missing feature: a dashboard that could act on
the trading engine would need the exact bidirectional-channel-with-timing-
guarantees that this whole design specifically avoids. It's read-only,
on purpose, permanently.
