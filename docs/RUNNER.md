# The standard experiment

What `/compare` does. It puts each candidate strategy, yours or a built-in one, in one fixed
seat of the reference market, on a named block of seeds, beside a placebo arm whose seat never
acts. Then it prints the paired difference with its uncertainty. Two strategies get identical
conditions from one command, and the same command on the same build writes byte-identical
files. [`REPORT.md`](REPORT.md) explains the report it prints.

```
> /compare imbalance smoke          # 4 seeds: does it run, does it trade
> /compare imbalance,maker          # the standard block -> runs/imbalance-vs-maker-standard/
> /compare imbalance,maker all      # every scenario family, one directory each
```

With one candidate the comparison is against the placebo, whose seat value is exactly zero,
so the paired difference is the strategy's own value. With several, every later candidate is
also paired against the first. To compare two versions of one strategy, install them under
two names, since a strategy's name is its file name.

**Where the numbers in this file come from.** Numbers marked "runner" are the runner's own
output at `e504824` (Linux, g++ 13.3, `-O3`). Numbers marked "probe" needed conditions the
suite forbids and came from scratch probes at `6ffdbc3`. Suite sessions last 25 ms, not the
published 200,000 requests, so their numbers never go in `results/`.

## Running it without the app

```bash
make strategy STRATEGY=examples/strategies/imbalance.hpp   # install; also builds suite_lab
make compare CANDIDATES=imbalance BLOCK=smoke
make compare CANDIDATES=imbalance,maker
build/release/suite --candidates=maker,momentum            # built-ins only; cannot see installed code
```

`make compare` takes `CANDIDATES` (required; empty exits 2), `BLOCK` (`standard`, `smoke` or `holdout`),
`SUITE` (`standard`, `wide`, `busy` or `all`) and `OUT`. It is `BLOCK`, not `SEEDS`, because
`bench/run_arena_evidence.sh` reads a numeric `SEEDS` from the environment.

The flags of `suite` and `suite_lab`:

| Flag | Meaning |
|---|---|
| `--candidates=A[,B,...]` | 1 to 8 strategies; the first is the baseline of the paired rows |
| `--block=NAME` | `standard` (64 seeds, the default), `smoke` (4 seeds) or `holdout` (64 confirmatory seeds) |
| `--suite=NAME` | `standard` (the default), `wide`, `busy`, or `all`: one directory each |
| `--out=DIR` | default `runs/<a>[-vs-<b>...]-<block>/`; a previous run there is replaced |
| `--reproduce=DIR` | re-run DIR from its `suite.cfg` and compare every file ([`REPRODUCE.md`](REPRODUCE.md)) |
| `--help` | lists the built-in names and, in `suite_lab`, the installed ones |

**No condition can be set on the command line.** There is no `--set`, `--param`, `--seeds`,
`--requests` or `--strategies`, and an unknown flag exits 2. A different set of conditions is a
new suite in code; exploration stays with `/sweep` and `sweep_lab`.

| Exit | Meaning |
|---|---|
| 0 | complete, whatever the verdicts |
| 1 | a candidate session differed on replay, or an inert candidate did not leave the placebo's market unchanged |
| 2 | usage or configuration error, decided before any session runs, or an unwritable run directory |
| 3 | the reference market is not admissible on this build: a placebo session failed the gate or differed from the market with no seat, or cash, position or value failed to sum to zero |

When several apply, the first in the order 2, 3, 1, 0 wins.

## What stays fixed

