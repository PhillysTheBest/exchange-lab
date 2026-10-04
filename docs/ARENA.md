# The arena

Bots trading against each other inside the matching engine, with a controllable
reaction delay each, rendered live in the terminal as a retro desktop.

This file is the design and the reasoning. The result is in
[README.md](../README.md); the evidence is in `results/arena-windows/`.

**Everything here is simulated.** A session invents its own nanoseconds so that a
participant's reaction delay can be a controlled variable instead of a property
of the machine. The engine's latency numbers are *measured*, on hardware, and
they are a different kind of number entirely. The code keeps the two apart in its
vocabulary too: a bot has a `reaction_ns`, the engine has a latency, and nothing
in `arena/` is called latency.

## Why it exists

The engine was finished, measured and correct, and a reader could not *see*
anything: the repository was numbers and tests. The arena makes it watchable, and
more importantly it turns the project from "a matching engine" into "an exchange
and the lab where strategies and speed compete" — on a venue where fills are
exact rather than estimated, which is the flaw that stopped the market-making
work in this repository's earlier life.

## The boundary

Everything compiles into one binary. Nothing talks over a network.

| In scope | Out of scope |
|---|---|
| Bots as C++ headers, registered at compile time | Web server, REST, JSON, sockets |
| Deterministic, seeded, replayable sessions | Database, accounts, auth |
| Value marked to a fundamental, adverse selection, queue position | npm, bundlers, a browser |
| One dependency-free terminal renderer | Plugin DLLs, an embedded scripting language |
| Per-bot reaction delay, scheduled in simulated time | Live market data, broker APIs |

Adding a bot means: write a header in `arena/bots/`, add a line to
`registry.hpp`, rebuild. That is how research platforms link strategies
internally, and it keeps this a systems-and-strategy project rather than a web
project.

## The rules, and which of them stop at the arena's edge

The engine's rules in [CLAUDE.md](../CLAUDE.md) are about `process()`. Two of
them do **not** extend up here, and saying so explicitly is the point:

- **Zero allocation does not apply to arena code.** `arena/` uses maps, deques
  and vectors freely. The guarantee belongs to `MatchingEngine`, where
  `test_alloc_audit` still holds it at zero; extending it to a renderer would
  cost a great deal and buy nothing. The session's event buffer is pre-reserved
  anyway and counts the times it had to grow, so the arena does not allocate
  *inside* a `process()` call.
- **No C-runtime calls on the hot path does not apply either.** There is no hot
  path here. Nothing in `arena/` is timed, and the benchmarks do not link it.

Three of them apply with full force, and one is added:

- **The matching core stays single-threaded and untouched.** The arena is another
  caller of `process()`, the way `sim/pipeline.hpp` and `tools/repl.cpp` are. It
  adds no engine API — see queue position below — so it cannot affect a measured
  number.
- **Determinism is a contract.** A seed and a roster reproduce a session bit for
  bit, and `Session::hash()` makes that one number.
- **Claims must be reproducible from `results/`.**
- **New: no floating point inside a session.** Prices, P&L, intervals and
  decisions are all integer. A `std::log` or a `std::exponential_distribution`
  would be free to differ between library versions and would quietly break
  cross-platform replay. Floating point appears only in reporting, after the
  session is over.

## Architecture

```
arena/clock.hpp        the simulated clock and the scheduler. Where determinism
                       lives or dies.
arena/fair_value.hpp   the fundamental value: the only exogenous information.
arena/book_mirror.hpp  a participant's own view of the book, rebuilt from the
                       event stream alone.
arena/accounting.hpp   positions, marked value, and the spread/adverse split.
arena/bot.hpp          the bot contract: MarketView in, ActionBatch out.
arena/bots/*.hpp       one header per strategy.
arena/bots/registry.hpp  the single list of who is in the market.
arena/session.hpp      the session: engine + clock + roster + accounting. No
                       rendering.
arena/sweep.cpp        the experiment, headless. Writes CSV.
arena/ui.hpp           the retro-desktop renderer. Knows nothing about trading.
arena/term.hpp         the four things the viewer needs from a terminal.
arena/arena.cpp        the viewer: the only place the renderer meets the market.
```

