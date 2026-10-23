# Bringing your own market data

You can bring a market data file you already have, free or paid, and use it two ways:

- **Calibrate:** set the simulated market so it resembles the file in three statistics
  (spread, trade size against depth, how many trades it takes the price to move), then score
  strategies on it exactly as on the standard market.
- **Replay:** play an order-by-order file back through the matching engine, with one
  strategy trading in it.

The app reads the file from disk and never fetches anything. This document is the design:
the tape every file becomes, the importers, what calibration matches and how it is checked,
replay and its control, and what went wrong on the way.

```
> /data add examples/data/DEMO_2012-06-21_34200000_57600000_message_3.csv
> /data show demo-2012-06-21
> /calibrate --known=known          # a tape whose answer is known in advance
> /calibrate known                  # writes markets/known.cfg
> /compare imbalance market=known   # the standard experiment, on that market
> /replay demo-2012-06-21           # the replay's control: the book must be the tape's
> /replay demo-2012-06-21 maker     # one strategy trading in the recorded market
```

Without the app: `make data FILE=path [OPTS='name=x']`, `make calibrate TAPE=name`,
`make compare CANDIDATES=a MARKET=name` and `make replay TAPE=name [STRATEGY=s]`. The
programs are `build/release/ingest`, `calibrate` and `replay`.

## What it is, and what it is not

- **A calibrated market is still invented.** It is the standard market with at most four
  parameters moved so that three statistics match the file's. It resembles the data in those
  three statistics and in nothing else, so it is **still not a backtester**: what a strategy
  earns on it says nothing about what it would earn on the venue the file came from. The
  referee (paired seeds, the placebo, replay, the controls) works on it unchanged.
- **Replay inherits the backtester's main flaw, and says so first.** History cannot react: a
  recorded order arrives when it did, whatever the strategy does. What a strategy earns in a
  replay is the most the recorded flow could have given a participant it never saw.
- **The file is yours and stays on your disk.** Tapes go to `data/` and calibrated markets to
  `markets/`, both ignored by git. Paid data usually may not be redistributed, so **never
  commit a tape or a source file**, and no result made from one goes in `results/`.
- **How the importers were checked.** Each was written from the format's published
  documentation and tested on small samples made by hand (`examples/data/`), not on a
  purchased or downloaded file. A real file may hold a case the samples do not. If one is
  refused or reads wrongly, the refusal names the line; please open an issue with that line.

## The tape

Every file is converted once into a **tape**, and calibration and replay read only tapes, so
a new format needs one importer and nothing downstream changes. A tape is text: a header of
`key=value` lines (instrument, date, source format, importer and version, the source file's
`cksum`, tick, price and quantity scales, rows skipped and why), then one event a row:

```
t_ns,kind,side,id,price,qty
0,A,B,16113575,58533,18
```

`t_ns` counts nanoseconds from the first event. `kind` is **A** an order rests, **C**
quantity leaves a resting order, **T** a trade, **L** the total at a price is now `qty`, or
**R** forget the book (a snapshot follows). For A and C, `side` is the resting order's; for T
it is the aggressor's, the side that crossed. `id` is the source's order id, and for T the
resting order that traded, 0 when not known. Prices are in ticks. Everything is an integer,
so the same file gives the same tape, byte for byte, on Windows and Linux.

Files come at three depths, and the tape records which:

| Depth | What it holds | `/calibrate` | `/replay` |
|---|---|---|---|
| order by order (L3) | every order, cancel and trade | yes | yes |
| price levels (L2) | the size at each price, and trades | yes | no |
| trades only | prints | refused (see the control below) | no |

`/data show NAME` prints the tape's statistics and what its depth can support.

## The importers

The format is found from the file where it can be, or named with `format=`. A file that
matches none is refused with the list. There are no third-party libraries, so a `.gz` or
`.zst` file is refused with the command that opens it.

