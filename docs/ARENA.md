# The arena

The arena is the simulated market: strategies trading against each other inside the
matching engine, each with a reaction delay you control, drawn live in the terminal. This
file is its design and the reasoning behind it, including the mistakes. The published
numbers are in [`RESULTS.md`](RESULTS.md); the evidence is in `results/arena-windows/`.

**Everything here is simulated.** A session invents its own nanoseconds, so a participant's
reaction delay is a variable you set, not a property of the machine. The engine's timings
are measured on hardware and are a different kind of number. The code keeps them apart in
its words too: a strategy has a `reaction_ns`, and nothing in `arena/` is called latency.

## Why it exists

The engine was finished, measured and correct, but nobody could *see* anything. The arena
makes it watchable, and turns "a matching engine" into "an exchange where strategies and
speed compete", on a venue where fills are exact rather than estimated.

## The boundary

Everything compiles into one binary, and nothing talks over a network.

| In scope | Out of scope |
|---|---|
| Strategies as C++ headers, compiled in | Web servers, sockets, a browser |
| Seeded, replayable sessions | Databases, accounts |
| Value marked to a fundamental, adverse selection, queue position | Plugin DLLs, scripting languages |
| One dependency-free terminal renderer | Live market data, broker APIs |
| A reaction delay per strategy, in simulated time | |

## The rules, and which of them stop at the arena's edge

The engine's rules in [`CLAUDE.md`](../CLAUDE.md) are about `process()`. Two of them stop
here, on purpose:

- **Zero allocation does not apply.** `arena/` uses maps and vectors freely. The guarantee
  belongs to `MatchingEngine`, where `test_alloc_audit` still holds it at zero.
- **No C-runtime calls on the hot path does not apply.** Nothing in `arena/` is timed, and
  the benchmarks do not link it.

Three apply in full, and one is added:

- **The matching core is untouched.** The arena is one more caller of `process()` and adds
  no engine API, so it cannot move a measured number.
- **Determinism is a contract.** A seed and a line-up reproduce a session bit for bit, and
  `Session::hash()` makes that one number.
- **Claims must be reproducible from `results/`.**
- **New: no floating point inside a session.** Prices, values, intervals and decisions are
  integers. A `std::log` may differ between library versions and would quietly break
  cross-platform replay. Floating point appears only in reports, after a session ends.

## Architecture

```
arena/clock.hpp          the simulated clock and scheduler: where determinism lives or dies
arena/fair_value.hpp     the fundamental value, the only outside information
arena/book_mirror.hpp    a participant's view of the book, rebuilt from the event stream
arena/accounting.hpp     positions, marked value, and the spread/adverse split
arena/strategy.hpp       the strategy contract: MarketView in, ActionBatch out
arena/strategies/        one header per strategy; registry.hpp lists who is in the market
arena/session.hpp        engine + clock + line-up + accounting; no drawing
arena/sweep.cpp          the experiment, headless (/sweep)
arena/suite.hpp, suite_report.hpp, suite.cpp   the standard comparison (/compare)
arena/controls.hpp, controls.cpp               the evaluator's controls (/controls)
arena/ui.hpp             the renderer; knows nothing about trading
arena/term.hpp           the four things the viewer needs from a terminal
arena/arena.cpp          the viewer (/watch): the only place renderer and market meet
```

## The reaction model

A strategy with reaction delay `d`, woken by something that happened at time `t`:

- **acts at `t + d`**: that is `now` in its `MarketView`, and the time its orders carry;
- **sees the world as of `t`**: its book mirror is advanced only to `now − d`.

One setting buys both halves of being slow: a stale view and a late order. `test_arena`
asserts `now_ns − as_of_ns == reaction_ns` on every wake of a real session.

At most one wake is outstanding per strategy, so a burst of events inside one reaction
window collapses into one decision. That is what a real system does, and it keeps a slow
strategy slow: at 1 ms, it cannot look more than a thousand times a second.

**Determinism.** Every scheduled item carries a sequence number, and the order is
`(time, sequence)`: a total order, so no tie is ever broken by luck. That plus no floating
point is the whole replay contract.

## Queue position, without touching the engine

The engine has no per-order query, and none was added. `arena/book_mirror.hpp` rebuilds
every level, every queue and every order's position from the `ExecEvent` stream alone,
which is all a real venue sends. So a strategy provably sees no more than a real
participant would. `test_arena` compares the mirror with the engine's own book after every
one of 200,000 requests, with the engine as the oracle.

