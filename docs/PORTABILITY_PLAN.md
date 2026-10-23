# Cross-platform support: plan

Written 2026-10-23, before any code changes, as `CLAUDE.md` asks. Nothing below is built
yet. When it is, this file becomes a "Platforms" section of `docs/ENGINE.md`, or a document
of its own, recording what is supported, what is checked and what is only expected.

## Three kinds of compatibility

| Kind | Question | Priority |
|---|---|---|
| **Behavioural** | Does the same experiment give the same trades, fills, book and results on every machine? | first |
| **Build** | Does it compile and run on Linux, Windows and macOS, x86-64 and ARM64? | second |
| **Performance** | Can engine timings be compared between machines? | documented separately, never promised |

**What the project already guarantees.** Everything inside a session is integer: prices
in ticks, quantities in lots, cash in price units times quantity, simulated time in made-up
nanoseconds. The random generator is hand-written (`arena/clock.hpp`). Equal-time events are
ordered by fixed rules. No hardware clock is read inside a session. Today the committed
evidence (`results/arena-windows/` and `results/arena-wsl2/`) shows every published number
and session hash identical on Windows and Linux, both x86-64 and both GCC 13 with libstdc++.
No other combination has been tested. Simulated time is already independent of hardware
timing, and this plan keeps it that way.

**What is canonical and what is not.**
- **Canonical, and must be identical everywhere:** the engine's event stream and its hash,
  every session hash, the suite fingerprints, every tape `ingest` writes, and every
  integer column of a run's CSVs.
- **Derived, from `double`, outside the session:** the report's means, standard errors,
  intervals and percentages (`arena/suite_report.hpp`, `arena/sweep.cpp`). These are what
  `/reproduce` compares byte for byte, so a last-digit difference here would make
  reproduction fail between platforms even though the market behaved identically. Stage 4
  measures whether this happens. It does not claim it cannot.
- **Measurements, never comparable between machines:** the engine benchmarks' service
  times.

## Why each platform-specific piece exists

| Piece | Where | Why it is there | Affects results? |
|---|---|---|---|
| `-fno-tree-loop-distribute-patterns` | `Makefile`, both builds | Stops GCC turning the hand-written shifts in `sorted_levels.hpp` back into `memmove`. Windows UCRT's `memmove` took about 7.9 µs for a 24 KB overlapping shift, against 0.6 µs for the loop. It protects the p99.9 gate. | No: speed only |
| `-march=native` | release flags | Lets the benchmarks use the build machine's instructions | No for integers; could matter for `double` with contraction (stage 4) |
| `-flto` | release flags | Whole-program inlining for the benchmarks | No |
| `-static` | Windows link | No DLL-search-order surprises | No |
| `_GLIBCXX_ASSERTIONS` | debug flags | libstdc++ bounds checks | No; libc++ ignores it, and has its own (stage 5) |
| `rdtsc`/`rdtscp` + `lfence` | `include/px/tsc_clock.hpp`, `sim/pipeline.hpp`, `include/px/histogram.hpp` | Service-time measurement in the benchmarks, and pacing the gateway thread | No: benchmarks and the pipeline only, never a session |
| `_mm_pause` (`cpu_relax`) | `include/px/platform.hpp`, the SPSC ring, the pipeline | Spin-wait hint for the busy-waiting ring threads | No |
| thread affinity, `SCHED_FIFO`, `mlockall`; Windows priority class | `src/platform_*.cpp` | Benchmark hygiene | No. Already optional: `pin_current_thread_to` returns false on failure and the callers carry on |

The arena, the suite, the app and `marketdata/` use none of the last three, but they link
`src/platform_*.cpp` through the engine library. On ARM that file and `platform.hpp` do not
compile, which is why nothing builds there today.

## Supported targets

| Target | Status after this plan | Checked by |
|---|---|---|
| Linux x86-64, GCC | supported, as today | CI and local |
| Windows x86-64, MSYS2 UCRT64 GCC | supported, as today | CI, and Philbert's laptop |
| macOS ARM64, Apple clang and libc++ | supported | CI only (no one here has a Mac) |
| Linux x86-64, clang | checked, not promised | CI |
| Linux ARM64 | experimental | CI, if the runner is available on the plan |
| Windows ARM64, Intel macOS | not supported | nothing |

## Stages, one commit each, every test green before the next

Each stage runs, on Linux, `make test test-cli test-lab test-app test-data`, and checks that
the pinned hashes (`77a560d8ffb4fa43`, `63bb035a395bae79`, `75ec1ab46ce5260a`, the session
hash `2853d2127a3486c5`, the suite fingerprint `d461c30ab2446acf`) have not moved, and that
`objdump ... | grep -c memmove` is still 0. Stages before CI exists also need a Windows run
from Philbert before the claim "Windows unchanged" is made.

**0. A canonical record to compare against (first, before any change).** Write a small
program, or a `test_determinism` mode, that prints one line per canonical output: the engine
hashes, the session hash on seeds 42 to 45, the four suite fingerprints, the tape hash of
every sample, and the `sha256` of a smoke run's `sessions.csv`, `participants.csv` and
`report.txt`. Commit its output from Linux as `tests/canonical.txt`. Every later stage, and
every CI job, compares against that file line by line, so a difference names what diverged
first. This is the golden scenario. It also makes the report's `double` arithmetic visible
separately from the integer state.

