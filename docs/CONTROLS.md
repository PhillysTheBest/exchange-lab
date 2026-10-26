# The evaluator's controls

How you know `/compare` measures what it claims to. The runner and the report are checked
two ways:

- **Controls whose answers are known in advance**, which the evaluator must reproduce on the
  standard block.
- **Planted defects**, each of which must make a named control fail in `test_arena`.

A control that cannot fail proves nothing, so every control is also run where it must fail:
against a real arm that differs, and against a defect planted on purpose.

```
> /controls          # every control on the 64 standard seeds; fails if one does not hold
> /controls smoke    # the same on 4 seeds
```

Without the app: `make controls`, or `build/release/controls --block=smoke`. `controls` is
built only on the evidence side, without `PX_ARENA_USER_STRATEGIES`, so it runs built-in
strategies only. Every number in this file was printed by `build/release/controls` on the
standard block, on Linux with g++ 13. It refuses the `holdout` block, which is kept for
confirming a finished strategy ([`RUNNER.md`](RUNNER.md#the-holdout)): the controls run
strategies on their block, and that would spend it.

## What you see

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

## The negative controls

Their answer is exactly zero, or exactly "the same".

| Control | What it checks |
|---|---|
| Placebo is zero | The empty seat sends nothing, so it makes and loses nothing. |
| Placebo is inert | Its sessions hash the same as the market with no seat at all. The whole comparison rests on this ([`RUNNER.md`](RUNNER.md#the-seat-and-wake-ordering)). |
| Common close | Every arm on a seed closes at the same fundamental after the same number of its steps. The time bound buys this, and it is what makes pairing by seed valid. |
| Replay | Every candidate session, run again from fresh instances, gives the same hash. |
| Closed identities | Cash, position and value each sum to zero in every arm, and each reference participant's change against the placebo sums to minus the seat's value. |
| A/A | `maker` run twice under two labels is identical on every seed. This catches misaligned pairing. |

**The noise floor.** `maker` with the seat at 9,999 ns against the same at 10,000 ns. A
nanosecond of reaction delay should not matter, and it does not separate. Its interval,
−2.04 to +1.92 currency per session, is also a published floor: **a difference between two
strategies smaller than about two currency units on this block should not be read as skill.**
[`RUNNER.md`](RUNNER.md) quotes a probe of the same control for `imbalance` (−0.72 ± 1.57);
this one is `maker`, from the evaluator itself.

## The positive control

`arena/strategies/roundtrip.hpp` has an answer known in sign and location. On a 50 µs timer
it buys 10 at market, then sells 10 at market, for ever, and looks at nothing but whether the
side it is about to hit has an order on it.

- Every unit crosses the book, so it **must lose money**, and the loss **must be spread**.
- Its inventory is never more than 10 and is flat every other wake, so **adverse markout and
  carry must not separate**.

The report finds exactly that: −79.55 ± 0.17 a session, of which spread is −79.61, while
adverse (+0.04 ± 0.03) and carry (+0.03 ± 0.12) straddle zero.

**What it does not do: predict its own size.** The obvious prediction is 2 ticks a unit, the
reference makers' half-spread. The measured cost is 1.59 ticks a unit (spread −79.61 a
session over 5,000 units). Why the touch is cheaper than the makers' nominal quote is **not
established**: the makers skew with inventory and estimate the fundamental with error, and
either could do it. A size fitted after the fact to the number it predicts is not a control.
So the control checks what is known exactly, the sign and the location, and the size is
reported, not claimed. Do not tune `roundtrip` to make it fit.

This does not redeem the positive control that failed in
[`ARENA.md`](ARENA.md#the-positive-control-and-the-limitation-it-found): that one tried to
calibrate the adverse column, and no admissible design could.

## The defects each control catches

`test_controls_catch_defects` plants each defect and checks that its control fails:

| Defect planted | Control that fails |
|---|---|
| pairing off by one seed (one arm's values rotated by a seed) | A/A |
| a 7,000-request bound instead of the 25 ms time bound | common close |
| the placebo inserted at index 3 instead of appended last | placebo is inert |
| a strategy that keeps state in `static` storage between sessions | replay |
| one participant's value off by one price unit | closed identities |
| a session whose values do not sum to zero | closed identities |
| a placebo that sends an order | placebo is zero |

`test_controls_hold` checks that the controls pass on a clean smoke run and fail on real arms
that differ: `maker` is not a positive control (it makes money), `maker` and `roundtrip` are
not an A/A pair, and `roundtrip` against the placebo is far above the noise floor.

One defect is not planted: gating on the floating-point `mean_gap_ticks()` instead of the
integer cross-multiplication. It would change a decision only at a boundary no seed reaches,
and `test_suite_health_gate` already pins that boundary exactly, at `gap_sum == 4 · samples`
and one more.

**A new control comes with its planted defect.** Keep that pairing.

## Two evaluator bugs found on the way

- **Comparisons between candidates ignored an inadmissible reference market.** The report
  said "nothing is compared" and then compared two candidates in that market. Those rows are
  now `not_admissible` too, and `test_suite_outcomes` checks it.
- **"It sent no orders" was inferred from value alone.** An arm whose value equals the
  placebo's on every seed is called identical, but the report now says it sent no orders only
  when the records say it was inert on every seed.

`test_suite_outcomes` also produces the `not_separated` verdict and the exit codes 1 and 3,
which no test produced before.

## Where it lives

- `arena/controls.hpp`: one function per control, each taking finished records and returning
  what it expected, what it measured and whether it held; `controls_text`.
- `arena/controls.cpp`: the `controls` program, which runs the placebo, `roundtrip`, `maker`
  twice and `maker` at 9,999 ns on a block, with every check the runner makes.
- `arena/strategies/roundtrip.hpp`: the positive control, a built-in strategy in no default
  line-up.
- `Makefile`: `make controls`. `test-cli` runs it on the standard block (about 10 s) and checks
  for `8/8 controls held`.
- `tests/test_arena.cpp`: `test_controls_hold`, `test_controls_catch_defects`,
  `test_suite_outcomes`.

## Not built

- **A zero-intelligence baseline.** `roundtrip` is the dumbest possible strategy, but a
  random limit-order strategy would be a better floor to publish beside real candidates.
