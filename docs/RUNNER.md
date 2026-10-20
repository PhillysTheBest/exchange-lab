# The standard experiment runner

The runner behind `make compare`, which makes any strategy comparable with any other.
It runs an installed or built-in strategy in one fixed seat of the reference market, on a
named block of seeds, beside a control arm, and prints a paired comparison with its
uncertainty. It was step 2 of the five that turned the arena into a strategy research
platform. The evaluation report (`docs/REPORT.md`), the evaluator's controls
(`docs/CONTROLS.md`) and one-command reproduction (`docs/REPRODUCE.md`) are built on top
of it.

This file began as the plan, written before building, as CLAUDE.md asks. It is now the
runner's design document. It keeps the reasoning and the measurements that decided each
choice, and it keeps the approaches that were rejected or turned out to be wrong.

**Where the numbers come from.** Every number the runner can print was re-taken from its
own output on the tree at `e504824`, the commit that added it, on Linux with g++ 13.3 at
`-O3`, with the example strategy `imbalance` installed through `make strategy`. These are
the runs:

```bash
build/release/suite_lab --candidates=imbalance,maker                  # the report under "What is printed"
build/release/suite_lab --candidates=imbalance,maker,momentum,noise   # the pairing figures
build/release/suite --candidates=noise                                # the health gate failing
build/release/suite --candidates=fader                                # an inert candidate
build/release/suite --candidates=momentum
build/release/suite --candidates=maker,momentum --block=smoke
build/release/suite_lab --candidates=imbalance [--block=smoke]        # timings
```

Ranges over sessions (steps, requests, trades, wakes, closing values, positions) were read
from the `sessions.csv` and `participants.csv` that those runs wrote. Wall times are from
the same machine and are approximate. Some numbers cannot come from the runner, because
they need conditions the suite deliberately does not allow, such as another session
length, seed range, seat delay, seat position or strategy parameter, or a request bound.
Those came from scratch probes against the tree at
`6ffdbc3`, which linked `arena/session.hpp` unchanged, and each one says so where it
appears. None of these numbers is in `results/`, and suite numbers never go there: they
come from 25 ms sessions, not the published 200k-request ones.

## Goal

Every strategy is evaluated against the same things:

- the same initial conditions;
- the same opposing line-up;
- the same predefined seeds.

The runner saves the configuration and the raw per-session results, not only a profit figure. It uses
the market and the strategies already in the tree, and adds no new market model.

> Acceptance criterion: two strategies can be run on identical conditions and compared without manually configuring separate experiments.

In concrete terms, `make compare CANDIDATES=mine,other` does three things:

- it runs both strategies on the same conditions;
- it writes a run directory holding the conditions and the raw results;
- it prints a paired comparison with its uncertainty.

None of the conditions can be changed on that command line. Typed twice on the same build it produces
byte-identical files: two runs of `imbalance,maker` into different directories wrote six identical
files, and `test-cli` checks the same on every run.

## What a developer runs

On Linux use `make`; on Windows use `mingw32-make`, and the binaries end in `.exe`.

**One strategy:**

```bash
make strategy STRATEGY=examples/strategies/imbalance.hpp   # step 1, unchanged; now also builds suite_lab
make compare CANDIDATES=imbalance BLOCK=smoke              # 4 seeds, about 0.3 s: does it run, does it trade
make compare CANDIDATES=imbalance                          # the standard block: 64 seeds, about 4 s
#   which runs: build/release/suite_lab --candidates=imbalance
#   and writes  runs/imbalance-standard/
```

With one candidate, the comparison printed is imbalance against the placebo, the empty seat. The paired
difference is then the strategy's own seat value, because the placebo's is exactly zero on every seed.

**Two strategies on identical conditions, with one command and nothing configured for either:**

```bash
make strategy STRATEGY=path/to/mine.hpp
make strategy STRATEGY=path/to/other.hpp       # installed headers accumulate in arena/strategies/user/
make compare CANDIDATES=mine,other             # placebo, mine and other on the same 64 seeds, about 6 s
make compare CANDIDATES=mine,maker             # or against a built-in in the same seat, with nothing to install
build/release/suite --candidates=maker,momentum   # built-ins only, from the binary that cannot see installed code
```

**Comparing two versions of the same strategy.** Install them under two names, for example
`mine.hpp` defining `Mine` and `mine_v2.hpp` defining `MineV2`. The name of a strategy is its file
name, so one binary cannot hold two versions under one name. Comparing two separate run directories
(`--compare=DIR,DIR`) is not built (`docs/REPRODUCE.md` says why); the fingerprint below is what would
make that comparison safe.

**`make compare` variables:**

- `CANDIDATES` (required)
- `BLOCK` (`standard` by default, or `smoke`)
- `OUT`

It builds `suite_lab` if needed and refuses an empty `CANDIDATES` with exit 2. The variable is called
`BLOCK` and not `SEEDS` because `bench/run_arena_evidence.sh` already reads a numeric `SEEDS` from the
environment, and an exported `SEEDS=24` would otherwise leak in.

**Flags of `suite` and `suite_lab`:**

| Flag | Meaning |
|---|---|
| `--candidates=A[,B,...]` | 1 to 8 distinct names. The first is the baseline of the paired rows. |
| `--block=standard\|smoke` | A named seed block. There is no free seed list. |
| `--out=DIR` | Defaults to `runs/<a>[-vs-<b>...]-<block>/`. A previous run directory (one that holds `suite.cfg`) is replaced. Anything else that exists and is not empty, or is not a directory, is refused. |
| `--help` | Also `-h`. Lists the built-in names and, in `suite_lab`, the installed ones. |

There is deliberately no `--set`, `--param`, `--seeds`, `--requests` or `--strategies`. Typing any of
them is an unknown flag, which exits 2. A different set of conditions is a different suite, defined in
code, and ad-hoc exploration stays with `sweep_lab --strategies=...`. Every session is always replayed;
there is no flag to turn that off. Eight candidates is enough for the table to stay readable, and it
keeps the multiple-comparison problem (see "Known weaknesses") small.

**Exit codes:**

| Code | Meaning |
|---|---|
| 0 | The run is complete, whatever the verdicts. |
| 1 | A candidate session gave a different hash on replay, or a candidate that sent no orders on a seed did not leave the placebo's market exactly as it was. |
| 2 | Usage or configuration error, decided before any session runs. It is also returned if the run directory cannot be written. |
| 3 | The suite itself is not admissible on this build: a placebo session failed the health gate or differed from the market with no seat, or cash, position or value failed to sum to zero in some session. |

When several apply, the precedence is 2, then 3, then 1, then 0.

## The standard suite

One definition, `standard_suite()` in `arena/suite.hpp`: suite `standard`, version 1. A change to any of
it, or to a reference strategy, a default, the session or the engine, moves the pinned fingerprint (see
"Tests"). That forces a deliberate version bump instead of a silent change of conditions.

### Line-up

The line-up is `maker,rival,informed,noise:2,momentum`, with every `RosterConfig` field at its default.
That is exactly the default `build_roster()` in `arena/strategies/registry.hpp`, which every published
number came from. The suite builds it through `roster_from_spec`, so the line-up is data that
`suite.cfg` records. Before building, it syncs `anchor` and `tick` from the session config, as
`Session::start` does (session.hpp:205-210).

A probe at `6ffdbc3` found that the spec and `build_roster(RosterConfig{})` give the same hash,
`2853d2127a3486c5`, under `test_arena`'s `small_session()`. `test_suite_reference_is_default` now holds
the two equal on every smoke seed under the suite's own settings.

The built-in `informed` is the only participant that sees the fundamental. The owner's instruction was
to start from the market already in the tree, so no parameter changes.

### The seat

The candidate occupies **index 6, owner 7, appended after the line-up**. The seat has three properties:

- Owner ids are index + 1 (session.hpp:262), and each strategy's reset seed is
  `seed + 0x9E37·(index + 1)` (session.hpp:268). Only an appended seat leaves every reference
  participant's owner id and random stream untouched.
- **The seat is never informed.** The runner refuses `informed` as a candidate and pushes
  `informed = false` for the seat. Installed strategies are never informed anyway: the manifest that
  `make strategy` generates pushes `informed = false` for each of them.
