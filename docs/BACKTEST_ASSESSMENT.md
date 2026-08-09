# Backtest/Replay Infrastructure Assessment — Stage 6, Part B.2

## The question

> Does the current replay infrastructure let us evaluate whether the live pipeline's
> execution decisions are actually good?

## What already exists, and what it actually proves

| Component | What it proves |
|---|---|
| `workload/generator.hpp` | Deterministic, seeded, realistic Kalshi-wire-shaped market data (snapshot/delta/ticker/trade/lifecycle), three traffic profiles. |
| `tests/test_replay_determinism.cpp` | A fixed frame sequence, through the real `Ingress::handle_message` + a real `Brain`, converges to an exact, hand-verified `ShadowBook` state — deterministically, run to run. |
| `tests/test_replay_fuzz.cpp` | The same real-`Brain` path under adversarial/randomized input, differentially checked against an independent reference model, `ShadowBook::check_invariants()` after every event. |
| `bench/bench_workload_throughput.cpp` | A real `Ingress → Brain → Gateway` closed loop, with a real `Strategy` registered, under sustained load — for **throughput**, not P&L. |

Every one of these proves the pipeline is *mechanically* correct and fast. **None of
them asks whether a trade was a good idea.**

## The concrete gap, found by reading the code, not assumed

`bench_workload_throughput.cpp`'s own `SimulatedExchangeSink::place()`
(and `tests/test_pipeline_e2e.cpp`'s independently-hand-rolled equivalent) fills
**exactly `qty_filled_at_entry_`** — a fixed constructor argument (`0` in the
throughput benchmark), regardless of what the market was actually doing at that
moment. It never reads `ShadowBook` state, never checks whether a resting order's
price was ever actually crossed, and has no notion of how much resting depth was
ahead of an order in the queue. That's the right design for what those files are
actually testing (does the pipeline stay correct/fast under load) — and the wrong
design for asking "if `PassiveMarketMaker` had been quoting against this exact
historical book, would it have made money." Concretely, this project currently has:

- **No fill realism**: no maker/taker distinction, no queue-position awareness, no
  partial-fill-by-available-depth modeling for a marketable order.
- **No fee model**: nothing computes Kalshi's actual trading fees anywhere.
- **No P&L tracking**: `RiskEngine` tracks position for exposure purposes only;
  nothing accumulates cash, marks an open position to a settlement or current price,
  or reports gross/net P&L.
- **No settlement modeling**: a market transitioning to `MarketStatus::Resolved` is
  gated correctly (Stage 4 — orders stop, cancels still allowed) but nothing marks an
  open position to its settlement value (0¢ or 100¢) when that happens.

## What does NOT need to be rebuilt

The market-data replay path itself — `workload::Generator` producing realistic wire
frames, `Ingress::handle_message` parsing them, `Brain` updating `ShadowBook` and
dispatching `Strategy::on_event` — is already correct, already tested three different
ways (determinism, adversarial fuzzing, throughput), and is exactly what a backtester
needs on the market-data side. Rebuilding it would violate the plan's own instruction
not to build a sophisticated simulator the architecture doesn't need. The gap is
entirely on the **execution/fill/accounting** side, downstream of where `Brain`
already correctly decides *what* a strategy wants to do — the same boundary
`live/kalshi_gateway.hpp`'s own header comment already draws ("Gateway never decides
*whether* to send... What Gateway decides is *how*").

## What Part B.3 adds — `eval/`

A small module reusing everything above as-is:

- **`eval/fees.hpp`** — Kalshi's real, externally-verified maker/taker fee formula
  (see `docs/EXTERNAL_REVIEW.md` item 3), integer-cent arithmetic throughout, no
  floating point on the fee path — same domain rule `px/types.hpp` already states for
  prices.
- **`eval/fill_simulator.hpp`** — the one genuinely new piece: a queue-position-aware
  maker-fill model built from data this project's real feed actually provides (trade
  events + level deltas), not invented from nothing. A resting GTC order records
  `depth_ahead` at ack time using the **exact same formula**
  `strategy::OrderTracker::on_acked` already uses for its own (already-tested)
  `depth_ahead_estimate` — real market volume trading through that price decrements
  it, and only once it's exhausted does further volume fill *our* resting order. A
  marketable (IOC) order sweeps `ShadowBook`'s aggregate levels the same way
  `OrderBook::submit`'s own MARKET-sweep code path does, capturing realistic
  multi-level slippage rather than assuming a single fill price.
