# Bringing your own market data: plan

Decided 2026-10-22: people must be able to bring real market data into exchange-lab, free or
paid, in whatever format they have it, and use it to make the simulated market look like a
real one. This file is the plan, written before building as `CLAUDE.md` asks. No code exists
yet. When it is built, this file becomes `docs/DATA.md`, the design.

## What it is, and what it is not

```
# what it would look like; the output is illustrative, nothing is built yet
> /data add ~/lobster/AAPL_2012-06-21_message_10.csv
added aapl-2012-06-21: LOBSTER, order by order, 400,391 events, 09:30-16:00
> /data show aapl-2012-06-21
> /calibrate aapl-2012-06-21
spread 1 tick, a one-tick move every 2.1 s, 4.3 trades a move, median trade 100 ...
market aapl-2012-06-21 passes the health gate on 64/64 seeds
> /compare imbalance market=aapl-2012-06-21
```

- **Calibration first, replay later.** Calibrating means measuring a file's statistics and
  setting the simulated market to match them. The market is still simulated, so the referee
  (paired seeds, the placebo, replay, the controls) keeps working. Replay, which plays
  recorded orders back as a participant, comes last. History cannot react to a strategy, so
  replay inherits the backtester's main flaw, and it will be labelled that way.
- **The user brings the file; the app never fetches it.** Free or paid, the user downloads it
  with their own account and the app reads it from disk. Nothing in the project connects to
  an exchange or a vendor, which keeps the rule in `CLAUDE.md`.
- **Still not a backtester.** A calibrated market resembles the data in the statistics it
  was fitted to and in nothing else. The README's "what it is not" stays true, and every
  calibrated result says which statistics were matched.

## One internal format, one importer per outside format

Every source is converted once into one internal format, the **tape**. Calibration and
replay read only tapes. Supporting a new source therefore means writing one importer, and
nothing downstream changes.

A tape is a text file of integer columns: time in nanoseconds since the file's start, event
(add, cancel, trade, or a price-level update), order id when the source has one, side, price
in ticks and quantity. A header records the instrument, the tick size, the price scale, the
source format, the importer's version and the source file's `cksum`. Integers only, so the
same file gives the same tape on Windows and Linux.

Sources come in three depths, and the importer records which one a file has:

| Depth | What it has | Examples | Calibration can measure |
|---|---|---|---|
| order by order (L3) | every order, cancel and trade | LOBSTER, Databento MBO, Nasdaq ITCH | everything below, plus cancel rates and order lifetimes; replay possible |
| price levels (L2) | changes to the size at each price | Tardis, Coinbase, Kraken, Binance depth | spread, depth, volatility, trade rate and size |
| trades only | prints | Binance trades, most free downloads | volatility, trade rate and size |

Importers, in the order they are built:

1. **Generic CSV.** Any file with a header, where the user names the columns:
   `/data add trades.csv columns=time,price,size,side`. This covers anything a user can
   export, including from tools nobody planned for.
2. **LOBSTER.** The academic standard (message files, prices × 10,000).
3. **Databento** CSV, and its own uncompressed DBN binary format, for the most common paid
   source.
4. **Tardis** CSV (`incremental_book_L2`, `trades`), for paid crypto data.
5. **Binance, Coinbase and Kraken** exports, for free crypto data.
6. **Nasdaq ITCH 5.0** binary, whose free sample files are the reference L3 feed.

The format is detected from the file's header where it can be, or named:
`format=databento`. A file that matches no importer is refused, with the list of formats.
There are no third-party libraries, so compressed files (`.zst`, `.gz`) must be
decompressed first, and the refusal says which command does it.

## What calibration measures and sets

| Statistic, measured from the tape | Sets |
|---|---|
| tick size and price level | the price grid |
| median quoted spread, in ticks | `maker.half_spread_ticks`, `rival.half_spread_ticks` |
| time between one-tick moves of the mid | `fair.step_mean_ns` |
| trades per one-tick move | `noise.timer_ns` with `noise.count` |
| median trade size | `noise.qty` |
| share of trades that cross the spread | `noise.pct_market` |
| depth at the touch | `maker.quote_qty`, `rival.quote_qty` |

