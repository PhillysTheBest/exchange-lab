# The arena's published results

Every number in this file is **simulated**. A session invents its own nanoseconds, so that
a participant's reaction delay can be a variable the experimenter controls rather than a
property of the machine. The engine's timings in [`ENGINE.md`](ENGINE.md) are measured on
hardware; these are not, and the two must not be read together. Nothing in `arena/` is
called latency: a strategy has a `reaction_ns`.

These come from the sweep: 24 seeds of 200,000 requests each, committed in
`results/arena-windows/` and `results/arena-wsl2/`. `python bench/summarize_arena.py
results/arena-windows` prints every table here, so a number in this file that the script
does not print has no evidence behind it. They are not the standard suite's numbers, whose
sessions last 25 ms of simulated time ([`RUNNER.md`](RUNNER.md)). [`ARENA.md`](ARENA.md)
is the design and the reasoning behind each result.

## What is 10 microseconds worth?

**In this market, 10 µs costs less than 24 seeds can resolve; 100 µs and 1 ms both cost
real money.** Over 24 seeds of 200,000 requests each, with every point run on the same
seeds so the difference can be taken seed by seed:

| Maker's reaction delay | Paired difference vs the 1 µs maker | Standard error | Smallest effect 24 seeds could resolve | Separated? |
|---|---|---|---|---|
| 10 µs | −39.96 | ±56.19 | ±157 | **no — underpowered** |
| 100 µs | −601.83 | ±93.05 | ±261 | yes |
| 1 ms | **−3117.13** | ±74.71 | ±209 | yes |

All figures are currency units of marked value over one 200,000-request session, and
`bench/summarize_arena.py` prints this table from the committed CSVs.

Paired differences are reported because the levels are unreadable: a maker's value is
dominated by what its leftover inventory happened to be worth, and the spread of levels
across seeds swamps the effect. Every point runs on the same seeds and the seed fixes the
path of the fundamental, so the same-seed difference cancels most of that noise.

**The standard error matters, and reporting it changed one of these answers.** At 10 µs
the effect is smaller than what 24 seeds can resolve, so the honest statement is *not
established* — "underpowered" and "nothing there" are different claims and only one of
them is earned here. At 100 µs the effect is about six standard errors from zero, so it
*is* separated, even though the full min-to-max range across seeds still straddles zero.
A range test over 24 draws is far more conservative than a paired standard error; this
table reports both so a reader can see which test they are being shown.

## And the control says it is not adverse selection

The obvious story — the slow maker's stale quotes get picked off — is **wrong here**, and
the null run is what catches it. `sweep --null` removes the informed trader, so there is
no information in the session and speed should be worth nothing. It is worth almost
exactly the same:

| Run | 1 µs → 1 ms, paired |
|---|---|
| With an informed trader | −3075.10 |
| **Nobody informed** | **−2948.30** |

96% of the effect survives with nothing to be adversely selected by. Splitting each fill
into the spread it earned and the price move afterwards (`edge = spread + adverse`, in
integers) shows where the money actually went:

| Maker's delay | Fills | Spread earned | Adverse selection | Value |
|---|---|---|---|---|
| 1 µs | 10,342 | +3,457 | +3.25 | +3,945 |
| 1 ms | 1,874 | +392 | +10.46 | +760 |

The cost of being slow is a **liquidity-provision** effect, not an information one. A
1 ms maker cannot keep a quote standing: it gets 5.5× fewer fills and earns 8.8× less
spread. The post-fill column moves by about **$7 out of a $3,185 swing** — and in the
*helpful* direction — with a paired range that straddles zero at every point.

**That column is empty for a structural reason, and a second control tried and failed to
calibrate it.** A null run cannot distinguish a measure that works from one that is
always zero, so the other control sets out to create the effect on purpose: freeze a
maker's learning and its inventory skew and its quotes never move at all, which is a
stale quoter by construction. Three variants were tried and **all three fail the health
line** — a maker in this session is not a probe you can insert without perturbing what it
measures, because jointly with the rival it *is* the price-discovery mechanism. Make it
stale enough to be reliably picked off and the book stops tracking the fundamental, so
the measure loses its reference. So the post-fill column's **sensitivity is not
established**, and that is published rather than glossed.

What the reading rests on instead is analytic, and solid: the fundamental is a martingale,
so the expected post-fill move is zero for *every* participant regardless of what it
traded. Only a participant that **predicts** the fundamental could make that column
non-zero, and none here does — the informed trader reacts to V's current value, it does
not forecast the next one. So the honest reading is **the post-fill channel is empty
because the model requires it**, not that nothing adverse occurred; a stale quote's loss
shows up in what the code calls `spread`, being filled at a price the fundamental has
already left behind. [docs/ARENA.md](ARENA.md) works through it, and
`results/arena-windows/stale.*` holds the failed control, because a control that failed
is still evidence about the instrument.

That is the honest answer, and it is the one the evidence supports rather than the one
the design was hoping for.

## And the ledger says where the money went

The session is closed, so every gain is some named participant's loss and the values must
sum to zero. That turns the claim above from an inference into an audit — if the slow
maker were being adversely selected, the informed trader would be richer for it:

