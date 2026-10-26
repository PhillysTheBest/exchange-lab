# The evaluation report

What `/compare` prints after it runs, and how to read it. The runner
([`RUNNER.md`](RUNNER.md)) says how much money the seat made, paired seed by seed against
the empty seat. The report says **how** it made that money, **from whom**, and whether the
answer **survives a change of market**.

Every number in this file was printed by
`build/release/suite_lab --candidates=imbalance,maker,momentum --suite=all` on the standard
block (64 seeds), on Linux with g++ 13. Suite numbers never go in `results/`.

```
> /compare imbalance,maker          # the report, on the standard suite
> /compare imbalance,maker all      # every scenario family, one directory each
> /compare imbalance,maker wide     # one family
```

None of it changes a session. Every number is read off the integer records the runner
wrote, or off a path sampled from outside the session, so no session hash or fingerprint
moves. A run directory holds `evaluation.csv`, `attribution.csv` and `holm.csv` beside the
runner's files; `all` writes one directory per family and a `families.txt` above them.

## Read it in this order

1. **Market health.** If the reference market came loose from the fundamental, nothing below
   it counts.
2. **The paired difference against the placebo**, with its interval and verdict.
3. **The seat, metric by metric**: how the value was made.
4. **Where the money came from**: who paid.
5. **The count of interval tests**, before believing any lone separation.

## The seat, metric by metric

| metric | from | unit |
|---|---|---|
| value | `seat_value` | currency per session |
| final inventory | seat `position` | quantity, signed |
| mean, max \|inventory\| | the sampled path | quantity |
| max drawdown | the sampled path | currency |
| fills, turnover | seat `fills`, `filled_qty` | count, quantity |
| fills per order | `fills / orders_sent` | ratio |
| maker share | `maker_fills / fills` | percent |
| spread earned | seat `spread_sum` | currency per session |
| adverse markout | seat `adverse_sum` | currency per session |
| carry to close | value − spread − adverse | currency per session |

**Value splits exactly.** Each fill's markout is spread, `(V(t) − price)·q`, plus adverse,
`(V(t+H) − V(t))·q`. What is left is carry, `(V(T) − V(t+H))·q`: what the inventory earned
between the markout horizon and the close. With the suite's zero fees,
value = spread + adverse + carry on every seed, and `test_report_metrics` checks it.

**Fills per order, not a fill rate.** One aggressive order can fill against several resting
ones, so the first run printed 115.58% for `imbalance` as a "rate". It is a count per order,
labelled as one.

**Inventory and drawdown come from a path sampled outside the session.** After each step
that moves the fundamental, and once at the end, the runner reads the seat's account and
marks it. Reading changes nothing, and `test_report_path` checks the placebo still hashes
like the no-seat market. The sample rate is the fundamental's, about every 100 µs of
simulated time; a finer one would add rows and no information. Drawdown is the largest fall
from the running peak, which starts at 0.

**A ratio is undefined on a seed with nothing to divide by.** That seed drops out of that
metric only, and the seed count is printed beside the cell.

**Against the placebo, every metric but value is its own level,** because the empty seat is
zero on all of them. So the report prints levels for each candidate, and pairs candidates
against each other: every later candidate against the first, on every metric.

The standard suite, 64 seeds, mean ± s.e.:

| metric | imbalance | maker | momentum |
|---|---|---|---|
| value | −34.92 ± 3.09 | +71.13 ± 7.05 | −2.36 ± 1.17 |
| mean \|inventory\| | 146.10 ± 2.77 | 247.46 ± 18.85 | 49.39 ± 3.95 |
| max drawdown | 48.87 ± 2.08 | 41.90 ± 5.63 | 11.76 ± 0.64 |
| spread earned | −36.39 ± 0.85 | +70.77 ± 1.04 | −3.96 ± 0.22 |
| adverse markout | −0.28 ± 0.84 | +0.90 ± 1.66 | −0.10 ± 0.30 |
| carry to close | +1.75 ± 2.97 | −0.54 ± 6.90 | +1.71 ± 1.07 |

