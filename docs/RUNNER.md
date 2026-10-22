# The standard experiment runner

The runner behind `make compare` (`arena/suite.hpp`, `arena/suite.cpp`). It puts each candidate
strategy, installed or built in, in one fixed seat of the reference market, on a named seed block,
beside a placebo arm, and prints a paired comparison with its uncertainty. It was step 2 of five;
`docs/REPORT.md`, `docs/CONTROLS.md` and `docs/REPRODUCE.md` build on it.

Acceptance criterion: two strategies can be run on identical conditions and compared without
manually configuring separate experiments. No condition can be set on the command line, and the same
command on the same build writes byte-identical files.

Numbers marked "runner" come from its own output at `e504824` (Linux, g++ 13.3, `-O3`). Numbers marked
"probe" needed conditions the suite forbids and came from scratch probes at `6ffdbc3`. Suite numbers
come from 25 ms sessions, not the published 200k-request ones, and never go in `results/`.

## Command line

```bash
make strategy STRATEGY=examples/strategies/imbalance.hpp   # install; also builds suite_lab
make compare CANDIDATES=imbalance BLOCK=smoke              # 4 seeds: does it run, does it trade
make compare CANDIDATES=imbalance,maker                    # standard block -> runs/imbalance-vs-maker-standard/
build/release/suite --candidates=maker,momentum            # built-ins only; cannot see installed code
```

With one candidate the comparison is against the placebo, whose seat value is exactly zero, so the
paired difference is the strategy's own value. Two versions of one strategy are installed under two
names, since a strategy's name is its file name.

`make compare` takes `CANDIDATES` (required; empty exits 2), `BLOCK` (`standard` or `smoke`), `SUITE`
(`standard`, `wide`, `busy` or `all`) and `OUT`. It is `BLOCK`, not `SEEDS`, because
`bench/run_arena_evidence.sh` reads a numeric `SEEDS` from the environment.

`suite` and `suite_lab` flags, as `--help` prints them:

| Flag | Meaning |
|---|---|
| `--candidates=A[,B,...]` | 1 to 8 strategies; the first is the baseline of the paired rows |
| `--block=NAME` | `standard` (64 seeds, the default) or `smoke` (4 seeds) |
| `--suite=NAME` | `standard` (the default), `wide`, `busy`, or `all`: one directory each |
| `--out=DIR` | default `runs/<a>[-vs-<b>...]-<block>/`; a previous run there is replaced |
| `--reproduce=DIR` | re-run DIR from its `suite.cfg` and compare every file (`docs/REPRODUCE.md`) |
| `--help` | lists the built-in names and, in `suite_lab`, the installed ones |

There is no `--set`, `--param`, `--seeds`, `--requests` or `--strategies`: an unknown flag exits 2. A
different set of conditions is a new suite in code; exploration stays with `sweep_lab`.

Exit codes, with precedence 2, 3, 1, 0: **0** complete, whatever the verdicts; **1** a candidate
session differed on replay, or an inert candidate did not leave the placebo's market unchanged; **2**
usage or configuration error, decided before any session runs, or an unwritable run directory; **3**
the reference market is not admissible on this build (a placebo session failed the gate or differed
from the market with no seat, or cash, position or value failed to sum to zero).

## The standard suite

`standard_suite()` in `arena/suite.hpp`: suite `standard`, version 1. The families `wide` and `busy`
are suites of their own (`docs/REPORT.md`).

### Line-up

`maker,rival,informed,noise:2,momentum` at default parameters: exactly the default `build_roster()`
behind every published number, built through `roster_from_spec` so `suite.cfg` records it.
`test_suite_reference_is_default` holds the two equal. Only `informed` sees the fundamental.

### The seat

**Index 6, owner 7, appended after the line-up.**

- Owner ids are index + 1 and reset seeds are `seed + 0x9E37·(index + 1)`, so only an appended seat
  leaves every reference participant's owner id and random stream untouched.
