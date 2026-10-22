# The arena's published results

The tables behind the README's negative result. Every number here is **simulated**: a
session invents its own nanoseconds, so a reaction delay is a setting, not a measurement.
The engine's timings in [`ENGINE.md`](ENGINE.md) are measured on hardware, and the two must
not be read together.

- **Where they come from:** the sweep, 24 seeds of 200,000 requests each, committed in
  `results/arena-windows/` and `results/arena-wsl2/`.
- **How to check them:** `python bench/summarize_arena.py results/arena-windows` prints every
  table here. A number in this file that the script does not print has no evidence behind it.
- **What they are not:** the standard suite's numbers, whose sessions last 25 ms
  ([`RUNNER.md`](RUNNER.md)).
- **Why they came out this way:** [`ARENA.md`](ARENA.md).

## What is 10 microseconds worth?

**10 µs costs less than 24 seeds can resolve; 100 µs and 1 ms both cost real money.** Each
row is the paired difference from the 1 µs maker, in currency units of marked value per
200,000-request session, taken seed by seed on shared seeds:

| Maker's reaction delay | Paired difference | Standard error | Smallest effect 24 seeds could resolve | Separated? |
|---|---|---|---|---|
| 10 µs | −39.96 | ±56.19 | ±157 | **no: underpowered** |
| 100 µs | −601.83 | ±93.05 | ±261 | yes |
| 1 ms | **−3117.13** | ±74.71 | ±209 | yes |

Differences rather than levels, because a maker's value is dominated by what its leftover
inventory happened to be worth. The seed fixes the fundamental's path, so the same-seed
difference cancels most of that.

At 10 µs the honest statement is *not established*: "underpowered" and "nothing there" are
different claims. At 100 µs the effect is about six standard errors from zero, so it *is*
separated, although the full min-to-max range across seeds still straddles zero. Both tests
are shown, so a reader can see which one they are reading.

## And the control says it is not adverse selection

The obvious story, that the slow maker's stale quotes get picked off, is **wrong here**.
`/sweep --null` removes the informed trader, so speed should be worth nothing. It is worth
almost exactly the same:

| Run | 1 µs → 1 ms, paired |
|---|---|
| With an informed trader | −3075.10 |
| **Nobody informed** | **−2948.30** |

96% of the effect survives with nothing to be adversely selected by. Splitting each fill
into the spread it earned and the price move afterwards shows where the money went:

| Maker's delay | Fills | Spread earned | Adverse selection | Value |
|---|---|---|---|---|
| 1 µs | 10,342 | +3,457 | +3.25 | +3,945 |
| 1 ms | 1,874 | +392 | +10.46 | +760 |

The cost of being slow is **liquidity provision**, not information: 5.5× fewer fills and
8.8× less spread. The post-fill column moves by about **$7 of a $3,185 swing**, in the
helpful direction, with a paired range that straddles zero at every point.

That column is near zero by construction, because the fundamental is a martingale and no
participant predicts it. A second control tried to calibrate it by building a stale quoter
on purpose, and every design failed the health line, so its **sensitivity is not
established**. A stale quote's loss lands in what the code calls `spread`.
[`ARENA.md`](ARENA.md#the-positive-control-and-the-limitation-it-found) works through it,
and `results/arena-windows/stale.*` holds the failed control.

## And the ledger says where the money went

The session is closed, so every gain is a named participant's loss and the values sum to
zero. If the slow maker were being adversely selected, the informed trader would be richer
for it. It is not:

| Participant | 1 µs maker | 1 ms maker | Transfer |
|---|---|---|---|
| maker | +3856.91 | +739.77 | **−3117.13** |
| rival | +3800.71 | +7466.36 | **+3665.65** |
| informed | +1744.25 | +1624.82 | −119.43 |
| noise | −5139.77 | −5346.70 | −206.93 |
| noise | −4221.65 | −4305.66 | −84.01 |
| momentum | −40.45 | −178.59 | −138.15 |
| **sum** | **+0.00** | **+0.00** | |

**The maker's loss goes to its competitor**, and the informed trader ends slightly worse
off. The −3117.13 is the paired mean from the first table, reached from the other side. The
sums are exactly zero in integers, so nothing is unaccounted for. The script keys this table
on participant index, not name, because two participants are both called `noise`.

## What this instrument cannot show

One informed trader with a one-tick threshold; a fundamental on one timescale; no second
venue, no quote fade, no cancel-replace race; and a book thin enough that queue priority
rarely binds. Those are how ten microseconds pays in a real market. So "10 µs is not
resolvable here" is a statement about this instrument at this seed count, not about trading.

What is established is narrower: here the *uptime* cost of being slow dominates the
*selection* cost by about three orders of magnitude, and an experiment can be built whose
control is able to overturn its headline. This one did.

## What else is in `results/arena-windows/`

- **Market health**, printed above every result: the mean `|mid − fundamental|`. 8–12 ticks
  in the headline runs against a 4-tick spread. In the null run, 41–47 ticks: without the
  informed trader nothing anchors price to value.
- **`no-rival`**: one maker and no competition. Its spread *per fill* **rises** with delay
  (+3,665 → +5,985), because a lone stale quote is the price and uninformed flow keeps
  paying it.
- **Four robustness sweeps**: informed aggression, maker spread, arrival rate and learning
  rate, published whatever they say.
- **Replay**: every headline and null session ran twice, and every pair of hashes matched.

Regenerate it with `bash bench/run_arena_evidence.sh windows`, or
`BUILD=build-linux bash bench/run_arena_evidence.sh wsl2` for Linux.

## The same answer on both platforms, to the last digit

Unlike the engine's benchmarks, these do not depend on the machine. `results/arena-wsl2/`
is everything run again under Linux, and **all eight sweeps are identical**: every paired
difference, every spread and adverse figure, every health gap and every session hash.

| | Windows (UCRT) | Linux (glibc) |
|---|---|---|
| headline, seed 42, four points | `aebce54544c10ca5 35859f327b69af22 800430353d000562 9f777b5eb3508703` | identical |
| 1 ms paired difference | −3075.1000 | −3075.1000 |

That is what the no-floating-point rule buys: a session is integer arithmetic over a seeded
generator, so nothing is left to differ. The two directories differ only in `about.txt`,
which records the compiler and binary. The arena's known limits, found by testing, are in
[`ARENA.md`](ARENA.md#known-limits-found-by-testing).
