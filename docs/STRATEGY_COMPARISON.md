# Strategy Comparison — Stage 6, Part H

This is the comparison table Part B.3's evaluation framework (`eval/`,
`docs/BACKTEST_ASSESSMENT.md`) was built to produce: `PassiveMarketMaker`,
`OrderBookImbalance`, and `TradeFlow` (`strategy/*.hpp`), run against several
seeded synthetic replays via `tools/run_backtest.cpp`, with the honest findings
that came out of actually running it — not just the numbers.

**Methodology, in one paragraph** (full detail in `docs/BACKTEST_ASSESSMENT.md` —
not repeated here): each strategy runs against its own fresh, identically-seeded
`eval::SyntheticMarket` (`eval/synthetic_market.hpp`) through a real
`pipeline::Brain`/`RiskEngine`, one strategy per market so none compete for the
same queue position. 20,000 events per run. `RiskEngine`'s token-bucket rate
limit is configured generously (real wall-clock time inside a sub-second replay
would otherwise drain and reject almost everything — an artifact, not a
finding, see `BACKTEST_ASSESSMENT.md`'s fourth finding); fat-finger and exposure
checks stay at their real production defaults. This document doesn't re-explain
any of that plumbing, and it inherits every limitation
`BACKTEST_ASSESSMENT.md`'s "What's still a documented limitation" section
already lists (synthetic data only, no signing/REST latency modeled,
aggregate-book depth estimate) — read that section before treating any number
below as more than what it is.

**Reproduce any of this**:

```bash
mingw32-make run_backtest
./build/release/run_backtest.exe --seed=42 --events=20000 --settle=yes
```

`--seed` selects the synthetic replay, `--events` its length, `--settle`
`yes`/`no`/(omitted) selects how any still-open position at the end is marked
(see "Settlement assumption swings the number" below) — every combination is
deterministic (same seed -> byte-identical report, the same guarantee
`tests/test_replay_determinism.cpp` established for the book-update path alone,
now extended through the fill/P&L path — see `BACKTEST_ASSESSMENT.md`).

## Headline finding: PassiveMarketMaker diverged in every seed tried

| seed | events to first divergence (approx.) | orders at divergence | final \|position\| |
|------|--------------------------------------:|----------------------:|---------------------:|
| 1     | before 20,000 (early — see below)    | 113   | 2,060 |
| 7     | before 20,000                        | 1,534 | 2,007 |
| 42    | between 4,500 and 5,000               | 843   | 2,016 |
| 100   | before 20,000                        | 1,532 | 2,004 |
| 12,345 | before 20,000                        | 453   | 2,010 |

5 out of 5 seeds tripped `eval/backtest_runner.hpp`'s DIVERGENCE GUARD
(`|position| > 2000`, 20x `RiskConfig`'s default exposure cap) before the
20,000-event window ended. This isn't new — `BACKTEST_ASSESSMENT.md`'s sixth
finding already documents the mechanism (RiskEngine's exposure check only sees
*filled* position, not resting/in-flight; removing this framework's two
real-world dampers — signing/REST latency and the real rate limit — lets
`PassiveMarketMaker`'s reactive exit logic submit many same-direction `Cross`
orders before any of their fills are known). What Part H adds is the
**consistency**: this isn't a one-seed fluke, it reproduces across every seed
tried, and `final position` lands at a strikingly narrow ~2,000-2,060 band each
time — the guard threshold itself, not the strategy's own decision logic,
determined when each run stopped. **This framework cannot currently produce a
full-window, undiverged comparison number for `PassiveMarketMaker`** — every
number in its table row below is frozen at the point the guard fired, not a
completed-run result, exactly as `StrategyReport::diverged` and this tool's own
`*** DIVERGED ***` banner say.

Timing check (seed 42, varying `--events` to bracket the divergence point):

| `--events` | orders submitted | diverged? |
|-----------:|------------------:|:---------:|
| 2,500 | 18 | no |
| 3,000–4,500 | 22 | no |
| 5,000 | 843 | **yes** |
| 20,000 | 843 | yes (same — frozen at the same point) |

The order count jumps from 22 to 843 in the same 500-event window where
divergence fires — the cascade, once triggered, is fast, not a slow drift. It
also doesn't happen in the first ~4,500 events: a short smoke-test run (a few
hundred or a couple thousand events, which is what earlier ad hoc manual checks
during Stage 3/4 development used) would not have caught this at all. That's
the concrete argument for why this stage's long-window, seeded evaluation
framework earns its keep over spot-checking a strategy by eye.

## Full comparison table, `--settle=yes`, `--events=20000`

Condensed to the metrics that differentiate the three strategies; the full
16-metric table is available from `run_backtest.exe`'s own output for any seed.

### seed=42

| metric | PassiveMarketMaker | OrderBookImbalance | TradeFlow |
|---|---:|---:|---:|
| diverged | **yes** | no | no |
| orders submitted | 843 | 768 | 2 |
| fill rate (qty) | 2.18% | 100.00% | 100.00% |
| maker ratio | 0.24 | 0.26 | 1.00 |
| net P&L (cents) | -110,097 | -1,602 | 118 |
| max drawdown (cents) | 2,625 | 1,194 | 0 |
| risk-adj return proxy | -0.02 | -0.03 | 0.01 |
| final position | -2,016 | 22 | 2 |
| rejected (risk/lifecycle) | 0 | 619 | 0 |

### seed=1

| metric | PassiveMarketMaker | OrderBookImbalance | TradeFlow |
|---|---:|---:|---:|
| diverged | **yes** | no | no |
| orders submitted | 113 | 131 | 2 |
| fill rate (qty) | 21.43% | 100.00% | 100.00% |
| net P&L (cents) | -133,755 | 3,467 | 118 |
| max drawdown (cents) | 3,740 | 612 | 0 |
| final position | -2,060 | 79 | 2 |
| rejected (risk/lifecycle) | 0 | 5 | 0 |

### seed=100

| metric | PassiveMarketMaker | OrderBookImbalance | TradeFlow |
|---|---:|---:|---:|
| diverged | **yes** | no | n/a (0 orders) |
| orders submitted | 1,532 | 160 | 0 |
| net P&L (cents) | 56,526 | 3,546 | 0 |
| final position | 2,004 | 86 | 0 |

### seed=12,345

| metric | PassiveMarketMaker | OrderBookImbalance | TradeFlow |
|---|---:|---:|---:|
| diverged | **yes** | no | no |
| orders submitted | 453 | 103 | 2 |
| net P&L (cents) | 87,410 | -5,855 | 100 |
| final position | 2,010 | -99 | 2 |
| rejected (risk/lifecycle) | 0 | 303 | 0 |

(seed=7's full table is in the "headline finding" section above via its
divergence-point row; its `OrderBookImbalance`/`TradeFlow` net P&L were
+3,074 / -92 respectively, both non-diverged.)

## What each strategy's row actually says

- **`PassiveMarketMaker`**: not evaluable as "profitable" or "unprofitable" from
  this framework as it stands — every run diverged, so every P&L number above is
  frozen mid-cascade, dominated by whatever direction the runaway position
  happened to move in (hence swinging from -$1,338 to +$874 net P&L across
  seeds with no relation to the strategy's actual quoting logic). The honest
  finding here is the divergence itself, not the P&L. Fixing this — either by
  giving `RiskEngine` visibility into resting/in-flight exposure, or by giving
  `PassiveMarketMaker`'s exit logic its own re-entry throttle — is future work,
  already scoped as such in `risk/risk_engine.hpp`'s own header comment and
  `BACKTEST_ASSESSMENT.md`'s sixth finding, not something this pass should
  patch quietly just to make the table look cleaner.
- **`OrderBookImbalance`**: the only strategy that produced a *complete*,
  undiverged run in all 5 seeds. Mixed but real results: net P&L positive in
  3/5 seeds (seed 1: +$34.67, seed 7: +$30.74, seed 100: +$35.46), negative in
  2/5 (seed 42: -$16.02, seed 12,345: -$58.55) — no seed dominates by an order
  of magnitude, consistent with a strategy that's taking real, two-sided risk
  rather than riding one lucky/unlucky synthetic path. `rejected` count is the
  most seed-sensitive figure (0 to 619) — worth a follow-up look at *which*
  check (fat-finger vs. exposure) is firing before reading too much into it.
- **`TradeFlow`**: the least evaluable of the three, for a different reason
  than `PassiveMarketMaker` — it placed only 0-2 orders per 20,000-event run
  across every seed. That's too small a sample to say anything about its
  decision quality one way or the other; it's a signal about the *synthetic
  generator's* trade-flow rate relative to `TradeFlow`'s own threshold, not a
  finding about the strategy. Flagged rather than glossed over: a fair
  `TradeFlow` evaluation needs either a synthetic generator tuned to produce
  more qualifying trade activity, or a real historical-data replay (already a
  documented future upgrade — `BACKTEST_ASSESSMENT.md`'s "Synthetic data only"
  limitation, `docs/EXTERNAL_REVIEW.md` item 13).

## Settlement assumption swings the number

Same seed=42 replay, same final `OrderBookImbalance` position (+22 contracts),
three settlement assumptions:

| `--settle` | meaning | net P&L (cents) |
|---|---|---:|
| `no` | position marked at 0c (market resolved No) | -3,802 |
| *(omitted)* | position marked at last observed mid | -2,702 |
| `yes` | position marked at 100c (market resolved Yes) | -1,602 |

A $22.00 swing (2,200 cents = 22 contracts x 100c) purely from which settlement
assumption is applied to the same open position at the same replay cutoff —
mechanically exactly `final_position * (mark_price_a - mark_price_b)`, not a
bug, but a reminder that any single net-P&L number in the tables above is only
meaningful alongside its settlement assumption, which is why every table in
this document states it explicitly rather than leaving it implicit.

## What this document is not

Not a recommendation to run any of these three strategies live, and not a
verdict on which one is "best" — `OrderBookImbalance` merely being the only
strategy that *completed* a run here is a much weaker claim than "should trade
real capital." That synthesis — combining this table with Part A's external
review, Part C/D's cross-market and external-information findings, and Part
G's observability build — is `docs/DECISION_GATE.md`'s job, not this one.