The engine, `sim/` and the benchmarks are unchanged.

## The reaction model

The one genuinely new mechanism. A bot with reaction delay `d`, woken by
something that happened at time `t`:

- **acts at `t + d`** — that is `now` in its `MarketView`, and the timestamp its
  orders carry into the engine;
- **sees the world as of `t`** — its book mirror is advanced only to `now - d`.

One knob therefore buys both halves of being slow: the view is stale *and* the
order is late. A fast bot sees fresher prices and lands sooner, which is exactly
the advantage the experiment prices. `test_arena` puts a watching participant in
a real session and asserts `now_ns - as_of_ns == reaction_ns` on every wake.

Each participant has its own delayed feed, so "what a bot knows" is a single
concept with a single implementation. At most one wake is outstanding per bot, so
a burst of events inside one reaction window collapses into one decision — which
is what a real system does, and what keeps a slow bot slow: with a 1 ms delay a
bot cannot look at the market more than a thousand times a second.

**Determinism.** Every scheduled item carries a monotonic sequence number and the
ordering is `(time, sequence)`, which is a *total* order, so no tie is ever broken
by heap internals or insertion luck. That plus the no-floating-point rule is the
whole replay contract.

## Queue position, without touching the engine

The engine exposes no per-order query and none was added. `arena/book_mirror.hpp`
reconstructs the whole book — every level, every FIFO, every order's queue
position — from the `ExecEvent` stream alone, which is the only thing a real venue
sends out. So a bot here provably sees no more than a real participant would, and
queue position is earned by tracking the feed rather than handed over by the
exchange.

It also buys a test. The mirror is compared against the engine's own ladder after
every one of 200,000 requests of a seeded stream — the same differential shape as
`fuzz_vs_reference`, with the engine as the oracle. That reconciliation is a real
exchange-systems artefact, and it would consume a live venue's feed unchanged.

## The market

Six participants, each a distinct `OwnerId`.

| Participant | What it does | Informed? |
|---|---|---|
| **maker** | Quotes both sides, learns from its own fills, leans against inventory. The one the sweep moves. | no |
| **rival** | The same strategy at a fixed fast delay. The competition. | no |
| **informed** | Can see the fundamental value. Takes quotes that are on the wrong side of it. | **yes** |
| **noise** ×2 | Trades on its own heartbeat for reasons the session does not model. | no |
| **momentum** | Takes after a run of same-direction prints. Second-hand information. | no |

Distinct owners matter more than they look. Self-trade prevention *cancels* a
resting order instead of trading it, so sharing one owner between bots would
silently delete the maker's quotes rather than filling them, and the experiment
would be measuring an artefact.

## The experiment

**What is 10 microseconds worth?** Run the same seeded market, change only the
maker's reaction delay, and read off what happened.

Three things make the answer worth reading, and all three are in
`bench/run_arena_evidence.sh`:

1. **A mechanism, not a correlation.** The informed trader can see a fundamental
   value that nobody else can, and the maker must infer price from its own fills.
   That is Glosten and Milgrom's setting: the maker collects the spread from
   uninformed flow and pays it back to the informed trader, and its reaction
   delay decides how much of the second it suffers. The qualitative shape is
   therefore predicted in advance rather than discovered afterwards.
2. **A null run.** `--null` switches the informed participant off. With no
   information in the session there is nothing for speed to be worth, so the
   curve should go flat. The control runs before the result is believed — and
   here it did not go flat, which is the single most useful thing the arena has
   produced. See "What the evidence actually says" below.
3. **Paired differences across shared seeds.** A maker's value is dominated by
   what its leftover inventory happened to be worth, which is a random walk, and
   the spread of *levels* across seeds swamps the effect. Every point runs on the
   same seeds, and the seed fixes the path of the fundamental, so the difference
   between two points *at the same seed* cancels most of that noise. The runner
   reports both, and says in plain words when a paired range straddles zero.

