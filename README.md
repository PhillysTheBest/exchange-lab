# exchange-lab

**A simulated market on a real exchange matching engine, with a referee, in your terminal.
For students learning how markets work and researchers who need experiments they can
control.**

Strategies trade inside a seeded market: two market makers, a trader who can see the true
value, two noise traders and a momentum strategy, all matched by a price-time-priority
engine written in C++20. Every participant has a reaction delay you set. A seed fixes the
whole market, so you can run the same market again with exactly one thing changed, such as
a strategy added, a maker slowed or a spread widened, and measure the difference seed by
seed. History happens once; this market happens as often as you like.

**What it is not.** It is not a backtester, and it contains no market data. The market is
invented rather than fitted to a real venue, so what a strategy earns here says nothing
about what it would earn on one. Use it to understand *why* a strategy makes or loses
money, and how a market's mechanisms produce that. Do not use it to decide whether to
trade the strategy.

## Start here

```bash
make app                      # g++ 13 or newer and make, nothing else
build/release/exchange-lab    # run it from the repository root
```

On Windows, use `mingw32-make app` in MSYS2 UCRT64. Then you are at a prompt:

```
exchange-lab · a simulated market on a real matching engine, re-run exactly · build 1a2b3c4d5e6f
8 built-in strategies, 0 installed. /help lists the commands; /learn starts the exercises.
> /learn 1
> /add examples/strategies/imbalance.hpp
> /compare imbalance smoke
> /watch imbalance
```

It works like Claude Code: one prompt, slash commands, tab completion and history. But there
is no AI model inside it, no browser and no network. Each command does one fixed thing by
running the programs the project already has, so nothing the app shows can differ from what
those programs print.

| Command | What it does |
|---|---|
| `/learn [N]` | the eight guided exercises in [`docs/LEARN.md`](docs/LEARN.md), one at a time |
| `/strategies` | the built-in strategies, and the ones you installed |
| `/add PATH` | installs a strategy of your own: one C++ header |
| `/compare NAME[,NAME...] [smoke] [wide\|busy\|all]` | scores strategies in the same seat of the same market, against an empty seat, and prints the report |
| `/runs`, `/report [RUN]` | lists the runs you made; shows one's report |
| `/reproduce [RUN]` | re-runs a run from its own record and compares every file it wrote |
| `/watch [NAME,...] [seed=N]` | watches the market trade live, full screen; `q` comes back |
| `/sweep FLAGS` | explores: one market many times, with one thing changed |
| `/controls` | checks the evaluator itself against answers known in advance |
| `/book` | an order book you drive by hand |
| `/help [command]`, `/quit` | explains a command; leaves |

`exchange-lab -c '/compare maker smoke'` runs one command and exits with its status, and a
file of commands can be piped in. [`docs/APP.md`](docs/APP.md) is the design.