Spread carries the result for all three; adverse and carry straddle zero for all three.
That is expected: the fundamental is a martingale, so the adverse column reads near zero for
everyone, and a stale quote's loss lands in spread
([`ARENA.md`](ARENA.md#the-positive-control-and-the-limitation-it-found)). The report prints
that caveat under the table, with the share of fills inside the last markout horizon, which
are marked at the close instead (3.9% here).

## Where the money came from

For each reference participant: the paired difference of its value between the candidate's
arm and the placebo's. The market is closed and every arm ends at the same fundamental, so
on every seed these sum to minus the seat's value, exactly. The report prints a check line,
and `test_report_attribution` breaks one row to show the check fails loudly.

| reference | imbalance | maker | momentum |
|---|---|---|---|
| maker | +10.56 ± 1.76 | −46.76 ± 1.58 | +1.37 ± 1.51 |
| rival | +8.51 ± 1.74 | −47.52 ± 1.45 | +0.29 ± 1.41 |
| informed | −9.50 ± 10.82 | +9.78 ± 9.85 | +6.69 ± 10.62 |
| noise | +6.25 ± 7.64 | +3.39 ± 8.68 | −10.79 ± 10.04 |
| noise | +17.78 ± 8.07 | +8.95 ± 6.87 | +5.23 ± 6.15 |
| momentum | +1.33 ± 1.25 | +1.03 ± 0.90 | −0.44 ± 1.34 |

A third maker in the seat makes its +71 almost entirely from the two reference makers
(−46.76 and −47.52), not from the informed trader or the noise. That is the arena's negative
result seen from the other side: a maker's fortune is competition for uninformed flow.
`imbalance` loses about 35 a session, mostly to the makers it trades against.

## Scenario families

Each family is a suite of its own, with its own fingerprint, placebo arm and health gate,
and changes one thing about the market:

| suite | the change | placebo gap, median (worst) | smoke fingerprint |
|---|---|---|---|
| `standard` | none | 1.51 (2.03) | `d461c30ab2446acf` |
| `wide` | `maker.half_spread_ticks=4` | 1.67 (2.02) | `9bb1a7c184ccb2d7` |
| `busy` | `noise.timer_ns=5000` | 1.48 (3.06) | `5d735a9faa384bf5` |

`test_arena` pins all three, so changing any family's conditions needs a version bump.
`all` prints each candidate's paired value against the placebo, side by side:

| candidate | standard | wide | busy |
|---|---|---|---|
| imbalance | −34.92 ± 3.09 | −46.65 ± 2.66 | −38.72 ± 3.11 |
| maker | +71.13 ± 7.05 | +7.63 ± 6.53, not separated | +141.17 ± 7.78 |
| momentum | −2.36 ± 1.17 | −9.64 ± 1.07 | −5.52 ± 1.31 |

**The columns are never pooled.** `maker` makes +71 in the default market, nothing 64 seeds
can tell from zero when makers quote wider, and twice as much when the noise is twice as
busy. That is three results, and an average would hide the only interesting thing about them.

**A built-in candidate takes the family's parameters.** In `wide`, `maker` in the seat quotes
4 ticks too, so that column is "a third maker in a wide market". An installed strategy reads
no roster settings, so for it the column means what it says.

**There is no `null` family.** A market with nobody informed failed the health gate on 56 of
the 64 standard seeds (median gap 8.39 ticks, worst 30.09): without the informed trader,
nothing pulls the book back to the fundamental within 25 ms. `/sweep --null`, over 200,000
requests, remains the place for that control, and `suite --suite=null` is refused.

## Many comparisons

The standard report above makes 28 interval tests: 3 against the placebo and 25 between
candidates. 22 separate. The report prints how many tests it ran and how many a 5% test would
separate by chance if nothing were there (1.4 here).

**The seat's value is the primary outcome, and only its tests are corrected.** This was decided
on 2026-10-26.

- **The corrected tests.** Holm's adjustment runs over the seat-value tests: each candidate
  against the placebo, and each later candidate against the first. These are the rows of
  `comparison.csv` the count above treats as tests. A row that is `identical` or
  `not_admissible` is not a test, before or after.
- **The rest.** The other eleven metrics are *descriptive*. They are printed with their
  intervals, never corrected, and the report says so. Several of their separations are true
  by construction (`maker` has a higher maker share than `imbalance`), and correcting those
  would mix mechanics with hypotheses.

Holm holds the chance of any false separation among the corrected tests, the *family-wise
error rate*, at 5%. It sorts the p-values in ascending order, multiplies the k-th smallest of
m by m − k + 1, caps the result at 1 and makes it non-decreasing. In the standard family
above, all five tests stay separated:

```
Holm, over the 5 seat-value tests above: the chance of any false separation among them held at 5%
  arm vs baseline          | p         | Holm p    | verdict
  ------------------------------------------------------------
  imbalance vs placebo     | <0.000001 | <0.000001 | separated
  maker vs placebo         | <0.000001 | <0.000001 | separated
  momentum vs placebo      | 0.047565  | 0.047565  | separated
  maker vs imbalance       | <0.000001 | <0.000001 | separated
  momentum vs imbalance    | <0.000001 | <0.000001 | separated
```

**Across families, one result changes.** `families.txt` corrects all three families' 15 tests
together, because a reader who reports several families has made all of those tests.
Correcting tests is not pooling estimates: each family's value is still its own. In that
table, `momentum` against the placebo on `standard` (−2.36 ± 1.17, p = 0.048) separates on
its own and within its family, but its Holm p across the 15 tests is 0.095, so it does not
separate there. Quote the `families.txt` adjustment when you report families together. Each
family's own `report.txt` and `holm.csv` correct within that family only.

- **Where it is printed.** Every run writes `holm.csv`, one row per row of `comparison.csv`:
  the t statistic, its degrees of freedom, the two-sided p-value, the number of tests, the
  Holm p and the Holm verdict. Rows that are not tests have an empty p. `report.txt` prints
  the table only when there are two or more tests, since for one test Holm changes nothing.
  A Holm verdict is `separated` when the adjusted p is below 0.05.
- **No library maths.** p comes from the closed form of Student's t for whole degrees of
  freedom (Abramowitz and Stegun 26.7.3 and 26.7.4), with an arctangent written by hand. It
  uses only `+ − × ÷` and `sqrt`, so no library function can move a printed digit between
  platforms. The canonical record holds a two-candidate run's `holm.csv` and `families.txt`
  as derived lines, and every CI target must agree on them. `test_holm` checks the
  distribution against closed forms and against every row of the t table.
- **A stated limit.** The raw verdict uses the t table's three decimals, while Holm uses the
  exact p. Within 0.0005 of the critical value, the table's rounding, the two can disagree. At
  the table's own rows p is within 5 × 10⁻⁵ of 0.05. Printing `<0.000001` hides the tail's
  rounding error, which only arises far below any threshold.

## Where it lives

- `arena/suite.hpp`: the path sampler, the families (`wide_suite`, `busy_suite`,
  `find_suite`).
- `arena/suite_report.hpp`: the metrics, attribution, the family table, the t distribution and
  Holm (`portable_atan`, `t_two_sided_p`, `holm_adjust`), and the report text.
  Past 30 degrees of freedom, `t975` uses a Cornish-Fisher expansion, within 0.0005 of the
  exact quantile.
- `arena/suite.cpp`: `--suite=NAME|all`. `outcome.cfg` is written only if every other file
  was.
- `tests/test_arena.cpp`: `test_report_path`, `test_report_metrics`,
  `test_report_attribution`, `test_report_t_table`, `test_report_families`, `test_holm`.

## Not built

- **A `null` family that is a market.** It would need another participant to anchor the book,
  which changes the market a second way and so is not the control it was meant to be.
