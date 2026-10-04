# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A price-time-priority exchange matching engine in C++20: a single-threaded matching
core fed by lock-free SPSC rings, no heap allocation after start-up, int64 fixed-point
prices, plus the test and measurement machinery that backs its latency claims.
README.md holds the design and the published results; this file covers how to work in
the repo.

**History, because the repo name and old commits mislead.** This was a Kalshi
prediction-market trading system. The whole live stack (WebSocket/REST client,
strategies, evaluation framework, telemetry, Rust dashboard) was deleted at `07b3be0`
and is not coming back: the user cannot trade on Kalshi. Nothing in the tree depends on
an exchange account, an API or a network. Do not reintroduce a market-data client or a
trading strategy into the engine. The old strategy code is still readable in history
(`git show 07b3be0^:strategy/passive_market_maker.hpp`) and may be adapted for the
arena below, where fills are exact rather than estimated.

**Status.** The engine is finished, measured and committed. Next up, planned but not
started, is `docs/ARENA_PLAN.md`: bots trading inside the engine, rendered as a retro
terminal desktop. That plan's scope boundary is binding — bots and rendering compile
into one binary, and nothing talks over a network. The GitHub repo is being renamed
from `prediction-markets` to `matching-engine`.

## Build and test

Hand-written Makefile: `mingw32-make` on Windows (MSYS2 UCRT64 g++ 13), `make` on
Linux. No CMake, no package manager, no third-party libraries — keep it that way.

```bash
mingw32-make test       # debug (-Og, PX_DEBUG invariants, _GLIBCXX_ASSERTIONS): builds and runs all 6 tests
mingw32-make bench      # release (-O3 -march=native -flto): builds and runs all 3 benchmarks
mingw32-make latency    # only the 1M-order matching-latency benchmark
mingw32-make repl       # then: build/release/repl.exe < tools/repl_demo.txt
mingw32-make all        # tests, benchmarks, REPL
mingw32-make clean
```

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

## Rules the code depends on

- **The matching core stays single-threaded.** No locks or atomics in the engine.
- **Zero allocation after construction.** Every container is sized by `EngineConfig`
  and allocated, zero-filled (so pre-faulted) in the constructor. `test_alloc_audit`
  holds this at 0 across 1.1M `process()` calls and across the pipeline's window.
- **No C-runtime calls on the hot path.** `sorted_levels.hpp` shifts with explicit
  loops: Windows UCRT's `memmove` took about 7.9µs for a 24KB overlapping downward
  shift versus 0.6µs for a plain loop. `-fno-tree-loop-distribute-patterns` (in both
  flag sets, because LTO generates code at link time) stops GCC turning them back into
  `memmove`. Verify with
  `objdump -d build/debug/src/matching_engine.o | grep -c memmove`, which must be 0.
- **Admission is decided before matching**, so an order is never partly executed and
  then rejected. Change this and `tests/reference_engine.hpp` must change identically.
- **Determinism is a contract.** Same input, bit-identical event stream, on every OS.
  Seed 42: pipeline hash `63bb035a395bae79` (thin), `75ec1ab46ce5260a` (deep);
  `test_determinism` hash `77a560d8ffb4fa43`. If a change moves a hash, it changed
  behaviour — verify that was intended and say so explicitly.
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

Claims in the README must be reproducible from `results/` by the two scripts above.

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

## Conventions

- **Commits carry no Claude co-author trailer** (`.claude/settings.local.json` sets
  `attribution` to empty). Existing history keeps the trailers it already has; do not
  rewrite it.
- Work on a branch, commit in coherent steps whose message says what changed and what
  the evidence was, and never commit a state where `mingw32-make test` fails.
- Plan before building anything substantial, in a markdown file (see
  `docs/ARENA_PLAN.md`), not in chat.