- **Its reaction delay is set by the suite, at 10,000 ns,** which is the reference maker's delay. The
  candidate's declared `reaction_ns` is recorded and not used. `SeatOverride` (see "Where it lives")
  wraps the candidate's handle and reports the suite's delay. The session reads the delay once, through
  the handle, at start (session.hpp:263), so nothing in the session changes.

Why fix the delay: in this project a reaction delay is an input the experimenter chooses (CLAUDE.md), not
something a strategy earns. If each candidate brought its own delay, speed would be mixed into every
comparison. The runner prints a seat value of −34.92 ± 3.09 for imbalance at the suite's 10 µs. A probe
at `6ffdbc3`, with the seat at imbalance's declared 20 µs on the same 64 seeds, measured −23.70 ± 2.91.
A candidate declaring 0 ns would act on every touch at the instant it saw it.

The delay is a `SuiteDef` field, so a "speed" scenario in step 3 is one more definition, not a flag.
Fixing the delay also bounds how often a candidate can look at the market when reacting to events. A
strategy has at most one outstanding event wake (strategy.hpp:34), so at 10 µs it gets at most one
event-driven decision per 10 µs.

The candidate's `timer_ns` is honoured, because a timer is part of a strategy's logic.

### Session length: simulated time, not requests

The session settings are the `SessionConfig` defaults, except for three fields:

| Field | Value | Default |
|---|---|---|
| `max_time_ns` | 25,000,000 | 0 |
| `max_requests` | 0 (no limit) | 200,000 |
| `stall_limit_ns` | 0 (no stall exit) | 100 ms |

`Session::check` allows `stall_limit_ns = 0` because `max_time_ns > 0` (session.hpp:198-202).

Everything else stays at its default:

- start price 100.00 and tick 0.01;
- fundamental step mean 100 µs and band 2,000 ticks;
- markout horizon 1 ms;
- fee 0;
- the arena's fixed `EngineConfig` (session.hpp:231-236).

**Why time and not requests.** `max_requests` counts every request, the candidate's own included. A
candidate that sends more orders therefore ends everyone's session earlier in simulated time, and its
arm is then marked at a different closing value V(T). The measurements:

- Under a 7,000-request bound, the closing V of the placebo and imbalance arms agreed on 5 of 64 seeds,
  and placebo, imbalance and maker all agreed on 1 of 64 (a probe at `6ffdbc3`).
- On the four smoke seeds, every one of the twelve (arm, seed) pairs for maker, noise and momentum
  disagreed with the placebo (a probe at `6ffdbc3`). That includes momentum, which sends a median of 17
  orders a session in the runner. `test_suite_common_conditions` re-measures this on every `make test`
  and requires at least one pair to disagree.
- Under the time bound, in the runner, the closing V and the number of fundamental steps were equal
  across the placebo and the five candidates imbalance, maker, momentum, noise and fader on 64 of 64
  standard seeds, and every one of those sessions ended on time. The probe at `6ffdbc3` found the same
  for a sixth candidate, imbalance with ratio 4, which the runner cannot express.

**Why there is no stall exit.** A stall could end two arms at different times for the same reason. With
a time bound, a quiet market is just an outcome that gets measured.

**The step ceiling.** The runner ends a session after 2,000,000 scheduler steps and marks it
`ended_by=ceiling`; that session fails the health gate. It is a bound on a runaway strategy, such as one
with a 1 ns timer; it is not a stopping rule. For comparison:

- a placebo-arm session takes 18,155 to 20,051 steps, and the busiest candidate arm measured, noise,
  takes at most 28,976;
- a probe at `6ffdbc3` that cancels and re-posts on a 2 µs timer took about 108,000 steps (82,000
  requests, about 80 ms of wall time).

The ceiling counts steps rather than requests, so it also bounds a strategy that wakes constantly
without ordering.

**Why 25 ms.** The reference market tracks the fundamental on every seed up to 30 ms, and first comes
loose somewhere between 30 and 40 ms. 25 ms keeps a margin below that edge, so a candidate that
degrades tracking somewhat still produces an admissible arm, and it still gives about a thousand trades
a session. "Gap" is each session's mean |mid − V| in ticks. The first row is the runner's own placebo
arm on the standard block. The rows between it and the last are the same placebo arm on other seed
ranges and at other lengths, which the runner deliberately cannot run; they came from probes at
`6ffdbc3`.

| Length | Seeds | Median gap | Worst session | Sessions over 4 ticks | One-sided, worst |
|---|---|---|---|---|---|
| 25 ms (the runner) | 42..105 (64) | 1.51 | 2.03 | 0 | 0.00% |
| 25 ms | 42..169 (128) | 1.53 | 2.16 | 0 | 0.00% |
| 25 ms | 1,000,000..1,000,063 | 1.49 | 1.83 | 0 | 0.00% |
| 30 ms | 42..105 | 1.53 | 2.58 | 0 | 0.00% |
| 40 ms | 42..105 | 1.51 | 5.27 | 1 | 0.00% |
| 50 ms | 42..105 | 1.53 | 7.46 | 2 | 1.03% |
| 100 ms | 42..105 | 1.51 | 26.77 | 9 | 1.69% |
| 500 ms | 42..65 (24) | 6.22 | 48.12 | 13 | 2.05% |
| published headline, 200k requests (about 0.7 s), 10 µs point | 42..65 | 10.64 | 67.79 | 16 | 0.6% (mean) |

The last row is `results/arena-wsl2/headline.txt`. The median gap barely moves with length; the tail
grows. The mechanism is the informed trader's position limit. In the probe at 500 ms it ended within
one 20-lot clip of its 5,000 limit on 13 of 24 seeds, and 12 of the 13 loose sessions are among them.
In the runner's placebo arm at 25 ms that happened on 2 of 64 seeds (59 and 105, both ending at exactly
5,000), and neither was loose: their gaps were 2.03 and 1.67 ticks. At 25 ms a candidate has about two
ticks of headroom under the gate before it fails.

A 25 ms placebo-arm session in the runner has:

- 232 to 270 fundamental steps, 250 on average;
- 6,912 to 7,906 requests;
- 1,027 to 1,462 trades;
- about 15 ms of wall time (384 sessions in 5.6 s).

The cost is the short horizon, recorded under "Known weaknesses".

### Seeds

The suite uses named blocks only:

| Block | Seeds | What it is for |
|---|---|---|
| `standard` | 42..105, 64 seeds | Base 42 is the repository's convention. |
| `smoke` | 42..45 | A prefix of `standard`, so smoke numbers are a subset of standard ones. It shows that a strategy runs and trades, and cannot separate anything. |

**Why 64 seeds.** Paired standard errors at 64 seeds:

- 1.0 to 1.8 currency units for small perturbations of one strategy (probes at `6ffdbc3`);
- 1.7 for a one-parameter change, imbalance ratio 3 against 4 (a probe at `6ffdbc3`);
- 3.26 to 17.02 for unrelated strategies, from the runner: momentum, maker and noise, each against
  imbalance.

At about 15 ms a session, 64 seeds is cheap, and 24 seeds would make every interval about 1.6 times
wider.

A holdout block is not built yet. A probe at `6ffdbc3` found seeds 1,000,000..1,000,063
admissible, with a worst placebo gap of 1.83 ticks at 25 ms; see "Later steps" below.

### Scenarios

This document describes the default market, `standard`. The scenario families `wide` and `busy` are
suites of their own, added with the report; `docs/REPORT.md` describes them, and the `null` family that
was built and dropped.

### The health gate

The gate is checked per session, in integers only, so a decision at the threshold cannot depend on
floating-point rounding on one platform. A session passes only if all of these hold:

- `gap_samples > 0`.
- `gap_sum_ticks ≤ 4 · gap_samples`. The mean |mid − V| is at most 4 ticks, which is one quoted maker
  spread.
- `100 · gap_unpriced ≤ 5 · (gap_samples + gap_unpriced)`. The book is one-sided at most 5% of the
  time.
- `book_full_rejects == 0`. Otherwise the candidate exhausted the arena's `EngineConfig`
  (session.hpp:231-236), and the session is an artefact of capacity.
- `wall_hits == 0`. Otherwise the fundamental stopped being a martingale.
- `ended_by == time`.
- The replay or no-seat check was identical (see the next section).

