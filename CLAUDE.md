# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A price-time-priority exchange matching engine in C++20: a single-threaded matching
core fed by lock-free SPSC rings, no heap allocation after start-up, int64 fixed-point
prices, plus the test and measurement machinery that backs its latency claims — and
`arena/`, a seeded simulated market of bots trading inside that engine, with a
controllable reaction delay each and a terminal renderer. README.md holds the design
and the published results, `docs/ARENA.md` the arena's design and reasoning, and
`handoff.md` the narrative of what was built, how the terminal viewer works and what is
left; this file covers how to work in the repo.

**History, because the repo name and old commits mislead.** This was a Kalshi
prediction-market trading system. The whole live stack (WebSocket/REST client,
strategies, evaluation framework, telemetry, Rust dashboard) was deleted at `07b3be0`
and is not coming back: the user cannot trade on Kalshi. Nothing in the tree depends on
an exchange account, an API or a network. Do not reintroduce a market-data client or a
trading strategy into the engine. Strategies live in `arena/bots/`, never in
`include/px/` or `src/`. The old strategy code is still readable in history
(`git show 07b3be0^:strategy/passive_market_maker.hpp`).

**Status.** The engine is finished, measured and committed. The arena is built,
measured and committed: see `docs/ARENA.md` for its design, the modelling mistakes that
produced confidently wrong results on the way, and the parameters that turned out to be
load-bearing. Its scope boundary is binding — everything compiles into one binary and
nothing talks over a network. All of it sits on the branch `arena`, open as pull request
#1 against `main`. The next piece of work is the strategy lab, designed and not built:
see "The strategy lab" below, and `docs/LAB_PLAN.md` for the task-by-task plan. It
leaves that scope boundary intact, which an earlier draft of it did not.

**The result is a negative one and the documentation leads with it.** "What is 10
microseconds worth?" answers *less than 24 seeds can resolve*; 1 ms costs real money,
but the null run (no informed participant at all) retains 96% of that effect, so the
cost is competition for uninformed flow rather than adverse selection. A second,
positive control then failed outright — no admissible way exists to calibrate the
post-fill column, because a maker stale enough to be reliably picked off also stops the
book tracking the fundamental. When writing about this project, do not restore the tidy
version: the controls changed the answer twice and that is the interesting part.

**The name.** The repository is `exchange-lab`, renamed from `prediction-markets`,
which described something this tree no longer contains. Not
`matching-engine`, which was the earlier plan: the arena is now larger than the engine
and holds the project's actual result, so that name undersells it and is also shared
with several hundred other repositories. `exchange-lab` is the tree's own description
of itself — an exchange, and the lab where strategies and speed compete.

Two names to avoid, for reasons the code depends on. Anything containing **latency**
would undo the one piece of vocabulary discipline this project keeps: the engine's
latency is measured and a bot's `reaction_ns` is invented, nothing in `arena/` is called
latency, and a repository named `latency-*` re-merges them before a reader opens a file.
**tick-to-trade** would be a false claim, because the measurement is service time and
README.md says so explicitly.

The GitHub **description** field does the keyword work the name deliberately does not,
and it is indexed where the name alone is not. Set it to:

> Price-time-priority exchange matching engine in C++20 — p99.9 under 2µs, zero
> allocation, bit-identical cross-OS replay — and the deterministic market simulator
> built on it.

That description is set. GitHub keeps redirects from the old URL, so an old clone keeps
working, but the remote here already points at
`https://github.com/PhillysTheBest/exchange-lab.git`.

**The local checkout is still `prediction-markets`,** and that is the one part not done:
renaming the folder pulls the directory out from under any running session. When it
moves, the WSL path in the sanitizer command below moves with it, and that is the only
path in this file that needs changing.

## Build and test

Hand-written Makefile: `mingw32-make` on Windows (MSYS2 UCRT64 g++ 13), `make` on
Linux. No CMake, no package manager, no third-party libraries — keep it that way.