Plus a sanity check printed above every result: the mean `|mid − fundamental|` in
ticks. A book that has come loose from the fundamental is not a market, and
nothing read off one would count.

### Defining adverse selection

"Adverse selection" is quoted loosely often enough that a number labelled with it
means little, so each fill is split into two parts that add up exactly. With `H`
the horizon, `V` the fundamental, and `q` positive when the account bought:

```
edge    = (V(t+H) - fill_price) * q     what the fill was worth in the end
spread  = (V(t)   - fill_price) * q     what it earned for providing liquidity
adverse = (V(t+H) - V(t))      * q     what the price did to it afterwards

edge = spread + adverse,  identically, in integers
```

`adverse` contains no spread, so it is the number the experiment turns, and it is
per fill, so it does not depend on how much a participant traded. All three go to
the CSV so the arithmetic can be checked rather than taken on trust.

## What the evidence actually says

The headline sweep separates a 1 ms maker from a 1 µs one and nothing else: 10 µs
and 100 µs both have paired ranges that straddle zero over 24 seeds. So the answer
to "what is 10 microseconds worth?" in this market is **nothing measurable**, and
that is a real answer rather than a failure to get one.

Then the null run refused to go flat. Removing the informed trader entirely leaves
96% of the 1 ms effect in place (−2948 against −3075). Splitting each fill into
spread and adverse selection shows why:

| Maker's delay | Fills | Spread earned | Adverse selection | Value |
|---|---|---|---|---|
| 1 µs | 10,342 | +3,457 | +3.25 | +3,945 |
| 1 ms | 1,874 | +392 | +10.46 | +760 |

**The cost of being slow here is a liquidity-provision effect, not an information
one.** A 1 ms maker cannot keep a quote standing — it gets 5.5× fewer fills and
earns 8.8× less spread — and adverse selection accounts for about $7 of a $3,185
swing, in the *helpful* direction, with a paired range that straddles zero at
every point. Making the informed trader maximally aggressive does not change it.

### Power, and one claim this corrected

A negative result needs a standard error or it is not a result. `summarize_arena.py`
prints the paired standard error and the smallest effect a given seed count could
resolve, and adding it changed one answer:

| Maker's delay | Paired mean | Std error | Resolvable at 24 seeds | Verdict |
|---|---|---|---|---|
| 10 µs | −39.96 | ±56.19 | ±157 | underpowered, not null |
| 100 µs | −601.83 | ±93.05 | ±261 | separated |
| 1 ms | −3117.13 | ±74.71 | ±209 | separated |

The sweep's own table calls 100 µs "not separated", because it tests whether the full
min-to-max range over 24 seeds straddles zero — a far more conservative criterion than a
paired standard error. At about six standard errors from zero, 100 µs *is* separated. Both
tests are reported rather than whichever one flatters the result, and the distinction
between "underpowered" and "nothing there" is now printed rather than implied.

### The positive control, and the limitation it found

A null run shows an effect disappears when its supposed cause is removed. It does
not show the measure can see the effect at all — a column that is always zero
passes a null run perfectly. So the second control goes the other way: it creates
adverse selection **by construction** and asks whether the columns respond.

A maker with `impact_centiticks=0` and `skew_ticks_at_limit=0` never moves its
quotes at all: it learns nothing from the tape and leans nothing against
inventory, so its prices sit where they started while the fundamental walks away.
That is a stale quoter in the purest available form. The rival keeps learning, so
the book stays anchored to the fundamental; freezing both makers instead simply
misprices the market, and the health line catches it (the mean `|mid − V|` goes
from 1.8 to 13.8 ticks).

**The control does not work, and why it cannot is the finding.** Three designs
were tried and all three fail the health line, which is the instrument's own
admissibility test:

| Design | Mean \|mid − V\| | Admissible? |
|---|---|---|
| Freeze both makers | 13.8 ticks | no — nothing prices the book |
| Freeze the swept maker only | 25.0 ticks | no |
| Freeze it and shrink it to a 5-lot | 14.8 ticks | no, and 4.7% one-sided |