| You are | Start with | Then |
|---|---|---|
| a student, new to market microstructure | `/learn`: from one order book to a strategy of your own, scored, controlled and reproduced | [`GLOSSARY.md`](GLOSSARY.md), every term as this project uses it |
| a researcher with a question about a mechanism or a strategy | [For researchers](#for-researchers), then [`docs/ARENA.md`](docs/ARENA.md): the model, its reasoning and its known limits | [`docs/RUNNER.md`](docs/RUNNER.md), [`docs/REPORT.md`](docs/REPORT.md), [`docs/CONTROLS.md`](docs/CONTROLS.md) |
| here for the matching engine | [The engine underneath](#the-engine-underneath) | [`docs/ENGINE.md`](docs/ENGINE.md) |

<img src="docs/arena.svg" alt="The arena viewer: order book, participants, reaction-delay sliders, session and tape, drawn in a terminal" width="100%">

That is `/watch still=60000000 seed=42`, turned into SVG by `bench/frame_to_svg.py`. It is
generated rather than screenshotted, so anyone can regenerate it and diff it.

## The first result is a negative one

The arena was built to ask what 10 microseconds of reaction speed is worth to a market
maker. In this market:

- **10 µs costs less than 24 seeds can resolve.** The paired difference is −39.96 ± 56.19,
  against a smallest resolvable effect of ±157. So the cost is *not established*, which is
  a different claim from "there is nothing there".
- **1 ms costs real money, −3117.13 ± 74.71 a session, but not through adverse
  selection.** With the informed trader removed, 96% of the effect remains. The slow
  maker's loss goes to the competing maker, not to the trader with information. Being slow
  costs order flow, not information.
- **A second control then failed outright.** It tried to make the post-fill column
  measure something on purpose. No maker stale enough to be picked off reliably keeps the
  book tracking the fundamental, so that column's sensitivity is not established. The
  failure is published, not glossed over.

The controls changed the answer twice, and that is what the project teaches, more than any
number in it: how a plausible story about a market gets tested and overturned. The tables
are in [`docs/RESULTS.md`](docs/RESULTS.md) and the reasoning in
[`docs/ARENA.md`](docs/ARENA.md). Every number in them is simulated, and
`bench/summarize_arena.py` prints it from `results/`.

## What makes it different

Like agent-based market simulators such as ABIDES, this is a market of simulated
participants rather than a replay of history. What it adds:

- **A real matching engine underneath.** Price-time priority, self-trade prevention, and
  admission decided before matching, with p99.9 service time under 2 µs and no allocation
  after start-up. A strategy sees the book only through the engine's event stream, as on a
  real venue, rebuilt by `arena/book_mirror.hpp`.
- **A referee.** Every candidate sits in the same seat of the same market on the same named
  seeds, beside a placebo arm whose seat never acts. Every session is replayed, a market
  whose book has come loose from the fundamental is refused, and every comparison is a
  paired difference with its standard error.
- **An evaluator that is itself tested.** Eight controls have answers known in advance, and
  each has a planted defect in the tests that makes it fail.
- **Reproduction you can check.** `/reproduce` refuses if the conditions or a strategy
  changed, then compares every file byte for byte. Sessions use integers only, and the
  committed evidence is identical on Windows and Linux, down to every session hash.
- **A market you can watch.** The viewer draws the book, the participants and the tape
  live in the terminal, and no key can change what happens in it.

## For students

The project lets you watch the mechanics a market-microstructure course describes, then
change them and see what moves:

- **An order book you can drive.** `/book` opens one: type orders and watch price-time
  priority, partial fills and self-trade prevention happen.
- **A seat in the market.** `/watch script=FILE` plays a file of orders in simulated time as
  a participant with its own reaction delay, accounted for to the last unit.
- **A strategy of your own.** One header is a view of the book in and orders out. `/add`
  installs it, and `/compare` scores it against the same market everyone else uses.
- **Where profit really comes from.** The report splits a strategy's value into spread
  earned, the price move after each fill and carry to the close. It then names which
  participant paid for it, and the books balance exactly.
- **How to tell a result from noise.** Paired seeds, standard errors, a placebo arm, the
  smallest effect a seed count can resolve, and controls with known answers.

`/learn` walks through all of it in order, and every command in it was run before it was
written down.

## For researchers

**Questions it answers well:**

- What is reaction speed worth to a strategy competing for uninformed flow?
- Does a strategy's profit come from spread, from carry or from moves after its fills, and
  which participants pay for it?
- Does a result survive a wider or a busier market? The scenario families are reported
  side by side and never pooled.

**What it gives you.** Fixed, named conditions: the standard suite's fingerprint is pinned,
and two runs are comparable only if their fingerprints are equal. A measured noise floor of
about ±2 a session on the standard block says how small a difference is too small to read
as skill. The report counts its own interval tests. A new set of conditions is a new suite
in [`arena/suite.hpp`](arena/suite.hpp), with a version and a fingerprint of its own;
ad-hoc exploration is `/sweep --set=...`, which never reaches the published evidence.

**What it cannot show.** The market has one instrument, one informed trader, a fundamental
that moves on one timescale, no second venue, no quote fade, no cancel-replace race, and a
book thin enough that queue priority rarely binds. Those are exactly the mechanisms through
which speed pays for itself on a real venue, so the arena's answers are statements about
this instrument, not about trading. The standard runner has
[known weaknesses](docs/RUNNER.md#known-weaknesses) of its own: its 25 ms sessions favour
short-horizon strategies, pairing removes less variance between unrelated strategies than
between two settings of one, and the path is chaotic enough that a 1 ns change scatters
results seed by seed.

## The engine underneath

The market runs on a single-threaded matching core fed by a lock-free single-producer,
single-consumer ring, with no heap allocation after start-up, int64 fixed-point prices and
a flat sorted price index. It is measured like a real engine, and its numbers are
**measured, on hardware**, unlike the arena's, which are simulated. The two must not be
read together.

Measured on a laptop (i5-12450H, Windows 11, on battery, Balanced power plan) over five
1M-order streams per profile, as service time: from dequeuing an order to writing its last
resulting event to the outbound ring.

| Book | p50 | p99.9 | Under 2 µs | Slowest order |
|---|---|---|---|---|
| Deep (about 1,500 price levels per side) | 278 ns | 1.2 µs | 99.95% | 158 µs |
| Thin | 154 ns | 891 ns | 99.96% | — |

Replays are bit-identical on Windows and Linux. [`docs/ENGINE.md`](docs/ENGINE.md) has the
design, how it is measured, the tail and what causes it, and how its correctness is tested
against a reference engine. `python bench/summarize_results.py results/windows` prints these
numbers from the committed evidence.

## Without the app

Everything the app does is a `make` target or a program, for scripts and for the published
evidence:

```bash
make test               # all 8 test programs (debug, with the engine's invariants checked)
make test-cli test-lab test-app   # the command lines, the strategy path and the app, checked
make strategy STRATEGY=examples/strategies/imbalance.hpp
make compare CANDIDATES=imbalance,maker [BLOCK=smoke] [SUITE=all] [OUT=runs/x]
make reproduce RUN=runs/imbalance-vs-maker-standard
make controls
make bench              # the engine's benchmarks
bash bench/run_arena_evidence.sh <label>   # regenerate results/arena-<label>/
```

Run directories go to `runs/`, never to `results/`. A suite session lasts 25 ms of simulated
time, so its numbers are not the published arena numbers, which come from 200,000-request
sweeps.

## Scope

**Built:**

- the matching engine: matching, the event protocol, SPSC rings, the threaded pipeline,
  and the measurement and test tooling;
- the arena, a seeded simulated market with a reaction-delay model, participant-side book
  reconstruction, marked P&L and a terminal viewer;
- the research workflow on top of it: installable strategies, the standard suite and its
  report, the evaluator's controls and one-command reproduction;
- the terminal app in front of all of it.

**Not for:** predicting what a strategy would earn on a real venue. There is no market
data, no historical replay and no calibration to a real market.

**Not built (out of scope):**

- a browser, web or desktop interface, or an AI model;
- live trading, a network gateway, or FIX/ITCH/OUCH encoding;
- order modify or amend (cancel and re-enter instead), stop orders, icebergs, auctions;
- journaling and recovery, and per-participant risk limits;
- the full LMAX multi-consumer graph (one SPSC ring into one core, by design).

## Layout

| Path | Contents |
|---|---|
| `app/` | the terminal app: the commands, the launcher and the line editor |
| `include/px/`, `src/` | the engine: types, messages, pools, hash map, level index, matching engine, ring, clock, platform |
| `sim/` | seeded order-stream generator, three-thread pipeline, allocation counter |
| `arena/` | the simulated market: clock, fundamental value, book mirror, accounting, the strategy contract, `strategies/`, the sweep, the suite, the controls and the viewer |
| `examples/` | [`strategies/imbalance.hpp`](examples/strategies/imbalance.hpp), a complete strategy of your own that states the convention |
| `tests/` | unit, fuzz (against a reference engine), allocation, determinism, arena and app tests |
| `bench/`, `results/` | the benchmarks and evidence scripts; the committed evidence every published number is printed from |
| `tools/` | the REPL behind `/book`, its demo, and a demo script for trading by hand |
| [`GLOSSARY.md`](GLOSSARY.md) | every term the docs and reports use, defined as this project uses it |
| `docs/` | [`LEARN.md`](docs/LEARN.md) the exercises; [`APP.md`](docs/APP.md) the app; [`ARENA.md`](docs/ARENA.md) the market's design and the mistakes it took to get there; [`RESULTS.md`](docs/RESULTS.md) its published results; [`RUNNER.md`](docs/RUNNER.md), [`REPORT.md`](docs/REPORT.md), [`CONTROLS.md`](docs/CONTROLS.md), [`REPRODUCE.md`](docs/REPRODUCE.md) the research workflow; [`ENGINE.md`](docs/ENGINE.md) the engine |