- **Never informed.** `informed` and `placebo` are refused as candidates.
- **Reaction delay 10,000 ns, set by the suite** (the reference maker's). `SeatOverride` reports it in
  place of the declared `reaction_ns`, which is recorded and not used. A strategy reading its own
  `reaction_ns` sees what it declared; its real delay is `now_ns − as_of_ns` in its `MarketView`.
- Its `timer_ns` is honoured: a timer is part of a strategy's logic.

A reaction delay is an input the experimenter chooses, not something a strategy earns; otherwise speed
is mixed into every comparison. Imbalance's seat value is −34.92 ± 3.09 at 10 µs (runner) and
−23.70 ± 2.91 at its declared 20 µs (probe).

### Session length: simulated time, not requests

`max_time_ns = 25,000,000`, `max_requests = 0`, `stall_limit_ns = 0`; everything else at the
`SessionConfig` defaults (fee 0, markout horizon 1 ms).

**Why time.** A request bound counts the candidate's own orders, so a busier candidate ends everyone's
session earlier and its arm is marked at a different closing value V(T). Under a 7,000-request bound
placebo and imbalance closed at the same V on 5 of 64 seeds (probe); under the time bound every arm
shared V(T) and the number of fundamental steps on 64 of 64 seeds (runner).
`test_suite_common_conditions` re-checks this on every `make test`. A stall exit would split arms the
same way.

**Why 25 ms.** The reference market tracks the fundamental on every seed up to 30 ms and first comes
loose between 30 and 40 ms. Gap is a session's mean |mid − V| in ticks:

| Length | Seeds | Median gap | Worst | Over 4 ticks |
|---|---|---|---|---|
| 25 ms (runner) | 42..105 | 1.51 | 2.03 | 0 |
| 40 ms (probe) | 42..105 | 1.51 | 5.27 | 1 |
| 500 ms (probe) | 42..65 | 6.22 | 48.12 | 13 |
| published, 200k requests | 42..65 | 10.64 | 67.79 | 16 |

The tail grows with length, driven by the informed trader's position limit. A 25 ms placebo session
has 1,027 to 1,462 trades and takes about 15 ms of wall time.

**Step ceiling.** A session is cut off at 2,000,000 scheduler steps, marked `ended_by=ceiling`, and
fails the gate. It bounds a runaway strategy, including one that wakes without ordering; it is not a
stopping rule. Placebo sessions take 18,155 to 20,051 steps.

### Seeds

Named blocks only: `standard` is 42..105 (64 seeds); `smoke` is its prefix 42..45, which shows a
strategy runs and cannot separate anything. Paired standard errors at 64 seeds were 1.0 to 1.8
currency units for small perturbations (probe) and 3.26 to 17.02 for unrelated strategies (runner);
24 seeds would make every interval about 1.6 times wider.

### The health gate

Per session, integers only:

- `gap_samples > 0`;
- `gap_sum_ticks ≤ 4 · gap_samples` (mean gap at most 4 ticks, one quoted maker spread);
- `100 · gap_unpriced ≤ 5 · (gap_samples + gap_unpriced)` (one-sided at most 5% of the time);
- `book_full_rejects == 0` and `wall_hits == 0`;
- `ended_by == time`, and the replay or no-seat check identical.

An arm is admissible only if it passes on every seed; a comparison only if both arms are. Failing
seeds are never dropped, which would condition on the outcome: the verdict changes and the numbers are
still written. A third noise trader in the seat fails on 2 of 64 standard seeds (79 and 105).

### Arms and checks per seed

Each in a fresh `Session`:

1. **Placebo arm**: the line-up plus `placebo` in the seat.
2. **No-seat check**: the line-up alone, which must hash equal to the placebo arm. This proves the
   reference replays and the seat is inert on this build.
3. **Each candidate**: its arm, then a replay from fresh instances, which must hash the same. If it sent
   no orders, its arm must also hash equal to the placebo arm.

Two candidates on the standard block is 384 sessions, about 5.6 s. An arm's rows do not depend on
which other candidates are named.

## Identical conditions

Two candidates cannot produce identical sessions; their orders are the treatment. "Identical
conditions" means identical exogenous inputs and an identical seat: the fundamental path and V(T),
session length, the opposing line-up with its owner ids and random streams, the timer and signal
schedule, the seat's index, seed, delay and information, and the venue. Value is
`cash + position · V(T)`, so inventory marked at a random-walk price is shared across arms and pairing
by seed cancels it.

### The seat and wake ordering

An inert participant appended last does not change wake ordering. The scheduler orders by
`(at, seq)`; the seat's wakes take sequence numbers but push nothing for anyone else, so everyone
else's items keep their relative order and the hash is unchanged. Under `small_session()` at seed 42
the reference alone, with an appended placebo, and with an inert strategy on a 7 µs timer all give
`2853d2127a3486c5`; the placebo inserted at index 3 gives `5a9a688a7ca5b9f8`. **Everything that differs
between a candidate arm and the placebo arm is caused by the candidate's orders.**

### Why a placebo arm at all

1. It proves the premise on every run: a change to scheduling or seeding exits 3.
2. It is each reference participant's counterfactual, so the seat's value is attributed exactly
   (`docs/REPORT.md`).
3. It is the health baseline and the suite's own admissibility.
4. It is the source of the fingerprint.
5. It identifies a candidate that never acts, reported inert, not as zero edge.

### What the placebo arm does not control for

- **Skill**: "against the placebo" means against doing nothing. Baselines are in `docs/CONTROLS.md`.
- **Seat position**: the seat comes last in same-instant ties. Moving it to 9,999 ns gave −0.72 ± 1.57
  for imbalance and −0.06 ± 0.99 for a maker (probe), both unresolvable.
- **Divergence after the first order**: pairing is exact for the fundamental and schedule, partial for
  endogenous flow. Variance removed, 1 − var(d) / (var(a) + var(b)): 85% for a one-parameter change
  (probe), 23% for maker against imbalance, 3% for momentum against imbalance, −31% for noise against
  imbalance (runner). The report prints it.
- **Overfitting**: the seeds are public.

### The fingerprint

FNV-1a 64 over `suite.cfg`'s condition lines (keys `suite.`, `seat.`, `health.`, `block.`, `market.`,
`session.`, `fair.`) plus `seed=<s> placebo=<hash>` per seed. Reference parameters enter through the
placebo hashes, so an unrelated new `RosterConfig` field does not move it. It moves with the engine, the
session, a reference strategy, a default, the suite or the block; two results are comparable only if
fingerprints are equal. The smoke fingerprint `d461c30ab2446acf` is pinned in `test_arena`
(`kPinnedSmokeFingerprint`, with one pin per family). Moving a pin needs a version bump in
`standard_suite()`, never a silent re-pin. `test-lab` asserts `suite` and `suite_lab` agree.

