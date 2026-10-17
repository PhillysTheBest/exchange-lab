# Validating the evaluator

Step 4 of turning the arena into a strategy research platform. Steps 1 to 3, the strategy
interface, the standard experiment runner (`docs/RUNNER.md`) and the evaluation report
(`docs/REPORT.md`), came first; step 5, one-command reproducibility (`docs/REPRODUCE.md`),
came after.

This file began as a short plan and is now the design document. Every number in it was
printed by `build/release/controls` on the standard block, on Linux with g++ 13.

## What it shows

The runner and the report claim to measure strategies. This step shows that they can,
in two ways:

- **controls whose answers are known in advance**, which the evaluator reproduces on the
  standard block;
- **planted defects**, each of which makes a named control fail in `test_arena`.

A control that cannot fail proves nothing. So every control is also run where it must fail:
against a real arm that differs, and against a defect planted on purpose.

```bash
make controls                      # every control on the 64 standard seeds; exit 1 if one fails
build/release/controls --block=smoke
```

`controls` is built only on the evidence side, without `PX_ARENA_USER_STRATEGIES`, so it can
run built-in strategies only.

## The controls

```
  ok   placebo is zero    expected: seat value exactly 0 on every seed
                          measured: 64/64 seeds
  ok   placebo is inert   expected: same session hash as the no-seat market on every seed
                          measured: 64/64 seeds
  ok   common close       expected: every arm closes at the placebo's fundamental, every seed
                          measured: 256/256 arm-seeds
  ok   replay             expected: every candidate session identical on replay
                          measured: 256/256 sessions
  ok   closed identities  expected: market closed in every arm; attribution sums to -seat value
                          measured: closed, attribution exact
  ok   A/A                expected: maker and maker-copy identical on every seed
                          measured: identical on 64 seeds
  ok   noise floor        expected: maker with the seat at 9,999 ns against 10,000 ns: not separated
                          measured: -0.06 +- 0.99 (-2.04 .. +1.92)
  ok   positive control   expected: roundtrip loses, separated; the loss is in spread; adverse and carry are not
                          measured: value -79.55 +- 0.17, spread -79.61, adverse +0.04 +- 0.03, carry +0.03 +- 0.12
8/8 controls held
```

**Negative controls**, whose answer is exactly zero or exactly "the same":

- **Placebo is zero.** The empty seat sends nothing, so it makes and loses nothing.
- **Placebo is inert.** Its sessions hash the same as the market with no seat at all. This is
  the premise the whole comparison rests on (`docs/RUNNER.md`, "The seat and wake ordering").
- **Common close.** Every arm on a seed closes at the same fundamental after the same number
  of its steps. This is what the time bound buys, and what makes pairing by seed valid.
- **Replay.** Every candidate session, run again from fresh instances, gives the same hash.
- **Closed identities.** Cash, position and value each sum to zero in every arm, and each
  reference participant's change against the placebo sums to minus the seat's value.
- **A/A.** `maker` run twice under two labels is identical on every seed. This is the control
  that catches misaligned pairing.
- **Noise floor.** `maker` with the seat at 9,999 ns against the same at 10,000 ns. A
  nanosecond of reaction delay should not matter, and it does not separate. The interval,
  −2.04 to +1.92 currency per session, is also a published floor: a difference between two
  strategies smaller than about two currency units on this block should not be read as skill.
  `docs/RUNNER.md` quoted a probe of the same control for `imbalance` at `6ffdbc3`
  (−0.72 ± 1.57); this one is `maker`, from the runner itself.

**The positive control**, whose answer is known in sign and location, is
`arena/strategies/roundtrip.hpp`. On a 50 µs timer it buys 10 at market, then sells 10 at
market, for ever, and looks at nothing but whether the side it is about to hit has an order on
it. Every unit crosses the book, so it must lose money, and the loss must be spread. Its
inventory is never more than 10 and is flat every other wake, so adverse markout and carry must
not separate. The report finds exactly that: −79.55 ± 0.17 a session, of which spread is
−79.61, while adverse (+0.04 ± 0.03) and carry (+0.03 ± 0.12) straddle zero.

**What the positive control does not do.** The plan asked for an *analytic* positive control,
one whose size is predicted as well as its sign. The obvious prediction is 2 ticks a unit, the
reference makers' half-spread. The measured cost is 1.59 ticks a unit (spread −79.61 a session
over 5,000 units). Why the touch is cheaper than the makers' nominal quote is not established
(the makers skew with inventory and estimate the fundamental with error, and either could do
it). A size could be predicted after the fact by fitting it, but a prediction fitted to the
number it predicts is not a control. So the control checks what is known exactly, the sign and
the location, and the size is reported rather than claimed.

## The defects each control catches

`test_controls_catch_defects` plants each defect and checks that its control fails.
`test_controls_hold` checks the controls pass on a clean smoke run, and fail on real arms that
differ: `maker` is not a positive control (it makes money), `maker` and `roundtrip` are not an
A/A pair, and `roundtrip` against the placebo is far above the noise floor.

| defect planted | control that fails |
|---|---|
| pairing off by one seed (one arm's values rotated by a seed) | A/A |
| a 7,000-request bound instead of the 25 ms time bound | common close |
| the placebo inserted at index 3 instead of appended last | placebo is inert |
| a strategy that keeps state in `static` storage between sessions | replay |
| one participant's value off by one price unit | closed identities |
| a session whose values do not sum to zero | closed identities |
| a placebo that sends an order | placebo is zero |

The plan also listed gating on the floating-point `mean_gap_ticks()` instead of the integer
cross-multiplication. That defect is not planted here, because it would change a decision only
at a boundary no seed reaches. `test_suite_health_gate` already pins the boundary exactly, at
`gap_sum == 4 · samples` and one more.

## The review findings fixed here

A review of step 2 was stopped early. Two of its confirmed findings were evaluator defects, and
are fixed in this step:

- **Candidate-against-candidate rows ignored an inadmissible reference market.** The report
  said "nothing is compared" and then compared two candidates in the same market. Those rows
  are now `not_admissible` too, and `test_suite_outcomes` checks it.
- **"It sent no orders" was inferred from value alone.** An arm whose value equals the
  placebo's on every seed is called identical, but the text now says it sent no orders only
  when the records say it was inert on every seed.

Two of its test gaps are closed too: the `not_separated` verdict and the exit codes 1 and 3
are now produced by a test (`test_suite_outcomes`). The rest of its findings are recorded in
the project's memory, not fixed here.

## Where it lives

- `arena/controls.hpp`: one function per control, each taking finished records and returning
  what it expected, what it measured and whether it held; `controls_text`.
- `arena/controls.cpp`: the `controls` binary, which runs the placebo, `roundtrip`, `maker`
  twice and `maker` at 9,999 ns on a block, with every check the runner makes.
- `arena/strategies/roundtrip.hpp`: the positive control, a built-in strategy, in no default
  line-up.
- `Makefile`: `make controls`; `test-cli` runs it on the standard block (about 10 s) and checks
  `8/8 controls held`.
- `tests/test_arena.cpp`: `test_controls_hold`, `test_controls_catch_defects`,
  `test_suite_outcomes`.

## Not done

- **A zero-intelligence baseline.** `roundtrip` serves as the dumbest possible strategy, but a
  random limit-order strategy would be a better floor to publish beside real candidates.
- **A holdout block** of seeds used by no test or example, for reporting after tuning on
  `standard`. It is one row in `kSeedBlocks`, deferred until people tune against the report.
