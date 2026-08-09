# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A C++20 prediction-market trading engine: an offline matching engine (array-backed
order book, integer-cent prices), a live pipeline that connects it to Kalshi's real
WebSocket/REST API, and a deterministic execution-decision layer (Strategy interface)
on top. See README.md for the full narrative — measured latency numbers, bugs found
and fixed, and what's honestly out of scope — and PERFORMANCE.md for every benchmark
number in one place, with methodology. This file is about how to build/test it and
how the pieces fit together, not a restatement of either.

## Build

No CMake/Ninja — a hand-written `Makefile`, built via `mingw32-make` (Windows/MSYS2
UCRT64) or `make` (Linux), no changes needed between platforms.

```bash
mingw32-make test         # debug build (PX_DEBUG invariants + _GLIBCXX_ASSERTIONS): core
                           # engine, MarketState/Strategy unit tests, Brain/ShadowBook fuzz test
mingw32-make bench        # release build (-O3 -march=native -flto): core engine + strategy
                           # dispatch latency benchmarks
mingw32-make repl         # interactive exchange REPL
mingw32-make strategy_demo   # local oracle/book -> strategy -> engine demo, no network
mingw32-make live         # Kalshi connectivity binaries: kalshi_shadow, kalshi_pipeline (needs OpenSSL)
mingw32-make test-live    # OpenSSL-dependent tests: feed schema, gateway dry-run, replay
                           # determinism, full pipeline E2E (real threads/rings), hostile-event
                           # fuzzing (duplicate/reordered messages, races, reconnect backoff),
                           # market lifecycle gating
mingw32-make bench-live   # RSA-PSS signing latency + full WS-frame -> OrderIntent decision-path
                           # latency, and Stage 5's per-stage breakdown of that same path
                           # (needs OpenSSL)
mingw32-make profile      # Stage 5: gprof flat profile + call graph (-pg, no LTO) of the
                           # per-stage benchmark -- see PERFORMANCE.md's Profiling section for
                           # why gprof itself produces no usable data in this environment
mingw32-make bench-perf   # Stage 5: builds the throughput/saturation/backpressure/scaling tool
                           # (bench_workload_throughput) and the dispatch-scan-cost isolator
                           # (bench_dispatch_scaling) -- see perf/run_sweeps.py to drive sweeps
mingw32-make perf-regression  # Stage 5: re-runs bench_engine/bench_ring/bench_strategy with
                           # --csv=, diffs against perf/baseline/'s frozen snapshot
                           # (perf/compare_baseline.py, tolerance-banded, not a hard CI gate)
mingw32-make run_backtest # Stage 6: builds tools/run_backtest.cpp -- runs PassiveMarketMaker/
                           # OrderBookImbalance/TradeFlow against eval/'s seeded synthetic
                           # market and prints the Part H comparison table (no OpenSSL)
mingw32-make all          # test + bench + repl + strategy_demo (the dependency-free set)
mingw32-make clean
```

`dashboard/` (Stage 6, Part E/F — the observability dashboard) is a separate Rust/Cargo
crate, not part of this `Makefile`:

```bash
cd dashboard && cargo build --release   # builds target/release/px-dashboard(.exe)
```

