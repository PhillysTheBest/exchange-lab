# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A price-time-priority exchange matching engine in C++20: a single-threaded matching
core fed by lock-free SPSC rings, no heap allocation after start-up, int64 fixed-point
prices, plus the test and measurement machinery that backs its latency claims — and
`arena/`, a seeded simulated market of strategies trading inside that engine, with a
controllable reaction delay each and a terminal renderer — and `marketdata/`, which brings
a data file the user already has in as a tape, calibrates the market to resemble it and
replays it — and `app/`, the terminal app in front of all of it. README.md is the front
page; `docs/APP.md` is the app's design, `docs/DATA.md` the market data's,
`docs/ENGINE.md` the engine's design and measurements, `docs/ARENA.md` the arena's design,
its reasoning and the viewer, `docs/RESULTS.md` the arena's published results,
`docs/LEARN.md` the students' path and `GLOSSARY.md` the terms; this file covers how to
work in the repo.

**Who it is for (decided 2026-10-17).** exchange-lab is a research and learning tool for
**students** of market microstructure and **researchers** who need controlled, reproducible
experiments on market mechanisms and strategies. It is not a backtester and not a tool for
deciding whether to trade a strategy: the market is invented, and calibrated to a data file
it matches three statistics and nothing else, so its P&L predicts nothing about a real venue. Write every user-facing document for those two
readers, and keep the README's opening in this order: what it is and is not, "Start here"
(the app), the negative result, what makes it different, then the two audiences. `docs/LEARN.md` is the students' path
(every command in it must run as written; re-run them when a command changes) and
`GLOSSARY.md` defines terms as this project uses them; a new term in a report or a doc gets
an entry there.

**The app is the front door (decided 2026-10-22).** One terminal program,
`build/release/exchange-lab`, with a prompt and slash commands in the spirit of Claude Code.
**No browser whatsoever**: no web or desktop interface, no mouse, no network, and no AI model
in it. It runs the existing programs as children and links none of the market
(`docs/APP.md` says why), so it cannot move a hash. Document new features as app commands
first, with the raw `make` target beside them.

**History.** This was a Kalshi prediction-market trading system; the live stack was
deleted at `07b3be0` and is not coming back. Nothing in the tree depends on an exchange
account, an API or a network. Do not reintroduce a market-data client, or put a trading
strategy in the engine: strategies live in `arena/strategies/`, never in `include/px/` or
`src/`.

**Status.** The engine is finished, measured and committed. The arena is built,
measured and committed: see `docs/ARENA.md` for its design, the modelling mistakes that
produced confidently wrong results on the way, and the parameters that turned out to be
load-bearing. Its scope boundary is binding — everything compiles into one binary and
nothing talks over a network. All of it is on `main`, merged on 2026-10-26 through pull
requests #2 (into `arena`) and #1 (into `main`). The arena is being turned into a strategy research platform in five
steps, and all five are built: the strategy interface, the standard experiment runner,
the evaluation report, its validation and one-command reproducibility. See "Strategies of
your own" below for what is built. The terminal app in front of it is built too.

**The result is a negative one and the documentation leads with it.** "What is 10
microseconds worth?" answers *less than 24 seeds can resolve*; 1 ms costs real money,
but the null run (no informed participant at all) retains 96% of that effect, so the
cost is competition for uninformed flow rather than adverse selection. A second,
positive control then failed outright — no admissible way exists to calibrate the
post-fill column, because a maker stale enough to be reliably picked off also stops the
book tracking the fundamental. When writing about this project, do not restore the tidy
version: the controls changed the answer twice and that is the interesting part.

**Names.** The repository is `exchange-lab`, renamed from `prediction-markets` (GitHub
redirects the old URL). Never name anything with **latency** in it: the engine's latency
is measured, a strategy's `reaction_ns` is invented, and nothing in `arena/` is called
latency. Never say **tick-to-trade** either: the engine's measurement is service time.
The GitHub description is currently set to:

> Price-time-priority exchange matching engine in C++20 — p99.9 under 2µs, zero
> allocation, bit-identical cross-OS replay — and the deterministic market simulator
> built on it.

A description for the new audience was proposed: *Price-time-priority matching engine in
C++20 (p99.9 under 2µs, zero allocation) and a deterministic market simulator for teaching
and researching trading strategies: paired experiments, tested controls, byte-for-byte
reproduction.* Change this paragraph when the user sets one. The user's local checkout is still named `prediction-markets`; when
it is renamed, the WSL path in the sanitizer command below changes with it.

## Build and test

Hand-written Makefile: `mingw32-make` on Windows (MSYS2 UCRT64 g++ 13), `make` on
Linux and macOS (Apple clang, GNU make 3.81). No CMake, no package manager, no third-party
libraries — keep it that way. `docs/PLATFORMS.md` lists the targets and what each
platform-specific flag and file is for; CI (`.github/workflows/ci.yml`) builds five of them
on every pull request and fails if any one's `make canonical` record differs.

```bash
mingw32-make app        # the terminal app, build/release/exchange-lab, and every program it runs
mingw32-make test       # debug (-Og, PX_DEBUG invariants, standard-library checks): builds and runs all 9 tests
mingw32-make bench      # release (-O3 -march=native -flto): builds and runs all 3 benchmarks
mingw32-make latency    # only the 1M-order matching-latency benchmark
mingw32-make repl       # then: build/release/repl.exe < tools/repl_demo.txt
mingw32-make arena      # the arena: build/release/sweep.exe, arena.exe, suite.exe and controls.exe
mingw32-make test-cli   # the sweep's and the suite's command lines: bad flags fail, bad configs refused
mingw32-make compare CANDIDATES=a,b   # the standard experiment, written to runs/; see "Strategies of your own"
mingw32-make controls   # the evaluator's own controls on the standard block (docs/CONTROLS.md)
mingw32-make reproduce RUN=runs/x   # re-run a run from its suite.cfg, compare every file (docs/REPRODUCE.md)
mingw32-make test-app   # the app driven through its commands (writes runs/maker-smoke)
mingw32-make data FILE=path [OPTS='name=x']   # bring a market data file in as data/NAME.tape (docs/DATA.md)
mingw32-make calibrate TAPE=name   # data/NAME.tape -> markets/NAME.cfg; then compare/sweep_lab/arena_lab take a market
mingw32-make compare CANDIDATES=a MARKET=name  # the standard experiment on a calibrated market (suite_lab only)
mingw32-make replay TAPE=name [STRATEGY=s]   # an L3 tape through the engine: the control, or one strategy in it
mingw32-make test-data  # ingest on the samples, the known-answer control, a calibrated market end to end (~90 s)
mingw32-make canonical  # every output that must match on every platform, diffed with tests/canonical.txt
mingw32-make memmove-check   # the engine object calls no memmove, whichever compiler built it
mingw32-make recipe-check    # no Makefile command near the 8,191 bytes past which MSYS2's make drops the rest
mingw32-make all        # tests, command-line, lab, app and data checks, benchmarks, REPL, arena
mingw32-make clean
```

Running the arena:

```bash
build/release/arena                                  # watch the default market
build/release/arena --strategies=maker,informed,noise:2    # compose the line-up by name
build/release/arena --script=tools/arena_script_demo.txt   # play orders as a participant
build/release/arena --still=60000000 --seed=42       # one reproducible frame, then exit
build/release/sweep --help                           # every sweepable field
build/release/sweep --seeds=24 --replay-check        # the headline sweep
build/release/sweep --null                           # the control: nobody informed
```

Viewer keys: `space` pause, `+`/`-` speed, `1`-`9` focus a participant, `f` re-read the
`--script` file and restart, `q` quit. No mouse support, by decision. No key can change
what happens — the viewer only reads the session, which is why `f` restarts rather than
injecting a participant into a running one.

Run one test, or one benchmark with flags:

```bash
mingw32-make build/debug/test_engine.exe && ./build/debug/test_engine.exe
./build/release/bench_matching_latency.exe --profile=deep --repeat=5 --outliers=slow.csv
```