A market maker in this session is not a probe that can be inserted without
perturbing what it measures: jointly with the rival, it *is* the price-discovery
mechanism. Make it stale enough to be reliably picked off and the book it is
being measured against stops tracking the fundamental, so the measure loses its
reference. Shrinking it does not help — a 5-lot stale order still sets the touch,
so the mid is then quoted by the very order under test.

The consequence is worth stating plainly: **the post-fill column's sensitivity is
not established.** No run in `results/` demonstrates that it can read a large
value when a large value exists. The reading that it is near zero in the headline
therefore rests on the argument below rather than on a calibration, and a reader
should weigh it accordingly.

The argument it rests on is analytic rather than empirical, and it is a strong
one. The fundamental is a
martingale, so E[V(t+H) - V(t)] = 0 *unconditionally* - for every participant,
whatever it traded and whenever it traded. A post-fill markout can only be
non-zero in expectation for a participant that **predicts** the fundamental, and
no participant here does: the informed trader reacts to V's current value, it
does not forecast the next move. So the post-fill column is close to zero by
construction in this model, and it was never going to carry the result.

Which makes the decomposition's naming the thing to be careful about. What the
code calls `spread` contains what most readers would call adverse selection: a
stale quote's loss is exactly "filled at a price the fundamental has already left
behind". What the code calls `adverse` is the narrower post-fill drift. Both are
defined precisely in `arena/accounting.hpp` and both are printed, so the
arithmetic is checkable - but the labels alone would mislead, and the headline's
"$7 of a $3,185 swing" is better read as *the post-fill channel is empty here, as
the model requires*, rather than *nothing adverse happened*.

`results/arena-windows/stale.*` holds the attempt, published because a control
that failed is still evidence about the instrument.


### What this instrument cannot show

The null is partly structural, and saying so is the difference between "my experiment
found nothing" and "I know the limits of my own instrument". There is one informed trader
with a one-tick threshold; the fundamental moves on a single timescale; there is no second
venue, no quote fade, no cancel-replace race, and no queue position worth winning, because
the book is thin enough that time priority rarely binds. Those are the mechanisms by which
ten microseconds pays for itself in a real market, and none of them is in the model. A
market where the *selection* cost dominates the *uptime* cost would need a faster
fundamental, a maker that does not withdraw, or far more seeds.

That is worth saying plainly because it is the opposite of what this was built to
show. The design went to some trouble to give latency a mechanism, and the
mechanism turns out not to be what moves the number at the delays a laptop
simulation can resolve. The honest reading is that the *uptime* cost of being slow
dominates the *selection* cost by three orders of magnitude in this market, and
that a market where the selection cost dominates would need either a much faster
fundamental, a much more patient maker that does not withdraw, or far more seeds.
The null run is what made that visible, and it is why it runs before the result is
believed rather than after.

## Four things that were wrong first, and what they cost

Each of these produced a confident, plausible, wrong result. They are written
down because the mistakes are more instructive than the design, and because a
reader is entitled to know which way the evidence had to be dragged.

**1. The markout measured the spread, not adverse selection.** Markout against a
later fundamental includes the half-spread the maker earned, so it is positive
almost by construction — and a *staler* quote sits further from fair value and
captures more spread on every fill it does get. The first sweep therefore showed
markouts rising with delay, which reads as "slower is better". Fixed by the
decomposition above.

**2. The fundamental was mean-reverting.** A pull back toward the opening price
bounds the walk and looks harmless. It also pays a market maker for holding
inventory: price deviates, the maker's skew leaves it leaning the right way, and
the drift hands it the move back. That swamped adverse selection completely. The
walk is now a martingale with a far-off reflecting wall, and the wall is counted
so that a session which touches it says so.