- **`eval/eval_metrics.hpp`** — the metrics Part B.3 asks for, accumulated per
  strategy: orders, fills, fill rate, cancel rate, maker/taker ratio, gross/net P&L,
  fees, max drawdown, inventory exposure/turnover, time-to-fill, and a realized-spread
  / adverse-selection decomposition (see `docs/EXTERNAL_REVIEW.md` item 9 for why the
  simple decomposition, not a full VPIN-style toxicity model). **Decision latency is
  deliberately NOT re-measured here** — `bench/bench_strategy.cpp` already measured
  it correctly, in isolation, with real p50/p99 numbers in the README; re-deriving it
  sloppily inside a backtest loop that's timing "how fast can we iterate scripted
  history," not real per-call latency, would be exactly the kind of unscrutinized
  measurement this project's own README warns against.
- **`eval/backtest_runner.hpp`** — the harness that ties it together. Drives a real
  `pipeline::Brain` — the *exact* class the live pipeline uses, not a reimplementation
  — through a **single-threaded, fully sequential** event loop instead of the live
  pipeline's three-threads-three-rings arrangement. This is a deliberate architectural
  choice, not a simplification of convenience: `Brain::run()`'s threaded loop
  interleaves ring A (book events) and ring C (exec reports) in whatever order the OS
  scheduler happens to produce, which is exactly the kind of nondeterminism
  `tests/test_replay_determinism.cpp`'s own header comment already flags as unsafe to
  claim ("the oracle/signal/risk path genuinely depends on wall-clock time... claiming
  byte-identical determinism there would itself be the kind of unverified claim this
  project avoids"). A backtest's entire value proposition is **reproducibility** — the
  plan's own Definition of Done requires it explicitly — so the eval framework instead
  calls `Brain::apply_book_event`/`Brain::apply_exec_report` directly and
  synchronously (both promoted from private to public on `Brain` for exactly this —
  see that class's own comment), feeding a local, single-threaded `RingB`/`RingC` pair
  it fully controls the order of. Same real dispatch/risk/strategy code path Brain's
  threaded `run()` uses internally; zero duplicated logic; fully deterministic.

## A fourth limitation, found only by actually running the tool

Not theorized in advance — found the same way this project has found several other
real bugs (see README's "Measuring latency" and "A synthetic workload generator
caught its own measurement bug" sections): the first real run of
`tools/run_backtest.cpp` produced an implausible ~13,600 rejected orders out of a
few dozen accepted ones for `OrderBookImbalance`. Cause: `RiskEngine`'s token bucket
(`risk/risk_engine.hpp`) is correctly, deliberately driven by real wall-clock time —
but `Brain::handle_decision` (`pipeline/brain.cpp`) reads that clock via its own
internal `now_ns()` (`std::chrono::steady_clock`), not a caller-injectable one.
Replaying 20,000 synthetic events takes single-digit milliseconds of real time, so a
bucket sized for Kalshi's real ~10-orders/sec budget drains once and then rejects
nearly everything for the rest of the run — an artifact of compressed replay speed,
not a real finding about any strategy's fill behavior.

**Not fixed by adding a clock-injection seam to `Brain`** — a real option, but one
that touches a class this project already treats with unusual care (see its own SIZE
WARNING and single-writer-thread discipline), for a benefit this framework doesn't
actually need: rate-limit behavior is already correctly tested, with real injected
timing, at the `RiskEngine` layer directly (`tests/test_risk.cpp`), and this
framework's job is strategy decision quality, not re-proving the rate limiter a
second time. Instead, `tools/run_backtest.cpp` configures a generously large token
budget for backtesting specifically (fat-finger and exposure checks — genuinely
price/position state, not wall-clock-dependent — stay at their real defaults) and
documents exactly why in code, rather than let a replay-speed artifact silently
distort every other number in Part H's table.

## A fifth finding: a real production clock bug, found and fixed

Not a limitation of this framework — a genuine, previously-undiscovered correctness
bug in `pipeline/brain.cpp`, from Stage 3, found by actually running a real
`TradeFlow` strategy through a real `Brain` in `eval/backtest_runner.hpp` and
noticing it placed **zero** orders across 20,000 synthetic events, no matter how
obviously favorable the synthetic trade tape was.

`Brain::dispatch_market_triggered`/`evaluate_oracle_event` both called
`extract_features(..., now_ns())`, passing a `std::chrono::steady_clock`
nanosecond reading into the parameter `strategy/market_state.hpp` itself names
`now_ms`. `strategy/trade_window.hpp`'s `TradeWindow::recent_counts(now_ms)`
compares that value directly against `TradeRecord::ts_ms` — Kalshi's own real
wall-clock, milliseconds-since-Unix-epoch trade timestamp
(`live/kalshi_feed.cpp`'s trade decoder). `steady_clock`'s epoch is arbitrary
(commonly time-since-boot) and its unit is nanoseconds, not milliseconds — so this
comparison was between two numbers with no defined relationship to each other.
`TradeFlow` is the *only* shipped strategy that reads `MarketState::recent_trades`,
and this bug meant it could never see a real trade as "recent" — a silent,
permanent no-op in the **live pipeline too**, not just this evaluation framework.

Existing tests never caught it because `tests/test_strategies.cpp` (76 checks)
exercises `TradeWindow`/`extract_features` directly, with explicit, controlled
`now_ms` values chosen by the test — never through this specific `Brain` call site
with a real elapsed wall-clock gap between recording a trade and querying for it.

**Fixed**: a second, separate clock helper (`now_ms_wall()`, `system_clock`-based)
now feeds `extract_features`'s two call sites specifically; `RiskEngine`'s own three
call sites (`check`/`check_cancel`/`on_rate_limited_response`) keep the original
`now_ns()` (`steady_clock`-based) unchanged — that one is correctly monotonic and
epoch-independent by design (a token bucket only cares about elapsed time, immune
to NTP step adjustments, not wall-clock alignment). Verified by re-running the
**entire** existing test suite (`test` + `test-live`, every prior check count
unchanged) and by confirming `TradeFlow` places real orders in this framework's own
backtest once the fix lands — see `tools/run_backtest.cpp`'s output.

## A sixth finding: PassiveMarketMaker can diverge under adversarial evaluation

Running `PassiveMarketMaker` through a long (20,000-event) seeded synthetic replay
surfaced a real, if narrow, interaction between two independently-reasonable
designs: `RiskEngine`'s exposure check only accounts for *filled* position (via
`record_fill`) — resting/in-flight exposure is invisible to it, a gap
`risk/risk_engine.hpp`'s own header comment already documents explicitly, naming
`PassiveMarketMaker` by name as the strategy shape that can trigger it. In
production that gap's blast radius is bounded by real signing/REST latency
(~1ms, `bench_sign.cpp`) and Kalshi's real rate limit — both of which this
evaluation framework deliberately widens or disables for other good, documented
reasons (the one-tick ack/fill delay in `eval/backtest_runner.hpp`; the generous
`backtest_risk_config()` in `tools/run_backtest.cpp`, needed to avoid the wall-clock
rate-limit-compression artifact described above). Remove both real-world dampers at
once, on a long enough adversarial run, and `PassiveMarketMaker`'s reactive exit
logic (re-evaluates on every `OwnOrderUpdate`, including its own not-yet-filled
order's `Acked`) can submit many same-direction `Cross` orders before any of their
fills are known, each individually accepted, compounding into a real position
excursion.

**Not fixed at the strategy or `RiskEngine` level** — both are stable, extensively
tested Stage 3/2 code, and the gap is already honestly documented as a known,
accepted simplification, not a silent one. Instead, `eval/backtest_runner.hpp`
adds an explicit, documented **divergence guard**: once a strategy's `|position|`
exceeds a generous multiple of `RiskConfig`'s own default exposure cap, the runner
stops advancing that strategy and `StrategyReport::diverged` says so — visible in
`tools/run_backtest.cpp`'s output, not silently absorbed into a corrupted number.
Part H's comparison table reports this plainly for any strategy/seed combination
where it fires, rather than pretending the run completed cleanly.

## What's still a documented limitation, not silently ignored

- **No network/signing latency modeled**: an order is treated as acked in the same
  tick it's submitted. The live pipeline's real ack latency (RSA-PSS signing +
  REST round-trip, ~1ms p50 per `bench/bench_sign.cpp`) is real time a resting order
  spends *not yet visible to the exchange* — this framework doesn't model that gap.
  Flagged, not faked; closing it would mean modeling Gateway's own timing inside the
  backtest, a materially bigger undertaking than this stage's evaluation-framework
  charter.
- **Aggregate-book queue estimate, not L3 ground truth**: `depth_ahead` is an upper
  bound at ack time (identical semantics/limitation to `OrderTracker`'s own live
  estimate — see `strategy/order_tracker.hpp`'s `RestingOrder::depth_ahead_estimate`
  comment), not a live/exact count. Kalshi's public feed doesn't expose per-order (L3)
  data, so no simulator built on it honestly could do better without inventing data.
- **Synthetic data only, this pass**: run against `workload::Generator`'s seeded
  synthetic traffic, not real historical Kalshi data (`docs/EXTERNAL_REVIEW.md` item
  13 — Kalshi's real `/historical/*` endpoints are a documented future upgrade, not
  pulled this stage). Part H's comparison table is explicit about this in its own
  results, not just here.