## The run directory

`runs/` is git-ignored; nothing goes under `results/`. A directory holding `suite.cfg` is replaced;
any other non-empty one is refused. `SUITE=all` adds one directory per family and `families.txt`. Raw
files are integer-only and LF; derived files are computed from them alone.

- `suite.cfg` (raw, first): condition lines, every reference parameter by `--set=` name, and `run.*`
  and `build.*` lines that are recorded, not conditions.
- `sessions.csv` (raw): one row per (seed, arm): ending, counts, V(T), gap, closed-world sums, seat
  value, inert, health, hashes, check.
- `participants.csv` (raw): seven rows per (seed, arm), every `Account` field as raw sums.
- `comparison.csv`: paired mean, s.e., 95% interval, resolvable (2.8 × s.e.), range, pairing, verdict.
- `evaluation.csv`: the seat's value broken down (`docs/REPORT.md`).
- `attribution.csv`: the seat's value per reference participant (`docs/REPORT.md`).
- `report.txt`: stdout except the `wrote` line; no wall time, no path.
- `outcome.cfg` (last, only if every other file was): `status=complete`, fingerprint, checks,
  admissibility, failing and inert seeds, exit code. Without `status=complete` a directory is
  incomplete.

Verdicts: `not_admissible`, `identical` (every difference 0), `separated` (95% interval excludes 0),
`not_separated`.