| Participant | 1 µs maker | 1 ms maker | Transfer |
|---|---|---|---|
| maker | +3856.91 | +739.77 | **−3117.13** |
| rival | +3800.71 | +7466.36 | **+3665.65** |
| informed | +1744.25 | +1624.82 | −119.43 |
| noise | −5139.77 | −5346.70 | −206.93 |
| noise | −4221.65 | −4305.66 | −84.01 |
| momentum | −40.45 | −178.59 | −138.15 |
| **sum** | **+0.00** | **+0.00** | |

**The maker's loss goes to its competitor, and the informed trader ends up slightly worse
off.** The −3117.13 here is the same number as the paired mean in the table above, arrived
at from the other direction. Slowing one maker down does not feed the trader with
information; it hands uninformed flow to the maker that is still quoting.

The sums are the check on the attribution rather than decoration: they are exactly zero in
integers, so nothing is unaccounted for. `summarize_arena.py` prints this table, and keys
it on participant index rather than name — two of the strategies are both called `noise`, and
grouping by name averages them into one participant and silently breaks the identity.

## What this instrument cannot show

The null result is partly a property of the model, not only of the market, and the limits
are worth stating before someone mistakes them for findings. There is **one** informed
trader with a one-tick threshold; the fundamental moves on a single timescale; there is no
second venue, no quote fade, no cancel-replace race, and no queue position worth winning
because the book is thin enough that priority rarely binds. Those are exactly the
mechanisms through which ten microseconds pays for itself in a real market. So a reader
should take "10 µs is not resolvable here" as a statement about this instrument at this
seed count, not about trading.

What the arena does establish is narrower and checkable: that the *uptime* cost of being
slow dominates the *selection* cost by roughly three orders of magnitude in a market with
one informed participant — and that an experiment can be built where the control is
capable of overturning the headline, which this one did.

## Known limits

Found by testing the arena, fixed, and pinned by regression tests in
[test_arena.cpp](../tests/test_arena.cpp):

- **At most 64 participants per session.** A session marks which strategies an event concerned
  in a fixed array. Past that bound the marks were silently dropped, so those strategies never
  got an own-event wake — with 104 identical noise traders, the first 64 took 151.9 fills
  each and the rest took 55.9: wrong numbers that looked entirely plausible. Rosters above
  the ceiling are now refused at construction. The ceiling is a hard limit rather than a
  resizable buffer on purpose: changing how wakes are marked would change wake *ordering*,
  which moves every session hash and would invalidate the committed evidence.
- **A session that goes quiet now ends and says so.** A roster with nothing but a market
  maker used to hang: once its quotes are where it wants them it submits nothing further,
  so the request count stalls while the fundamental keeps rescheduling itself. A session
  now ends after `stall_limit_ns` of *simulated* time with no request — simulated, so it
  triggers identically on every machine and every replay — and reports `stalled`, which
  the sweep surfaces as a warning rather than averaging the run in silently.
- **The renderer makes no measured claim.** `ui.hpp` is covered for its invariants (one
  cell per code point, clipping at every edge, a deterministic composed frame pinned by
  hash) rather than for how it looks, which is a matter of taste and not of evidence.

## What else is in `results/arena-windows/`

- **Market health**, printed above every result: the mean `|mid − fundamental|` in ticks.
  8–12 ticks in the headline runs against a 4-tick spread, so the book tracks a value no
  participant can see. In the null run it is 41–47 ticks, because without the informed
  trader nothing anchors price to value at all — which is itself the mechanism showing up.
- **`no-rival`**: the degenerate market the design notes warn about. With one maker and no
  competition, its spread *per fill* **rises** with delay (+3,665 → +5,985), because a lone
  stale quote is the price and the uninformed flow keeps paying it. Published so the
  warning is checkable rather than a story.
- **Four robustness sweeps** — informed aggression, maker spread, arrival rate and
  learning rate — published whatever they say. A shape that only exists at one set of
  parameters is not a finding.
- **Replay**: every session in the headline and null runs was executed twice and the
  session hashes compared. All identical.

Regenerate the whole directory with `bash bench/run_arena_evidence.sh windows`, or
`BUILD=build-linux bash bench/run_arena_evidence.sh wsl2` for the Linux tree.
`python bench/summarize_arena.py results/arena-windows` prints every table quoted above,
so a number here that the script does not print has no evidence behind it.

## The same answer on both platforms, to the last digit

Unlike the engine's benchmarks, these results do not depend on the machine, the power
plan or the load. `results/arena-wsl2/` is the whole suite run again under Linux, and
**all eight sweeps come out identical** — every paired difference, every adverse and
spread figure, every book-to-fundamental gap, and every session hash:

| | Windows (UCRT) | Linux (glibc) |
|---|---|---|
| headline, seed 42, four points | `aebce54544c10ca5 35859f327b69af22 800430353d000562 9f777b5eb3508703` | identical |
| 1 ms paired difference | −3075.1000 | −3075.1000 |

That is what the no-floating-point rule buys. A session is integer arithmetic over a
seeded RNG, so there is no libm, no `long double` spill and no ordering difference left
to diverge; the arena's numbers are a property of the code rather than of the computer
that ran it. The evidence directories differ only in `about.txt`, which records which
compiler and which binary produced them.
