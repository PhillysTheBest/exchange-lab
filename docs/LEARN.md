# Learning with exchange-lab

A guided path through exchange-lab in eleven short exercises, each building on the last. It
starts with one order book, goes on to a strategy of your own, scored, controlled and
reproduced, brings in a file of market data, runs one study the way a paper should run it,
and ends by changing a strategy's settings and the market itself without weakening the
experiment. It is written for students meeting market microstructure for the first time,
and for researchers who want the tour before the design documents.
[`GLOSSARY.md`](../GLOSSARY.md) defines every term used here.

**What you are working with.** A real price-time-priority matching engine sits under a seeded
simulated market, in which strategies trade with a reaction delay you choose. It is not a
backtester, and it forecasts nothing about a real venue. The market is invented: its
fundamental is a one-tick random walk drawn from the seed (`arena/fair_value.hpp`), fitted
to no data, and missing the mechanisms listed in `docs/RESULTS.md`'s
[What this instrument cannot show](RESULTS.md#what-this-instrument-cannot-show). What it
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
   never mixes with the engine's measured timings in `docs/ENGINE.md`.
2. **Market health comes first.** A book that has come loose from the fundamental is not a
   market, and nothing read off it counts ([The experiment](ARENA.md#the-experiment) in `docs/ARENA.md`).
3. **Report the paired difference, with its standard error.** "Underpowered" and "nothing
   there" are different claims.

**Setup.** You need g++ 13 and GNU make, nothing else. From the repository root:

```bash
make app                      # Windows: mingw32-make app, in MSYS2 UCRT64
build/release/exchange-lab
```

Lines starting with `>` are typed at the app's prompt; the other lines run in an ordinary
shell. `/learn N` shows exercise N inside the app, and `/help NAME` explains any command.
Every command here was checked on Linux.

## 1. See an order book

**Goal:** price-time priority, self-trade prevention and a market order; then a whole market.
The first line plays the REPL's demo from a shell. Then, at the prompt, `/book` opens the same
order book for you to type into, and `quit` brings you back.

```
build/release/exchange-lab -c /book < tools/repl_demo.txt
> /book
add ACME
sell ACME 30 10.00 as 2
sell ACME 30 10.00 as 3
buy ACME 40 10.05 as 4
book ACME
quit
> /watch still=60000000 seed=42
```

**Look for.** In the demo the buy for 150 at 10.02 fills 70 at 10.01, then 80 at 10.02: better
price first, then oldest first. A market order's remainder is cancelled, never rested, and
`cancel 1 as 9` is refused. Predict the four orders you type first: which seller fills, at what price?
The still frame shows ORDER BOOK (the fundamental on its middle line), PARTICIPANTS (delay,
position, marked value) and SESSION (the book-value gap), and ends with a seed and a hash
(`still at 60ms, seed 42, session hash 207af9614ae8f2f4` when written). Run it twice.

**Think about.** Why does the engine cancel participant 3's resting ask rather than let it trade
with itself? What would two strategies sharing an owner id lose ([The market](ARENA.md#the-market) in `docs/ARENA.md`)?
**Read next:** [What it does](ENGINE.md#what-it-does) in `docs/ENGINE.md`; `help` inside `/book`.

## 2. Trade by hand

**Goal:** become a participant. A file of orders in simulated time plays as a strategy called
`you`. Keystrokes cannot, because a wall clock in a session would break replay
([Trading by hand](ARENA.md#trading-by-hand)).

```
> /watch script=tools/arena_script_demo.txt                  # live: 24-bit colour, 96x32 or larger
> /watch script=tools/arena_script_demo.txt still=60000000 seed=42
> /watch script=tools/arena_script_demo.txt still=11000000 seed=42
```

**Look for.** Live keys: `space` pause, `+`/`-` speed, `1`-`9` focus a participant, `f` re-read
the script and restart from the same seed, `q` back to the prompt; none changes what happens
([The viewer](ARENA.md#the-viewer) in `docs/ARENA.md`). In the frame, a row `7 you` at 10us and your 100.08 offer resting at
60 ms. The hash and every other row differ from exercise 1's: one more participant changes the
whole session. An off-grid price in a copy of the script is refused, naming file and line.

**Think about.** Account for your final position order by order, stepping through the first
13 ms with `still=`: on seed 42, is the 99.95 bid still live when `cancel 1` fires at 12 ms, as
the script's comment expects? Why does an order stamped at `t` see the market as of `t` minus your
reaction delay (`docs/ARENA.md`)? **Read next:** [Trading by hand](ARENA.md#trading-by-hand) in `docs/ARENA.md`.

## 3. Read a strategy against its contract

**Goal:** a strategy is a struct with five members, a view in, at most six actions out, and no
way to reach the engine, clock or fundamental (`arena/strategy.hpp`).

```bash
less arena/strategy.hpp examples/strategies/imbalance.hpp arena/strategies/fader.hpp
```

**Look for.** The wake set: own events, a touch change, a timer, a signal; a burst inside one
reaction window collapses into one decision (`arena/strategy.hpp`). `MarketView::book` is
the market as of `now_ns - reaction_ns`. Both examples wait while `in_flight`, the count of
unacknowledged orders, is non-zero. `signal` is 0 for all but the informed participant. No
floating point anywhere.

**Think about.** What would `imbalance` do if it ignored `in_flight`? Why is the ban on floating
point a replay rule, not a style rule (`docs/ARENA.md`)? Both examples are uninformed and
take liquidity: what sign do you predict for their spread earned? **Read next:**
[The reaction model](ARENA.md#the-reaction-model) and [Writing a strategy](ARENA.md#writing-a-strategy) in `docs/ARENA.md`.

## 4. Write and install a first strategy

**Goal:** a strategy of your own. `/add` copies it into the git-ignored
`arena/strategies/user/` and rebuilds the lab programs. The evidence programs and the tests
never contain it: `build/release/suite --candidates=imbalance` is refused as unknown. The
convention is in `imbalance.hpp`: `<name>.hpp` defines struct `<Name>` in
`px::arena::strategies`, and the name must not be a built-in one.

Here is a start that joins the touch on both sides and pulls what it leaves behind. Save it
as `$HOME/strategies/joiner.hpp`:

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

```
> /add examples/strategies/imbalance.hpp
> /add ~/strategies/joiner.hpp
> /strategies
> /watch joiner still=20000000 seed=42
```

**Look for.** `installed joiner -> arena/strategies/user/joiner.hpp`, `joiner` under
"Installed by you", and a `7 joiner` row in the frame: names given to `/watch` join the default
line-up. A built-in name or a non-`.hpp` file is refused (`Makefile`). Installing is a copy:
after every edit, `/add` it again.

**Think about.** What does the joiner do when a side of the book is empty, or the touch moves
away from its order? What does pulling a left-behind order cost it? **Read next:**
[Writing a strategy](ARENA.md#writing-a-strategy) in `docs/ARENA.md`.

## 5. Score it

**Goal:** the standard experiment: your strategy in one seat of the reference market, on named
seeds, beside a placebo arm whose seat never acts, every session replayed.

```
> /compare imbalance smoke          # 4 seeds: does it run, does it trade?
> /compare imbalance                # the standard block, 64 seeds
> /runs
> /report imbalance-standard
> /compare joiner,maker             # several candidates, identical conditions
```

**Look for**, section by section in the report (a rerun replaces it; in a shell,
`make compare CANDIDATES=... OUT=runs/<name>` keeps one):

- **Header:** seeds, the seat (#6, never informed, run at 10,000 ns whatever you declared), and
  the fingerprint; two runs are comparable only if their fingerprints match.
- **Market health**, then **the seat**, then **paired, seed by seed**, "the statistic to read".
  `imbalance` gave −28.32 ± 11.23 on smoke, *not separated*; −34.92 ± 3.09 on standard, *separated*.
- **Metric by metric:** value = spread earned + adverse markout + carry to close, exactly
  (`docs/REPORT.md`); `fills per order` can exceed 1 (`docs/REPORT.md`).
- **Where the money came from:** each reference participant's change, summing to minus the
  seat's value on every seed (the market is closed); then the test count and the checks.

**Think about.** The smoke footer says 4 seeds cannot separate anything, yet its verdict column
can print "separated" (try `/compare joiner,maker smoke`): which do you
trust? Why does the suite fix the seat's delay (`docs/RUNNER.md`)? The joiner's maker
share is not quite 100%: how does a stale view make a passive quote cross? **Read next:**
[`docs/REPORT.md`](REPORT.md); [The seat](RUNNER.md#the-seat) and [The health gate](RUNNER.md#the-health-gate) in `docs/RUNNER.md`.

## 6. Change one thing

**Goal:** an experiment with one variable: the maker's delay, one parameter, or a family.

```
> /sweep --help
> /sweep --seeds=8 --values=1000,10000,1000000
> /sweep --strategies=maker,rival,informed,noise:2,momentum,joiner --seeds=4 --values=1000,1000000
> /sweep --strategies=maker,rival,informed,noise:2,momentum,joiner --seeds=4 --values=1000,1000000 --set=noise.timer_ns=5000
> /compare joiner all
> /report joiner-all-standard
```

**Look for.** `/sweep` prints health, levels, then the paired table. At 8 seeds the 10 µs row
came out +108.00 (range −309.40 .. +293.25), opposite in sign to the published −39.96 ± 56.19
over 24 seeds: an effect below the resolvable size. The 1 ms row separates even at 8 seeds.
`--strategies=` adds a row per participant. `all` writes one directory per family
(`standard`; `wide`, `maker.half_spread_ticks=4`; `busy`, `noise.timer_ns=5000`) and a
`families.txt` that never pools them (`docs/REPORT.md`).

**Think about.** When the maker slows from 1 µs to 1 ms, whose row picks up its flow? A `/sweep`
session ends after 200,000 requests: with busier noise, does it cover the same simulated time,
and why does the suite end on 25 ms instead (`docs/ARENA.md`)? **Read next:**
[The experiment](ARENA.md#the-experiment) and [Comparing strategies](ARENA.md#comparing-strategies) in `docs/ARENA.md`.

## 7. The controls

**Goal:** why the headline answer changed twice; then check the evaluator itself.

```
> /sweep --null --seeds=8 --values=1000,1000000
> /controls
```

**The story.** The tidy version: a slow maker's stale quotes get picked off by the informed
trader. Remove the informed trader and 96% of the 1 ms effect remains; the ledger sends the slow
maker's loss to the rival ([the ledger](RESULTS.md#and-the-ledger-says-where-the-money-went) in `docs/RESULTS.md`). Then a positive control tried to
build a stale quoter on purpose and all three designs failed the health line, so the post-fill
column's sensitivity is **not established**; its near-zero reading rests on an argument (the
fundamental is a martingale), not a calibration ([The positive control, and the limitation it found](ARENA.md#the-positive-control-and-the-limitation-it-found)).

**Look for.** Health first: at 1 µs this null run printed a gap of 38.72 ticks, against 4.97 in
exercise 6's run; the published null shows 41–47 ([What else is in results/arena-windows/](RESULTS.md#what-else-is-in-resultsarena-windows)). `/controls` ends
`8/8 controls held`: placebo zero and inert, a common close, replay and the identities hold, an
A/A pair identical, 9,999 ns against 10,000 ns not separated, `roundtrip` losing in spread. Each
control has a planted defect in `test_arena` that makes it fail (`docs/CONTROLS.md`).

**Think about.** The suite admits a session only if its mean gap is at most 4 ticks
(`docs/RUNNER.md`), and dropped its null family because the placebo failed that gate on
56 of 64 seeds (`docs/REPORT.md`). How much weight can the 96% bear, and how would you
test it? Why is a null control not enough alone (`docs/ARENA.md`)? **Read next:**
[What the evidence actually says](ARENA.md#what-the-evidence-actually-says) to
[Four things that were wrong first](ARENA.md#four-things-that-were-wrong-first-and-what-they-cost)
in `docs/ARENA.md`; [`docs/CONTROLS.md`](CONTROLS.md).

## 8. Reproduce a run

**Goal:** check a result instead of trusting it: yours, and the project's.

```
> /reproduce imbalance-standard
> /sweep --seeds=1 --quiet
> /compare joiner
# edit joiner.hpp (say qty = 20), reinstall it, and reproduce the run you just made:
> /add ~/strategies/joiner.hpp
> /reproduce joiner-standard
```

**Look for.** Every file `identical`, then `REPRODUCED: every file the run wrote came out the
same`. The sweep's last line gives the seed-42 hashes `aebce54544c10ca5 35859f327b69af22
800430353d000562 9f777b5eb3508703`, which [`docs/RESULTS.md`](RESULTS.md#the-same-answer-on-both-platforms-to-the-last-digit)
publishes for Windows and Linux alike. After the edit, the reproduction is refused before anything runs (exit 2): `joiner has
changed since the run (checksum ... then, ... now): not the same strategy`.

**Think about.** Why are `suite.cfg`'s `run.*` and `build.*` lines recorded but never conditions
(`docs/REPRODUCE.md`)? Cross-platform replay of an installed strategy is checked, not
promised (`docs/REPRODUCE.md`): what in your header could break it? **Read next:**
[`docs/REPRODUCE.md`](REPRODUCE.md).

## 9. Bring a file of market data

**Goal:** turn a data file into a tape, play it back through the engine, and set the
simulated market to resemble one, keeping in mind what each can and cannot tell you.

```
> /data add examples/data/DEMO_2012-06-21_34200000_57600000_message_3.csv
> /data show demo-2012-06-21
> /replay demo-2012-06-21
> /replay demo-2012-06-21 maker
> /calibrate --known=known
> /calibrate known
> /compare maker smoke market=known
```

**Look for.** The sample is a made-up LOBSTER file, so it carries no vendor's licence: 771 events, read
order by order. `/replay` alone runs the replay's control and ends `The control holds: replay
plays this tape back as it was.` With `maker` in it, the report opens `History cannot react`
and counts where history and the strategy collided: 1300 of history's recorded flow traded
with the maker instead of the orders it was recorded against. `/calibrate --known=known`
writes a tape from sessions whose parameters are known (3 / 40 / 20000); `/calibrate known`
gives them back, `64/64 seeds pass`, and writes `markets/known.cfg`. The `/compare` report
says `calibrated from the tape known (L3): like it in spread, size, pace, and in nothing else`,
and its fingerprint, `85106d50923a64eb`, is not the standard suite's `d461c30ab2446acf`.

**Think about.** `/calibrate demo-2012-06-21` refuses the sample as too short: why does
calibration need many prints? A calibrated market keeps the standard informed trader, so what
would a strategy that beats it have learned about the real market? Why does a replay flatter a
market maker? **Read next:** [`docs/DATA.md`](DATA.md).

## 10. A study, start to finish (advanced)

**Goal:** the whole discipline in one go: say what you expect before you look, tune on one set
of seeds, confirm once on another, read the uncertainty, check the market you ran on, and ask
whether the answer survives a change. Do exercises 4, 5 and 9 first: this one uses `imbalance`
and the `known` tape.

**1. A hypothesis, written down first.** "`imbalance` does better than chance." Chance is
`zi`, the built-in zero-intelligence strategy: random limit orders around the mid, no view at
all. What would refute it: the row `imbalance vs zi` not separating, or separating below zero.

```
> /compare zi,imbalance
```

**2. Tune on the standard block, and only there.** A pickier variant that acts only when the
touch is five times lopsided, not three:

```bash
mkdir -p ~/strategies
sed 's/Imbalance/Picky/; s/"imbalance"/"picky"/; s/ratio = 3;/ratio = 5;/' examples/strategies/imbalance.hpp > ~/strategies/picky.hpp
```

```
> /add ~/strategies/picky.hpp
> /compare imbalance,picky
```

**3. Confirm once, on seeds you have not looked at.**

```
> /compare imbalance,picky holdout
```

**4. Check the market, then whether the answer survives a change.**

```
> /calibrate known holdback=30
> /facts known
> /compare picky sensitivity
```

**Look for.** Step 1: `imbalance vs zi` is +56.90 ± 4.87, separated, ahead on 61 of 64 seeds:
`imbalance` loses (−34.92) but loses much less than chance (−91.82). Its `pairing` is −52%:
pairing *added* variance, because two unrelated strategies share little noise. With three
value comparisons, the report prints a Holm correction beside them, and all three survive it.
Step 2: `picky vs imbalance` is +19.05 ± 1.88, `pairing` 81%: two versions of one strategy share
most of their noise, so a small change is resolvable (±5.25). Step 3: the holdout report says
`confirmatory`, and the gain holds on seeds the tuning never saw, +18.27 ± 2.27. Report both
runs, side by side, never pooled. Step 4: the calibration prints `NOT UNIQUELY IDENTIFIED`
(3 / 50 / 20000 fits as well as the 3 / 40 / 20000 it recovered). Its held-back part puts the
market 9.3% from the unseen pace, against the 12.0% by which the tape's own two parts differ.
`/facts` shows the standard market's lag-1 return autocorrelation, +0.131, outside its ±0.066
band. `picky vs placebo` holds its sign in 6 of 6 conditions, but its size runs from 26%
(`no-rival`) to 111% of standard's.

**Think about.** What would you have concluded from step 1 against the placebo alone? If you
had tried twenty ratios on the standard block and kept the best, what would the holdout run
still tell you, and what would it not? The gain held, but `picky` still loses money against
the placebo in every condition: what is the hypothesis you have actually confirmed? Without the
rival maker, `picky` loses a quarter as much: whose spread was it paying (`/report` the
`no-rival` condition, and read its attribution)? Why can a calibration that recovers its known
answer still not identify it? **Read next:** [The seeds](RUNNER.md#the-seeds),
[The floor: `zi`](RUNNER.md#the-floor-zi) and [Sensitivity](RUNNER.md#sensitivity) in
`docs/RUNNER.md`; [Many comparisons](REPORT.md#many-comparisons) in `docs/REPORT.md`;
[Stylised facts](DATA.md#stylised-facts-what-was-not-fitted) in `docs/DATA.md`.

## 11. Change a setting, then the market

**Goal:** ask a new question without weakening the experiment: change a number in your
strategy from the command line, then write down a market of your own as a suite file, score
against it, and reproduce it. Do exercises 4, 5 and 8 first: this one uses `imbalance`.

**1. A setting on the strategy's name.** `imbalance` names the fields a run may set in its
`settings()` member, so exercise 10's `picky` needs no second file:

```
> /compare imbalance+ratio=5,imbalance
> /sweep --strategies=maker,rival,informed,noise:2,momentum,imbalance --param=imbalance.ratio --values=3,5,8 --seeds=8
```

**2. A suite of your own.** Copy the worked example, the standard market with the seat at
100 µs instead of 10 µs, and score against it:

```bash
mkdir -p suites
cp examples/suites/slow_seat.cfg suites/slow_seat.cfg
```

```
> /compare imbalance suite=slow_seat
```

**3. Reproduce it, without the file.** Change the file, try to reproduce, then move the file
away and try again:

```bash
sed 's/^seat.reaction_ns=100000$/seat.reaction_ns=50000/' examples/suites/slow_seat.cfg > suites/slow_seat.cfg
```

```
> /reproduce imbalance-slow_seat-file
```

```bash
mv suites/slow_seat.cfg ~/slow_seat.cfg
```

```
> /reproduce imbalance-slow_seat-file
```

**4. A suite that is not a market.** A third noise trader, at the standard delay:

```bash
sed 's/^suite.name=slow_seat$/suite.name=crowded/; s/noise:2/noise:3/; s/^seat.reaction_ns=100000$/seat.reaction_ns=10000/' examples/suites/slow_seat.cfg > suites/crowded.cfg
```

```
> /compare imbalance suite=crowded
```

**Look for.** Step 1: `imbalance+ratio=5 vs placebo` is −15.87 ± 3.00 and `imbalance vs
imbalance+ratio=5` −19.05 ± 1.88, exercise 10's `picky` numbers to the digit, because it is the
same strategy. The fingerprint is standard's, `9e8d07b24ee5d562`: a setting belongs to the
candidate, not to the market. The sweep ends with a table for `imbalance` itself: ratios 5 and 8
gain +401.45 and +676.58 on ratio 3, mostly by trading less (4,424 orders, then 1,642, then 88).
Its health table puts the book about 5 ticks from the fundamental, because a sweep session runs
200,000 requests, not the suite's 25 ms. Step 2: the report opens `suite slow_seat v1, block
file`, admits the suite on 64 seeds, and gives `imbalance vs placebo` −7.60 ± 1.94 on 56 fills,
against −34.92 on 310 at 10 µs. The fingerprint is `eabf7a1767548125`, not standard's. Step 3:
the first `/reproduce` is refused before running (exit 2): `suites/slow_seat.cfg has changed
since the run (seat.reaction_ns=100000 in the run, seat.reaction_ns=50000 now): not the same
suite`. With the file gone, the run is rebuilt from its own `suite.cfg`, admitted again, and
every file is `identical`: `REPRODUCED`. Step 4 writes nothing and exits 3: `suite crowded is
not admitted: its placebo fails the health gate on 3 of 64 seeds: 79 (gap), 94 (gap), 105
(gap)`.

**Think about.** Why does a setting leave the fingerprint alone when a suite file moves it?
At 100 µs `imbalance` loses about a fifth as much: divide `spread earned` by `fills` in both reports.
Is the slower seat better per trade, or does it just trade less? The sweep said the same about
`ratio` on a market 5 ticks loose. Which of its numbers would you report, and which would you
re-run as a suite? `crowded` fails on 3 of 64 seeds. Why not drop those three and keep the
other 61? If you had written `slow_seat.cfg` after seeing the sweep, what would a run on it
prove? **Read next:** [A strategy's own settings](RUNNER.md#a-strategys-own-settings) and
[A suite of your own](RUNNER.md#a-suite-of-your-own) in `docs/RUNNER.md`, including when a
`/sweep` is enough; [`docs/REPRODUCE.md`](REPRODUCE.md).

## Where to go from here

Write a second strategy and run exercise 10 on it: a hypothesis first, tuning on the standard
block, one confirmation on `holdout`, `zi` beside it, and `sensitivity` after. Report it as the
project does: health first, the paired difference with its standard error, the Holm line when
there is more than one comparison, and a plain statement when an interval straddles zero. Then work out from `arena/fair_value.hpp` why
the expected adverse markout must be zero for any strategy you write ([And the control says it is not adverse selection](RESULTS.md#and-the-control-says-it-is-not-adverse-selection)).
