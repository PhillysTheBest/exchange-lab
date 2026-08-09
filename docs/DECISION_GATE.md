# Decision Gate — Stage 6, Part I

The final roll-up `docs/EXTERNAL_REVIEW.md`'s summary table promises: every
KEEP / BUILD / DEFER / REJECT call made across this stage's five other
documents, in one place, plus the question none of those five were scoped to
answer on their own — **is this system ready to trade real capital, and if
not, what specifically is blocking it.**

**Verdict up front: NOT READY for real capital.** Nothing found this stage is
catastrophic, and several parts of the system (the engine core, the live
pipeline's reconnection/lifecycle handling, the evaluation framework itself)
are genuinely solid — but real, concrete, specifically-named gaps remain
between here and a responsible live-money decision. They're listed below, not
glossed over. This document does not change `tools/kalshi_pipeline.cpp`'s
existing default posture (`DryRunSink`, demo host only, real sends require
both `--demo` and credentials) — nothing in this stage's findings argues for
loosening that, several findings argue for keeping it exactly where it is.

## Roll-up: every decision made this stage

| Source | Item | Decision |
|---|---|---|
| Part A | Inventory-aware (Avellaneda-Stoikov) quoting | DEFER — correctly sequenced after Part B/H's evaluation framework existed; revisit now that it does, but not this stage |
| Part A | Realistic backtesting (queue/latency/partial fills) | **BUILT** — `eval/`, `docs/BACKTEST_ASSESSMENT.md` |
| Part A | Maker/taker fee-aware P&L | **BUILT** — `eval/fees.hpp` |
| Part A | Cross-exchange arbitrage | see Part C below |
| Part A | LLM/AI-ensemble signals | REJECT |
| Part A | Global cross-market exposure caps | DEFER |
| Part A | Sharpe/drawdown/equity-curve scorecards | **BUILT** — `eval/eval_metrics.hpp`'s `max_drawdown_cents` + `risk_adjusted_return_proxy()` (added this pass specifically to close this item out — see "What changed since Part A's original BUILD calls" below) |
| Part A | Real-time observability dashboard | **BUILT** — `telemetry/`, `dashboard/` |
| Part A | Realized-spread / adverse-selection metric | **BUILT** (simple form only) — `eval/eval_metrics.hpp` |
| Part A | On-chain trade-direction inference | REJECT (inapplicable — Kalshi isn't on-chain) |
| Part A | Rust-periphery / C++-hot-path split | **BUILT** — `dashboard/` is the Rust periphery; nothing about the hot path moved |
| Part A | Pluggable strategy framework | KEEP (already had it — Stage 3) |
| Part A | Official Kalshi historical-data endpoints | DEFER |
| Part C | Cross-exchange (Kalshi/Polymarket) arbitrage | DEFER — one real snapshot checked, gaps found were fee-sized (≤2pp), not exploitable; see `docs/CROSS_MARKET_RESEARCH.md` |
| Part D | Weather (NOAA) oracle feed | DEFER — verified buildable, not built (avoiding pre-evaluation feature creep) |
| Part D | Economic-release APIs (BLS/FRED) as a trading signal | REJECT — confirmed unusable by their own documented publication lag |
| Part D | Scheduled-release risk-window flag | DEFER — real idea, belongs to a future `RiskEngine` extension (ties to Part A item 6), not built |
| Part D | News/LLM-summarized signals | REJECT — violates this project's own "no LLM on/near the trading decision path" line |
| Part D | Structured official feeds (EDGAR, FAA) | DEFER — no concrete matching Kalshi market identified yet |

## What Parts B/G/E/F/H actually found, not just what they built

Six real, previously-unknown findings came out of *running* this stage's own
tools against a real `Brain`, not from writing the tools:

1. **A clock-unit bug that silently disabled `TradeFlow` everywhere**, live
   pipeline included — `Brain` was passing a `steady_clock` nanosecond reading
   into a parameter `TradeWindow` compares against Kalshi's own wall-clock
   trade timestamps. Found building `eval/`, fixed in `pipeline/brain.cpp`,
   verified against the full test suite. (`docs/BACKTEST_ASSESSMENT.md`,
   fifth finding.)
2. **A same-tick Acked-before-Fill race** in the original synchronous-replay
   design, fixed with `eval/backtest_runner.hpp`'s explicit one-tick delivery
   delay. (`docs/BACKTEST_ASSESSMENT.md`, fourth/sixth findings' surrounding
   text.)
3. **`PassiveMarketMaker` diverges under this evaluation framework's
   adversarial (zero real-world-latency) conditions in 5 out of 5 seeds
   tried** — not a one-off. `RiskEngine`'s exposure check only sees *filled*
   position, not resting/in-flight; removing signing/REST latency and the real
   rate limit (both deliberately, for other documented reasons) lets its
   reactive exit logic compound into a real position excursion before any
   single order's fill is known. Caught by `eval/backtest_runner.hpp`'s
   DIVERGENCE GUARD, not silently absorbed into a corrupted P&L number.
   (`docs/BACKTEST_ASSESSMENT.md`'s sixth finding; consistency across seeds
   confirmed in `docs/STRATEGY_COMPARISON.md`, Part H.)
4. **`telemetry::MarketSnapshot::status` defaulted to `PreOpen` (0),
   independent of `Brain`'s own real default of `Open`** — a market `Brain`
   already considered tradeable would show "PreOpen" on the dashboard until
   (if ever) an explicit lifecycle event arrived for it. Found by actually
   looking at the rendered dashboard against a real replay, not by code
   inspection — fixed in `telemetry/telemetry.hpp` (corrected default) and
   `pipeline/brain.cpp` (status now republished on every book event, not only
   explicit `LifecycleStatus` ones), with a regression test
   (`tests/test_telemetry.cpp`) added specifically because this class of bug
   (a display default silently drifting from the source of truth's default)
   had zero test coverage before.
5. **A real, empirically-reproduced Windows file-locking race** between the
   C++ writer's atomic rename and the dashboard's own concurrent read of the
   same file — reproduced by hand while testing the dashboard against a live
   pipeline (not theorized). Self-heals one publish cycle (~500ms) later;
   deliberately not "fixed" with Windows-specific sharing flags on the Rust
   side — see `telemetry/telemetry_publisher.hpp`'s updated header comment for
   the full reasoning.
6. **`ExecReport` carries no fee field** — `Brain`'s live-pipeline telemetry
   hook (`on_order_filled`) always publishes `fee_cents=0` for a real fill,
   unlike the backtest path (`eval/fees.hpp`), which computes a real one. This
   is a genuine live-readiness gap, not just a cosmetic one — see below.

None of these six were caught by the pre-Stage-6 test suite. All six were
caught by actually *running* the tools this stage built — the concrete
argument, if one were needed, for why Part B/G/H's evaluation-and-observability
work earned its place in this stage rather than being purely additive polish.

## Concrete blockers to a real-capital decision

In priority order — highest-impact first:

1. **`PassiveMarketMaker` cannot currently be evaluated, let alone deployed.**
   Every backtest run diverges, so there is no honest number for its expected
   P&L, fill behavior, or risk profile. Fixing this needs one of: (a) giving
   `RiskEngine` visibility into resting/in-flight exposure (a real, scoped
   change to a class this project treats with above-average care — see
   `risk/risk_engine.hpp`'s own header comment, which already names this gap),
   or (b) adding a re-entry throttle to `PassiveMarketMaker`'s own exit logic.
   Neither is started. **This strategy should not run with real capital in its
   current form, full stop** — not "run it small and see," since the failure
   mode observed (an unbounded position excursion) is exactly the one class of
   bug real capital cannot absorb safely.
2. **No live fee tracking** (finding 6 above). A live dry-run/demo session's
   dashboard would show `Fees Paid: $0.00` regardless of what Kalshi actually
   charges — invisible, not just imprecise, since the field silently reads a
   valid-looking zero rather than an obvious placeholder. Needs `ExecReport`
   (or a parallel channel) to carry a real fee from Gateway's own REST
   response before a live P&L number on this dashboard can be trusted.
3. **`OrderBookImbalance` is the only strategy with a complete, undiverged
   evaluation — and even that is a much weaker bar than "ready."** Its Part H
   results are genuinely mixed (net P&L positive in 3/5 seeds, negative in
   2/5, no seed dominating) — real two-sided risk-taking, not a red flag by
   itself, but also not yet a demonstrated edge across enough independent
   conditions to size real capital against. Five seeds of one synthetic
   generator family is a start, not a sufficient sample.
4. **`TradeFlow` is unevaluated, not evaluated-and-passing.** 0–2 orders per
   20,000-event run across every seed tried is too small a sample to say
   anything. This is a synthetic-data-generator gap (see item 5), not a
   strategy-quality finding either way.
5. **Synthetic data only, one generator family, no real settled-market
   history.** Every number in `docs/STRATEGY_COMPARISON.md` is against
   `eval::SyntheticMarket` — already an explicit, named limitation in
   `docs/BACKTEST_ASSESSMENT.md`, worth repeating here because it caps how
   much weight *any* of Part H's numbers can carry toward a real-money
   decision, independent of which strategy is being discussed. Kalshi's real
   `/historical/*` endpoints (Part A item 13, DEFER) are the concrete next
   step, not a new synthetic-market variant.
6. **No automated kill-switch or alerting beyond the dashboard's own visual
   staleness indicator.** `RiskEngine`'s per-request checks (fat-finger,
   exposure, rate limit) are real and tested, but there's no standing process
   that pages a human or halts trading if, say, `reconnect_count` starts
   climbing rapidly or `pnl_net_cents` crosses a threshold while nobody's
   looking at the dashboard tab. Acceptable for a supervised demo/dry-run
   session; not acceptable as the sole safety net for unattended real-capital
   operation.

## What's actually in good shape

Worth stating plainly, not just cataloging gaps: the core matching engine
(Stage 1), the live pipeline's three-thread/three-ring architecture and its
reconnection/market-lifecycle handling (Stage 2/4), the Strategy execution-
decision interface (Stage 3), and this stage's own evaluation/telemetry/
dashboard machinery are all real, tested, and — per the findings above — have
already survived being pointed at their own edge cases hard enough to surface
six genuine bugs, all fixed, all verified against the full existing suite with
zero regressions. That's the foundation a real-capital decision would be built
on, once the blockers above are closed — not a foundation that itself needs
rebuilding.

## Recommended order of work, if this continues

1. Close blocker 1 (`PassiveMarketMaker` divergence) — either the `RiskEngine`
   exposure fix or the strategy-level throttle, whichever review of
   `risk/risk_engine.hpp`'s existing gap comment favors, with its own tests.
2. Close blocker 2 (live fee tracking) — smaller, more mechanical, and it's
   the difference between the dashboard's P&L numbers being trustworthy or
   just plausible-looking once real orders start flowing.
3. Pull a real historical-data sample (blocker 5) via Kalshi's
   `/historical/*` endpoints and re-run Part H's comparison against it
   alongside the synthetic one, not instead of it — two independent evidence
   sources agreeing is a much stronger basis than either alone.
4. Only then: a small, explicitly-bounded, closely-supervised real-capital
   pilot (smallest possible size, one strategy — `OrderBookImbalance`,
   currently the least-unevaluated of the three — one market, a human
   watching the dashboard the whole time), not a switch flipped to "live" and
   left running unattended.

Nothing above is this session's call to make unilaterally — it's the honest
input a real go/no-go decision needs, assembled from what six documents
(`EXTERNAL_REVIEW.md`, `CROSS_MARKET_RESEARCH.md`, `EXTERNAL_INFORMATION.md`,
`BACKTEST_ASSESSMENT.md`, `STRATEGY_COMPARISON.md`, `DASHBOARD.md`) and this
one actually found.

## What changed since Part A's original BUILD calls

One item is worth flagging explicitly: Part A's summary table (reproduced
above) already marked "Sharpe/drawdown/equity-curve scorecards" **BUILD** back
when it was written, but `eval/eval_metrics.hpp` as originally committed for
Part B.3 only ever implemented the drawdown half (`max_drawdown_cents`) — no
equity-curve-derived risk-adjusted-return figure existed until this pass added
`risk_adjusted_return_proxy()` specifically to make that BUILD call true rather
than leave it silently unfulfilled. It's deliberately not called a Sharpe
ratio (a single-path synthetic backtest has no independent trials to compute
one honestly — see that function's own header comment) — closing the letter of
Part A's decision without overclaiming what the number actually is.