`standard_suite()` in `arena/suite.hpp` defines suite `standard`, version 1. The families
`wide` and `busy` are suites of their own ([`REPORT.md`](REPORT.md#scenario-families)).

### The line-up

`maker,rival,informed,noise:2,momentum` at default parameters: exactly the default line-up
behind every published number, built through `roster_from_spec` so `suite.cfg` records it.
`test_suite_reference_is_default` holds the two equal. Only `informed` sees the fundamental.

### The seat

**Index 6, owner 7, appended after the line-up.**

- Owner ids are index + 1 and reset seeds are `seed + 0x9E37·(index + 1)`, so only an
  appended seat leaves every reference participant's owner id and random stream untouched.
- **Never informed.** `informed` and `placebo` are refused as candidates.
- **Reaction delay 10,000 ns, set by the suite** (the reference maker's). The strategy's
  declared `reaction_ns` is recorded and not used. A strategy that reads its own
  `reaction_ns` sees what it declared; its real delay is `now_ns − as_of_ns` in its
  `MarketView`.
- Its `timer_ns` is honoured, because a timer is part of a strategy's logic.

The delay is fixed because it is an input the experimenter chooses, not something a strategy
earns; otherwise speed would be mixed into every comparison. `imbalance` is worth
−34.92 ± 3.09 at 10 µs (runner) and −23.70 ± 2.91 at its declared 20 µs (probe).

### Session length: 25 ms of simulated time

`max_time_ns = 25,000,000`, `max_requests = 0`, `stall_limit_ns = 0`; everything else at the
`SessionConfig` defaults (fee 0, markout horizon 1 ms).

**Time, not requests.** A request bound counts the candidate's own orders, so a busier
candidate would end everyone's session earlier and its arm would be marked at a different
closing value V(T). Under a 7,000-request bound, placebo and `imbalance` closed at the same V
on only 5 of 64 seeds (probe). Under the time bound every arm shared V(T) and the number of
fundamental steps on 64 of 64 seeds (runner), and `test_suite_common_conditions` re-checks it
on every `make test`. A stall exit would split arms the same way.

**Why 25 ms.** The reference market tracks the fundamental on every seed up to 30 ms and first
comes loose between 30 and 40 ms. The gap is a session's mean |mid − V| in ticks:

| Length | Seeds | Median gap | Worst | Over 4 ticks |
|---|---|---|---|---|
| 25 ms (runner) | 42..105 | 1.51 | 2.03 | 0 |
| 40 ms (probe) | 42..105 | 1.51 | 5.27 | 1 |
| 500 ms (probe) | 42..65 | 6.22 | 48.12 | 13 |
| published, 200k requests | 42..65 | 10.64 | 67.79 | 16 |

The tail grows with length, driven by the informed trader's position limit. A 25 ms placebo
session has 1,027 to 1,462 trades and takes about 15 ms of wall time.

**A step ceiling, not a stopping rule.** A session is cut off at 2,000,000 scheduler steps,
marked `ended_by=ceiling`, and fails the gate. It bounds a runaway strategy, including one that
wakes without ordering. Placebo sessions take 18,155 to 20,051 steps.

### The seeds

Named blocks only. `standard` is 42..105 (64 seeds). `smoke` is its first four, 42..45, which
shows a strategy runs and cannot separate anything. `holdout` is 1,000,000..1,000,063 (64
seeds), shares no seed with either and is used by no example.

**Tune on `standard`, report once on `holdout`.** Every change you make after reading a
standard report, whether a threshold, a size or a whole idea, is fitted to those 64 seeds a
little, and the standard interval no longer means what it says. The holdout block is the
answer to that: when the strategy is final, run `/compare NAME holdout` once and report it
beside the standard run. Its report says `confirmatory` and is never pooled with standard,
because pooling would mix a fitted number with an unfitted one. Running it again after
another change spends it: the honest report then says how many times it was looked at. The
seeds sit far from standard's so that nobody reaches them by extending a range. The block's
fingerprint is `6e1fe05931a6f560`, pinned in `test_arena` (`kPinnedHoldoutFingerprint`), which
also checks that every placebo session on it passes the health gate. Paired standard errors at 64 seeds were 1.0
to 1.8 currency units for small changes to one strategy (probe) and 3.26 to 17.02 between
unrelated strategies (runner). 24 seeds would make every interval about 1.6 times wider.

### The health gate

Checked per session, in integers:

- `gap_samples > 0`;
- `gap_sum_ticks ≤ 4 · gap_samples`: a mean gap of at most 4 ticks, one quoted maker spread;
- `100 · gap_unpriced ≤ 5 · (gap_samples + gap_unpriced)`: one-sided at most 5% of the time;
- `book_full_rejects == 0` and `wall_hits == 0`;
- `ended_by == time`, and the replay or no-seat check identical.

An arm is admissible only if it passes on every seed, and a comparison only if both arms are.
**Failing seeds are never dropped**, because that would condition on the outcome: the verdict
changes and the numbers are still written. A third noise trader in the seat fails on 2 of 64
standard seeds (79 and 105).

### What runs for each seed

Each in a fresh `Session`:

1. **The placebo arm:** the line-up plus `placebo` in the seat.
2. **The no-seat check:** the line-up alone, which must hash equal to the placebo arm. This
   proves the reference market replays and the seat is inert on this build.
3. **Each candidate:** its arm, then a replay from fresh instances, which must hash the same.
   If it sent no orders, its arm must also hash equal to the placebo arm.

Two candidates on the standard block is 384 sessions, about 5.6 s. An arm's rows do not
depend on which other candidates are named.

## What "identical conditions" means

Two candidates cannot produce identical sessions: their orders are the treatment. Identical
conditions means identical inputs from outside the seat, and an identical seat:

- the fundamental path and V(T), and the session length;
- the opposing line-up, with its owner ids and random streams;
- the timer and signal schedule;
- the seat's index, seed, delay and information, and the venue.

Value is `cash + position · V(T)`, so leftover inventory marked at a random-walk price is
shared across arms, and pairing by seed cancels it.

### The seat and wake ordering

An inert participant appended last does not change wake ordering. The scheduler orders by
`(at, seq)`. The seat's wakes take sequence numbers but push nothing for anyone else, so
everyone else's items keep their relative order and the hash is unchanged. Under
`small_session()` at seed 42, the reference alone, with an appended placebo, and with an inert
strategy on a 7 µs timer all give `2853d2127a3486c5`; the placebo inserted at index 3 gives
`5a9a688a7ca5b9f8`. **Everything that differs between a candidate arm and the placebo arm is
caused by the candidate's orders.**

### Why a placebo arm at all

1. It proves the premise on every run: a change to scheduling or seeding exits 3.
2. It is each reference participant's counterfactual, so the seat's value is attributed
   exactly ([`REPORT.md`](REPORT.md#where-the-money-came-from)).
3. It is the health baseline and the suite's own admissibility.
4. It is the source of the fingerprint.
5. It identifies a candidate that never acts, which is reported as inert, not as zero edge.

### What the placebo arm does not control for

- **Skill.** "Against the placebo" means against doing nothing. Baselines are in
  [`CONTROLS.md`](CONTROLS.md).
- **Seat position.** The seat comes last in same-instant ties. Moving it to 9,999 ns gave
  −0.72 ± 1.57 for `imbalance` and −0.06 ± 0.99 for a maker (probe), both unresolvable.
- **Divergence after the first order.** Pairing is exact for the fundamental and the
  schedule, and partial for the flow the strategies cause. The variance it removes,
  1 − var(d) / (var(a) + var(b)), was 85% for a one-parameter change (probe), 23% for `maker`
  against `imbalance`, 3% for `momentum` against `imbalance` and −31% for `noise` against
  `imbalance` (runner). The report prints it.
- **Overfitting.** The seeds are public.

### The fingerprint

FNV-1a 64 over `suite.cfg`'s condition lines (keys starting `suite.`, `seat.`, `health.`,
`block.`, `market.`, `session.`, `fair.`) plus `seed=<s> placebo=<hash>` for each seed.
Reference parameters enter through the placebo hashes, so an unrelated new `RosterConfig`
field does not move it.

It moves with the engine, the session, a reference strategy, a default, the suite or the
block, and **two results are comparable only if their fingerprints are equal.** The smoke
fingerprint `d461c30ab2446acf` is pinned in `test_arena` (`kPinnedSmokeFingerprint`, with one
pin per family and one for the holdout block). Moving a pin needs a version bump in `standard_suite()`, never a silent
re-pin. `test-lab` checks that `suite` and `suite_lab` agree.

## The run directory

`runs/` is git-ignored, and nothing goes under `results/`. A directory holding `suite.cfg` is
replaced; any other non-empty one is refused. `all` adds one directory per family and
`families.txt`. Raw files are integer-only with LF line endings; derived files are computed
from them alone.

| File | Kind | Holds |
|---|---|---|
| `suite.cfg` | raw, written first | condition lines, every reference parameter by `--set=` name, and `run.*` and `build.*` lines that are recorded, not conditions |
| `sessions.csv` | raw | one row per seed and arm: ending, counts, V(T), gap, closed-world sums, seat value, inert, health, hashes, check |
| `participants.csv` | raw | seven rows per seed and arm, every `Account` field as raw sums |
| `comparison.csv` | derived | paired mean, s.e., 95% interval, resolvable (2.8 × s.e.), range, pairing, verdict |
| `evaluation.csv` | derived | the seat's value broken down ([`REPORT.md`](REPORT.md)) |
| `attribution.csv` | derived | the seat's value per reference participant ([`REPORT.md`](REPORT.md)) |
| `report.txt` | derived | what was printed, except the `wrote` line; no wall time, no path |
| `outcome.cfg` | written last, only if every other file was | `status=complete`, fingerprint, checks, admissibility, failing and inert seeds, exit code |

A directory without `status=complete` is incomplete. The verdicts are `not_admissible`,
`identical` (every difference 0), `separated` (the 95% interval excludes 0) and
`not_separated`.

## The floor: `zi`

A positive result needs something to be positive against. The placebo is zero by
construction, so it says whether a strategy made money, not whether it did anything a
strategy with no intelligence would not. `zi` (`arena/strategies/zi.hpp`) is that strategy,
after Gode and Sunder's zero-intelligence traders: on its timer it withdraws its resting
order, flips a coin for the side and posts one limit order at a price drawn uniformly from 8
ticks either side of the mid it sees, never trading past ±200. It has no view, no memory and
no signal.

**What it was expected to do,** written in its header before it ran: lose money, mostly in
the `spread` column, because its orders that cross pay the spread and its resting orders fill
mostly once the market has moved through them. How much was not predicted. On the standard
block (`suite --candidates=zi,roundtrip,maker`):

| arm vs baseline | mean ± s.e. | verdict |
|---|---|---|
| zi vs placebo | −91.82 ± 2.47 | separated |
| maker vs zi | +162.95 ± 8.00 | separated |
| roundtrip vs zi | +12.27 ± 2.51 | separated |

The qualitative expectation held: −91.46 ± 0.71 of the −91.82 is `spread`, with adverse
markout and carry each within two standard errors of zero, and every one of its 64 markets
passes the health gate. Two things it says about the evaluator:

- **Random is worse than dumb.** `roundtrip`, which only ever crosses the spread, loses about
  12 a session less than `zi`, whose resting orders are filled when they are stale. Beating
  `zi` is a low bar; beating the placebo is not the same bar.
- **Put `zi` beside a candidate** (`/compare mine,zi`) and the paired row `mine vs zi` is the
  question "did it beat chance on these seeds?". A candidate that does not separate from `zi`
  has shown nothing, whatever its value against the placebo.

`zi` is a built-in and never in a default line-up, so adding it moved no hash and no
fingerprint. `test_arena` (`test_zi_floor`) checks that it trades, keeps to its budget on the
path and not only at the close, replays, and leaves its markets healthy.

## Known weaknesses

- **25 ms favours short-horizon logic.** `fader` is inert on all 64 seeds; inert seeds are
  reported.
- **Pairing is partial** between unrelated strategies, so they need more seeds. Counter-based
  noise draws would help but would move every published hash.
- **The path is chaotic.** A 1 ns change in the seat's delay moved `imbalance` by −0.72 ± 1.57
  or +1.76 ± 1.76 (probe). A `separated` effect of a few currency units is weak evidence:
  `momentum` against the placebo is −2.36 ± 1.17, `separated`, with a per-seed range of
  −24.40 to +26.60, and it does not survive the Holm correction across the three scenario
  families ([`REPORT.md`](REPORT.md#many-comparisons)). [`CONTROLS.md`](CONTROLS.md)
  publishes the noise floor.
- **Tie order** is fair between candidates but works against the reference maker.
- **The fixed delay can surprise an author** who reads their strategy's own `reaction_ns`.
- **Installed strategies are not policed.** Replay catches `static` state but cannot prove its
  absence. Floating point cannot be detected, so cross-platform replay is promised only for
  built-ins. An infinite loop in `on_event` hangs the runner, and a wall-clock timeout would
  break replay.
- **The thresholds are judgement calls.** A candidate that drags the gap from 1.5 to 3.9 ticks
  on every seed still passes.
- **The published market is loose at its published length:** 16 of 24 headline sessions
  are more than 4 ticks from the fundamental. The 25 ms suite sidesteps this; it does not fix
  it.
- **The runner's timings are from Linux only.**

## Wrong turns and rejected alternatives

- **A `--candidates` mode in `sweep`:** `sweep` makes the published evidence and stops on a
  request count.
- **Request-bounded sessions, or the stall exit:** arms close at different V(T).
- **500 ms, or the published length:** the book comes loose.
- **Keeping only seeds where the placebo passes:** that selects calm paths, so it is no longer
  the model.
- **The candidate's declared delay, or a flag for it:** speed would become the candidate's
  choice.
- **Inserting the seat earlier, or replacing a reference participant:** it shifts random
  streams or changes the line-up.
- **Two candidates in one session:** that measures their interaction, a different question.
- **Only the no-seat market, or only the placebo:** it loses either the exact-zero arm or the
  per-run check.
- **Dropping failing seeds:** conditions on the outcome.
- **"The range straddles zero" alone as the test:** far more conservative than a paired
  standard error. Both are reported.
- **Replaying only the first seed per arm:** misses state carried across later seeds.
- **`--set`, `--param` or a free seed list:** the conditions would be whatever was typed.
- **Hashing all of `suite.cfg`:** an unrelated `RosterConfig` field would force a version bump.
- **A Python runner:** ruled out by the project's constraints.
- **A wrong premise, corrected:** the first plan assumed that adding a participant
  re-interleaves the session. For an appended inert seat it does not.

Not built: showing a scored arm in the viewer. A holdout block was on this list; it is built
("The seeds").

## Where it lives

- `arena/suite.hpp`: header-only and integer-only. Blocks, the gate, `SuiteDef`,
  `SeatOverride`, `run_arm`, the records, the writers and the fingerprint.
- `arena/suite_report.hpp`: the only floating point, reading only a `RunRecord`.
- `arena/suite.cpp`: compiled into `suite` without `PX_ARENA_USER_STRATEGIES` and into
  `suite_lab` with it.
- `arena/strategies/placebo.hpp`: the empty seat. The engine was not touched.
- Tests: `tests/test_arena.cpp` (`test_placebo_is_inert`, `test_suite_*`), `make test-cli`
  and `make test-lab`.