The gate never calls `SessionStats::mean_gap_ticks()` (session.hpp:121), which returns a double.

**Admissibility.** An arm is admissible only if it passes the gate on every seed of the block. A
comparison is admissible only if both of its arms are. Failing seeds are never dropped from the
statistics: dropping them would condition on the outcome. A failure changes the verdict instead, and the
numbers are still written.

The gate does bite on a plausible candidate. With a third noise trader in the seat
(`suite --candidates=noise`), the book came loose on 2 of 64 standard seeds (seed 79: 8.01 ticks; seed
105: 5.75, from `sessions.csv`), so the runner reports `noise` as not admissible.

### Arms and checks per seed

For each seed, in this order, each run in a fresh `Session`:

1. **The placebo arm**: the line-up plus `placebo` in the seat.
2. **The no-seat check**: the line-up alone. Its hash must equal the placebo arm's. That one comparison
   proves both that the reference market replays and that the seat is inert on this build.
3. **For each candidate in the order named**: the arm, then a replay built from fresh instances. The
   replay must give the same hash. If the candidate sent no orders on that seed, its arm must also hash
   equal to the placebo arm; otherwise the check fails, exactly as a replay difference does.

Rows are written for the arms. The checks appear as columns on those rows.

A standard run with two candidates is 64 × 6 = 384 sessions; the `imbalance,maker` run took 5.6 s on
Linux.

## Identical conditions

Two different candidates cannot produce identical sessions. Their orders change the engine's events, so
the other participants' fills, wakes and random draws diverge after the candidate's first order. That
divergence is the thing being measured. So "identical conditions" means **identical exogenous inputs
and an identical seat**, and each part has a stated mechanism:

| Held exactly common, across every arm on a seed | Why it holds |
|---|---|
| The fundamental path V(t) and the closing mark V(T) | `FairValue` is seeded from the session seed alone (session.hpp:248). It draws only from its own random stream (fair_value.hpp:86, 93-108) and reschedules itself whatever the market does (session.hpp:460). The time bound makes V(T) common as well (64 of 64 seeds in the runner). |
| Session length | `max_time_ns`, with no request bound and no stall exit |
| The opposing line-up: who is in it, their parameters, owner ids and random streams | The seat is appended, so indices 0-5 are unchanged (session.hpp:262, 268) |
| The exogenous schedule: timer phases, the informed signal schedule, opening looks | Timers re-arm at now + timer (session.hpp:500-506). Signal wakes depend only on fundamental steps and the informed trader's own delay (467-471). |
| The seat: index, owner id, reset seed, reaction delay, information | `SeatOverride`, and `informed = false` for the seat. The reset seed is the same for every candidate, so two randomised candidates draw from the same stream. |
| The venue: engine capacity, tick, start price | The `EngineConfig` fixed in `Session::start` |

Value is `cash + position · V(T)` (accounting.hpp:84-86). The dominant noise term, inventory marked at
a random-walk price, is therefore shared across arms, and pairing by seed cancels it. The candidate's own
market impact is not common to the arms, because it is the treatment.

### The seat and wake ordering

The worry is that adding a participant changes wake ordering, and so moves the session hash and
everyone's numbers. That is true for a participant that **acts**: its orders are the treatment. It is
also true for a participant **inserted** before others, because every later participant's owner id and
reset seed shift. It is **false for an inert participant appended last**, and the runner rests on that.

Why it is false:

- The scheduler orders items by `(at, seq)` (clock.hpp:66-73), and `seq` is assigned when an item is
  pushed (clock.hpp:89-95).
- An inert seat's wakes take sequence numbers, but its processing pushes nothing for anyone else. Its
  only pushes are its own timer re-arm. It submits nothing (session.hpp:538-540), and it touches only
  its own mirror, feed and wake count.
- So, by induction over the items processed, the other participants' items come out in the same
  relative order, the engine sees the same requests, and the hash is the same. The hash covers only
  events and their arrival times (session.hpp:375-380).

Measured:

- Under `small_session()` at seed 42, the reference alone gives `2853d2127a3486c5`. An appended placebo
  also gives `2853d2127a3486c5`, and so do a placebo with reaction 0 and an inert strategy on a 7 µs
  timer; `test_placebo_is_inert` asserts all three on every `make test`. In the probe at `6ffdbc3`, five
  appended placebos also gave it, the placebo inserted at index 3 gave `5a9a688a7ca5b9f8`, and at index
  0 it gave `acc8b70cc0ea7b48`. The test asserts that the index-3 hash differs.
- Under the suite's settings, the runner checks the hash on every seed of every run. The
  `imbalance,maker` run printed "placebo identical to the no-seat market on 64/64 seeds". The probe at
  `6ffdbc3` also compared every `SessionStats` field, every reference account and every reference wake
  count on those 64 seeds and found them equal, and `test_placebo_is_inert` holds the accounts and wake
  counts equal under `small_session()`. The placebo itself was woken 629 times at seed 42.
- At seed 42 the placebo arm and the no-seat market both give `5625d5cc916761b0` in `sessions.csv`. The
  evidence binary prints the same hash for
  `sweep --values=10000 --seeds=1 --set=session.max_time_ns=25000000 --set=session.max_requests=0 --set=session.stall_limit_ns=0`,
  and `test-cli` checks that equality.

This corrects the premise of the earlier lab plan (`git show f6db04d:docs/LAB_PLAN.md`): "adding a
participant adds wake events … so a bare with/without pairing would conflate the [strategy]'s market
impact with a re-interleaved session." For an appended seat there is no re-interleaving artefact to
separate from the candidate's own orders. **Everything that differs between a candidate arm and the
placebo arm is caused by the candidate's orders.**

### Why a placebo arm at all

Equalising roster size or wake order would be the obvious reason, and it does not apply, as shown
above. The placebo arm is still run on every seed, for five reasons:

1. **It proves the premise on every run.** It is checked against the no-seat market on every seed, and
   a difference exits 3. A future change to the scheduler, to wake marking or to how seeds are derived
   would show up there instead of silently in every comparison.
2. **It is each reference participant's counterfactual on that seed.** The market is closed and fees
   are 0, so seat value = −Σᵢ (value of reference participant i in the candidate arm − its value in the
   placebo arm). That attribution is exact and needs no model; `test_suite_closed_identity` holds it on
   every smoke arm. The report prints it as "where the seat's money came from".
3. **It is the health baseline,** and the source of the suite's own admissibility.
4. **It is the source of the fingerprint** (below).
5. **It identifies a candidate that never acts.** If the seat sent no orders, the arm must hash equal to
   the placebo's, and the runner checks that. The arm is reported as inert on k seeds, not as zero
   edge. The built-in `fader` is inert on all 64 standard seeds (`suite --candidates=fader`): it trades
   only when the last print is 6 ticks from its slow average, and a market this tight does not produce
   that in 25 ms.

It also keeps every arm at seven participants, so the rows line up by index.

### What the placebo arm does not control for

- **Skill.** The placebo's seat value is exactly 0, so "against the placebo" means "against doing
  nothing". A skill baseline is step 4.
- **Seat position.** Within any one push loop the seat comes last (session.hpp:281-291, 635-643), so in
  a same-instant tie with a reference participant woken by the same event it acts afterwards. That is
  common to every candidate, which makes it fair between candidates but not between a candidate and the
  reference maker. In a probe at `6ffdbc3` over 64 seeds, moving the seat to 9,999 ns, so that it acts
  before those participants instead of after them, against 10,000 ns gave:
  - −0.72 ± 1.57 for imbalance;
  - −0.06 ± 0.99 for a maker in the seat.

  Both are unresolvable.
- **Divergence after the first order.** The common random numbers are exact for the fundamental and
  the schedule, and only partial for endogenous flow. Noise traders draw two or three numbers per timer
  wake depending on their state (noise.hpp:49-75), so their streams desynchronise once the candidate
  has traded. The share of variance that pairing removes, 1 − var(d) / (var(a) + var(b)), varies a lot.
  The last three rows are the runner's own, from `suite_lab --candidates=imbalance,maker,momentum,noise`.
  The first came from a probe at `6ffdbc3`, because the runner cannot change a strategy's parameter.

  | Comparison, 64 seeds | Paired difference | Pairing removed |
  |---|---|---|
  | imbalance ratio 4 − ratio 3 | +10.71 ± 1.70 | 85% |
  | maker − imbalance | +106.04 ± 6.77 | 23% |
  | momentum − imbalance | +32.56 ± 3.26 | 3% |
  | noise − imbalance | −160.25 ± 17.02 | −31% (the arms are negatively correlated) |

  The runner prints this figure for every comparison between two candidates because it varies this
  much. It marks the noise row `NOT ADMISSIBLE`, because noise in the seat fails the gate on two seeds;
  the arithmetic is still written to `comparison.csv`.