## The market

Six participants, each a distinct owner to the engine:

| Participant | What it does | Informed? |
|---|---|---|
| **maker** | Quotes both sides, learns from its fills, leans against inventory. The one the sweep moves. | no |
| **rival** | The same strategy at a fixed fast delay: the competition. | no |
| **informed** | Sees the fundamental value and takes quotes on the wrong side of it. | **yes** |
| **noise** ×2 | Trades on its own timer, for reasons the session does not model. | no |
| **momentum** | Takes after a run of prints in one direction. | no |

Distinct owners matter. Self-trade prevention cancels a resting order rather than trading
it, so two strategies sharing an owner would silently delete the maker's quotes.

## The experiment

**What is 10 microseconds worth?** Run the same seeded market, change only the maker's
reaction delay, and see what happens. Three things make the answer worth reading, and
`bench/run_arena_evidence.sh` runs all three:

1. **A mechanism, not a correlation.** The informed trader sees a value nobody else can,
   and the maker must infer it from its fills: Glosten and Milgrom's setting. The maker
   earns the spread from uninformed flow and pays some back to the informed trader, and its
   delay should decide how much. The shape is predicted before it is measured.
2. **A null run.** `--null` removes the informed trader. With no information in the market,
   speed should be worth nothing. Here it was not, which is the most useful thing the arena
   has produced.
3. **Paired differences across shared seeds.** A maker's value is dominated by what its
   leftover inventory was worth, and that varies hugely between seeds. Every point runs on
   the same seeds, and the seed fixes the fundamental's path, so the difference at each seed
   cancels most of that noise.

Above every result, the runner prints the mean `|mid − fundamental|` in ticks. A book that
has come loose from the fundamental is not a market, and nothing read off it counts.

### Defining adverse selection

Each fill is split into two parts that add up exactly. With `H` the horizon, `V` the
fundamental and `q` positive when the account bought:

```
edge    = (V(t+H) - fill_price) * q     what the fill was worth in the end
spread  = (V(t)   - fill_price) * q     what it earned at the moment of the fill
adverse = (V(t+H) - V(t))      * q      what the price did to it afterwards

edge = spread + adverse, exactly, in integers
```

All three are written out, so the arithmetic can be checked.

## What the evidence actually says

**10 µs and 100 µs both have paired ranges that straddle zero over 24 seeds; 1 ms does
not.** Then the null run refused to go flat: with the informed trader removed, 96% of the
1 ms effect remains (−2948 against −3075). The split shows why:

| Maker's delay | Fills | Spread earned | Adverse selection | Value |
|---|---|---|---|---|
| 1 µs | 10,342 | +3,457 | +3.25 | +3,945 |
| 1 ms | 1,874 | +392 | +10.46 | +760 |

**Being slow here costs liquidity provision, not information.** A 1 ms maker cannot keep a
quote standing: 5.5× fewer fills, 8.8× less spread. Adverse selection moves by about $7 of a
$3,185 swing, in the helpful direction. A maximally aggressive informed trader does not
change this. [`RESULTS.md`](RESULTS.md) has the full tables and the ledger of who gained.

### Power, and one claim this corrected

A negative result needs a standard error. Adding one changed an answer:

| Maker's delay | Paired mean | Std error | Resolvable at 24 seeds | Verdict |
|---|---|---|---|---|
| 10 µs | −39.96 | ±56.19 | ±157 | underpowered, not null |
| 100 µs | −601.83 | ±93.05 | ±261 | separated |
| 1 ms | −3117.13 | ±74.71 | ±209 | separated |

The sweep's own table calls 100 µs "not separated", because it asks whether the whole
min-to-max range over 24 seeds straddles zero, a far stricter test. At about six standard
errors from zero, 100 µs *is* separated. Both tests are reported, not whichever flatters.

### The positive control, and the limitation it found

A null run shows an effect disappears when its cause is removed. It cannot show the measure
would see the effect at all: a column that is always zero passes a null run perfectly. So
the second control tries to *create* adverse selection on purpose.

A maker with `impact_centiticks=0` and `skew_ticks_at_limit=0` never moves its quotes, a
stale quoter in its purest form. **The control does not work, and why is the finding.**
Every design failed the health line:

| Design | Mean \|mid − V\| | Admissible? |
|---|---|---|
| Freeze both makers | 13.8 ticks | no: nothing prices the book |
| Freeze the swept maker only | 25.0 ticks | no |
| Freeze it and shrink it to a 5-lot | 14.8 ticks | no, and 4.7% one-sided |

With the rival, the maker *is* the price discovery. Make it stale enough to be reliably
picked off and the book stops tracking the fundamental, so the measure loses its reference.
A 5-lot stale order still sets the touch.

So **the post-fill column's sensitivity is not established**. Its near-zero reading rests on
an argument instead: the fundamental is a martingale, so E[V(t+H) − V(t)] = 0 for every
participant, whatever it traded. Only a participant that *predicts* the fundamental could
make that column non-zero, and none here does; the informed trader reacts to V's current
value. So the column is near zero by construction.

That makes the names the thing to be careful about. What the code calls `spread` contains
what most readers call adverse selection: a stale quote is "filled at a price the
fundamental has already left". What it calls `adverse` is only the drift after the fill.
`results/arena-windows/stale.*` holds the failed attempt, because a failed control is still
evidence about the instrument.

### What this instrument cannot show

One informed trader with a one-tick threshold; a fundamental on one timescale; no second
venue, no quote fade, no cancel-replace race; and a book thin enough that queue priority
rarely binds. Those are how ten microseconds pays in a real market, and none is in the
model. The honest reading: here, the *uptime* cost of being slow dominates the *selection*
cost by about three orders of magnitude. A market where selection dominates would need a
faster fundamental, a maker that does not withdraw, or far more seeds.

## Four things that were wrong first, and what they cost

Each produced a confident, plausible, wrong result. They are kept because the mistakes teach
more than the design.

1. **The markout measured the spread, not adverse selection.** A markout against a later
   fundamental includes the half-spread earned, and a *staler* quote earns more spread per
   fill. So the first sweep said "slower is better". Fixed by the split above.
2. **The fundamental was mean-reverting.** That pays a maker for holding inventory: the price
   drifts back and hands it the move. It swamped adverse selection. The walk is now a
   martingale with a far-off reflecting wall, counted when touched.
3. **The maker learned price from the tape.** "Follow the last trade" is catastrophic when
   the maker *is* the market: the last trade is its own quote. The book wandered thousands
   of ticks from fair value. The update is now Glosten–Milgrom, about *direction*: if
   someone lifts my ask, value is probably higher.
4. **There was one maker, so slowing it measured the wrong thing.** A lone stale maker *is*
   the price, so uninformed flow keeps paying it. The `rival` fixes that;
   `--set=rival.enabled=0` reproduces the broken market, and `results/arena-windows/no-rival.*`
   keeps the claim checkable.

Two parameters turned out to be load-bearing, both found by sweeping:

- **The informed trader's position limit.** At 1,000 it ran out of room and stopped
  correcting the book: mean `|mid − V|` 26 ticks against a 4-tick spread. At 5,000, about 1.
- **The maker's inventory limit.** At 500 it withdrew a side after ten net fills, so 42% of
  the session had no two-way market. At 2,000, 0.3%. The skew is now set by how far quotes
  lean *at the limit*, so the two settings no longer multiply.

## The viewer

`/watch` (or `build/release/arena`) draws a purple-grid desktop in 24-bit colour, with no
curses and no libraries. It makes an order book something you watch move, and shows the
central claim, a slow participant losing to a fast one, as one bar shrinking while another
grows.

| Window | Shows |
|---|---|
| **ORDER BOOK** | Asks above, bids below, size as bars, with the fundamental on the dividing line. Drawn from the engine itself |
| **PARTICIPANTS** | Per strategy: reaction delay, position, and marked value as a bar either side of zero |
| **REACTION DELAY** | Each delay on a log slider from 1 µs to 1 ms, labelled *simulated, not measured* |
| **SESSION** | Seed, simulated clock, orders, trades, and the book-to-value gap |
| **TAPE** | Prints as they happen, with the aggressor |
| **! NOTABLE** | A print that moved the mid several ticks, or a message from `f` |