| `format=` | Files | Depth |
|---|---|---|
| `csv` | any CSV with a header, or `columns=` naming each column: trades, best quotes or both | trades or L2 |
| `lobster` | LOBSTER message files; the `_orderbook_` file beside it seeds the opening book | L3 |
| `databento` | Databento's CSV export or uncompressed DBN: `mbo`; `mbp-1`, `mbp-10`, `tbbo`; `trades` | L3, L2, trades |
| `tardis` | Tardis.dev `incremental_book_L2`, `book_snapshot_N`, `quotes`, read with the `trades` file beside it | L2; a trades file alone, trades |
| `binance` | data.binance.vision `trades`, `aggTrades`, and futures `bookTicker` with its trades file | trades, L2 |
| `coinbase` | REST trades, and captured feeds one JSON message a line: `level2`, `matches`, `full` | trades, L2, L3 |
| `kraken` | the downloadable trade history, REST trades, a captured WebSocket v2 feed | trades, L2 |
| `itch` | a day's Nasdaq TotalView-ITCH 5.0 file, read for one stock (`symbol=`), streamed | L3 |

An importer never guesses. A side it cannot read is left unknown; rows it cannot use for a
stated reason (an auction trade, a halt) are counted in the header, never dropped silently; a
malformed file is refused, naming the line. A few choices worth knowing:

- A Databento `mbo` trade does not change the book; the fills that follow name the orders it
  took, so each fill becomes the tape's trade, and a trade with no fill after it (a hidden
  order's) is a print that names no order.
- Coinbase Advanced Trade's trade side is not read: its documentation does not say whose side
  it is, and a guess would put the wrong sign on every print.
- Kraken's trade history does not say who crossed the spread, so its prints have no side.
- ITCH's non-printable executions (part of a cross) leave the book as cancels and make no
  print. A day's file is gigabytes, so it is streamed a megabyte at a time.

## What calibration matches: ratios, not the clock

A suite session lasts 25 ms of simulated time; a real market moves over seconds. So
calibration matches the market's **shape** in ratios and keeps the session's clock:

| Statistic, from the tape | Parameter it sets |
|---|---|
| mean quoted spread, in ticks, weighted by time | `maker.half_spread_ticks`, `rival.half_spread_ticks` |
| mean print against mean depth at the touch | `noise.qty` |
| prints it takes the mid to move one tick | `noise.timer_ns` |

A print is the trades one aggressor made at one moment. "Prints per move" is 20 over the mean
squared change of the mid, in ticks, across every run of 20 consecutive prints. Real time
(prints a second) is shown by `/data show` for reference and never matched. Means rather than
medians, because a median of whole ticks or lots moves in jumps a search cannot steer by.

Calibration runs the standard market with no seat on seeds of its own (9001 to 9008, apart
from every block a strategy is scored on), writes those sessions out as a tape, and measures
it with **the same function that measured the data, read at the data's depth**. So a
statistic is never measured one way on the file and another way on the simulation. It then
searches for the parameters that make the two sets of statistics closest: a coarse scan of
all three together on two seeds, then a descent on eight seeds from the scan's three best
points.