- **Overfitting to the block.** The seeds are public.

### Across runs: the fingerprint

`outcome.cfg` records a `fingerprint`: FNV-1a 64 over the conditions lines of `suite.cfg`, then one
line `seed=<s> placebo=<hash>` per seed, in block order. The conditions lines are those whose keys
start with `suite.`, `seat.`, `health.`, `block.`, `market.`, `session.` or `fair.`.

Roster parameters are not hashed as text, for two reasons:

- the reference participants' parameters reach the market, so they are already in the placebo hashes;
- the parameters of strategies outside the line-up describe built-in candidates, not conditions.

The fingerprint therefore moves if anything about what the candidates faced moves: the engine, the
session, a reference strategy, a default, the suite's settings or the block. It does not move when
someone adds an unrelated `RosterConfig` field. Two results are comparable only if their fingerprints
are equal. On this tree the standard block's fingerprint is `9e8d07b24ee5d562`, the same in every
standard run above, and the smoke block's is `d461c30ab2446acf`, which `test_arena` pins. `test-lab`
asserts that `suite` and `suite_lab` produce the same fingerprint, so the lab binary's reference market
is the evidence binary's.

### Determinism of the comparison itself

- Every candidate session is replayed. That also catches a strategy whose `static` state changes the
  second run.
- The raw files contain only integers and identifiers, and are opened `"wb"`, so they are LF on Windows
  too. `sweep` opens its CSVs with `"w"` (sweep.cpp:380, 542), which writes CRLF there.
- Re-running the same command on the same build produces byte-identical files.
- Arm results do not depend on which other candidates are named, or in what order. The placebo,
  imbalance and maker rows of the `imbalance,maker` run are byte-identical to the same arms' rows in the
  `imbalance,maker,momentum,noise` run, and the placebo and momentum rows from `suite` equal those from
  `suite_lab`.

## Outputs

Each run writes one directory: `runs/<a>[-vs-<b>...]-<block>/` by default, or `--out=DIR`. `runs/` is
git-ignored, and nothing is ever written under `results/`.

**Replacement rule.** If the directory holds a `suite.cfg`, the runner deletes exactly the six files
named below and writes them again. It never deletes anything else. If the directory exists, is not empty
and holds no `suite.cfg`, the runner refuses with exit 2.

**Write order.** `suite.cfg` is written first, before any session runs. `outcome.cfg` is deleted at the
start and written last. A directory without `outcome.cfg` saying `status=complete` is incomplete, and
every reader must refuse it.

The **raw** files contain integers and identifiers only. They are byte-identical on a re-run. For
built-in candidates they are expected to be byte-identical across operating systems too, as the
committed arena evidence is; that has not been checked yet, because the Windows build has not been run
(see "Known weaknesses"). The **derived** files contain doubles, are computed only from the raw files'
contents, and are byte-identical on a re-run on the same build.

### 1. `suite.cfg` (raw)

`key=value` lines in a fixed order, with `#` comments, in three groups. This is the file from the
`imbalance,maker` standard run.

**Conditions.** These `key=value` lines are fingerprinted; the comments above them are not.

```
# The conditions of one standard-suite run. Simulated, not measured.
# Conditions: fingerprinted, with every placebo session hash (outcome.cfg).
suite.name=standard
suite.version=1
suite.lineup=maker,rival,informed,noise:2,momentum
suite.session_ns=25000000
suite.step_ceiling=2000000
seat.index=6
seat.owner=7
seat.reaction_ns=10000
seat.informed=0
health.max_mean_gap_ticks=4
health.max_one_sided_pct=5
health.book_full_rejects=0
health.wall_hits=0
block.name=standard
block.first_seed=42
block.seeds=64
market.tick=100
market.start_price=1000000
session.max_requests=0
session.max_time_ns=25000000
session.stall_limit_ns=0
session.markout_horizon_ns=1000000
session.fee_per_qty=0
fair.step_mean_ns=100000
fair.band_ticks=2000
```

The `session.*` and `fair.*` lines are every name in `Session::kSessionParamNames` except
`session.seed`, in that list's order, read through `Session::get_param`.

**Roster.** All 28 names in `kRosterParamNames`, in that order, read through `get_roster_param`, with
booleans as 0 or 1:

```
# Every reference participant's parameters, under the names --set= takes.
maker.reaction_ns=10000
maker.half_spread_ticks=2
maker.quote_qty=50
maker.max_position=2000
maker.skew_ticks_at_limit=40
maker.impact_centiticks=15
rival.enabled=1
rival.reaction_ns=1000
rival.half_spread_ticks=2
rival.quote_qty=50
rival.impact_centiticks=-1
informed.enabled=1
informed.reaction_ns=1000
informed.edge_ticks=1
informed.qty=20
informed.max_position=5000
noise.count=2
noise.reaction_ns=50000
noise.timer_ns=10000
noise.qty=25
noise.pct_market=25
momentum.enabled=1
momentum.reaction_ns=20000
momentum.qty=20
fader.enabled=0
fader.reaction_ns=30000
fader.qty=15
fader.trigger_ticks=6
```

The keys are the `--set=` names, so step 5 can read the file back.

**Run.** What this invocation ran. These lines are not conditions.

```
# What this invocation ran. Not conditions.
run.binary=suite_lab
run.user_strategies=imbalance
run.candidates=imbalance,maker
run.candidate.imbalance.origin=installed
run.candidate.imbalance.declared_reaction_ns=20000
run.candidate.imbalance.timer_ns=0
run.candidate.maker.origin=builtin
run.candidate.maker.declared_reaction_ns=10000
run.candidate.maker.timer_ns=50000
```

### 2. `sessions.csv` (raw)

One row per (seed, arm), in seed-major order: the placebo first, then the candidates as named. The
standard block with two candidates gives 192 rows. Columns:

```
suite,version,block,seed,arm,origin,seat,seat_reaction_ns,declared_reaction_ns,timer_ns,
ended_by,steps,requests,events,trades,volume,rejects,self_trade_cancels,book_full_rejects,
action_drops,sink_growths,fair_steps,wall_hits,elapsed_ns,v_final,
gap_samples,gap_sum_ticks,gap_max_ticks,gap_unpriced,
cash_sum,position_sum,value_sum,
seat_orders,seat_fills,seat_value,inert,healthy,health_reason,
session_hash,event_hash,event_count,check_kind,check_hash,check_ok
```

What the less obvious columns hold:

- `origin` is `reference` (the placebo), `builtin` or `installed`.
- `ended_by` is `time` or `ceiling`.
- `v_final` is V(T) in price units.
- `cash_sum`, `position_sum` and `value_sum` are the closed-world identity (accounting.hpp:159-173).
  They must be 0, and a non-zero value exits 3.
- `seat_value` is in price units, marked at `v_final`.
- `inert` is 1 when `seat_orders == 0`.
- `health_reason` is `ok`, `no_samples`, `gap`, `one_sided`, `book_full`, `wall`, `ceiling` or `check`.
- The three hashes are written as 16 hex digits.
- `check_kind` is `no_seat` for the placebo and `replay` for candidates.
- `check_ok` is 1 or 0.

The gap is stored as a sum and a count, never as a mean.

### 3. `participants.csv` (raw)

Seven rows per (seed, arm), one per participant. Columns:

```
suite,version,block,seed,arm,index,owner,role,name,informed,reaction_ns,timer_ns,wakes,
orders_sent,rejects,self_trade_cancels,fills,maker_fills,taker_fills,filled_qty,
position,cash,fees,value,edge_sum,spread_sum,adverse_sum,markout_n,adverse_fills,unresolved_fills
```