**How a frame is drawn.** `arena/ui.hpp` composes a grid of cells and knows nothing about
trading. The frame goes out in one `fwrite`, with a colour escape only where the colour
changes, so a full repaint is a few kilobytes and is never seen half-drawn.
`arena/term.hpp` does the four things a terminal is needed for (24-bit colour, window size,
a key without blocking, restoring everything on exit), and only `arena/arena.cpp` includes
it. ncurses would have added a dependency to save time on a repaint that is already cheap.

| Key | Action |
|---|---|
| `space` | pause and resume |
| `+` / `-` | faster or slower, from 1/64× to 512× |
| `1`–`9` | focus a participant |
| `f` | re-read the `--script` file and restart from the same seed |
| `q` | quit, or back to the app's prompt |

There is no mouse support. Movable panes would need hit-testing, z-order and a focus model:
a window manager for six fixed panes. The layout reflows when the terminal is resized.

**The viewer cannot change what it watches.** It paces the session by simulated time, and
keys only change how fast it asks for the next instant. A session watched at 512×, paused
and resumed, has the same hash as the same seed run headless. That is why `f` restarts
rather than injecting: a participant appearing when a human pressed a key would be a
wall-clock event, and the session could never be reproduced.

`still=NS` (`--still=NS`) runs headless to one simulated instant and prints one frame, which
is how the README's picture stays reproducible. The viewer needs 24-bit colour and at least
96×32 characters, and says so rather than drawing garbage.

## Running it

```
> /watch                                  # watch the default market
> /sweep --seeds=24 --replay-check        # the headline sweep
> /sweep --null                           # the control: nobody informed
> /sweep --help                           # every field you can set or sweep
```

Outside the app, `build/release/arena --maker-reaction=1000000` watches a slow maker get run
over, and `bash bench/run_arena_evidence.sh windows` regenerates `results/arena-windows/`.
`make test` includes `test_arena`, which holds the mirror against the engine, the
scheduler's total order, replay, the accounting identity and the reaction model.

## Writing a strategy

A strategy is a plain struct. It inherits from nothing, never touches the engine and cannot
read a clock. The whole contract is in `arena/strategy.hpp`: `name`, `reaction_ns`,
`timer_ns`, `reset(seed)` and `on_event(view)`.

