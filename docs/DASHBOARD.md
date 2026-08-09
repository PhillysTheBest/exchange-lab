# Observability Dashboard — Stage 6, Part E/F

`dashboard/` is a small Rust/Axum web service (`px-dashboard`) that polls the
JSON telemetry snapshot `telemetry/telemetry_publisher.hpp` writes and serves a
browser page over it. This document is the "why" behind its shape; the code
itself (`dashboard/src/main.rs`, `dashboard/static/index.html`) carries the
"how" in its own header comments, not duplicated here.

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

Connection status / mode (dry-run vs. live) / reconnect count, event and order
counters, realized/unrealized/net P&L and total exposure, a per-market position
table (position, average entry, live bid/ask, per-market P&L, lifecycle
status), and the last 64 order events / strategy decisions each (server-side
ring buffers — `telemetry/telemetry.hpp`'s `RecentRing`), newest first. Every
number here is exactly what `telemetry::TelemetryState` computed inside
`Brain`, cents and all — this page performs zero of its own trading-domain
arithmetic, only presentation (currency formatting, status-code-to-name
mapping via `static/index.html`'s own `STATUS_NAMES`/`STATUS_CLASS` tables,
which must stay in sync with `px::MarketStatus`'s ordinal order,
`include/px/types.hpp` — `PreOpen`=0, `Open`=1, `Halted`=2, `Resolved`=3).

## What this is not

Not a control surface — there is no button anywhere on this page that sends an
order, cancels one, or changes `Brain`'s behavior in any way. That's a
deliberate scope boundary, not a missing feature: a dashboard that could act on
the trading engine would need the exact bidirectional-channel-with-timing-
guarantees that this whole design specifically avoids. It's read-only,
on purpose, permanently.