**3. The maker learned price from the tape.** "Follow the last trade" sounds
sensible and is catastrophic when the maker *is* the market: the last trade is its
own quote, so the estimate was a function of itself — a random walk with nothing
anchoring it. The book wandered thousands of ticks from fair value, the informed
trader ran out of position limit trying to drag it back and then stopped
correcting, and every number downstream became a measurement of that drift. The
update is now Glosten–Milgrom and about *direction*: if somebody lifts my ask,
value is probably higher. What that reads — which side got filled — is the one
thing a maker cannot manufacture by quoting.

**4. There was one maker, so slowing it down measured the wrong thing.** A lone
stale maker *is* the price, so the uninformed flow keeps trading against its stale
quotes and hands it money. Slowing it down did not measure what its delay was
worth; it measured how much the market depended on it. The `rival` maker fixes
that, and `--set=rival.enabled=0` reproduces the degenerate market on demand —
`results/arena-windows/no-rival.*` keeps that claim checkable.

Two parameters also turned out to be load-bearing, and both were found by
sweeping rather than by thinking:

- **The informed trader's position limit.** At 1,000 it ran out of room
  correcting the book and stopped trading, and the mean `|mid − V|` was 26 ticks
  against a 4-tick spread. At 5,000 it is about 1 tick. Starving the informed
  trader of capacity breaks price discovery rather than making it conservative.
- **The maker's inventory limit.** At 500 it withdrew a side after ten net fills,
  so 42% of the session had no two-way market. At 2,000 that is 0.3%. The skew is
  now parameterised by how far the quotes lean *at the limit*, so the two knobs
  no longer multiply.

## The viewer

A purple grid desktop of chunky windows with magenta, teal, orange and violet
title bars, hard drop shadows and block-character meters, in 24-bit ANSI. No
curses and no libraries: `arena/ui.hpp` owns a grid of coloured cells and writes
each frame in one call, emitting a colour escape only where a colour changes.

| Window | Shows |
|---|---|
| **ORDER BOOK** (teal) | The ladder: asks above, bids below, size as bars, the fundamental on the middle line |
| **PARTICIPANTS** (violet) | One row per bot: reaction delay, position, marked value as a signed bar |
| **REACTION DELAY** (orange) | Each bot's delay as a log slider — the dial the experiment turns |
| **SESSION** (teal) | Seed, simulated clock, orders, trades, the book-to-value gap |
| **TAPE** (magenta) | Prints as they happen: price, size, aggressor |
| **! NOTABLE** (magenta dialog) | Interrupts on a print that moved the mid several ticks |

Keys: `space` pause, `+`/`-` speed, `1`–`9` focus a participant, `q` quit.

**The viewer cannot change what it watches.** It paces a session by calling
`run_until()` with a simulated timestamp, and the keyboard only changes how fast
it asks for the next one. A session watched at a hundred times speed, paused and
resumed, has the same hash as the same seed run headless. That settles the
question of trading by hand, too: live keystrokes would be a wall-clock read
inside a session and would break replay, so the human gets the controls and the
market gets none of them.

`--still=NS` runs a session headless to one simulated instant and prints a single
frame, which is how the picture in the documentation stays reproducible from a
seed.

## Running it

```bash
mingw32-make arena                      # builds the sweep runner and the viewer
build/release/arena                     # watch a session
build/release/arena --maker-reaction=1000000   # watch a slow maker get run over
build/release/sweep --help              # every sweepable field
build/release/sweep --seeds=24 --replay-check  # the headline curve
build/release/sweep --null              # the control
bash bench/run_arena_evidence.sh windows       # regenerate results/arena-windows/
```

`mingw32-make test` includes `test_arena`, which holds the mirror against the
engine, the scheduler's total order, the replay hash, the closed-world accounting
identity, and the reaction model.

## Writing a bot

A bot is a plain struct. It does not inherit from anything, never touches the
engine, and cannot read a clock.

**Bots are compiled in, not loaded.** There is no way to hand the running
program a strategy — no plugin DLL, no scripting language, no socket, nothing to
type at the terminal. That is the scope boundary at the top of this file, not an
omission. A strategy is a header that gets compiled into the binary, which is how
research platforms actually link strategies, and it is what lets a bot be a plain
struct with no ABI, no sandbox and no marshalling. The cost is real and worth
stating plainly: **a third party needs the source tree and a compiler, and has to
rebuild.** On this machine that is about twenty seconds.

