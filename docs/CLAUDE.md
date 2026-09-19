# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

## What this is

A price-time-priority exchange matching engine in C++20, with a single-threaded
core fed by an SPSC lock-free ring, zero heap allocation on the hot path, and the
measurement machinery that backs its latency claims. README.md has the design and
the published results; this file covers building, testing and the rules the code
depends on.

## Build and test

A hand-written Makefile: `mingw32-make` on Windows (MSYS2 UCRT64 g++ 13), `make` on
Linux. No CMake.

```bash
mingw32-make test        # debug build (-Og, PX_DEBUG, _GLIBCXX_ASSERTIONS); runs all 6 tests
mingw32-make bench       # release build (-O3 -march=native -flto); runs all 3 benchmarks
mingw32-make latency     # just the 1M-order matching-latency benchmark
mingw32-make repl        # interactive REPL: build/release/repl.exe < tools/repl_demo.txt
mingw32-make all         # test + benchmarks + REPL
mingw32-make clean
```

- Objects carry `-MMD -MP` dependency files, so editing a header rebuilds what
  includes it. `-Wall -Wextra -Wpedantic` everywhere; a clean build has zero warnings
  on both Windows and Linux.
- Windows links with `-static`, so no DLL-search-order problems.
- No sanitizer runtime ships with MSYS2 mingw-w64. Run ASan/UBSan under WSL:
  `wsl -d Ubuntu -- bash -lc "cd /mnt/c/Users/Philb/prediction-markets && make BUILD=build-asan SAN='-fsanitize=address,undefined -fno-sanitize-recover=all' test"`
- `bench/run_evidence.sh <label>` regenerates `results/<label>/` (machine state plus
  every benchmark run the README quotes). `python bench/summarize_results.py
  results/<label>` turns that into the README's tables.

## Layout

- `include/px/`: the engine.
  - `types.hpp`: int64 fixed-point prices, 1.0 == 10000.
  - `messages.hpp`: the 64-byte OrderRequest / ExecEvent, the event protocol, and EventHasher.
  - `order.hpp`: 32-byte Order / PriceLevel.
  - `pool.hpp`, `flat_map.hpp`, `sorted_levels.hpp`: the per-side price index.
  - `matching_engine.hpp` plus `src/matching_engine.cpp`.
  - `spsc_ring.hpp`, `tsc_clock.hpp`, `histogram.hpp`, `platform.hpp`.
- `sim/`: everything that feeds or measures the engine.
  - `order_stream.hpp`: seeded generator.
  - `pipeline.hpp`: gateway → ring → matcher → ring → publisher threads, plus timing.
  - `alloc_counter.hpp`: global operator new counter.
- `tests/`: hand-rolled checks (`test_harness.hpp`).
  - `reference_engine.hpp` is a naive std::map oracle for `fuzz_vs_reference`.
- `bench/`: `bench_matching_latency` (headline), `bench_engine` (per operation),
  `bench_ring` (ring hop), and the evidence scripts.
- `tools/repl.cpp`.

## Rules the code depends on

- **The matching core is single-threaded.** Only the pipeline's matcher thread ever
  calls `MatchingEngine::process`. The engine has no locks or atomics and must not
  gain any.
- **Zero allocation after construction.** Every container is sized by `EngineConfig`
  and allocated, zero-filled, and so pre-faulted, in the constructor.
  `test_alloc_audit` proves this: 0 allocations across 1.1M `process()` calls and
  across the pipeline's recorded window. Keep it at 0.
- **No C-runtime calls on the hot path.** `sorted_levels.hpp` shifts with explicit
  loops because Windows UCRT's `memmove` took about 7.9µs for a 24KB overlapping
  downward shift. `-fno-tree-loop-distribute-patterns`, set in the Makefile for both
  builds because LTO generates code at link time, stops GCC from turning those loops
  back into `memmove`. Check with
  `objdump -d build/debug/src/matching_engine.o | grep -c memmove`, which must print 0.
  Release objects are LTO bytecode, so check the debug object.
- **Admission before matching.** Every reject, including BookFull, happens before
  any state changes. An order is never partly executed and then rejected. If you
  change this rule, change `tests/reference_engine.hpp` identically.
- **Events carry no timestamps.** The same input must give a bit-identical event
  stream. For seed 42 the thin-profile pipeline hash is `63bb035a395bae79` and
  test_determinism's is `77a560d8ffb4fa43`, on Windows and Linux alike. A change that
  moves either hash changed behaviour; say so.
- **Ring payloads stay trivially copyable and 64 bytes** (both are `static_assert`ed).
- **Sides hold at most `levels_per_side` (2048) price levels.** A GTC order that would
  need level 2049 is rejected with BookFull. The worst-case shift (about 24KB) is
  bounded by that capacity and measured in `bench_engine`.

## What "matching latency" means here

Measured on the matcher thread from `tsc_begin()` right after an order is dequeued
from the inbound ring to `tsc_end()` after its last event is written to the outbound
ring. Time spent queued in the inbound ring is excluded; this is service time, not
wire-to-wire tick-to-trade. The timer floor (about 32ns) is printed, never
subtracted.

The gate is fixed: p99.9 < 2µs, zero outbound-ring stalls, zero allocations. The
count over 2µs and the max are always reported beside it. Do not add knobs that move
the gate, and do not trim outliers.

## Gotchas

- `sim/alloc_counter.hpp` replaces the global `operator new`. Include it from exactly
  one translation unit per executable.
- The generator (`sim/order_stream.hpp`) runs a private shadow engine while it
  generates, so that cancels target orders that are really resting. Changing engine
  behaviour therefore changes generated streams too.
- `bench_engine`'s cases are single-threaded and restore the book untimed after every
  sample. Keep that pairing when adding cases.
- Latency results on a laptop depend on power source and power plan;
  `results/*/machine.txt` records both. Run on AC power before publishing numbers.
- Linux `harden_process()` calls `mlockall(MCL_FUTURE)` only when `RLIMIT_MEMLOCK` is
  unlimited. With a finite limit, `MCL_FUTURE` makes every allocation past the limit
  fail, and the 1M-order benchmark then aborts with `std::bad_alloc`.
- `.gitattributes` forces LF line endings. The Windows and WSL builds share one
  working tree, and a CRLF shell script fails under bash with `bash\r`. When editing
  files from Python on Windows, write bytes or pass `newline=''`.
