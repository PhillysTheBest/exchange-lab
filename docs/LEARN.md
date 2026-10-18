# Learning with exchange-lab

A guided path through exchange-lab in eight short exercises, each building on the last. It
starts with one order book and ends with a strategy of your own, scored, controlled and
reproduced. It is written for students meeting market microstructure for the first time,
and for researchers who want the tour before the design documents.
[`GLOSSARY.md`](../GLOSSARY.md) defines every term used here.

**What you are working with.** A real price-time-priority matching engine sits under a seeded
simulated market, in which strategies trade with a reaction delay you choose. It is not a
backtester, and it forecasts nothing about a real venue. The market is invented: its
fundamental is a one-tick random walk drawn from the seed (`arena/fair_value.hpp`), fitted
to no data, and missing the mechanisms listed in the README's
[What this instrument cannot show](../README.md#what-this-instrument-cannot-show). What it
offers instead is something real data cannot: the same market, re-run exactly, with one
thing changed.

**Its headline is a negative result, and you will reproduce it.** What is 10 µs worth? Less
than 24 seeds can resolve. 1 ms costs real money, but a null run with nobody informed keeps
96% of that cost, so the cost is competition for uninformed flow, not adverse selection.
Then a positive control failed. The controls changed the answer twice
([The first result is a negative one](../README.md#the-first-result-is-a-negative-one));
exercise 7 is about why.

Three rules apply to any arena number:

1. **It is simulated.** A reaction delay is an input you set, not a measurement, and it
   never mixes with the engine's measured timings in the README.
2. **Market health comes first.** A book that has come loose from the fundamental is not a
   market, and nothing read off it counts (`docs/ARENA.md`, "The experiment").
3. **Report the paired difference, with its standard error.** "Underpowered" and "nothing
   there" are different claims.

**Setup.** You need g++ 13 and GNU make, nothing else. Run `make arena` and `make repl` from
the repository root. On Windows, use MSYS2 UCRT64's `mingw32-make` and add `.exe`. The
commands were checked on Linux.

## 1. See an order book

**Goal:** price-time priority, self-trade prevention and a market order; then a whole market.

```bash
build/release/repl < tools/repl_demo.txt
printf 'add ACME\nsell ACME 30 10.00 as 2\nsell ACME 30 10.00 as 3\nbuy ACME 40 10.05 as 4\nbook ACME\n' | build/release/repl
build/release/arena --still=60000000 --seed=42
```

**Look for.** In the demo the buy for 150 at 10.02 fills 70 at 10.01, then 80 at 10.02: better
price first, then oldest first. A market order's remainder is cancelled, never rested, and
`cancel 1 as 9` is refused. Predict the `printf` run first: which seller fills, at what price?
The still frame shows ORDER BOOK (the fundamental on its middle line), PARTICIPANTS (delay,
position, marked value) and SESSION (the book-value gap), and ends with a seed and a hash
(`still at 60ms, seed 42, session hash 207af9614ae8f2f4` when written). Run it twice.

**Think about.** Why does the engine cancel participant 3's resting ask rather than let it trade
with itself? What would two strategies sharing an owner id lose (`docs/ARENA.md`, "The market")?
**Read next:** the README's [What it does](../README.md#what-it-does); `help` in the REPL.

## 2. Trade by hand

**Goal:** become a participant. A file of orders in simulated time plays as a strategy called
`you`; keystrokes cannot, as a wall clock in a session breaks replay (`docs/ARENA.md`).

```bash
build/release/arena --script=tools/arena_script_demo.txt      # live: 24-bit colour, 96x32 or larger
build/release/arena --script=tools/arena_script_demo.txt --still=60000000 --seed=42
build/release/arena --script=tools/arena_script_demo.txt --still=11000000 --seed=42
```

**Look for.** Live keys: `space` pause, `+`/`-` speed, `1`-`9` focus a participant, `f` re-read
the script and restart from the same seed, `q` quit; none changes what happens
(the README's [The arena](../README.md#the-arena)). In the frame, a row `7 you` at 10us and your 100.08 offer resting at
60 ms. The hash and every other row differ from exercise 1's: one more participant changes the
whole session. An off-grid price in a copy of the script is refused, naming file and line.

**Think about.** Account for your final position order by order, stepping `--still` through the
first 13 ms: on seed 42, is the 99.95 bid still live when `cancel 1` fires at 12 ms, as the
script's comment expects? Why does an order stamped at `t` see the market as of `t` minus your
reaction delay (`docs/ARENA.md`)? **Read next:** `docs/ARENA.md` "Trading by hand".

## 3. Read a strategy against its contract

**Goal:** a strategy is a struct with five members, a view in, at most six actions out, and no
way to reach the engine, clock or fundamental (`arena/strategy.hpp`).

```bash
less arena/strategy.hpp examples/strategies/imbalance.hpp arena/strategies/fader.hpp
```

**Look for.** The wake set: own events, a touch change, a timer, a signal; a burst inside one
reaction window collapses into one decision (`arena/strategy.hpp`). `MarketView::book` is
the market as of `now_ns - reaction_ns` . Both examples wait while `in_flight`, the
count of unacknowledged orders, is non-zero (`arena/strategy.hpp`, `imbalance.hpp`, `fader.hpp`).
`signal` is 0 for all but the informed participant . No floating point anywhere.

**Think about.** What would `imbalance` do if it ignored `in_flight`? Why is the ban on floating
point a replay rule, not a style rule (`docs/ARENA.md`)? Both examples are uninformed and
take liquidity: what sign do you predict for their spread earned? **Read next:**
`docs/ARENA.md` "The reaction model" and "Writing a strategy".

## 4. Write and install a first strategy

**Goal:** a strategy of your own, in the lab binaries only. `make strategy` copies it into the
git-ignored `arena/strategies/user/`; evidence binaries and tests never contain it
(`Makefile`; `suite --candidates=imbalance` is refused as unknown). `<name>.hpp` defines
struct `<Name>` in `px::arena::strategies`, not a built-in name (`imbalance.hpp`). A start
that joins the touch on both sides and pulls what it leaves behind; save it as
`$HOME/strategies/joiner.hpp`:

```cpp
#pragma once
#include "arena/strategy.hpp"
namespace px::arena::strategies {
struct Joiner {
    const char* name = "joiner";
    SimTime reaction_ns = 10'000;
    SimTime timer_ns = 0;
    Qty qty = 10;
    std::int64_t max_position = 100;
    void reset(std::uint64_t) {}
    ActionBatch on_event(const MarketView& v) {
        ActionBatch out;
        if (v.in_flight != 0) return out;
        const Price bid = v.best(Side::Buy), ask = v.best(Side::Sell);
        if (bid == 0 || ask == 0) return out;
        bool have_bid = false, have_ask = false;
        for (std::size_t i = 0; i < v.my_order_count; ++i) {
            const BookMirror::OrderRef& o = v.my_orders[i];
            if (o.price != (o.side == Side::Buy ? bid : ask)) out.push(Action::cancel(o.id));
            else (o.side == Side::Buy ? have_bid : have_ask) = true;
        }
        if (!have_bid && v.position < max_position) out.push(Action::limit(Side::Buy, bid, qty));
        if (!have_ask && v.position > -max_position) out.push(Action::limit(Side::Sell, ask, qty));
        return out;
    }
};
} // namespace px::arena::strategies
```

```bash
make strategy STRATEGY=examples/strategies/imbalance.hpp
make strategy STRATEGY=$HOME/strategies/joiner.hpp
build/release/arena_lab --strategies=maker,rival,informed,noise:2,momentum,joiner --still=20000000 --seed=42
```

**Look for.** `installed joiner -> arena/strategies/user/joiner.hpp`, the commands to compare,
explore and watch it, and a `7 joiner` row in the frame. A built-in name or a non-`.hpp` file is
refused (`Makefile`). Installing is a copy: after every edit, run `make strategy` again.

**Think about.** What does the joiner do when a side of the book is empty, or the touch moves
away from its order? What does pulling a left-behind order cost it? **Read next:**
the README's [Add your own strategy](../README.md#add-your-own-strategy).

## 5. Score it

**Goal:** the standard experiment: your strategy in one seat of the reference market, on named
seeds, beside a placebo arm whose seat never acts, every session replayed.

```bash
make compare CANDIDATES=imbalance BLOCK=smoke   # 4 seeds: does it run, does it trade?
make compare CANDIDATES=imbalance               # the standard block, 64 seeds
less runs/imbalance-standard/report.txt
make compare CANDIDATES=joiner,maker            # several candidates, identical conditions
```

**Look for**, section by section in `report.txt` (a rerun replaces it; `OUT=runs/<name>` keeps one):

- **Header:** seeds, the seat (#6, never informed, run at 10,000 ns whatever you declared), and
  the fingerprint; two runs are comparable only if their fingerprints match.
- **Market health**, then **the seat**, then **paired, seed by seed**, "the statistic to read".
  `imbalance` gave −28.32 ± 11.23 on smoke, *not separated*; −34.92 ± 3.09 on standard, *separated*.
- **Metric by metric:** value = spread earned + adverse markout + carry to close, exactly
  (`docs/REPORT.md`); `fills per order` can exceed 1 (`docs/REPORT.md`).
- **Where the money came from:** each reference participant's change, summing to minus the
  seat's value on every seed (the market is closed); then the test count and the checks.

**Think about.** The smoke footer says 4 seeds cannot separate anything, yet its verdict column
can print "separated" (try `make compare CANDIDATES=joiner,maker BLOCK=smoke`): which do you
trust? Why does the suite fix the seat's delay (`docs/RUNNER.md`)? The joiner's maker
share is not quite 100%: how does a stale view make a passive quote cross? **Read next:**
`docs/REPORT.md`; `docs/RUNNER.md` "The seat" and "The health gate".

## 6. Change one thing

**Goal:** an experiment with one variable: the maker's delay, one parameter, or a family.

```bash
build/release/sweep --help
build/release/sweep --seeds=8 --values=1000,10000,1000000
build/release/sweep_lab --strategies=maker,rival,informed,noise:2,momentum,joiner --seeds=4 --values=1000,1000000
build/release/sweep_lab --strategies=maker,rival,informed,noise:2,momentum,joiner --seeds=4 --values=1000,1000000 --set=noise.timer_ns=5000
make compare CANDIDATES=joiner SUITE=all
cat runs/joiner-all-standard/families.txt
```

**Look for.** `sweep` prints health, levels, then the paired table. At 8 seeds the 10 µs row
came out +108.00 (range −309.40 .. +293.25), opposite in sign to the published −39.96 ± 56.19
over 24 seeds: an effect below the resolvable size. The 1 ms row separates even at 8 seeds.
`--strategies=` adds a row per participant. `SUITE=all` writes one directory per family
(`standard`; `wide`, `maker.half_spread_ticks=4`; `busy`, `noise.timer_ns=5000`) and a
`families.txt` that never pools them (`docs/REPORT.md`).

**Think about.** When the maker slows from 1 µs to 1 ms, whose row picks up its flow? A `sweep`
session ends after 200,000 requests: with busier noise, does it cover the same simulated time,
and why does the suite end on 25 ms instead (`docs/ARENA.md`)? **Read next:**
`docs/ARENA.md` "The experiment" and "Comparing strategies".

## 7. The controls

**Goal:** why the headline answer changed twice; then check the evaluator itself.

```bash
build/release/sweep --null --seeds=8 --values=1000,1000000
make controls
```

**The story.** The tidy version: a slow maker's stale quotes get picked off by the informed
trader. Remove the informed trader and 96% of the 1 ms effect remains; the ledger sends the slow
maker's loss to the rival (the README's [The arena](../README.md#the-arena)). Then a positive control tried to
build a stale quoter on purpose and all three designs failed the health line, so the post-fill
column's sensitivity is **not established**; its near-zero reading rests on an argument (the
fundamental is a martingale), not a calibration (`docs/ARENA.md`, "The positive control, and the limitation it found").

**Look for.** Health first: at 1 µs this null run printed a gap of 38.72 ticks, against 4.97 in
exercise 6's run; the published null shows 41–47 (the README's [What else is in results/arena-windows/](../README.md#what-else-is-in-resultsarena-windows)). `make controls` ends
`8/8 controls held`: placebo zero and inert, a common close, replay and the identities hold, an
A/A pair identical, 9,999 ns against 10,000 ns not separated, `roundtrip` losing in spread. Each
control has a planted defect in `test_arena` that makes it fail (`docs/CONTROLS.md`).

**Think about.** The suite admits a session only if its mean gap is at most 4 ticks
(`docs/RUNNER.md`), and dropped its null family because the placebo failed that gate on
56 of 64 seeds (`docs/REPORT.md`). How much weight can the 96% bear, and how would you
test it? Why is a null control not enough alone (`docs/ARENA.md`)? **Read next:**
`docs/ARENA.md`, "What the evidence actually says" to "Four things that were wrong first";
`docs/CONTROLS.md`.

## 8. Reproduce a run

**Goal:** check a result instead of trusting it: yours, and the project's.

```bash
make reproduce RUN=runs/imbalance-standard
build/release/sweep --seeds=1 --quiet | tail -1
make compare CANDIDATES=joiner
# edit joiner.hpp (say qty = 20), reinstall it, and reproduce the run you just made:
make strategy STRATEGY=$HOME/strategies/joiner.hpp
make reproduce RUN=runs/joiner-standard
```

**Look for.** Every file `identical`, then `REPRODUCED: every file the run wrote came out the
same`. The sweep's last line gives the seed-42 hashes `aebce54544c10ca5 35859f327b69af22
800430353d000562 9f777b5eb3508703`, which the README publishes for Windows and Linux
alike. After the edit, the reproduction is refused before anything runs (exit 2): `joiner has
changed since the run (checksum ... then, ... now): not the same strategy`.

**Think about.** Why are `suite.cfg`'s `run.*` and `build.*` lines recorded but never conditions
(`docs/REPRODUCE.md`)? Cross-platform replay of an installed strategy is checked, not
promised (`docs/REPRODUCE.md`): what in your header could break it? **Read next:**
`docs/REPRODUCE.md`.

## Where to go from here

Write a second strategy, compare it with your first on the standard block and `SUITE=all`, and
report it as the project does: health first, the paired difference with its standard error, and
a plain statement when an interval straddles zero. Then work out from `arena/fair_value.hpp` why
the expected adverse markout must be zero for any strategy you write (the README's [And the control says it is not adverse selection](../README.md#and-the-control-says-it-is-not-adverse-selection)).