What adding one actually takes, start to finish:

1. Write the header in `arena/bots/`. The whole surface is in `arena/bot.hpp`:
   `name`, `reaction_ns`, `timer_ns`, `reset(seed)`, `on_event(view)`.
2. `#include` it in `arena/bots/registry.hpp` and push one instance in
   `build_roster()`.
3. Optionally add its fields to `RosterConfig` and one line each to
   `set_roster_param()`, which makes them settable with `--set=` and sweepable
   like every other parameter.
4. `mingw32-make arena`.

[`arena/bots/fader.hpp`](../arena/bots/fader.hpp) is that worked example, written
against the contract alone — it includes `arena/bot.hpp` and nothing else, and it
could not reach the engine, the session or the scheduler if it tried. It fades
the recent move: keeps a slow integer average of the traded price and takes the
other side when the last print is far enough away. It loses money, which is the
honest outcome for uninformed flow crossing the spread.

`test_arena` keeps the claim from being a claim: it asserts the example is absent
by default, present when enabled, that it really trades (938 orders, 964 fills in
a 60,000-request session), that it is never handed the fundamental value, that its
registry parameters reach it, and — the part that matters for anyone else's bot —
that an extra participant cannot break the closed-world accounting identity, the
book's tracking of the fundamental, or the session's ability to end.

It is off by default, so adding it moved no published number: the session hashes
`dc1ab7b9798723c0` and `4c05d629279645e9` are unchanged.

```cpp
struct MyBot {
    const char* name = "mine";
    SimTime reaction_ns = 10'000;
    SimTime timer_ns = 0;             // 0, or a periodic self-wake

    void reset(std::uint64_t seed) {}

    ActionBatch on_event(const MarketView& v) {
        ActionBatch out;
        if (v.in_flight != 0) return out;
        const Price bid = v.best(Side::Buy);
        if (bid != 0 && v.position < 100) out.push(Action::limit(Side::Buy, bid, 10));
        return out;
    }
};
```

`ActionBatch` carries up to six actions per reaction, because a maker
cancel-replacing both sides needs four and an API that forced it to spend four
reaction windows doing so would be measuring itself.

### Trading by hand

Live keystrokes cannot go into a session: knowing *when* a key arrived means
reading a wall clock, and a wall clock inside a session breaks the replay
contract everything else here rests on. A file of orders stamped in **simulated**
time has none of that problem, so that is what `--script=` plays, as an ordinary
participant called `you`:

```bash
build/release/arena --script=tools/arena_script_demo.txt
```

```
2ms    buy   50   99.95      # join the bid
3ms    sell  50  100.05      # and the offer: now you are a two-sided quoter
8ms    buy   25   market     # pay the spread to get long
12ms   cancel 1              # pull your first still-live order
```

One order per line: `<at> buy|sell <qty> <price>`, `<at> buy|sell <qty> market`,
or `<at> cancel <n>` for your own n-th live order. Times take `us` and `ms`;
prices are currency units on the tick grid. Comments and blank lines are ignored,
timestamps may not go backwards, and every malformed line is a refusal naming the
line number rather than a silently different session.

It is a participant like any other, which means its reaction delay applies: an
order stamped at `t` is submitted when the bot wakes, and what it saw was the
market as of `t - reaction_ns`. Writing a time down does not buy clairvoyance.
One limitation worth knowing: the bot contract offers a periodic timer and not a
"wake me at exactly T", so a scripted time is honoured at the first wake at or
after it — 10 µs granularity by default, finer than anyone writes a script by
hand.

The point of the file over a keyboard is that the result is *reproducible*:
`test_arena` plays the same script twice and checks the session hashes match, so
a session you played by hand can be replayed by anyone who has the file. A
recording of somebody playing could never offer that.

### Choosing who is in the market