- Objects carry `-MMD -MP`, so editing a header rebuilds what includes it.
  `-Wall -Wextra -Wpedantic` everywhere; a clean build has zero warnings on both
  platforms, and should stay that way.
- Windows links `-static`: no DLL-search-order surprises.
- MSYS2 ships no sanitizer runtime. Run ASan/UBSan on Linux:
  `wsl -d Ubuntu -- bash -lc "cd /mnt/c/Users/Philb/prediction-markets && make BUILD=build-asan SAN='-fsanitize=address,undefined -fno-sanitize-recover=all' test"`
  (`BUILD` picks the output directory so Windows and WSL builds coexist.)
- `bench/run_evidence.sh <label>` regenerates `results/<label>/` — machine state plus
  every run README.md and `docs/ENGINE.md` quote. `python bench/summarize_results.py results/<label>`
  prints their tables. Never hand-edit numbers in `results/` or the docs.
- `python bench/summarize_arena.py results/arena-<label>` prints the arena tables that
  README, `docs/RESULTS.md` and `docs/ARENA.md` quote, including the paired standard errors and the smallest
  effect a given seed count could resolve. Same rule as the engine's: a number in the
  documentation that this script does not print has no evidence behind it.
- `bench/run_arena_evidence.sh <label>` does the same for `results/arena-<label>/`:
  the headline sweep, the null run, the failed positive control, the degenerate
  single-maker market and four robustness sweeps. It takes `BUILD=` like the Makefile,
  and **must** be given it on Linux: WSL runs Windows `.exe` files on `/mnt/c` through
  interop, so a run that found `build/release/sweep.exe` would silently measure the
  Windows binary and write the output into `results/arena-wsl2/`.
- Unlike the engine's benchmarks, arena results do not depend on the machine. Both
  `results/arena-windows/` and `results/arena-wsl2/` are committed and **every published
  number and session hash in them is identical**, down to the rendered still frame.
- `bench/frame_to_svg.py` turns an `arena --still` capture into the SVG README.md shows.
  The picture is generated from a seed rather than screenshotted, so it can be
  regenerated and diffed.

## Architecture

**Three threads, two rings, one owner.** `sim/pipeline.hpp` runs a gateway thread that
pushes pre-generated requests into an inbound `SpscRing`, a matcher thread that is the
*only* caller of `MatchingEngine::process`, and a publisher thread that drains the
outbound ring of events. Because one thread owns all mutable state, the engine itself
contains no locks, atomics or fences.

**The engine** (`include/px/matching_engine.hpp`, `src/matching_engine.cpp`) holds
engine-wide pools of orders and price levels plus an order-id hash map, with per
instrument a bid and an ask index. Order flow through `process()`:

1. Validate (instrument, quantity, price and tick, duplicate id).
2. Decide admission: a GTC order needs a free order slot and, if its price is not
   already a level on its own side, a free level and room in that side's index.
3. Emit `Ack`.
4. Match against the opposite side, best level first, FIFO within a level, filling at
   the resting price. A resting order from the same owner is cancelled instead of
   traded (self-trade prevention).
5. Rest a GTC remainder, or cancel an IOC/market remainder.

**The price index** (`include/px/sorted_levels.hpp`) is the part worth understanding
before changing anything. Each side is a fixed-capacity flat sorted array of
(key, level-slot), where the key is `price` for bids and `-price` for asks so one class
serves both, with the best price at the *back*. Touching the touch is `pop_back` or an
append; other lookups binary-search a contiguous key array. Orders at a level form an
intrusive FIFO by slot index, so cancel is an O(1) unlink. Price, side and instrument
live on the level, which is what keeps `Order` at 32 bytes.

**Events** (`include/px/messages.hpp`) are 64-byte PODs carrying an engine-wide
sequence number and no timestamps, which is what makes replay bit-identical.
`EventHasher` folds them field by field so padding can never leak in.