- `role` is `reference` or `seat`.
- `reaction_ns` is the effective delay.
- `value = cash + position · v_final − fees`, in price units.

The other columns are every field of `Account` (accounting.hpp:64-82), stored as raw sums and not as
per-fill means. That lets step 3 pool per-fill statistics correctly across seeds.

### 4. `outcome.cfg` (raw; written last)

From the `imbalance,maker` standard run:

```
status=complete
fingerprint=9e8d07b24ee5d562
sessions_run=384
rows=192
checks=identical
closed_identity=1
reference_admissible=1
arm.placebo.admissible=1
arm.placebo.failing_seeds=-
arm.placebo.inert_seeds=64
arm.placebo.check_failures=0
arm.imbalance.admissible=1
arm.imbalance.failing_seeds=-
arm.imbalance.inert_seeds=0
arm.imbalance.check_failures=0
arm.maker.admissible=1
arm.maker.failing_seeds=-
arm.maker.inert_seeds=0
arm.maker.check_failures=0
exit=0
```

For an arm that fails the gate the lines read, from `suite --candidates=noise`:

```
arm.noise.admissible=0
arm.noise.failing_seeds=79:gap,105:gap
```

`closed_identity` is 0 when cash, position or value failed to sum to zero in some session, which exits
3. `check_failures` counts an arm's sessions whose replay or no-seat check differed.

### 5. `comparison.csv` (derived)

One row per candidate against the placebo, and one per later candidate against the first. Values are
in currency units per 25 ms session. Columns:

```
baseline,arm,seeds,mean_diff,se_diff,t_crit,ci95_lo,ci95_hi,resolvable,median_diff,
min_diff,max_diff,range_straddles_zero,arm_ahead_seeds,var_removed_pct,verdict
```

- `resolvable` is 2.8 × `se_diff`, the smallest effect this many seeds could resolve, defined exactly
  as `bench/summarize_arena.py` defines it.
- `var_removed_pct` has one decimal (22.5 for maker against imbalance, which the report rounds to 23%),
  and is empty for rows against the placebo, which has zero variance.
- `verdict` takes one of four values:
  - `not_admissible` when either arm failed the gate on any seed;
  - `identical` when every per-seed difference is exactly 0;
  - `separated` when the 95% interval excludes 0;
  - `not_separated` otherwise.

The statistics are always computed over every seed of the block.

### 6. `report.txt` (derived)

The printed report: everything on stdout except the closing `wrote DIR` line. It holds no wall time and
no path, so it is byte-identical across directories. Progress and wall time go to stderr.

## What is printed

This is the stdout of `build/release/suite_lab --candidates=imbalance,maker`, which is what
`make compare CANDIDATES=imbalance,maker` runs, verbatim: `report.txt`, then the `wrote` line. Two
progress lines, the number of sessions and the wall time, go to stderr before it.

```
arena suite - simulated, not measured. The engine's latency is in README.md.
  suite     standard v1, block standard: 64 seeds (42..105), 25,000,000 ns of simulated time each
  market    maker,rival,informed,noise:2,momentum, every parameter at its default
  seat      #6 (owner 7), appended last, never informed; reaction 10,000 ns, set by the suite
            declared: imbalance 20,000 ns, maker 10,000 ns; all ran at 10,000
  arms      placebo (the empty seat), imbalance (installed), maker (built in)
  checks    every candidate session run twice; every placebo session against the market with no seat
  fingerprint 9e8d07b24ee5d562

market health: read this first. A session that fails it is not a market, and an arm with one is not compared
  arm            | |mid - V| ticks: median (worst) | one-sided, worst | healthy | inert
  --------------------------------------------------------------------------------------------
  placebo        |   1.51 (2.03)                   |   0.00%          | 64/64   | -
  imbalance      |   1.53 (1.88)                   |   0.00%          | 64/64   | 0/64
  maker          |   1.44 (2.94)                   |   0.00%          | 64/64   | 0/64

the seat: currency units per session, 64 seeds; fills, orders and rejects are medians
  arm            | mean +- s.e.      | median    | range                 |  fills | orders | rejects
  ------------------------------------------------------------------------------------------------
  placebo        |    +0.00 +-  0.00 |     +0.00 |     +0.00 ..    +0.00 |      0 |      0 |       0
  imbalance      |   -34.92 +-  3.09 |    -38.00 |    -83.95 ..   +21.15 |    310 |    270 |       0
  maker          |   +71.13 +-  7.05 |    +88.15 |   -209.10 ..  +148.75 |    323 |    864 |      51

paired, seed by seed on a shared fundamental: the statistic to read
  arm vs baseline          | mean +- s.e.       | 95% interval         | resolvable | range                 | ahead | pairing | verdict
  --------------------------------------------------------------------------------------------------------------------------------------------
  imbalance vs placebo     |   -34.92 +-   3.09 |   -41.09 ..   -28.74 | +-    8.65 |    -83.95 ..   +21.15 |  5/64 |     -   | separated
  maker vs placebo         |   +71.13 +-   7.05 |   +57.05 ..   +85.20 | +-   19.73 |   -209.10 ..  +148.75 | 59/64 |     -   | separated
  maker vs imbalance       |  +106.04 +-   6.77 |   +92.51 ..  +119.57 | +-   18.96 |   -127.95 ..  +169.75 | 61/64 |    23%  | separated
  The 95% interval uses t with 63 degrees of freedom. "resolvable" is the smallest difference
  64 seeds could resolve (2.8 x s.e., as bench/summarize_arena.py). "pairing" is the share of
  variance that pairing by seed removed: high for related strategies, low for unrelated ones.

  checks: every candidate session replayed identically; placebo identical to the no-seat market on 64/64 seeds
  imbalance is installed: replay is verified on this build; cross-platform replay is not claimed for it
wrote runs/imbalance-vs-maker-standard/  suite.cfg sessions.csv participants.csv comparison.csv report.txt outcome.cfg
```

Variant lines. Each was taken from a run, except the last two, which no strategy in the tree triggers;
their format is the one `arena/suite_report.hpp` prints, with placeholders in angle brackets.

- **An arm fails the gate** (`suite --candidates=noise`). The health table reads
  `noise          |   1.54 (8.01)                   |   0.00%          | 62/64   | 0/64`, the verdict
  column reads `NOT ADMISSIBLE`, and after the paired table comes
  `NOT ADMISSIBLE: noise failed on 2/64 seeds (79: gap, 105: gap); its numbers are in comparison.csv and do not count`.
  The gap sizes are in `sessions.csv`, not in the report.
- **A candidate never acts** (`suite --candidates=fader`). The health table shows the arm inert on
  `64/64`, the verdict column reads `identical`, and under the paired table comes
  `fader vs placebo: identical on every seed; it sent no orders, so its sessions are the placebo's`.
- **The smoke block** (`suite --candidates=maker,momentum --block=smoke`), printed for any block under
  16 seeds:
  `smoke block: 4 seeds show that a strategy runs; they cannot separate anything. Report the standard block.`
- **A candidate fails replay** (exit 1). The checks line reads
  `checks: a candidate session was DIFFERENT on replay; placebo identical to the no-seat market on <k>/<n> seeds`,
  followed by
  `<name>: DIFFERENT on replay at seeds <s>, <s>: state carried between sessions? not compared`.
  A failed check also fails the gate with reason `check`, so the arm is reported `NOT ADMISSIBLE` as
  well.
- **The reference fails** (exit 3). The whole report is printed, every comparison against the placebo
  reads `NOT ADMISSIBLE`, and after the paired table comes
  `THE REFERENCE MARKET IS NOT ADMISSIBLE on this build (<seed>: <reason>, ...): nothing is compared`.
  The files are still written in full. A broken closed identity also exits 3, but the report has no
  line for it; `outcome.cfg` says `closed_identity=0`.

## Where it lives

The engine (`include/`, `src/`, `sim/`) is not touched: `git diff 6ffdbc3 e504824` is empty for those
directories, and also for `results/`, `bench/`, `arena/sweep.cpp`, `arena/arena.cpp`,
`arena/ui.hpp`, `arena/term.hpp`, `arena/clock.hpp`, `arena/accounting.hpp`, `arena/fair_value.hpp`,
`arena/book_mirror.hpp` and every existing strategy header.