## Code and tests

`arena/suite.hpp` is header-only and integer-only (blocks, gate, `SuiteDef`, `SeatOverride`,
`run_arm`, records, writers, fingerprint). `arena/suite_report.hpp` holds the only floating point and
reads only a `RunRecord`. `arena/suite.cpp` compiles into `suite` without `PX_ARENA_USER_STRATEGIES`
and into `suite_lab` with it. `arena/strategies/placebo.hpp` is the empty seat. The engine was not
touched. Tests: `tests/test_arena.cpp` (`test_placebo_is_inert`, `test_suite_*`), `make test-cli` and
`make test-lab`.

## Later steps

Steps 3 to 5 have their own documents: `docs/REPORT.md`, `docs/CONTROLS.md`, `docs/REPRODUCE.md`. Not
built: a holdout block (seeds 1,000,000..1,000,063 were admissible in a probe) and showing a scored arm
in the viewer.

## Known weaknesses

- **25 ms favours short-horizon logic.** `fader` is inert on all 64 seeds; inert seeds are reported.
- **Pairing is partial** for unrelated strategies, so they need more seeds. Counter-based noise draws
  would help but would move every published hash.
- **The path is chaotic.** A 1 ns change in the seat's delay moved imbalance by −0.72 ± 1.57 or
  +1.76 ± 1.76 (probe). A `separated` effect of a few currency units is weak evidence: momentum against
  the placebo is −2.36 ± 1.17, `separated`, with a per-seed range of −24.40 to +26.60.
  `docs/CONTROLS.md` publishes the noise floor.
- **Tie order** is fair between candidates but asymmetric against the reference maker.
- **The fixed delay can surprise an author** reading its own `reaction_ns`.
- **Installed strategies are not policed.** Replay catches `static` state but cannot prove its
  absence; floating point cannot be detected, so cross-platform replay is promised only for built-ins;
  an infinite loop in `on_event` hangs the runner, and a wall-clock timeout would break replay.
- **The thresholds are judgement calls.** A candidate dragging the gap from 1.5 to 3.9 ticks on every
  seed still passes.
- **The published market's health at its published length is worse than the documents say**: 16 of 24
  headline sessions are more than 4 ticks loose. The suite sidesteps this; it does not fix it.
- **Platform**: the runner's timings are from Linux only.

## Rejected alternatives and wrong turns

- **A `--candidates` mode in `sweep`**: it makes the published evidence and stops on a request count.
- **Request-bounded sessions or the stall exit**: arms close at different V(T).
- **500 ms or the published length**: the book comes loose.
- **Keeping only seeds where the placebo passes**: selects calm paths, so it is no longer the model.
- **The candidate's declared delay, or a flag for it**: speed would be the candidate's choice.
- **Inserting the seat earlier or replacing a reference participant**: shifts streams or changes the
  line-up.
- **Two candidates in one session**: measures interaction, a different question.
- **Only the no-seat market, or only the placebo**: loses the exact-zero arm or the per-run check.
- **Dropping failing seeds**: conditions on the outcome.
- **Range-straddles-zero alone**: far more conservative than a paired standard error; both are reported.
- **Replaying only the first seed per arm**: misses state carried across later seeds.
- **`--set`, `--param` or a free seed list**: the conditions would be whatever was typed.
- **Hashing all of `suite.cfg`**: an unrelated `RosterConfig` field would force a bump.
- **A Python runner**: ruled out by the project's constraints.
- **Wrong premise, corrected**: the earlier lab plan assumed adding a participant re-interleaves the
  session. For an appended inert seat it does not.