**The generator** (`sim/order_stream.hpp`) pre-builds a seeded stream so no generation
cost lands in a measured window. It replays its own stream through a private shadow
engine as it goes, so cancels target orders that are really resting.

**The arena** (`arena/`) is a fourth consumer of the same engine, alongside the
pipeline, the benchmarks and the REPL. `arena/session.hpp` owns a `MatchingEngine`, a
simulated clock, a roster of strategies and the accounting; `arena/sweep.cpp` runs the
experiment headless; `arena/suite.cpp` runs the standard comparison of strategies
(`docs/RUNNER.md`); `arena/arena.cpp` renders it. It adds no engine API: a strategy's view
of the book, including its queue position, is reconstructed from the `ExecEvent`
stream in `arena/book_mirror.hpp`. Read `docs/ARENA.md` before changing anything in
there — several of its parameters are load-bearing in ways that are not obvious, and
the document records which and why.

**The app** (`app/`, `docs/APP.md`) is not a consumer of the engine: it links none of it.
`app/commands.hpp` turns a typed line into a plan (answer it inside, or a `make`
invocation and a program to run) as pure functions that `test_app` checks, including that
every command in the `/help` table has a handler and every name in `kStrategyNames` has a
line in `kStrategyNotes`. `app/run.hpp` starts children without a shell; `app/line.hpp` is
the line editor. A new command is a row in `kCommands` and a branch in `plan_for()` (or in
`main.cpp`'s internal commands), plus a check in `test-app`.

**Strategies** are plain structs in `arena/strategies/`, compiled in and never loaded at runtime.
Adding one is a header plus one line in `add_named_strategy()` and its name in `kStrategyNames`,
which `test_arena` holds together so the two cannot drift. `arena/strategies/fader.hpp` is the
worked example, written against `arena/strategy.hpp` alone. `--strategies=maker,rival,noise:3`
composes a line-up by name; without it the registry's default line-up is used, which is
what every published number came from. The strategy lab below turns those two steps
into one make target for a strategy of your own; it does not change what a strategy is.

**Market data** (`marketdata/`, `docs/DATA.md`) is read from a file on disk, never fetched.
`ingest` (`/data`) converts a file once into a **tape** (`marketdata/tape.hpp`: integer text,
prices in ticks, the source's `cksum` in the header) through one importer per format in
`marketdata/importers.hpp` (csv, lobster, databento, tardis, binance, coinbase, kraken,
itch); nothing downstream reads anything else, and `ingest` links none of the market.
`marketdata/stats.hpp` measures a tape, whatever made it. Calibration
(`marketdata/calibrate.hpp`, `/calibrate`) runs the reference market on seeds 9001-9008,
writes the sessions out as a tape through `Session::set_event_tap` (read-only, null by
default; a tapped session's hash is the untapped one's) and `marketdata/session_tape.hpp`,
measures it with the same function, and moves only the four parameters
`kMarketParamNames` lists. It writes `markets/NAME.cfg` (`marketdata/market_file.hpp`),
which only the lab programs take (`--market=`); the evidence programs refuse it. `replay`
(`marketdata/replay.hpp`, `/replay`) is a lab program with a `MatchingEngine` of its own
and three owners (history's book, history's flow, one strategy); it adds no engine API.

**`arena/ui.hpp`** owns a grid of coloured cells and knows nothing about trading;
`arena/arena.cpp` is the only place the renderer and the market meet. A frame composes
into the grid and goes out in one `fwrite`, emitting a colour escape only where a colour
changes. `docs/ARENA.md`, "The viewer", explains it and why it exists.

## Strategies of your own

The project is becoming a strategy research platform: someone writes a strategy,
runs it against the reference market on fixed seeds, and gets a reproducible
result. The plan is in five steps (strategy interface, standard experiment runner,
evaluation report, validating the evaluator, one-command reproducibility). **All five
are built**: the strategy interface, the standard experiment runner, the evaluation report,
the evaluator's controls and one-command reproducibility. Everything participating in a
session is called a *strategy*, never a bot: built-in ones are the reference market.

```bash
make strategy STRATEGY=examples/strategies/imbalance.hpp   # validate, install, build the lab binaries
make compare CANDIDATES=imbalance BLOCK=smoke              # 4 seeds: does it run, does it trade
make compare CANDIDATES=imbalance                          # the standard block, 64 seeds -> runs/imbalance-standard/
make compare CANDIDATES=imbalance,maker                    # two strategies, identical conditions, one command
make compare CANDIDATES=imbalance,maker SUITE=all          # every scenario family, side by side, never pooled
make reproduce RUN=runs/imbalance-vs-maker-standard        # re-run it from its own record, compare every file
make controls                                              # the evaluator's own controls
build/release/suite --candidates=maker,momentum            # built-ins only, from the binary that cannot see installed code
build/release/sweep_lab --strategies=maker,rival,informed,noise:2,momentum,imbalance --seeds=8   # explore
build/release/arena_lab --strategies=maker,rival,informed,noise:2,momentum,imbalance             # watch
make test-lab                                              # the whole path, checked
```

- **The convention** is in `examples/strategies/imbalance.hpp`: `<name>.hpp` defines
  struct `<Name>` (CamelCase of the file name) in `px::arena::strategies`, with the
  five members `arena/strategy.hpp` lists and no floating point. The name must not
  collide with a built-in; the Makefile reads the built-in list from
  `kStrategyNames` so the two cannot drift.
- **An installed strategy cannot reach the published evidence, by construction.**
  `make strategy` copies the header into `arena/strategies/user/` (git-ignored) and
  generates `$(BUILD)/generated/px_user_strategies.hpp` (never committed). The
  registry includes that manifest only under `-DPX_ARENA_USER_STRATEGIES`, which only
  `sweep_lab`, `arena_lab` and `suite_lab` set, from their own object subtree
  `$(REL)/obj-lab`. `sweep`, `arena`, `suite` and every test are compiled without it,
  so a stranger's header is not in the binaries that pin hashes, and one with a typo
  cannot break `make test`. `test-lab` asserts that `sweep` and `suite` refuse an
  installed name, and that `suite` and `suite_lab` report the same fingerprint.
- **It is not runtime loading.** A strategy links into the binary; installing is a
  build step, and the viewer is unchanged.
- **With `--strategies=`, `sweep` prints a row per participant** (value, orders,
  fills, rejects), because the maker-centred tables say nothing about a strategy of
  your own. `--replay-check` now replays the composed line-up; before step 1 it
  silently replayed the default one.
- **`make compare` is how a strategy is scored** (`docs/RUNNER.md` is the design). Every
  candidate sits in one seat (index 6, owner 7, appended after the default line-up
  `maker,rival,informed,noise:2,momentum`, never informed) on a named seed block
  (`standard` is 42..105, `smoke` is 42..45), beside a placebo arm with an inert strategy
  in the seat. Every candidate session is replayed; every placebo session is checked
  against the market with no seat. No condition can be set on the command line: a
  different set of conditions is a new suite in `arena/suite.hpp`, and ad-hoc
  exploration stays with `sweep_lab`. Exit 0 complete, 1 a replay differed, 2 usage,
  3 the reference market is not admissible on this build.
- **Run directories go to `runs/`, which is git-ignored and is never `results/`.** Each
  holds `suite.cfg`, `sessions.csv`, `participants.csv`, `comparison.csv`,
  `evaluation.csv`, `attribution.csv`, `report.txt` and `outcome.cfg`, written last and
  only if every other file was; a directory without `status=complete` in it is
  incomplete. `SUITE=all` writes one such directory per family and `families.txt`.
- **The report (`docs/REPORT.md`) breaks the seat's value down** into spread, adverse and
  carry (exact, at zero fees), inventory and drawdown from a path sampled outside the
  session, and attribution to each reference participant (exact by the closed identity).
  Scenario families `wide` and `busy` are suites of their own with pinned fingerprints
  beside `standard`'s; a built-in candidate takes the family's parameters too. There is
  no `null` family: its placebo fails the health gate on 56 of 64 seeds.
- **The evaluator has controls of its own** (`docs/CONTROLS.md`, `make controls`, also run by
  `test-cli`): the placebo is zero and inert, arms share a close, replay holds, the
  identities hold, an A/A pair is identical, 9,999 ns against 10,000 ns does not separate
  (the noise floor, about ±2 a session), and `roundtrip` loses money in spread. Each control
  has a planted defect in `test_arena` that makes it fail; keep that pairing when adding one.
  `roundtrip` is the positive control: do not tune it, and do not claim its size was predicted.
- **`make reproduce RUN=runs/x` re-runs a run from its own `suite.cfg`** (`docs/REPRODUCE.md`).
  It refuses before running if a condition line differs or an installed strategy's `cksum`
  digest changed, then compares every file byte for byte (`suite.cfg` on its condition
  lines only). `run.*` and `build.*` lines (commit, dirty flag, compiler, platform, flags,
  from the generated `$(BUILD)/generated/px_build_info.hpp`) are recorded, never conditions
  and never fingerprinted: two builds facing the same conditions must stay comparable.
- **Suite sessions end on 25 ms of simulated time, not on requests.** A request bound
  counts the candidate's own orders, so a busy candidate would end its arm early and be
  marked at a different closing fundamental. Suite numbers are therefore not the
  published 200,000-request numbers, and they never go in `results/`.
- **The seat runs at the suite's reaction delay, 10,000 ns, not the strategy's declared
  `reaction_ns`.** The declared value is recorded in `suite.cfg` and not used. A
  strategy that reads its own `reaction_ns` member sees what it declared; the delay it
  actually has is `now_ns - as_of_ns` in its `MarketView`.
- **The suite fingerprints are pinned** (`kPinnedSmokeFingerprint` in `test_arena`,
  `d461c30ab2446acf`, and one per family). A change to a reference strategy, a default, the session or the
  engine moves it and needs a suite version bump in `standard_suite()`, not a silent
  re-pin: "standard v1" names one set of conditions, and two runs are comparable only if
  their fingerprints are equal.
- **One claim not to make.** Nothing here redeems the positive control that failed in
  `docs/ARENA.md`: that control failed because a maker stale enough to be reliably
  picked off also stops tracking the fundamental.

## Rules the code depends on

These are rules about the **engine**. Two of them stop at `arena/`'s edge, and that
is deliberate rather than an oversight — see the "rules" section of `docs/ARENA.md`:
arena code allocates freely, and nothing in it is on a hot path. The arena adds one
rule of its own: **no floating point inside a session**, because a `std::log` is free
to differ between library versions and would silently break cross-platform replay.

- **The matching core stays single-threaded.** No locks or atomics in the engine.
- **Zero allocation after construction.** Every container is sized by `EngineConfig`
  and allocated, zero-filled (so pre-faulted) in the constructor. `test_alloc_audit`
  holds this at 0 across 1.1M `process()` calls and across the pipeline's window.
  This is a claim about `process()`; `arena/` is exempt and says so.
- **No C-runtime calls on the hot path.** `sorted_levels.hpp` shifts with explicit
  loops: Windows UCRT's `memmove` took about 7.9µs for a 24KB overlapping downward
  shift versus 0.6µs for a plain loop. `-fno-tree-loop-distribute-patterns` (in both
  flag sets, because LTO generates code at link time) stops GCC turning them back into
  `memmove`. Verify with
  `objdump -d build/debug/obj/src/matching_engine.o | grep -c memmove`, which must be 0.
- **Admission is decided before matching**, so an order is never partly executed and
  then rejected. Change this and `tests/reference_engine.hpp` must change identically.
- **Determinism is a contract.** Same input, bit-identical event stream, on every OS.
  Seed 42: pipeline hash `63bb035a395bae79` (thin), `75ec1ab46ce5260a` (deep);
  `test_determinism` hash `77a560d8ffb4fa43`. If a change moves a hash, it changed
  behaviour — verify that was intended and say so explicitly. The arena has its own
  hash over the event stream *and* every event's arrival time, printed by `sweep` and
  asserted by `test_arena`; it moves whenever any strategy or any default changes, which is
  expected, so it is not pinned here. The suite's fingerprint is the exception: it is
  pinned, and moving it means a suite version bump (see "Strategies of your own").
- **Ring payloads stay trivially copyable and exactly 64 bytes** (`static_assert`ed).
- **Sides hold at most `levels_per_side` (2048) levels**; beyond that a GTC order is
  rejected `BookFull`. That bound is what makes the worst-case index shift bounded.

## Measurement, and how claims are worded

Matching latency is timed on the matcher thread from `tsc_begin()` right after a
request is dequeued to `tsc_end()` after its last event is written to the outbound
ring. Queueing time in the inbound ring is excluded: this is service time, **not**
wire-to-wire tick-to-trade. The ~32ns timer floor is printed, never subtracted.

The gate is fixed: **p99.9 < 2µs, zero ring stalls, zero allocations in the window.**
Do not add knobs that move it and do not trim outliers; the count over 2µs and the
slowest single order are always reported beside it. Paced runs currently fail that gate
on this laptop and `docs/ENGINE.md` says so. When a cause is not established (most of the
paced tail), write "not established" rather than blaming the OS.

Claims in README.md, `docs/ENGINE.md` and `docs/RESULTS.md` must be reproducible from
`results/` by the scripts above.

**Arena numbers are a different kind of number and must never be mixed with these.**
A session invents its nanoseconds; a reaction delay is an input the experimenter
chooses, not a measurement of anything. Nothing in `arena/` is called latency, and the
sweep prints the distinction at the top of every run. When writing about the arena,
report the **paired** difference across shared seeds, say plainly when a range
straddles zero, and print the book-to-fundamental gap first: a book that has come
loose from the fundamental is not a market and nothing read off one counts.

## Gotchas

- **Flags are not dependencies.** Changing a flag in the Makefile does not rebuild existing
  objects; `make clean` (or a fresh `BUILD=`) before comparing builds. Keep
  `-ffp-contract=off` for every compiler: GCC fuses C++ by default, and a fused multiply-add
  moves the report's last digit (`docs/PLATFORMS.md`).
- **`tests/canonical.txt` is not re-recorded to make CI pass.** A line that moved means an
  output changed; find out why, the way a moved hash is handled. It is in C glob order,
  which `tools/canonical.sh` forces with `LC_ALL=C`.
- **Nothing in a session may include `px/cycle_clock.hpp` or `px/platform.hpp`**;
  `test_arena` stops compiling if one does. Simulated time never reads hardware.
- `sim/alloc_counter.hpp` replaces global `operator new`; include it from exactly one
  translation unit per executable.
- Changing engine behaviour changes generated streams too, because the generator runs a
  shadow engine.
- `bench_engine`'s cases are single-threaded and restore the book untimed after each
  sample; keep that pairing when adding cases.
- Laptop results depend on power source and plan — `results/*/machine.txt` records
  both, and the committed Windows numbers were captured on battery. Prefer AC power.
- Linux `harden_process()` only calls `mlockall(MCL_FUTURE)` when `RLIMIT_MEMLOCK` is
  unlimited; with a finite limit it turns later allocations into `std::bad_alloc`.
- `.gitattributes` pins LF. Windows and WSL share one working tree and a CRLF shell
  script fails under bash with `bash\r`. Editing files from Python on Windows writes
  CRLF unless you write bytes or pass `newline=''`, and Windows binaries write CRLF
  through stdout redirection, so generated evidence needs normalising before it is
  committed.
- **MSYS2's make cuts a shell command off at 8,191 bytes** and runs what is left, so a
  check target longer than that passes on Windows without running its end. `test-data`
  did, unnoticed, until 2026-10-26. A long check is several recipe commands, each under
  7,500 bytes, which `make recipe-check` holds (`docs/PLATFORMS.md`).
- Rebuilding `sweep.exe` or `arena.exe` while one is still running fails with a bare
  `Permission denied` from the linker on Windows. Check for the process first.
- `arena/term.hpp` reaches for `<windows.h>` and `<termios.h>`, so exactly one
  translation unit includes it: `arena/arena.cpp`. Nothing measured links it. The app's
  line editor has its own blocking raw mode in `app/line.hpp` rather than include it.
- Several arena parameters are load-bearing in non-obvious ways (the informed trader's
  position limit, the maker's inventory limit, the learning rate, the inventory skew).
  Changing one and reading only the P&L will produce a confident wrong answer; read
  the book-to-fundamental gap too. `docs/ARENA.md` records what each one cost.
- **A session holds at most 64 participants and refuses more.** The ceiling is hard, not
  a resizable buffer: changing how own-event wakes are marked would change wake *ordering*
  and move every session hash, invalidating the committed evidence. Raising it is a
  separate change with its own re-measurement. Past the old silent bound, participants got
  2.7x fewer fills — plausible-looking wrong numbers.
- **A session that stops generating orders ends on `stall_limit_ns` of simulated time**
  and sets `stats().stalled`. Simulated, so it fires identically on every replay; a
  wall-clock timeout here would have broken the replay contract. Before the bound, a
  roster of nothing but a maker hung forever.
- Report a negative arena result with its standard error. `summarize_arena.py` prints the
  smallest effect a seed count could resolve, because "underpowered" and "nothing there"
  are different claims and only one of them is usually earned.
- **Binaries and objects live in separate subtrees**: `$(REL)/name` and `$(REL)/obj/...`.
  They have to. Object paths mirror the source tree, so without the split the viewer's
  binary (`$(REL)/arena`) and its object directory (`$(REL)/arena/`) are the same path:
  Windows hides the clash behind `.exe`, and on Linux the directory wins and the viewer
  cannot be linked at all. Anything that hard-codes an object path — the `memmove` check
  above, for one — goes through `obj/`.
- The post-fill ("adverse") column is near zero for everyone by construction, because the
  fundamental is a martingale and no participant predicts it. A stale quote's loss lands
  in the contemporaneous column, which the code calls `spread`. Do not describe `adverse`
  reading zero as evidence that nothing adverse happened; `docs/ARENA.md` works through
  it, and the failed calibration is published in `results/arena-*/stale.*`.
- **Never commit a tape, a source data file or a market file.** `data/` and `markets/` are
  git-ignored; paid data usually may not be redistributed. A new importer comes with a
  sample in `examples/data/` that is made up by hand (so it carries no one else's
  licence) and a test in `test_data`, and its version string moves when what it writes
  changes (a tape records `importer=databento 2`). Importers are integer, like a session:
  no `double` anywhere.
- **No calibrated result without the known-answer control.** `calibrate` runs it on the build
  first and refuses if it does not hold; a trades-only tape is refused because it does not.
  `calibrate --robustness` recovers 13 of 15 known-answer runs within one grid step; do not
  describe calibration as exact. `replay`'s control has two planted defects in `test_data`;
  keep that pairing when changing it.

## Conventions

- **Commits and pull requests carry no Claude attribution.** `.claude/settings.json`
  is checked in and sets both `attribution` strings to empty, so the rule travels with
  the repository rather than living in one machine's `settings.local.json`. Existing
  history keeps the trailers it already has; do not rewrite it.
- Work on a branch, commit in coherent steps whose message says what changed and what
  the evidence was, and never commit a state where `mingw32-make test` fails.
- Plan before building anything substantial, in a markdown file, not in chat. When the
  thing is built, turn the plan into the design document rather than leaving a stale
  plan behind — `docs/ARENA.md` is what became of the arena's, and it keeps a record
  of the approaches that were wrong, because those are the part worth reading.

## Agent skills

### Issue tracker

Issues live in this repo's GitHub Issues. See `docs/agents/issue-tracker.md`.

### Domain docs

Single-context: one `GLOSSARY.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.