Once a strategy is registered, `--bots=` composes a line-up by name without
editing anything:

```bash
build/release/sweep --bots=maker,rival,informed,noise:3,fader
build/release/arena --bots=maker,informed,noise:2
```

Comma-separated names, each optionally `:count` to repeat it. Repeated noise
traders get staggered heartbeats, so naming three does not put them in lockstep.
Naming a participant `informed` is what grants it the fundamental value — the
roster decides that, never the strategy. Every parameter still comes from the
config, so `--set=` and sweeps work unchanged; the spec only decides who is in
the room.

An unknown name, a count that is not a number, a count of zero and an empty spec
are each an error naming the offending token, because a typo that silently
produced a different market would be worse than a failure. A line-up with no
`maker` is refused by the sweep runner, since every column it prints is about the
swept maker.

Without `--bots=`, the registry's default line-up is used — which is what every
published number was produced with, and the session hashes confirm it:
`dc1ab7b9798723c0` and `4c05d629279645e9` are unchanged by any of this.

What a bot cannot see: the fundamental value (unless the roster marks it
informed), the clock, other participants' positions, the engine's internals, or
its queue position except by having tracked the feed itself.

## Known limits, found by testing

Two configurations used to go wrong, both found by probing the finished arena rather
than by reasoning about it. Both are now refused or bounded at construction, and both
are pinned by regression tests — a bug that is found and not pinned gets found again by
a reader.

**A session holds at most 64 participants.** `deliver_order()` marks which bots an event
concerned in a fixed array; past that bound the marks were silently dropped, so those
bots never received an own-event wake. Measured with 104 identical noise traders: the
first 64 took 151.9 fills each, the rest took 55.9. Nothing crashed and nothing looked
wrong. Rosters above the ceiling are now refused.

The ceiling is deliberately a hard limit rather than a resizable buffer. The bound is not
the real cost — changing how wakes are marked would change wake *ordering* for large
rosters, which moves every session hash and invalidates the committed evidence. Refusing
at the boundary is provably no behavioural change at or below 64, which the unchanged
hashes confirm. Raising it is a separate change with its own re-measurement.

**A session that stops generating orders now ends.** A roster of nothing but the market
maker used to hang forever: once its quotes are where it wants them it submits nothing
further, so `max_requests` stayed unreachable while the fundamental kept rescheduling
itself. The first attempted fix was a static check for "can this roster generate flow",
and it was wrong — that lone maker *has* a heartbeat timer, it simply returns an empty
batch every time it fires. Whether a participant ever submits an order is a property of
its code, not its configuration, so no static check can decide it.

The bound is `stall_limit_ns`: if that much *simulated* time passes with no request
reaching the engine, the session ends and sets `stats().stalled`, which the sweep reports
as a warning rather than averaging the run in. It is a quiescence detector, and it is
allowed here only because it compares two simulated timestamps, so it triggers at exactly
the same instant on every machine and every replay. A wall-clock timeout would not have,
and would have broken the replay contract.

**The renderer is covered for its invariants, not its appearance.** `ui.hpp` is tested
for one cell per code point (the bug that tore every block glyph across three cells),
clipping at every edge, and a composed frame pinned by hash. How it looks is a matter of
taste; evidence is owed where claims are made, and the renderer makes none.

## What is not built

- **Runtime bot loading of any kind.** No plugin DLL, no embedded scripting
  language, nothing a running session will accept from outside itself, and no way
  to type a strategy into the terminal. A bot is compiled in; see "Writing a bot"
  above for what that costs. This is the scope boundary, and it is the single
  thing most likely to be mistaken for a missing feature.
- More than one instrument on screen. The session supports one, deliberately: one
  legible ladder beats four grids of numbers, and the engine's multi-instrument
  support is already demonstrated by the benchmarks.
- **Live** keyboard trading, for the determinism reason above. A scripted order
  file is the replayable way to do it and that exists — see "Trading by hand"
  below.
- Any form of fee model beyond a flat per-unit fee, which defaults to zero and is
  reported when set.