- **`arena/strategies/placebo.hpp`.** `struct Placebo`, the empty seat: no timer, `reset` does nothing,
  and `on_event` returns an empty batch. Its delay is irrelevant, since it is inert at any value. It is
  written against `arena/strategy.hpp` alone, in the `fader.hpp` style, and its header comment records
  that it is invisible only when appended last, with the hashes.
- **`arena/strategies/registry.hpp`.** `placebo` is addable by name in `add_named_strategy` and listed in
  `kStrategyNames`, so the Makefile's `BUILTIN_STRATEGIES` reserves the name for installed strategies.
  It is in no default line-up. `get_roster_param` is the read-side counterpart of `set_roster_param`,
  so `suite.cfg` can record every roster field under its `--set=` name. `build_roster` is unchanged.
- **`arena/session.hpp`.** Only `Session::get_param`, the counterpart of `set_param` over the same eight
  keys. No behaviour changes.
- **`arena/suite.hpp`** (header-only, integer-only). The conditions and the runs, reporting nothing:
  the seed blocks (`kSeedBlocks`, `find_block`); the gate (`HealthGate`, `judge`); `SuiteDef` and
  `standard_suite()`; the session and roster configs; `SeatOverride`; candidate checks
  (`check_candidates`, `is_builtin`, `Origin`); `build_arm_roster`, by name or, for test fixtures, from a
  ready-made handle; `run_arm`, which owns the step loop and the ceiling rather than calling
  `Session::run()`; the records (`SessionRow`, `ParticipantRow`, `ArmRecord`, `RunInfo`, `RunRecord`);
  admissibility (`summarize_arms`, `outcome_of`); and the text writers for the three raw files and the
  fingerprint. Each writer builds a `std::string`, so tests need no files.
- **`arena/suite_report.hpp`.** The only floating point in the feature, run after every session has
  finished, the same split `sweep.cpp` draws. Every function takes a `RunRecord` and nothing else, so
  step 3 can feed it one read back from disk: `t975` (a table for 1 to 30, 63 and 127 degrees of
  freedom), `paired`, `compare`, `comparison_csv` and `report_text`.
- **`arena/suite.cpp`.** The command line, compiled twice: into `suite` from `$(ROBJ)` without the
  define, and into `suite_lab` from `$(LOBJ)` with `LAB_FLAGS`. It parses the flags, validates the
  candidates and builds every arm's roster before anything runs, so a typo costs no time, applies the
  replacement rule with `std::filesystem`, runs the seeds in order, opens every file `"wb"`, prints the
  report and the `wrote` line, and writes `outcome.cfg` last. Like `sweep`, its first printed line says
  the numbers are simulated, not measured. Wall time goes to stderr only.
- **`Makefile`.** `suite` is in `ARENA_BINS` and `suite_lab` in `LAB_BINS`. `make compare` is the new
  target. The `strategy` target's closing hints are now three lines (compare it, explore it, watch it),
  and the explore and watch lines include `momentum`, so they use the published market; before, they
  left it out. `test-cli` and `test-lab` gain the suite checks under "Tests".
- **`.gitignore`.** `runs/`. Run directories belong to whoever ran them, as
  `arena/strategies/user/*.hpp` do, and they never go into `results/`, which stays the published
  evidence.
- **`tests/test_arena.cpp`.** The fifteen tests below, `kPinnedSmokeFingerprint`, and
  `test_roster_from_spec` now counts 7 named participants.

## Tests

### `test_arena`

These run in the debug build. A probe at `6ffdbc3` timed a 25 ms session there at about 23 ms, so the
additions cost a few seconds.

| Test | What it checks | What it proves |
|---|---|---|
| `test_placebo_is_inert` | (a) `placebo` is addable by name, has `timer_ns() == 0`, is not informed, and returns an empty batch for a default view. (b) Under `small_session()`, the reference alone hashes to `2853d2127a3486c5`, and the reference plus an appended placebo has the same `hash()`, `event_hash()` and `event_count()`. Accounts 0-5 and `wakes_of(0..5)` are equal field by field, the placebo's account is all zero, and `wakes_of(6) > 0`. (c) The same hash with the placebo's reaction overridden to 0, and with a test-local inert strategy on a 7 µs timer, which wakes more often than the placebo. (d) The placebo inserted at index 3 gives a different hash. | The seat is inert only when last, and the check can fail. |
| `test_suite_reference_is_default` | `suite_session_config` has `max_requests == 0`, `stall_limit_ns == 0` and `max_time_ns == 25'000'000`, and `Session::check` passes. On the smoke seeds, `roster_from_spec(standard lineup)` and `Session::start(sc, RosterConfig{})` give equal hashes, and `run_arm` of the line-up with no seat equals the latter, ends on time and has 6 participants. | The suite's market is the published line-up under the suite's length, and driving `step()` with a ceiling is `run()`. |
| `test_suite_common_conditions` | On the smoke seeds, the arms maker, noise and momentum have the placebo's `v_final` and `fair_steps`, end on time, and stay below the ceiling. Under `max_requests = 7'000` with the default stall limit and no time bound, at least one of the twelve (arm, seed) pairs has a different closing value; the test prints how many. | The time bound is load-bearing. |
| `test_suite_health_gate` | Synthetic `SessionStats` at each boundary. `gap_sum == 4 · samples` passes and one more fails. Unpriced at exactly 5% passes and one more fails. `book_full_rejects = 1`, `wall_hits = 1`, `gap_samples = 0` and `EndedBy::Ceiling` each fail with their own reason. | Admissibility is an exact integer decision. |
| `test_suite_reference_admissible` | Every placebo arm on the smoke block passes the gate, is inert and has seat value 0. | The gate is calibrated against the reference market. The full standard block is checked in `test-cli`, in release. |
| `test_suite_seat_rules` | `check_candidates` refuses an empty list, `informed`, `placebo`, an unknown name, a duplicate, an empty name and nine names, and the message names the cause for `informed`, `placebo`, the unknown name and the duplicate. It accepts five built-ins. A maker seat is built-in, sits at index 6 of 7, and is not informed. `fader` declares 30,000 ns but runs at 10,000, in both its session row and its participant row. A test-local strategy that declares 30,000 ns sees `now_ns − as_of_ns == 10'000` on every one of more than 100 wakes. | Speed and information are set by the suite, not the candidate. |
| `test_suite_arm_determinism` | `run_arm(seed 42, maker)` twice gives byte-identical rows. A noise arm run in between leaves the maker arm unchanged, and the noise arm is the same both times. | Arms are independent of each other and replay. |
| `test_suite_inert_arm` | A test-local strategy with a timer that never orders is seated through the fixture overload. On every smoke seed it is marked inert, its hash equals the placebo arm's, and it wakes more often than the placebo. | Inert detection, and the hash check behind it. |
| `test_suite_ceiling` | A test-local strategy on a 1 µs timer that posts a bid and cancels it on every wake, under a `SuiteDef` copy with `step_ceiling = 5'000`. It ends with `ended_by == Ceiling`, health `Ceiling`, exactly 5,000 steps, more than 100 orders and `value_sum == 0`. | A runaway candidate is flagged, not compared. |
| `test_suite_closed_identity` | In the placebo, maker and momentum arms on every smoke seed, `cash_sum`, `position_sum` and `value_sum` are 0, and seat value == −Σ (reference value in the arm − reference value in the placebo arm). | The attribution step 3 will print is exact. |
| `test_suite_param_dump` | Every name in `kRosterParamNames` and `kSessionParamNames` is gettable, and `get(set(x)) == x`, with each boolean toggled. Unknown names are refused. `suite_cfg_text` contains every name except `session.seed` exactly once. Applying its `key=value` lines through `set_roster_param` and `Session::set_param` to fresh configs reproduces the roster lines byte for byte, and `session.max_time_ns` reads back as the suite's 25,000,000. | `suite.cfg` is complete and can be replayed (step 5), and cannot drift from the setters. |
| `test_suite_fingerprint` | The smoke fingerprint equals `kPinnedSmokeFingerprint`, `d461c30ab2446acf`. A `SuiteDef` copy with `seat_reaction_ns = 10'001` gives a different fingerprint although every placebo hash is unchanged. A copy with `roster.maker_impact_centiticks = 16` moves the placebo hashes and so the fingerprint. | A change to the engine, the session, a reference strategy or the suite's settings fails here and forces a version bump. |
| `test_paired_stats` | `a = {1,2,3,4}` and `b = {2,4,6,8}` (so `d = {1,2,3,4}`) give mean 2.5, sd 1.2910, s.e. 0.6455, `t975(3) = 3.182`, interval 0.446..4.554, median 2.5, min 1, max 4, ahead 4, and pairing removed 80%. Constant differences give s.e. 0 without dividing by zero, all-zero differences give `identical`, and `t975` covers every block. | The reporting arithmetic. |
| `test_suite_compare` | A synthetic 8-seed `RunRecord` with four candidates. An A/A pair is `identical`. A pair whose interval excludes 0 but whose range straddles zero is `separated`, with `straddles` set. An arm that fails one seed is `not_admissible`, against the first candidate and against the placebo, and its statistics still cover all 8 seeds. The exit code is 0. | Failing seeds are never dropped, and both tests are reported. |
| `test_suite_rows` | Each header's column count equals each row's. No row or header contains `.`, and the text has no `\r`. A maker arm writes 7 participant rows. | The raw files are integer-only and LF. |

