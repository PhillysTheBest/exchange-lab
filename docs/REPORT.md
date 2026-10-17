# The evaluation report

Step 3 of turning the arena into a strategy research platform. Steps 1 and 2, the strategy
interface and the standard experiment runner (`docs/RUNNER.md`), came first; steps 4 and 5,
validating the evaluator (`docs/CONTROLS.md`) and one-command reproducibility
(`docs/REPRODUCE.md`), came after.

This file began as the plan, written before building, as CLAUDE.md asks, and is now the
report's design document. Every number in it was printed by
`build/release/suite_lab --candidates=imbalance,maker,momentum --suite=all` on the standard
block, on Linux with g++ 13, unless it says otherwise. None of these numbers go in `results/`.

## What it answers

Step 2 answers one question: how much money the seat made, paired seed by seed against the
empty seat. Step 3 says how it made that money, and whether the answer survives a change of
market. It adds:

- P&L, inventory, fill counts, spread earned and adverse markouts for every candidate;
- paired uncertainty on every one of them, seed by seed, as step 2 does for P&L;
- where the money came from: which reference participant lost what the seat gained;
- scenario families, each a market of its own, reported side by side and never pooled.

None of it changes a session. Every new number is read off the integer records the runner
already wrote, or off a path sampled from outside the session, so no session hash and no
pinned fingerprint moved.

```bash
make compare CANDIDATES=imbalance,maker                 # the report, on the standard suite
make compare CANDIDATES=imbalance,maker SUITE=all       # every family, one directory each
make compare CANDIDATES=imbalance,maker SUITE=wide      # one family
```

A run directory gains two raw-derived files, `evaluation.csv` and `attribution.csv`, beside
the six of step 2. `--suite=all` writes one directory per family and `families.txt` above them.

## The path, sampled from outside the session

`run_arm` owns the step loop (step 2 kept it for exactly this). After each step that moves
the fundamental, and once after `finish()`, it reads the seat's account and marks it at the
current fundamental. Reading the ledger and the fundamental changes nothing, so the session is
the same session; `test_report_path` checks that the placebo's hash is still the no-seat
market's. Four integer columns are appended to `sessions.csv`:

- `seat_path_samples` and `seat_abs_position_sum`, whose ratio is the mean absolute inventory;
- `seat_max_abs_position`, the largest inventory the seat carried;
- `seat_max_drawdown`, the largest fall of the marked value from its running peak. The peak
  starts at 0, the seat's value before it trades.

The sample rate is the fundamental's, about one every 100 µs of simulated time. A position
held between two moves of the fundamental is marked at a price that did not change, so a finer
sample would add rows and no information.

## The seat, metric by metric

| metric | from | unit |
|---|---|---|
| value | `seat_value` | currency per session |
| final inventory | seat `position` | quantity, signed |
| mean, max \|inventory\| | the path columns | quantity |
| max drawdown | `seat_max_drawdown` | currency |
| fills, turnover | seat `fills`, `filled_qty` | count, quantity |
| fills per order | `fills / orders_sent` | ratio |
| maker share | `maker_fills / fills` | percent |
| spread earned | seat `spread_sum` | currency per session |
| adverse markout | seat `adverse_sum` | currency per session |
| carry to close | value − spread − adverse | currency per session |

**Value decomposes exactly.** A fill's markout splits into spread, `(V(t) − price)·q`, and
adverse, `(V(t+H) − V(t))·q`. What is left of the value is carry, `(V(T) − V(t+H))·q`: what
the inventory earned between the markout horizon and the close. With the suite's zero fees,
value = spread + adverse + carry on every seed, and `test_report_metrics` checks it.

**Fills per order, not a fill rate.** The plan said fill rate, `fills / orders` as a percent.
The first run printed 115.58% for `imbalance`, because one aggressive order fills against
several resting ones. It is a count per order, labelled as one.

**A ratio is undefined on a seed with nothing to divide by.** That seed leaves that metric
only, and the seed count is printed beside the cell. Pairs use the seeds where both arms are
defined. That leaves degrees of freedom no block has, so `t975` gained a Cornish-Fisher
expansion past its table. It is within 0.0005 of the exact quantile for every df above 30, and
it gives 1.9984 at 63 and 1.9788 at 127, the table's entries, which stay as they were so the
step-2 numbers did not move.

**Against the placebo every metric but value is its own level,** because the empty seat holds
zero on all of them. So the report prints levels for every candidate, and pairs only candidate
against candidate: every later candidate against the first, on every metric.

The standard suite, 64 seeds, mean ± s.e.:

| metric | imbalance | maker | momentum |
|---|---|---|---|
| value | −34.92 ± 3.09 | +71.13 ± 7.05 | −2.36 ± 1.17 |
| mean \|inventory\| | 146.10 ± 2.77 | 247.46 ± 18.85 | 49.39 ± 3.95 |
| max drawdown | 48.87 ± 2.08 | 41.90 ± 5.63 | 11.76 ± 0.64 |
| spread earned | −36.39 ± 0.85 | +70.77 ± 1.04 | −3.96 ± 0.22 |
| adverse markout | −0.28 ± 0.84 | +0.90 ± 1.66 | −0.10 ± 0.30 |
| carry to close | +1.75 ± 2.97 | −0.54 ± 6.90 | +1.71 ± 1.07 |

Spread carries the result for all three; adverse and carry straddle zero for all three. That
is what CLAUDE.md predicts for the adverse column: the fundamental is a martingale, so it reads
near zero for everyone, and a stale quote's loss lands in spread. The report prints that
caveat under the table, with the share of the seat's fills that fell inside the last markout
horizon and were marked at the close instead (3.9% here).

## Where the money came from