```cpp
struct MyStrategy {
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

`ActionBatch` holds up to six actions, because a maker replacing both sides needs four, and
spending four reaction windows on it would measure the API instead of the strategy.

**Strategies are compiled in, not loaded.** No plugin, no scripting language, no socket.
The cost is real: someone adding one needs the source and a compiler, and has to rebuild.
There are two ways:

- **A strategy of your own:** `/add path/to/name.hpp` (`make strategy STRATEGY=...`). It is
  copied into `arena/strategies/user/` and built into the lab programs only, so it can never
  reach published evidence. `examples/strategies/imbalance.hpp` states the convention.
- **A built-in one:** a header in `arena/strategies/`, included in `registry.hpp`, a branch in
  `add_named_strategy()`, its name in `kStrategyNames`, and a line in the app's
  `kStrategyNotes`. `test_arena` and `test_app` fail if these disagree. Fields added to
  `RosterConfig` and `set_roster_param()` become settable with `--set=`.

[`arena/strategies/fader.hpp`](../arena/strategies/fader.hpp) is the worked example, written
against `arena/strategy.hpp` alone. It fades the recent move and loses money, the honest
outcome for uninformed flow crossing the spread. `test_arena` checks that it is absent by
default, really trades (938 orders, 964 fills in a 60,000-request session), never sees the
fundamental, and cannot break the accounting identity, the book's tracking or the session's
ability to end. Being off by default, it moved no published number.

### Trading by hand

A live keystroke cannot go into a session: knowing *when* it arrived means reading a wall
clock, which breaks replay. A file of orders stamped in **simulated** time can, and
`/watch script=FILE` plays one as a participant called `you`:

```
2ms    buy   50   99.95      # join the bid
3ms    sell  50  100.05      # and the offer: now you quote both sides
8ms    buy   25   market     # pay the spread to get long
12ms   cancel 1              # pull your first still-live order
```

One order per line: `<at> buy|sell <qty> <price|market>` or `<at> cancel <n>` for your own
n-th live order. Times take `us` and `ms`; prices are on the tick grid. A malformed line is
refused with its line number, never silently ignored. The file is a participant like any
other, so its reaction delay applies: an order stamped at `t` sees the market as of
`t − reaction_ns`. A scripted time is honoured at the first wake at or after it, every
10 µs by default.

`f` re-reads the file and restarts from the same seed, so you can edit and replay.
`test_arena` asserts that what you watch hashes the same as `--script=PATH --seed=N` run
headless, and that a script played twice hashes the same. A recording of someone playing
could never offer that.

### Choosing who is in the market

`/watch NAME,...` adds strategies to the default line-up, and `lineup=` or `--strategies=`
replaces it:

```
> /watch lineup=maker,informed,noise:2
> /sweep --strategies=maker,rival,informed,noise:3,fader
```

Names are comma-separated, each with an optional `:count`; repeated noise traders get
staggered timers. Naming a participant `informed` is what grants it the fundamental: the
line-up decides, never the strategy. An unknown name, a bad count or an empty list is an
error naming the token. The sweep refuses a line-up with no `maker`, since every column it
prints is about the maker. Without `--strategies=`, the default line-up is used, which is
what every published number came from.

A strategy cannot see the fundamental (unless informed), the clock, other participants'
positions, the engine's internals, or its queue position except by tracking the feed.

## Comparing strategies

`/compare` (`make compare`) is the standard way to score a strategy. Every candidate takes
the same seat of the default market on the same named seeds, beside a placebo arm where
the seat never acts. The seat is appended last, never informed, and runs at 10,000 ns set by
the suite, so speed and information are conditions, not part of what is compared.
[`RUNNER.md`](RUNNER.md) is the design. Two measured facts carry it.

**An appended inert seat is invisible.** In `test_arena`'s 30,000-request session at seed
42, the market alone and the market with the placebo appended both hash to
`2853d2127a3486c5`; the placebo inserted at index 3 gives `5a9a688a7ca5b9f8`. The sweep
prints the same three hashes:

```bash
build/release/sweep --values=10000 --seeds=1 --requests=30000 --quiet --strategies=maker,rival,informed,noise:2,momentum
build/release/sweep --values=10000 --seeds=1 --requests=30000 --quiet --strategies=maker,rival,informed,noise:2,momentum,placebo
build/release/sweep --values=10000 --seeds=1 --requests=30000 --quiet --strategies=maker,rival,informed,placebo,noise:2,momentum
```

So the placebo arm *is* the reference market, and every difference from it is caused by the
candidate's orders. The suite checks this on every seed and exits 3 if it ever fails.

**A request bound breaks pairing.** The published numbers end after a number of requests,
and that count includes the candidate's own orders. A busy candidate ends its session
earlier in simulated time and is marked at a different closing fundamental. Under
`max_requests=7,000`, the closing fundamental differed from the placebo's on all 12 (arm,
seed) pairs of the smoke block. So suite sessions end on 25 ms of simulated time, and every
arm closes on the same fundamental, which `test_arena` checks. On the 64 standard seeds the
placebo passes the health gate everywhere, with a worst mean |mid − fundamental| of
2.03 ticks. Suite numbers are therefore a different quantity from the published ones, and go
to `runs/`, never `results/`.

## Known limits, found by testing

Both were found by probing the finished arena, and both are pinned by regression tests.

**At most 64 participants.** Past that bound, an event's marks were silently dropped and those
strategies never got an own-event wake: with 104 noise traders, the first 64 took 151.9 fills
each and the rest 55.9. Nothing crashed. Larger line-ups are now refused. The ceiling is hard
on purpose: changing how wakes are marked would change wake order, move every session hash
and invalidate the committed evidence.

**A session that stops sending orders now ends.** A line-up of only the maker used to hang:
once its quotes were set it sent nothing, so the request bound was never reached. A static
"can this line-up trade?" check was wrong, since whether a strategy ever sends an order is a
property of its code. Instead, if `stall_limit_ns` of *simulated* time passes with no request,
the session ends and sets `stats().stalled`, which the sweep reports as a warning. Simulated
time makes it fire identically on every replay; a wall-clock timeout would not.

**The renderer is tested for its invariants, not its looks:** one cell per character, clipping
at every edge, and a composed frame pinned by hash.

## What is not built

- **Loading a strategy at runtime**, in any form. It is compiled in; see "Writing a strategy".
- More than one instrument on screen: one legible ladder beats four grids of numbers.
- Live keyboard trading, for the replay reason above. A scripted order file does the job.
- Fees beyond a flat per-unit fee, which defaults to zero and is reported when set.