What it cannot set is said every time: the informed trader's edge and position are not
visible in any public data, so they keep their defaults. The parameters `docs/ARENA.md` calls
load-bearing (the maker's inventory limit and skew, the learning rate) are never moved by
calibration.

**A calibrated market must still be a market.** It goes through the same health gate as the
standard suite. A calibration whose book comes loose from the fundamental is refused, and
the gap is printed. `docs/ARENA.md` records what ignoring that cost the first time.

## How a calibrated market is used, and stays reproducible

- `/calibrate NAME` writes `markets/NAME.cfg`: the measured statistics, the parameters set,
  the tape's checksum and the importer version. `markets/` and `data/` are git-ignored.
  **Paid data usually may not be redistributed, so a tape or a source file must never be
  committed**, and nothing published may contain one.
- `market=NAME` is accepted by `/compare`, `/watch` and `/sweep`. A calibrated market is a
  scenario family of its own, with its own fingerprint. It is never pinned, never compared
  with `standard`, and never written to `results/`.
- A run records the market's parameters as conditions. So anyone can `/reproduce` it
  **without the data**: the parameters are in the run, and the checksum shows which file
  they came from. Re-calibrating from a changed file gives a different market, and
  `/reproduce` refuses it, as it refuses a changed strategy.

## The app's commands

| Command | Does |
|---|---|
| `/data add FILE [format=X] [name=N] [columns=...]` | imports a file into `data/NAME.tape` and summarises it |
| `/data` | lists the tapes: source, depth, instrument, dates, events |
| `/data show NAME` | the tape's statistics, and what its depth can and cannot support |
| `/calibrate NAME` | measures, sets, runs the health gate and writes `markets/NAME.cfg` |
| `/replay NAME` | later: the tape played back as a participant, L3 only, labelled as unable to react |

Each is also a make target and a program, as the rest of the app is: `make data FILE=...`
and `build/release/ingest`. `ingest` is compiled apart from the evidence programs, like the
lab programs, so no importer can reach a pinned hash.

## The control that proves calibration works

The project tests its evaluator with answers known in advance, and calibration gets the
same treatment. An arena session can be written out as a tape. Calibrating from it must
recover the parameters it was run with, within a stated tolerance. A planted defect, such as
an importer that drops every cancel or misreads the price scale, must make that control
fail. Until that control passes, no calibrated result is reported.

## Stages

Each stage is a commit with every test green.

1. **The tape and the first two importers.** The tape format, generic CSV, LOBSTER,
   `/data add`, `/data`, `/data show`. Tests: each importer on a small committed sample that
   was written by hand, so it carries no licence; a malformed file refused naming the line.
2. **The known-answer control.** Writing an arena session as a tape, and calibration
   recovering its parameters, with a planted defect that fails it.
3. **`/calibrate` and `market=`.** The health gate, `markets/NAME.cfg`, conditions recorded
   in `suite.cfg`, and `/reproduce` refusing a changed calibration.
4. **More importers:** Databento, Tardis, Binance, Coinbase, Kraken, ITCH, one at a time.
5. **`/replay`**, for L3 tapes only.

## Decisions for you

- **The time scale.** A suite session lasts 25 ms of simulated time, and a real market moves
  over seconds. Calibration can match **ratios** (spread in ticks, trades per price move,
  size against depth), keeping sessions short and every published number comparable. Or it
  can match the **real clock**, which needs sessions thousands of times longer.
  Recommended: ratios. Real-clock matching can come later as an option.
- **The first importer after the two above.** Recommended: Databento, since it is what
  people buying data most often have.

## Not in this plan

- Downloading data, an API key, a vendor account, or any network access.
- Committing any data, or publishing a result made from paid data in `results/`.
- Fitting the informed trader, or any statistic the data cannot show.
- Floating point inside a session. Importers and calibration run outside sessions, but stay
  integer so a tape and a calibration are identical on every platform.