### `make test-cli`

The existing 11 sweep checks are unchanged. After them it prints
`17/17 suite command-line checks passed`, and the suite checks cover:

- `suite --help` succeeds.
- Each of these exits 2: no `--candidates`; `--candidates=nonsense`; `--candidates=informed`;
  `--candidates=placebo`; `--candidates=maker,maker`; `--block=bogus`; `--set=maker.quote_qty=1`.
- `--out` naming a non-empty directory with no `suite.cfg` exits 2, and the directory's contents are
  left untouched.
- `suite --block=smoke --candidates=maker,momentum --out=$(BUILD)/cli/suite-a` exits 0 and writes all
  six files; `sessions.csv` and `participants.csv` start with their headers, and `outcome.cfg` holds
  `status=complete`.
- The same command into `suite-b` writes six files byte-identical to `suite-a`'s.
- Re-running into `suite-a` replaces it, and it still matches `suite-b`.
- **The reference arm is the evidence binary's market.** The placebo arm's `session_hash` at seed 42,
  looked up by column name in `sessions.csv`, equals the hash printed by
  `sweep --values=10000 --seeds=1 --set=session.max_time_ns=25000000 --set=session.max_requests=0 --set=session.stall_limit_ns=0 --quiet`.
  On this tree that hash is `5625d5cc916761b0`.
- **The seat is inert through the evidence binary.**
  `sweep --values=10000 --seeds=1 --requests=2000 --strategies=maker,rival,informed,noise:2,momentum,placebo --quiet`
  prints the same hash as the same command without `,placebo`. On this tree both print
  `e2207fdb3bd88bb2`.
- **The reference market is admissible on the whole standard block.**
  `suite --candidates=momentum --out=$(BUILD)/cli/suite-std` exits 0, not 3, and `outcome.cfg` holds
  `reference_admissible=1`. That is 256 sessions, which took 3.5 s.
- **The integer-only header stays integer-only.** `! grep -nwE 'double|float' arena/suite.hpp`.

### `make test-lab`

The existing 7 strategy-install checks are unchanged. After them it prints
`4/4 suite checks passed`:

1. `suite_lab --block=smoke --candidates=imbalance,maker --out=$(BUILD)/lab/suite` exits 0,
   `report.txt` contains `maker vs imbalance`, and every imbalance row in `sessions.csv` has
   `origin=installed`.
2. That run's `fingerprint` equals the one from `suite --block=smoke --candidates=maker`. The lab
   binary's reference market is the evidence binary's.
3. `suite --candidates=imbalance` exits 2. The evidence-side binary still cannot see an installed
   strategy.
4. `make compare CANDIDATES=imbalance BLOCK=smoke OUT=$(BUILD)/lab/compare` exits 0.

### Held unchanged

Recorded when the runner was committed at `e504824`:

- `make test` passes, with `test_determinism` at `77a560d8ffb4fa43`, `test_arena`'s session hash at
  `2853d2127a3486c5` and the ui frame at `ddfa48c44bff4de8`.
- `git diff --stat 6ffdbc3 e504824 -- include src sim` is empty.
- `objdump -d build/debug/obj/src/matching_engine.o | grep -c memmove` is 0.

Planned, and not part of the evidence recorded for `e504824`:

- `sweep --seeds=8 --requests=100000 --replay-check` printing the same hashes before and after.
- `BUILD=... bash bench/run_arena_evidence.sh check`, run before and after, writing the same
  `results/arena-check/` both times except `about.txt`.
- The WSL ASan/UBSan `make test`, which matters because the feature adds file I/O.
- A Windows build with zero warnings.

## Invariants kept

**The engine is untouched.** No file under `include/`, `src/` or `sim/` changes. The runner reaches the
engine only through `Session`, which calls `MatchingEngine::process` exactly as before (session.hpp:583).
No engine API is added. Checked by `git diff -- include src sim` being empty and by the `memmove`
objdump check.

**The pinned hashes and the published evidence do not move.**

- `build_roster` in `registry.hpp` is not edited, so the default line-up behind every published number
  is the same roster.
- `session.hpp` gains one static, read-only function. `start`, `step`, `run_strategy`, `deliver_order`
  and `hash` are byte-for-byte unchanged.
- `placebo` is reachable only by name and is in no default line-up.
- `test_determinism` includes no arena code, so `77a560d8ffb4fa43` cannot move.
- `test_session_replay_and_accounting` builds `RosterConfig{}`, so `2853d2127a3486c5` cannot move, and
  `test_placebo_is_inert` now asserts that the placebo-appended session equals it.
- `test_ui_grid` draws a fixed scene, so `ddfa48c44bff4de8` cannot move.

The only visible change to existing binaries is text: `placebo` appears in `sweep --help` and in the
unknown-name message. No file in `results/` contains either.

**User strategies stay isolated.** `arena/suite.cpp` is compiled twice:

- into `suite` from `$(ROBJ)`, without the define;
- into `suite_lab` from `$(LOBJ)`, with `LAB_FLAGS`.

The registry's manifest include and its delegation to `add_user_strategy` stay behind
`#if defined(PX_ARENA_USER_STRATEGIES)`. So `suite`, `sweep`, `arena` and every test contain no
installed strategy. `test-lab` asserts that `suite` refuses an installed name and that both binaries
report the same fingerprint.

**No floating point inside a session.** Everything that runs inside a session, or decides about one, is
integer:

- roster building;
- `SeatOverride`;
- the step loop and the ceiling;
- the health gate;
- the fingerprint;
- every raw row.

`arena/suite.hpp` contains no floating point, and `test-cli` greps for it. Doubles exist only in
`arena/suite_report.hpp`, after every session has finished.

**At most 64 participants.** A suite roster has 7. `build_arm_roster` refuses a `SuiteDef` whose line-up
plus one exceeds `Session::kMaxParticipants`, and `Session::check` still applies underneath. Own-event
marking is untouched.

**Vocabulary.** Every participant, built-in or installed, is a strategy, and nothing new names it
otherwise. The seat's delay is `reaction`, and nothing new in `arena/` is called latency.

## Later steps

This section was the sketch for steps 3 to 5, written before they existed. Each is now built and has a
design document of its own, which records where it departed from the sketch: `docs/REPORT.md`,
`docs/CONTROLS.md` and `docs/REPRODUCE.md`. They all build on one split, which still holds:

- `suite.hpp` defines conditions and produces raw records;
- `suite_report.hpp` reads only a `RunRecord`;
- the run directory serialises a `RunRecord` completely.

Two ideas from the sketch are not built:

- **A holdout block,** for example seeds 1,000,000..1,000,063, used by no test or example, for
  reporting after tuning on `standard`. It is one row in `kSeedBlocks`.
- **Showing one arm in the viewer.** `arena_lab --suite=standard --candidate=X --seed=N` could call
  `build_arm_roster` and `suite_session_config` to show exactly the session the runner scored.

## Known weaknesses

- **25 ms favours short-horizon logic.** A trend follower over hundreds of milliseconds cannot express
  itself, and the built-in `fader` is inert on all 64 seeds. The runner reports inert seeds, so this
  shows up instead of reading as zero edge. Longer horizons belong in a step-3 scenario that publishes
  its health failures.