```bash
mingw32-make test       # debug (-Og, PX_DEBUG invariants, _GLIBCXX_ASSERTIONS): builds and runs all 7 tests
mingw32-make bench      # release (-O3 -march=native -flto): builds and runs all 3 benchmarks
mingw32-make latency    # only the 1M-order matching-latency benchmark
mingw32-make repl       # then: build/release/repl.exe < tools/repl_demo.txt
mingw32-make arena      # the arena: build/release/sweep.exe and build/release/arena.exe
mingw32-make test-cli   # the sweep runner's command line: bad flags fail, bad configs refused
mingw32-make all        # tests, command-line checks, benchmarks, REPL, arena
mingw32-make clean
```

Running the arena:

```bash
build/release/arena                                  # watch the default market
build/release/arena --bots=maker,informed,noise:2    # compose the line-up by name
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
  every run the README quotes. `python bench/summarize_results.py results/<label>`
  prints the README's tables. Never hand-edit numbers in `results/` or README.
- `python bench/summarize_arena.py results/arena-<label>` prints the arena tables that
  README and `docs/ARENA.md` quote, including the paired standard errors and the smallest
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
simulated clock, a roster of bots and the accounting; `arena/sweep.cpp` runs the
experiment headless; `arena/arena.cpp` renders it. It adds no engine API: a bot's view
of the book, including its queue position, is reconstructed from the `ExecEvent`
stream in `arena/book_mirror.hpp`. Read `docs/ARENA.md` before changing anything in
there — several of its parameters are load-bearing in ways that are not obvious, and
the document records which and why.

**Bots** are plain structs in `arena/bots/`, compiled in and never loaded at runtime.
Adding one is a header plus one line in `add_named_bot()` and its name in `kBotNames`,
which `test_arena` holds together so the two cannot drift. `arena/bots/fader.hpp` is the
worked example, written against `arena/bot.hpp` alone. `--bots=maker,rival,noise:3`
composes a line-up by name; without it the registry's default line-up is used, which is
what every published number came from. The strategy lab, designed below, turns those
two steps into one make target for a bot of your own; it does not change what a bot is.

**`arena/ui.hpp`** owns a grid of coloured cells and knows nothing about trading;
`arena/arena.cpp` is the only place the renderer and the market meet. A frame composes
into the grid and goes out in one `fwrite`, emitting a colour escape only where a colour
changes. `handoff.md` explains the viewer and its purpose at length.

## The strategy lab (designed, not built)

The next piece of work: anyone with the repo and a compiler adds their own bot to the
baseline market and gets a reproducible, honestly-qualified score for it. **None of it
exists in the tree yet.** The design is settled and the task-by-task plan is
`docs/LAB_PLAN.md`; this section is the specification, and the parenthesis in its
heading comes off when it lands.

**What it is.** `make bot BOT=path/to/mybot.hpp` validates the header, copies it into
`arena/bots/user/`, regenerates a manifest, and prints the two commands to run it:
`arena_lab --bots=...,mybot` to watch it, `sweep_lab --score=mybot` to score it.

**It is not runtime loading, and `docs/ARENA.md`'s boundary prose stays true as
written.** A strategy still links into the binary: no plugin DLL, no embedded scripting
language, no ABI, no sandbox, nothing a running session accepts from outside itself.
Installing a bot is a build step, not something the viewer does — the viewer is not
modified by this work at all. An earlier version of the design had it accept a dragged
file, rebuild itself and respawn; that was cut after review, and "What was cut" below
says why, because that is the part worth reading.

**A user bot cannot reach the published evidence, by construction,** and this is the
decision everything else rests on. The manifest is included only under
`-DPX_ARENA_USER_BOTS`, and the only targets setting it are two new binaries,
`$(REL)/arena_lab` and `$(REL)/sweep_lab`. `arena`, `sweep`, every test and `test-cli`
are compiled without it, so a user bot is not in their translation units at all. The
reason is specific rather than cautious: `arena/session.hpp` includes
`arena/bots/registry.hpp`, `tests/test_arena.cpp` includes the session, and `test-cli`
builds and runs the *release* `sweep`. An unguarded delegation would therefore have put
a stranger's header into the binaries that assert the pinned hashes, and a header with
a typo would have broken `mingw32-make test`. Lab objects need their own object subtree,
because their flags differ from `$(ROBJ)/arena/*.o` and make would otherwise reuse an
object built without the define.

**The manifest is generated into the build tree and never committed** —
`$(BUILD)/generated/px_user_bots.hpp`, already covered by `.gitignore`'s `build*/`.
Committing a generated file that the installer rewrites would leave it permanently
dirty and conflict on every pull. Only `arena/bots/user/*.hpp` is ignored; the directory
keeps a `.gitkeep` so a clean clone has it.

**Scoring is paired against a placebo seat, not against the bot's absence.** Adding a
participant adds wake events, and changed wake ordering moves the session hash, so a
bare with-and-without comparison would conflate a bot's market impact with a
re-interleaved session. Both arms carry the same extra seat instead: one holds the
user's bot, the other holds `placebo`, an inert participant that never acts. The scoring
baseline is consequently the default line-up *plus one inert seat*, which is not
identical to the published market, and the output says so. Report the gap first, then
the paired value with its standard error and the smallest effect the seed count can
resolve, then the shift in each baseline participant's value.

**A scored run containing a user bot prints no session hash, and says why.** No floating
point inside a session is what makes replay bit-identical across platforms, and a user's
C++ bot is free to use a `double`. That cannot be detected: a same-machine
`--replay-check` cannot see it, because the divergence is between library versions on
different platforms, and a source scan for `double` misses `n/2.0`, `std::sqrt` and a
`0.5` in a template. An 80%-accurate warning is the kind of overclaim the rest of this
project refuses, so the hash is withheld instead. The arena's own numbers do not depend
on this, because no user bot is in the default line-up or in `sweep` at all.

**What was cut, and why.** The first design had a visitor drag a bot header onto the
running viewer, which would install it, shell out to the compiler, link a second binary,
hand over the terminal and respawn into it. A council review rejected it unanimously and
two facts settled it. The audience is empty: `g++` and `mingw32-make` exist only inside
the MSYS2 UCRT64 shell, so a visitor who could drag a file out of Explorer has no
compiler, and one who has a compiler already has a shell where `make bot` is better. And
it is too slow: one arena translation unit plus its LTO link measures **27 seconds** on
this laptop, and a manifest change invalidates both `arena.o` and `sweep.o`, so a drop
costs about 50-60 seconds rather than the 20 the earlier draft claimed. The same review
found the include chain above. `docs/LAB_PLAN.md` keeps the whole record, including the
measurements.

**One claim not to make.** Scoring does *not* redeem the positive control that failed in
`docs/ARENA.md`. That control failed because a maker stale enough to be reliably picked
off also stops tracking the fundamental; a paired design measures a perturbation, it
does not make a stale probe admissible.

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
  asserted by `test_arena`; it moves whenever any bot or any default changes, which is
  expected, so it is not pinned here.
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
on this laptop and the README says so. When a cause is not established (most of the
paced tail), write "not established" rather than blaming the OS.

Claims in the README must be reproducible from `results/` by the scripts above.

**Arena numbers are a different kind of number and must never be mixed with these.**
A session invents its nanoseconds; a reaction delay is an input the experimenter
chooses, not a measurement of anything. Nothing in `arena/` is called latency, and the
sweep prints the distinction at the top of every run. When writing about the arena,
report the **paired** difference across shared seeds, say plainly when a range
straddles zero, and print the book-to-fundamental gap first: a book that has come
loose from the fundamental is not a market and nothing read off one counts.

## Gotchas

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
  CRLF unless you write bytes or pass `newline=''`.
- Rebuilding `sweep.exe` or `arena.exe` while one is still running fails with a bare
  `Permission denied` from the linker on Windows. Check for the process first.
- `arena/term.hpp` reaches for `<windows.h>` and `<termios.h>`, so exactly one
  translation unit includes it: `arena/arena.cpp`. Nothing measured links it.
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