**1. Compiler-specific flags.** Detect the compiler in the Makefile (`$(CXX) --version`).
Pass `-fno-tree-loop-distribute-patterns` to GCC only. Clang does not rewrite these loops in
the same way, so the `memmove` check runs on the clang build too, to prove it rather than
assume it. Keep `-march=native` (Apple clang 15 and later accept it). Use `-static` on
Windows only, and nothing extra on macOS. Expected effect: none on GCC builds, so the
canonical file must not change.

**2. CPU-specific timing and spin-wait behind one header each.**
- `px/cycle_clock.hpp`: `rdtsc` on x86-64, `cntvct_el0` on ARM64 (a fixed-frequency
  counter), `std::chrono::steady_clock` elsewhere. Each reports its name and resolution, and
  the benchmarks print them beside every number.
- `cpu_relax()`: `_mm_pause` on x86, `yield` on ARM64, nothing elsewhere. It is a hint,
  so a missing one is still correct.

Whether busy-waiting is needed at all: the ring's threads spin by design, because the
benchmarks measure service time with a consumer that never sleeps. A blocking or atomic wait
would change what is measured, so it stays as it is.

Nothing in a session reads either header. A check in `test_arena` asserts that, for
example by building the session without including them. Expected effect: none on x86, so
the canonical file must not change.

**3. Affinity as a capability.** Add `src/platform_posix.cpp` for macOS and any other POSIX
system. It reports that affinity is unavailable, and uses `mlockall` where permitted. The
platform API returns one of: applied, requested but unavailable, or default scheduling. The
benchmarks print it in their header, as they already print the power plan. Nothing promises
equivalent pinning on macOS, whose affinity tags are only placement hints. `ENGINE.md` says
that benchmark numbers from different machines, and from any shared CI runner, are not
comparable.

**4. Floating-point contraction, measured.** Build the report with and without
`-ffp-contract=off`, under GCC and clang, and compare against stage 0's canonical file.
Report what changed, if anything, and the cost on the benchmarks (expected nil, because the
engine has no floating point). If clang's default changes a digit, set `-ffp-contract=off`
for every compiler. Under GCC with `-std=c++20` it is already the default, so GCC output
must not change. The claim made afterwards is narrow: "with contraction off, the
report's numbers agreed on the targets CI ran". Contraction is one known source of
difference. Others (library `sqrt` and printing, evaluation order) are named as not ruled
out. If the report ever disagrees between platforms, the fix is to compute its statistics in
integers or fixed point. That is a larger change and needs its own plan.

**5. libc++.** On Linux, build with clang and `-stdlib=libc++`, and on macOS with Apple's
libc++. Compare against the canonical file. Iterating an `unordered_map` gives a different
order under libc++. A code review found every such iteration either feeds a sort or an
order-independent sum, but the comparison is what proves it. Add libc++'s hardening mode to
the debug flags beside `_GLIBCXX_ASSERTIONS`.

**6. CI.** One GitHub Actions workflow, run on every push to the branch and every pull
request:

| Job | Runner | Runs |
|---|---|---|
| linux-gcc | ubuntu, x86-64 | `make test test-cli test-lab test-app test-data`, plus the sanitizer build |
| linux-clang-libcxx | ubuntu, x86-64 | `make test`, the canonical file |
| windows | windows, MSYS2 UCRT64 | the same targets as Linux, via `mingw32-make` |
| macos-arm64 | macos, ARM64 | `make test test-app test-data`, the canonical file |
| linux-arm64 | ubuntu ARM64, if available | `make test`, the canonical file |
| compare | after all of them | collects every job's canonical output and fails on any line that differs from `tests/canonical.txt`, naming the target and the line |

Benchmarks run in CI only to check that they start. Their timings are never compared:
shared runners cannot arbitrate service time. The workflow sets up a compiler and nothing
else, and the project still has no third-party libraries. Philbert confirms before it is
added, because it is the project's first contact with an outside service and runner minutes
depend on the repository's plan.

**7. Report.** A short document of what passed on each target, which configurations are
untested, and any change to output or performance. Plus README's "Start here" gains the
macOS line, only once the macOS job is green.

## Not in this plan, and why

- **A canonical simulation server (running experiments remotely).** It would put a network
  between the user and the market, and `CLAUDE.md` rules out any network. The project's
  version of "canonical" is the committed evidence and the pinned fingerprints, re-runnable
  by anyone. If remote runs are wanted later, that is a decision for Philbert, not a
  portability fix.
- **Making benchmark numbers comparable across machines.** They are measurements of one
  machine and are published as such.
- **Strategy portability.** A strategy is integer C++ against `arena/strategy.hpp`, already
  versioned by the suite and recorded by checksum. `/reproduce` already says, on another
  platform, whether an installed strategy reproduced. Stage 0's record gives that a check.

## Decisions for you

- **CI on GitHub Actions** (stage 6): yes or no. Recommended yes. Without it, macOS is
  only "should build".
- **Linux ARM64 runner**: include it if the plan provides it. Recommended yes, as
  experimental.