No separate lint target — `-Wall -Wextra -Wpedantic` (Makefile's `WARN` var) apply to every
compile, debug and release alike; a clean build with zero warnings *is* the lint check.

To run or debug a single test binary directly (all test binaries are hand-rolled, no
framework — see `tests/test_harness.hpp`'s `PX_CHECK` macro). Dependency-free tests build to
`build/debug/`; anything under `test-live`/`live`/`bench-live` (OpenSSL-linked) builds to
`build/release/` even though it's not itself performance-sensitive — one release object set
is shared across all of `live`/`test-live`/`bench-live` rather than duplicating a debug one:

```bash
mingw32-make test                       # builds build/debug/test_book.exe etc.
./build/debug/test_book.exe             # run just that one

mingw32-make test-live                  # builds build/release/test_lifecycle.exe etc.
./build/release/test_hostile_events.exe # run just that one
```

Live/OpenSSL-linked binaries (`kalshi_shadow`, `kalshi_pipeline`, `test-live`,
`bench-live` targets) need MSYS2 UCRT64's `libssl`/`libcrypto` DLLs findable at
runtime. If more than one MSYS2 subsystem is installed, Windows' DLL search order can
resolve a *different* build's OpenSSL than the one linked, producing a
`DECODER routines::unsupported` error unrelated to your actual key/cert. Fix: put the
matching bin dir first —
`PATH="/c/msys64/ucrt64/bin:$PATH" ./build/release/kalshi_shadow.exe ...`.

No sanitizer runtime is installed in this toolchain (no `-lasan`/`-lubsan`); debug
builds rely on `_GLIBCXX_ASSERTIONS` plus hand-written `PX_DEBUG`-gated invariant
checks (`OrderBook::check_invariants`, `ShadowBook::check_invariants`) instead.

`make bench-perf` only builds the Stage 5 throughput/scaling tools — driving them across
sweep points is a separate step, needs Python 3 (+ matplotlib for plotting), and is not
itself a `make` target:

```bash
mingw32-make bench-perf         # builds bench_workload_throughput, bench_dispatch_scaling
python perf/run_sweeps.py all   # runs every sweep, writes perf/results/*.csv
python perf/plot_results.py     # turns those into perf/results/*.png
```

## Architecture

### Two engines sharing one book type

`OrderBook` (`include/px/order_book.hpp`) is the offline matching engine: full
order pool + FIFO + id map, real price-time-priority matching, used by
`MatchingEngine`/`tools/repl.cpp`/`tools/strategy_demo.cpp`. It doubles as a
single-market Kalshi shadow book (`shadow_set_level`) for the standalone
`tools/kalshi_shadow.cpp` debugging tool. `ShadowBook` (`include/px/shadow_book.hpp`)
is a separate, much smaller type (~440 B vs. OrderBook's ~5.7 MB) used one-per-market
by the live pipeline's `Brain` — it has no order pool, since in shadow mode Kalshi has
already matched and this side only ever sets aggregate level quantities. Don't conflate
the two or assume one can substitute for the other.

**Both are heap-allocate-only.** `OrderBook` (~5.7 MB) and `Brain` (~3.0 MB — it embeds
a `ShadowBook`/`TradeWindow`/order-tracking table per market, `kMaxMarkets` = 1024 as of
Stage 5) are far too large for a
stack frame; every real construction site uses `std::make_unique`. This has bitten
development twice (see each type's own SIZE WARNING doc comment) — always check for
that comment before declaring one of these as a local.

### The live pipeline: 3 threads, 3 lock-free rings

```
KALSHI --TLS/WS--> [T1 Ingress]  --ring A-->  [T2 Brain]  --ring B-->  [T3 Egress/Gateway]
                    (pipeline/)                (pipeline/)              (live/kalshi_gateway)
                                                    ^                         |
                                                    +--------ring C-----------+
```

- **T1 Ingress** (`pipeline/ingress.*`) owns the Kalshi WebSocket, decodes messages
  (`live/kalshi_feed.*`), resolves ticker strings to compact `MarketId`s
  (`live/ticker_registry.hpp`) — nothing downstream of T1 ever touches a `std::string`.
  Retries connect/subscribe/receive failures with capped backoff rather than exiting —
  see "Reconnection and market lifecycle" below.
- **T2 Brain** (`pipeline/brain.*`) is the *sole* owner of all mutable market/risk/order
  state, on one thread, no locks. It mutates per-market `ShadowBook`s, dispatches to
  registered `Strategy` objects, runs `RiskEngine` pre-trade checks, and tracks this
  account's own resting orders (`strategy/order_tracker.hpp`).
- **T3 Egress/Gateway** (`live/kalshi_gateway.*`) does the RSA-PSS signing and REST
  send/cancel. This is deliberately its own thread, not inline in T2: signing measures
  at ~1 ms p50 (`bench/bench_sign.cpp`), ~15,000–30,000x a book update — inline it would
  freeze the book mid-update.

All three rings are the same `SpscRing<T, Capacity>` (`include/px/spsc_ring.hpp`).
**Every ring payload type must be `std::is_trivially_copyable_v`** — checked via
`static_assert` in `pipeline/events.hpp`, not just documented. This is why Kalshi's own
UUID `order_id` (a string) never appears on any ring: Brain assigns its own integer
`OrderId` to every order it places, Gateway privately maps that to Kalshi's UUID
(`live/kalshi_gateway.hpp`'s `kalshi_uuid_of_`), and a later cancel intent carries the
same integer id back. If you need to add a new field to `BookEvent`/`OrderIntent`/
`ExecReport`, it must stay POD — no `std::string`, no containers.

### Strategy interface: decisions, not just signals

`strategy/strategy.hpp`'s `Strategy::on_event(MarketState, TriggerKind, OracleEvent*)`
returns an `ExecutionDecision` (`Join` / `Improve` / `Cross` / `Cancel` / `Wait`), not
just "trade or don't." `TriggerKind` is `BookUpdate` (book mutation *or* a public trade
notification — collapsed into one trigger since every strategy re-reads the full
`MarketState` regardless), `OracleEvent`, or `OwnOrderUpdate` (fires immediately when
one of *this account's own* orders is acked/filled/cancelled, so a strategy's exit logic
doesn't have to wait for the next incidental book tick).

`strategy/market_state.hpp`'s `extract_features()` assembles a `MarketState` (spread,
BBO depth, imbalance, position, recent trade flow, this account's own acked resting
orders) fresh on every dispatch — it's a pure function over already-owned state
(`ShadowBook` + `OrderTracker` + `TradeWindow`), independently unit-testable with no
pipeline/thread/network involved (see `tests/test_market_state.cpp`).

`strategy/order_tracker.hpp`'s `OrderTracker::orders_for()` exposes **acked orders
only** — a pending (not-yet-confirmed) order is invisible to every `Strategy`. This is
the structural fix for a real race: a strategy literally cannot construct a `Cancel`
decision against an order Gateway hasn't confirmed yet, because it can't see it.

Four concrete strategies exist (`strategy/*.hpp`): `PassiveMarketMaker` (the one with
an explicit Flat→Quoting→Position→Exiting state machine),
`OrderBookImbalance` and `TradeFlow` (book/trade-driven), and
`OracleThresholdStrategy` (a straight port of the original oracle-driven signal —
explicitly reference/demo-only, kept so `strategy/oracle_feed.hpp` and
`tools/strategy_demo.cpp` stay exercised, not a peer of the other three).

### Every external API fact is independently verified, not assumed

Kalshi's WebSocket message schema, REST order-create/cancel schema, and endpoint hosts
are all fetched from `docs.kalshi.com` and cross-checked against real payload examples
— see the SCHEMA STATUS comments at the top of `live/kalshi_feed.hpp` and
`live/kalshi_rest.hpp`. An earlier version of the feed parser guessed the schema and
was wrong (parsed nothing while reporting success — every numeric field Kalshi sends is
a *quoted decimal string*, not a bare number). Never add or modify a Kalshi field
assumption without fetching the current docs first; when something is genuinely
unconfirmed even after checking (e.g. whether `seq` is scoped per-subscription or
per-market — see `kalshi_feed.hpp`'s SEQ SCOPING comment), the code says so explicitly
and picks the direction that fails safe, rather than picking silently and hoping. Same
discipline caught a wrong assumption before it was ever written: `market_lifecycle_v2`'s
`event_type` enum has **no `"halted"` value** — confirmed by checking the spec, not
inferred from the channel's name — so the mapping (`activated`→Open,
`deactivated`→Halted, `determined`/`settled`→Resolved) uses the real values, not an
invented one (see `kalshi_feed.hpp`'s SCHEMA STATUS note).

### Reconnection and market lifecycle (Stage 4)

`Ingress::run` (`pipeline/ingress.cpp`) retries connect/subscribe/receive failures with
capped exponential backoff (1s→30s) rather than exiting T1's thread on the first one — a
fresh `live::KalshiFeed` is constructed per attempt, never reset in place, because
`TlsSocket` deletes its copy ctor/assignment without declaring move, which suppresses
the *implicit* move too (found while building this — see `pipeline/ingress.hpp`'s `feed_`
comment). After a successful reconnect, every watched ticker gets an explicit
`request_resync` — reusing the same mechanism a seq gap already triggers — so the
`ShadowBook` resync goes through already-tested paths (`SnapshotBegin` clearing the book,
a `Snapshot` re-baselining `SeqTracker`) rather than new ones.

`Brain` tracks one `px::MarketStatus` per market (reused from the offline engine's own
`PreOpen`/`Open`/`Halted`/`Resolved` vocabulary, `px/market.hpp` — not a parallel live-only
enum), defaulting to `Open` rather than `PreOpen` since Kalshi's lifecycle channel reports
*changes* from subscription time onward, not a backfill of current status.
`Brain::handle_decision` gates on it before `RiskEngine` runs: `Halted` blocks new orders
but still allows `Cancel` (risk-reducing, same reasoning `RiskEngine::check_cancel` already
gets); `Resolved` blocks everything. See `risk::Verdict::RejectMarketHalted`/
`RejectMarketResolved` — produced by `Brain`, not `RiskEngine::check()`/`check_cancel()`,
which still does "exactly three things and nothing more" as its own header claims.

### Performance tooling and one accepted optimization (Stage 5)

`workload/` (new) holds a deterministic seeded workload generator
(`generator.hpp`, reusing `tests/test_replay_fuzz.cpp`'s
`std::mt19937_64 rng(42)` convention) and a cross-core latency-measurement
primitive (`latency_watcher.hpp`) for measuring throughput under sustained,
many-events-in-flight load — a different regime from `bench_ring.cpp`/
`bench_pipeline_e2e.cpp`'s deliberate one-event-at-a-time ping-pong
discipline, which exists specifically to isolate per-op cost from queueing
delay (see PERFORMANCE.md's "methodology bug" section). `perf/` holds the
sweep drivers, plotting scripts, baseline snapshots, and regression
comparator built on top. Full methodology, results, and an expanded
Limitations section are in [PERFORMANCE.md](PERFORMANCE.md)'s Stage 5
section, not repeated here.

Two changes from that pass affect the code directly:

- **`kMaxMarkets` raised 256 → 1024** (`px/types.hpp`) to reach the
  multi-market scaling sweep's higher points — mechanical (every use is
  flat `std::array` sizing), but it grew `sizeof(Brain)` to ~3.0 MB and
  `sizeof(OrderTracker)` to ~1.04 MB; re-check both types' SIZE
  WARNING/NOTE comments before declaring either as a local.
- **`Brain::dispatch_market_triggered` now indexes strategies by market**
  (`strategy_index_by_market_`, populated once in `add_strategy`) instead
  of linearly scanning every registered strategy on every dispatch —
  profile-driven (a targeted micro-benchmark found a real, if modest, O(N)
  cost at N=500-1000 total registered strategies), measured before and
  after, and re-verified against the full existing correctness suite.
  `evaluate_oracle_event`'s own all-strategies scan is unchanged — that one
  is intentional (a single oracle event can matter to several differently-
  registered strategies at once), not the bug this fixed.

### Evaluation, telemetry, and the dashboard (Stage 6)

`eval/` (Part B.3) drives a real `pipeline::Brain`/`Strategy`/`RiskEngine` — not a
parallel implementation — synchronously against a seeded synthetic market
(`eval/synthetic_market.hpp`), for reproducible strategy comparisons.
`eval/backtest_runner.hpp`'s `BacktestRunner` is the harness: one strategy per
runner (never several sharing a market — see that class's own header comment for
why), a one-tick delay between an intent's own cause and its resulting exec
report (found necessary, not designed in from the start — same file), and an
explicit DIVERGENCE GUARD that stops advancing a strategy once `|position|`
blows through a generous threshold rather than let a runaway scenario silently
corrupt its metrics. `tools/run_backtest.cpp` is the CLI front end; see
`docs/BACKTEST_ASSESSMENT.md` for full methodology/limitations and
`docs/STRATEGY_COMPARISON.md` for actual results (headline finding:
`PassiveMarketMaker` diverges in every seed tried — a real strategy/RiskEngine
interaction bug, not a framework artifact — see `docs/DECISION_GATE.md` for what
that blocks). `eval/eval_metrics.hpp`'s `risk_adjusted_return_proxy()` is
deliberately not called a Sharpe ratio — a single backtest path has no
independent trials to compute one honestly — see that function's own comment.

`telemetry/` (Part G) is the metrics model: `TelemetryState` holds atomics +
fixed-capacity `RecentRing` buffers, written ONLY from `Brain`'s own thread (T2)
via `pipeline::Brain::attach_telemetry` (optional, nullptr by default, same
pattern as `Brain::on_reject` — zero behavior change when not attached), safe for
any other thread to read at any time without coordinating with T2. Two tiers,
deliberately separate (see `telemetry/telemetry.hpp`'s own header comment):
`telemetry::PositionAccount` (`position_account.hpp`) is T2-private VWAP cost-basis/
realized-P&L bookkeeping, never read cross-thread directly; `TelemetryState`'s
atomic fields are the published, cross-thread-safe derived view. **A market's
telemetry `status` field must stay in sync with `Brain`'s own `market_status_`
default (`Open`, not `PreOpen`)** — republished on every book event in
`Brain::apply_book_event`, not only on explicit `LifecycleStatus` ones; a prior
version of this only defaulted correctly on paper and drifted from `Brain`'s real
default until the dashboard's own rendering caught it (see
`telemetry/telemetry.hpp`'s `MarketSnapshot::status` comment and the regression
test in `tests/test_telemetry.cpp`). `telemetry/telemetry_publisher.hpp`'s
`publish_snapshot` writes JSON via temp-file-then-rename for atomicity; its own
comment documents a real, empirically-found Windows-specific caveat (the rename
can transiently fail if a reader's file handle is open at that instant) that
self-heals one publish cycle later and is deliberately not "fixed" with
Windows-specific code.

`dashboard/` (Part E/F) is a separate Rust/Axum crate (`cargo build --release`,
not part of this `Makefile`) that polls the JSON file `tools/kalshi_pipeline.cpp`'s
existing 500ms loop publishes and serves a page over it — **no channel back into
the C++ process at all**, a hard architectural line, not a simplification (see
`docs/DASHBOARD.md`). `/api/snapshot` passes through whatever valid JSON is
currently in the file rather than deserializing into a hand-written Rust struct,
specifically so a schema drift between the two languages fails loudly (a table
cell renders `undefined`) instead of silently. Both binaries default their
respective `--telemetry-out`/`--telemetry-path` flags to the same relative path,
`telemetry/snapshot.json` (matching `.gitignore`'s pre-existing entry) — run both
from the repo root, or pass matching explicit paths.

**Stage 6A** replaced the original stat-card-grid frontend with a hierarchical
trading-workstation layout (system status → performance+P&L chart → market/order-
book → positions/orders → strategy activity → risk → system events), which needed
real backend additions, not just CSS: per-market order-book depth
(`TelemetryState::on_depth_update`, a bounded top-N scan of `ShadowBook::level_qty`
from `Brain::apply_book_event`, gated on `telemetry_ != nullptr` same as everything
else), a public trade tape (`on_public_trade`, from `BookEventKind::Trade`), a
session P&L history ring (`on_order_filled` samples unthrottled, `on_book_update`
throttled to 1/`kPnlSampleMinIntervalMs`), live risk-limit figures
(`set_risk_limits` once at startup from `RiskConfig`, `set_rate_limit_tokens`
sampled in `Brain::handle_decision` alongside its own existing `risk_.check()`
call), a distinct `risk_rejections_` counter for Brain's own pre-send rejections
(vs. `orders_rejected_`, Kalshi's post-send ones — see `OrderEventKind::RiskBlocked`),
and an `OrderId` threaded into every `OrderEventRecord` so the frontend can group
one order's own lifecycle client-side. Full field-by-field inventory — including
what was deliberately NOT added rather than fabricated — is
`docs/DASHBOARD_DATA_CONTRACT.md`, written per that redesign's own "document what's
real before touching the UI" requirement.

### Domain rules that shape everything

- **Integer cents only**, ticks 1–99 (0/100 are settlement values, never live order
  prices) — no floating point on any price path. See `include/px/types.hpp`.
- **YES/NO collapses to one book**: `include/px/normalize.hpp` rewrites every NO order
  to its YES-equivalent at the entry boundary (`BUY NO @ p` ≡ `SELL YES @ (100-p)`), so
  the matching engine and the live book both only ever handle one asset. Kalshi's own
  V2 REST API converged on the identical normalization (`side: bid/ask` on a single
  book, not `yes/no` token selection) — see `live/kalshi_rest.cpp`.
- **MARKET is LIMIT-at-the-extreme-tick + IOC internally** — one matching code path
  serves both order types (`OrderBook::submit`).