For each reference participant, the paired difference of its value between the candidate's
arm and the placebo arm. Every arm closes at the same fundamental and the market is closed
(cash, position and value each sum to zero), so on every seed these differences sum to minus
the seat's value exactly. The report says so in a check line, and says so loudly if it fails;
`test_report_attribution` breaks one row to show it does.

| reference | imbalance | maker | momentum |
|---|---|---|---|
| maker | +10.56 ± 1.76 | −46.76 ± 1.58 | +1.37 ± 1.51 |
| rival | +8.51 ± 1.74 | −47.52 ± 1.45 | +0.29 ± 1.41 |
| informed | −9.50 ± 10.82 | +9.78 ± 9.85 | +6.69 ± 10.62 |
| noise | +6.25 ± 7.64 | +3.39 ± 8.68 | −10.79 ± 10.04 |
| noise | +17.78 ± 8.07 | +8.95 ± 6.87 | +5.23 ± 6.15 |
| momentum | +1.33 ± 1.25 | +1.03 ± 0.90 | −0.44 ± 1.34 |

A third maker in the seat makes its +71 almost entirely out of the two reference makers
(−46.76 and −47.52), not out of the informed trader or the noise. That is the arena's own
negative result seen from the other side: in `docs/ARENA.md` the cost of a slow maker is
competition for uninformed flow, not adverse selection, and here the seat's gain is that same
competition. `imbalance` loses about 35 a session, and the makers it trades against are the
ones it pays.

## Scenario families

`standard` is unchanged and its fingerprint is still pinned. Each family is a suite of its
own, with its own name and therefore its own fingerprint, placebo arm and health gate, and it
changes one thing about the market:

| suite | the change | placebo gap, median (worst) | smoke fingerprint |
|---|---|---|---|
| `standard` | none | 1.51 (2.03) | `d461c30ab2446acf` |
| `wide` | `maker.half_spread_ticks=4` | 1.67 (2.02) | `9bb1a7c184ccb2d7` |
| `busy` | `noise.timer_ns=5000` | 1.48 (3.06) | `5d735a9faa384bf5` |

`test_arena` pins all three, so a change to any family's conditions needs a version bump, as
`standard`'s does. `--suite=all` prints one table across them, each candidate's paired value
against the placebo:

| candidate | standard | wide | busy |
|---|---|---|---|
| imbalance | −34.92 ± 3.09 | −46.65 ± 2.66 | −38.72 ± 3.11 |
| maker | +71.13 ± 7.05 | +7.63 ± 6.53, not separated | +141.17 ± 7.78 |
| momentum | −2.36 ± 1.17 | −9.64 ± 1.07 | −5.52 ± 1.31 |

**The columns are never pooled.** `maker` makes +71 in the default market, nothing that
64 seeds can tell from zero when the makers quote wider, and twice as much when the noise is
twice as busy. Those are three results, not one average, and an average would hide the only
interesting thing about them.

**A built-in candidate takes the family's parameters.** `maker` in the seat is built from the
same roster config as the reference makers, so in `wide` it quotes 4 ticks too. The `wide`
column is "a third maker in a wide market", not "a 2-tick maker against 4-tick ones". An
installed strategy reads no roster config, so for it the column means what it says.

**`null` was built and is not shipped.** A family with no informed participant was the first
on the plan, because it asks whether a result needs adverse selection. Its placebo failed the
health gate on 56 of the 64 standard seeds, with a median gap of 8.39 ticks and a worst of
30.09. Without the informed trader nothing pulls the book back to the fundamental within
25 ms, and a book that has come loose from the fundamental is not a market. `sweep --null`,
under its 200,000-request bound, remains the place for that control; `suite --suite=null` is
refused, and `test-cli` checks that.

## Many comparisons

The standard report above makes 28 interval tests: 3 against the placebo and 25 between
candidates, metric by metric. Of those, 22 separate. The report says how many tests it ran and
how many a 5% test would separate by chance alone if nothing were there (1.4 here), and it
applies no correction silently. Several of those 22 are trivially true (`maker` has a higher
maker share than `imbalance`), which is why the line is a warning about lone separations, not a
correction.

## Where it lives

- `arena/suite.hpp`: the path sampler in `run_arm`, the four `SessionRow` columns,
  `SuiteDef::change` (printed, not fingerprinted), `wide_suite`, `busy_suite`, `kSuiteNames`
  and `find_suite`.
- `arena/suite_report.hpp`: `paired_values` (the double form of `paired`), `t975` past 30,
  `kMetrics`, `seat_series`, `evaluate`, `evaluation_csv`, `attribute`, `attribution_csv`,
  `families_text`, and three new sections of `report_text`.
- `arena/suite.cpp`: `--suite=NAME|all`, and `run_suite`, one suite start to finish.
  `outcome.cfg` is now written only if every other file was, which closes a gap the step-2
  review found.
- `Makefile`: `make compare` takes `SUITE=`. `test-cli` gains nine suite checks.
- `tests/test_arena.cpp`: `test_report_path`, `test_report_metrics`,
  `test_report_attribution`, `test_report_t_table`, `test_report_families`, and the two family
  fingerprints.

## Deferred

- **Reading a run back from disk** (`--report=DIR`, comparing two runs whose fingerprints
  match) moved to step 5, which built re-running from `suite.cfg` instead and left reading
  back undone; `docs/REPRODUCE.md` says why.
- **A correction for multiple comparisons.** Counting is honest; a Holm correction across a
  table this mixed would need a decision about which metrics are hypotheses at all.
- **A `null` family that is a market.** It needs some other participant to anchor the book to
  the fundamental, which changes the market in a second way and so is not the control it was
  meant to be.