- **Pairing is only partial.** In the runner it removed 23% of the variance for imbalance against maker
  and 3% for imbalance against momentum. For noise against imbalance the arms were negatively
  correlated, so pairing added variance (−31%). Only for a one-parameter change, measured in a probe at
  `6ffdbc3`, did it remove most of it (85%). Unrelated strategies need more seeds, which is why the
  figure is printed. Making the noise traders' draws counter-based would strengthen pairing, but it
  would move every published hash.
- **The path is chaotic.** In probes at `6ffdbc3`, moving the seat's delay by 1 ns changed imbalance's
  seat value by −0.72 ± 1.57 or +1.76 ± 1.76. Neither is separated, but the per-seed scatter is as large
  as that of a genuine one-parameter comparison (ratio 3 against 4: +10.71 ± 1.70). An effect of a few
  currency units marked `separated` is therefore weak evidence of skill. The runner already prints one:
  momentum against the placebo is −2.36 ± 1.17, interval −4.69 .. −0.03, verdict `separated`, while its
  per-seed range runs from −24.40 to +26.60. Step 4 should publish this floor.
- **The seat comes last in same-instant ties.** That is fair between candidates. Against the reference
  maker at an equal delay it is a systematic asymmetry, measured as unresolvable in a probe at
  `6ffdbc3`.
- **The fixed delay can surprise an author.** A strategy reading its own `reaction_ns` member sees the
  value it declared, not the 10 µs the seat imposes. The truth is `now_ns − as_of_ns` in the
  `MarketView`.
- **Installed strategies can break the assumptions without the build catching it.**
  - Replay catches `static` state that carries from one session into the next, because each replay
    starts from whatever the first run left behind. It cannot prove that no such effect exists.
  - Floating point inside an installed strategy cannot be detected, so cross-platform replay is promised
    only for built-in candidates, and the report says so.
  - A strategy that loops forever inside `on_event` hangs the runner. A wall-clock timeout would break
    replay, so none is added.
- **The thresholds are judgement calls.** 4 ticks and 5% leave about two ticks of margin over the worst
  placebo session (2.03 ticks). A candidate that drags tracking from 1.5 to 3.9 ticks on every seed
  still passes, so step 3 should report the change in gap against the placebo. The all-seeds rule is
  strict on purpose: one failing seed out of 64 voids a verdict, and the data is kept.
- **Another pinned number.** `kPinnedSmokeFingerprint` (`d461c30ab2446acf`) moves with any change to a
  reference strategy, a default, the session or the engine. The arena session hash is deliberately left
  unpinned, because nothing promises it stays fixed. The suite does make that promise, "standard v1"
  names one set of conditions, and the pin is what forces the decision to bump the version.
- **The published market's health at its published length is worse than the documents say.**
  `docs/ARENA.md` says that with the informed trader's limit at 5,000 the mean |mid − V| "is about 1
  tick", and the comment in `registry.hpp` says "1 tick". The committed headline at 200k requests
  prints a median of 10.64 ticks and a worst seed of 67.79 (`results/arena-wsl2/headline.txt`), and 16
  of its 24 sessions at the 10 µs point are more than 4 ticks loose. The suite sidesteps this by running
  25 ms; it does not fix it. Correcting that sentence is a separate change for the owner. It is not
  part of this step.
- **Platform.** Every timing here is from Linux; Windows is not measured. `std::filesystem` under MSYS2
  UCRT64 g++ 13 with `-static` is expected to work and must be verified on the Windows build, and so
  must the claim that the raw files are byte-identical across operating systems.

## Out of scope

- The step-3 report:
  - markouts, fill rate, spread earned, inventory exposure, drawdown and turnover tables;
  - attribution to reference participants;
  - multiple-comparison correction;
  - reading a run back (`--report`) and comparing across directories (`--compare`).

  The raw data and the fingerprint those need are produced now.
- Scenario families. Step 2 ships one scenario.
- Step 4:
  - baselines and controls as published results;
  - the analytic positive control;
  - the holdout block;
  - defect-injection tests beyond those above.
- Step 5:
  - build provenance;
  - header digests;
  - `--config=` re-runs;
  - the README restructure.
- Head-to-head sessions with two candidates in one market.
- Replacing a reference participant with the candidate.
- Any change to the reference market: its parameters, counter-based noise draws, or the informed
  trader's limit.
- Raising the 64-participant ceiling, or changing own-event wake marking.
- Viewer changes.
- Running arms in parallel.
- Any change to `sweep`, `bench/`, `results/` or the engine.
- Python anywhere in the build or the runner.

## Rejected alternatives

- **A `--candidates` mode inside `sweep`.** It needs the least code, but `sweep` is the binary that
  produces the published evidence, its structures are maker-centred, and its stopping rule is a request
  count. Step 3 would have to pull the comparison back out first.
- **Request-bounded sessions,** including the published 200k. The candidate's own orders use up the
  shared budget, so closing V agreed on 5 of 64 seeds under a 7,000-request bound (a probe at
  `6ffdbc3`).
- **The stall exit as a stopping rule.** It ends arms at different times for the same reason.
- **500 ms sessions, or the published length.** 13 of 24 seeds are more than 4 ticks loose at 500 ms
  (a probe at `6ffdbc3`), and CLAUDE.md says nothing read off such a book counts.
- **A longer horizon, keeping only seeds where the placebo passes.** That does not condition on a
  candidate's outcome, because the placebo arm is the same for all of them. But it selects calm
  fundamental paths, so the suite would no longer be the model. At 25 ms no seed needs screening.
- **The candidate's declared reaction delay.** Speed would then be chosen by the candidate. Measured:
  −23.70 at imbalance's declared 20 µs (a probe at `6ffdbc3`) against −34.92 at the suite's 10 µs (the
  runner) for the same strategy. An opt-in flag for it was also dropped, since a different delay is a
  different scenario.
- **Inserting the seat earlier, or replacing a reference participant.** Insertion shifts later owner ids
  and random streams (`acc8b70cc0ea7b48` at index 0, in a probe at `6ffdbc3`). Replacement changes the
  opposing line-up, which the owner fixed.
- **Two candidates in one session.** The seats would differ in owner id, seed and tie order, and it
  measures interaction, which is a different question.
- **No placebo, only the no-seat market.** It would save one session per seed and lose the per-run
  proof that the seat is inert, the aligned rows and the exact-zero control. Running only the placebo
  and trusting the proof saves the same session and loses the per-run check.
- **Dropping failing seeds from the pairing.** That conditions on the outcome and favours a candidate
  that breaks the market on its worst seeds.
- **The range-straddles-zero test alone.** `docs/ARENA.md` shows it is far more conservative than a
  paired standard error. Both are reported.
- **Leaving standard errors to step 3.** CLAUDE.md requires an arena result to be reported with its
  paired standard error, and a comparison without one cannot tell "underpowered" from "nothing there".
  Step 2 prints the minimum the acceptance criterion needs: mean, s.e., interval, resolvable effect and
  range. Everything else in the report waits for step 3.
- **Replaying only the first seed of each arm.** It is cheaper, but state carried across later seeds
  would go unseen. Replaying everything costs about 2 s on a standard run with two candidates: 128 of
  its 384 sessions are replays.
- **Refusing any existing output directory without `--force`.** It gets in the way of iterating. A
  previous run directory is replaced instead, and anything else is refused.
- **`--set`, `--param` or a free seed list on the runner.** Then the conditions would be whatever was
  typed, which is what the acceptance criterion rules out.
- **A conditions digest separate from the fingerprint.** One number is enough when it covers both the
  suite's settings and the placebo hashes.
- **Hashing the whole `suite.cfg` into the fingerprint.** Adding an unrelated `RosterConfig` field would
  then force a version bump.
- **Reading the report back from files now (`--report`).** It is deferred to step 3. The seam is kept by
  making the report a function of `RunRecord` alone.
- **Copying candidate headers, generating build information or checking file timestamps in step 2.**
  That is step-5 machinery. An mtime check is a heuristic that step 5's build-time digest replaces.
- **A Python runner or summariser.** Ruled out by the constraints. The report is C++ and reads only the
  run's own data.