Everything else keeps the standard market's value: the price grid, the fundamental's pace,
and above all the informed trader and the parameters [`ARENA.md`](ARENA.md) calls
load-bearing (the maker's inventory limit and skew, the learning rate). No public data shows
them, and a market file that tries to set one is refused.

**A calibrated market must still be a market.** It runs the health gate on all 64 seeds of
the standard block: the book must stay with the fundamental (mean gap at most 4 ticks,
one-sided at most 5% of the time) on every seed, or nothing is written. This is the output
on the known-answer tape:

```
2. Matching the tape: the standard market, no seat, 8 sessions of 25 ms on seeds 9001-9008
   statistic                                             tape      market    apart    sets
   spread: mean spread, hundredths of a tick             445       443       0.4%     maker.half_spread_ticks, rival.half_spread_ticks = 3
   size: mean print / mean touch depth, thousandths      377       389       3.1%     noise.qty = 40
   pace: prints per one-tick move, hundredths            547       552       0.9%     noise.timer_ns = 20000

3. The health gate on the standard block (64 seeds), with no seat: ...
   64/64 seeds pass; the widest mean gap is 2.59 ticks, on seed 85
```

## The known-answer control

The project tests its evaluator against answers known in advance, and calibration gets the
same. Sessions run with known parameters (half spread 3 ticks, `noise.qty` 40,
`noise.timer_ns` 20000) on seeds 7001 to 7008 are written as a tape, and calibrating from it
must recover each parameter within one step of its grid. Two planted defects must break it.
`/calibrate --control` prints:

```
  depth   reader                                      recovered: spread / size / pace   held
  L3      as written                                  3 / 40 / 20000                    yes
  L3      planted: drops every cancel                 1 / 15 / 2500                     no, as it must not
  L3      planted: misreads the price scale by ten    10 / 100 / 60000                  no, as it must not
  trades  as written                                  - / - / 80000                     no
```

`/calibrate NAME` runs this control first, at the tape's depth, and refuses to calibrate if it
does not hold on the build. The last row is why a **trades-only tape is refused**: from prints
alone the pace comes back as 80000 instead of 20000. The bounce of prints between bid and ask
cannot be told apart from the pace of the flow, so a tape with quotes or the book is needed.

**How far the control generalises.** `/calibrate --robustness` runs it on five known markets,
each written on three sets of seeds:

```
  truth: spread / size / pace   seeds       recovered               steps off   within one step
  5 / 15 / 40000                7001-7008   5 / 15 / 40000          0 0 0       yes
  5 / 15 / 40000                7101-7108   6 / 10 / 10000          +1 -1 -4    no
  5 / 15 / 40000                7201-7208   6 / 10 / 7500           +1 -1 -5    no

  13/15 within one grid step on every parameter (825 parameter sets simulated)
```

(Only the market that missed is shown; the other twelve rows are within one step.) The two
misses are the same market, a wide spread with slow, small noise, on two of its three seed
sets. Calibration settled instead on a wider spread with smaller, much faster noise: the
confusion described under "What was wrong on the way", where two such markets have nearly
the same pace. Read a calibrated market's parameters as one market that fits the statistics,
not as the only one.

## A calibrated market in use, and reproduced

`/calibrate NAME` writes `markets/NAME.cfg`: the statistics matched, the parameters set, the
tape's `cksum` and the importer, the control's result and the health gate's.
`market=NAME` is taken by `/compare`, `/watch` and `/sweep`, which run the lab programs
(`suite_lab`, `arena_lab`, `sweep_lab`). The evidence programs (`suite`, `arena`, `sweep`)
refuse it, so no calibrated market can reach a published number or a pinned hash.

A calibrated market is a scenario family of its own, with its own fingerprint, never compared
with `standard` and never pooled with it. A run on one records the market's parameters and
the tape's checksum among its conditions, so `/reproduce` re-runs it **without the tape or
the market file**. If the market file has since been calibrated again from a changed file,
`/reproduce` refuses, as it refuses a changed strategy ([`REPRODUCE.md`](REPRODUCE.md)).

## Replay

`/replay NAME` plays an order-by-order tape back through a `MatchingEngine` of its own. The
session is not involved, so no session hash can move. Three owners share the engine:

- **history's book:** every add on the tape is a GTC limit at its recorded time, and a cancel
  cancels it. Size the tape knew only by level (the book LOBSTER says was resting when the
  file began) is one order a level, ahead of everything added later.
- **history's flow:** every recorded execution is an IOC at the trade's price and size, so it
  trades with whatever is first in the queue there, which is the recorded order unless the
  strategy got ahead of it. A print that names no resting order (a hidden execution) changes
  no book and is not replayed.
- **the strategy:** one, built in or installed, woken as the session wakes one (at the open,
  on its own events, when the touch moves, on its timer) and seeing the book as it was its
  reaction delay ago. The delay is the suite's 10 µs unless `reaction=` sets it; `from=` and
  `for=` pick a window, and the tape before `from=` builds the book. `informed` is refused: a
  replay has no fundamental for it to know.

**The engine has no partial cancel, and is not given one.** A partial cancel cancels that
order and every order of history's behind it at its price, then adds them back in the same
order, so the queue keeps its order. An order of the strategy's behind them moves up past
them, and that is counted.

**The control.** With no strategy, the replayed book must equal the tape's own book after
every event, and every execution must find its order first in the queue. `/replay NAME`
alone runs it, and exits 1 if it fails:

```
What the tape did
  events        771: 324 adds, 243 cancels (56 partial, the queue put back in order), 194 executions
  level sizes   6 (size the tape knew only by level: one order a level)
  not replayed  4 prints that name no resting order (hidden executions): they change no book

The control
  book          the replayed book equals the tape's after every one of 771 events
  queue         every execution found its order first in the queue

The control holds: replay plays this tape back as it was.
```

It holds on the LOBSTER, Databento, ITCH and Coinbase full-channel samples, and on two arena
sessions written as a tape (13,872 events, a reset between them). `test_data` holds it there,
with two planted defects that must break it: a partial cancel replayed as a full one (the
book's size goes wrong), and the order put back behind its queue (executions trade out of
turn). An inert strategy (`placebo`) leaves the event stream's hash exactly as the control's.

**The report leads with the label**, then counts every place the tape and the strategy
collided: history's flow that traded with the strategy, size the strategy took from
history's book, adds that crossed it, cancels whose order it had taken, executions that found
their price taken, and partial cancels that moved it up. Then its orders, fills, position,
and value marked to the last mid, in the tape's currency. `maker` on the LOBSTER sample:

```
Where history and the strategy collided
  history's flow that traded with the strategy    1300
  taken by the strategy from history's book       0
  history's adds that crossed the strategy        150
  cancels whose order the strategy had taken      0
  execution size that found its price taken       50
  partial cancels that moved the strategy up      8
```

Every one of those is a place where history would have behaved differently, which is the size
of the flaw, printed beside the result. The sample is 7 seconds of made-up market, so its
value means nothing; the report's hash shows that the same tape and strategy give the same
stream again.

## What was wrong on the way

- **Medians could not be calibrated.** The first statistics were medians. A median print of
  a few discrete sizes jumped between them from one set of seeds to the next, and a median
  spread moves in whole ticks, so the search had nothing to steer by. Means replaced them.
- **A descent alone found the wrong market.** One market with a wide spread and slow noise,
  and another with a wider spread and fast noise, have nearly the same pace. A descent from
  the standard market stalled in the wrong one on 3 of 15 known-answer runs. The coarse joint
  scan before it fixed most of that; the robustness table above shows what is left.
- **Trades alone were expected to give the pace.** The plan said a trades-only file could
  calibrate volatility. The control said otherwise (80000 for 20000), so such a tape is now
  refused rather than calibrated badly.
- **A Databento replay traded nothing.** The first importer read `mbo` trades as prints that
  name no order, which is correct for the statistics and useless for replay: every execution
  was a hidden print and history's flow never ran. Trades are now read through their fills
  (importer version 2).
- **Smaller ones:** a Coinbase snapshot, which carries no time, took one from a `last_match`
  message that is otherwise skipped; a Tardis trades file read alone took the name of the
  book tape made from its pair, and replaced it.

## Not built

- Downloading data, an API key, a vendor account, or any network access.
- Matching the real clock. Calibration matches ratios; sessions stay 25 ms.
- Fitting the informed trader, or any statistic no public data can show.
- Compressed files, which must be opened first.
- Committing any data, or publishing a result made from it in `results/`.
- Floating point anywhere: importers, statistics and calibration are integer, so a tape and a
  calibration are identical on every platform.
