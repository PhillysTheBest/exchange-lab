# Platforms

Which machines exchange-lab builds and runs on, what is checked on each, and what "the
same result on every machine" does and does not mean. This was a plan, written
2026-10-23 before any code changed, and built in seven stages. Where the plan turned out
to be wrong, this document says so.

## Supported targets

| Target | Status | Checked by |
|---|---|---|
| Linux x86-64, GCC 13 | supported | CI (`linux-gcc`), and locally under WSL2 |
| Windows x86-64, MSYS2 UCRT64 GCC 13 | supported | CI (`windows`), and the development laptop |
| macOS ARM64, Apple clang 15 and libc++ | supported | CI only (`macos-arm64`): nobody here has a Mac |
| Linux x86-64, clang 18 and libc++ | checked, not promised | CI (`linux-clang-libcxx`) |
| Linux ARM64, GCC | experimental: passes, never blocks a run | CI (`linux-arm64`) |
| Windows ARM64, Intel macOS | not supported | nothing |

On macOS, `make` is the GNU make 3.81 Apple ships, and that is enough. Nothing else needs
installing besides the compiler Xcode's command-line tools provide.

## What must be identical, and what need not be

- **Canonical: identical everywhere.** The engine's event stream and its hash, every
  session hash, the suite fingerprints, every tape `ingest` writes, and every integer
  column of a run's CSVs. All of it is integer arithmetic: prices in ticks, quantities in
  lots, simulated nanoseconds, a hand-written random generator, fixed rules for
  equal-time events, and no hardware clock read inside a session.
- **Derived: computed in `double`, outside the session.** The report's means, standard
  errors, intervals and percentages (`arena/suite_report.hpp`). `/reproduce` compares
  these byte for byte, so a difference in the last digit would make reproduction fail
  between machines even though the market behaved identically.
- **Measured: never comparable between machines.** The engine benchmarks' service times.
  [ENGINE.md](ENGINE.md) says so beside the numbers.

## The canonical record

`make canonical` (`tools/canonical.sh`) prints one line for each canonical output: the
engine determinism hash, the session and frame hashes, the sweep's seed-42 hashes, the
three suite fingerprints, the `cksum` of a smoke run's `sessions.csv` and
`participants.csv`, and of every tape the samples and the known-answer control produce.
The four run files that hold derived statistics come last, labelled `derived.*`, so a
difference in arithmetic is told apart from a difference in the market. It then diffs
the result with `tests/canonical.txt`, recorded on Linux x86-64 with GCC 13. The first line
that differs names what diverged first.

**Result.** All 25 lines are identical on all five CI targets, the derived lines
included, and under every compiler configuration tried locally: GCC and clang, libstdc++
and libc++, and each floating-point contraction setting below.

## What each platform-specific piece is for

| Piece | Where | Why it is there | Affects results? |
|---|---|---|---|
| `-fno-tree-loop-distribute-patterns` | `Makefile`, GCC only | Stops GCC turning the shift loops in `sorted_levels.hpp` back into `memmove`, which on Windows UCRT took 7.9 µs against 0.6 µs. Clang refuses the flag, and `make memmove-check` proves its build has no `memmove` instead of assuming it. | No: speed only |
| `-ffp-contract=off` | `Makefile`, every compiler | See below | It decides the report's last digit |
| `-march=native`, `-flto` | release flags | The benchmarks | No, now that contraction is off |
| `-static` | Windows link | No DLL-search-order surprises | No |
| `_GLIBCXX_ASSERTIONS`, `_LIBCPP_HARDENING_MODE=EXTENSIVE` | debug flags | Each standard library's own bounds checks. Each library ignores the other's macro. | No |
| the cycle counter | `include/px/cycle_clock.hpp` | Service time in the benchmarks, and pacing the pipeline's gateway: `rdtsc` with `lfence` on x86-64, `cntvct_el0` with `isb` on ARM64, `steady_clock` elsewhere. Each benchmark prints which counter it used and its resolution. | No: never read in a session |
| `cpu_relax()` | `include/px/platform.hpp` | Spin-wait hint for the ring threads: `pause` on x86-64, `yield` on ARM64, nothing elsewhere. It is only a hint, so a spin loop without it is still correct. | No |
| affinity, `SCHED_FIFO`, `mlockall`, priority class | `src/platform_{linux,win32,posix}.cpp` | Benchmark hygiene. `pin_current_thread_to` reports `applied`, `requested but unavailable` or `default scheduling`, and the benchmarks print it. | No |

`test_arena` fails to compile if any session, suite or report header includes the counter
or the platform header, so "never read in a session" is checked, not assumed.

The ring threads keep busy-waiting on every platform. The benchmarks measure service time
with a consumer that never sleeps, so a blocking wait would change what they measure.

`src/platform_posix.cpp` serves macOS and any POSIX system that is not Linux. It pins
nothing. macOS affinity tags are hints that threads share a cache, not a placement, and
printing "pinned" above numbers from threads the scheduler moved freely would be false.
It uses `mlockall` under the same `RLIMIT_MEMLOCK` rule as Linux.

## Floating-point contraction, measured

A fused multiply-add rounds `a*b+c` once where the separate operations round twice, so
whether a compiler fuses decides the last digit of a statistic.

**The plan was wrong here.** It assumed GCC already defaults to `-ffp-contract=off` under
`-std=c++20`. It does not: that default applies only to ISO C, and GCC fuses C++. The
release `suite` binary had 6 FMA instructions, all in `paired_values` (mean ± t·se and the
variances). That held under GCC's default and clang 18's default alike, while clang with
`=fast` made 4. On x86-64 this needs `-march=native` on a machine with FMA. On ARM64 FMA
is always there.

Every setting tried gave the same canonical record, the derived lines included. That is
only because the report prints four decimals: the arithmetic itself differed. So
`-ffp-contract=off` is set for every compiler, and the `suite` binary now has no FMA. The
engine has no floating point. Its debug object and the release benchmark's
`MatchingEngine::process` (794 instructions) are unchanged, and the benchmarks had no FMA
either way, so the flag costs nothing there.

The claim is narrow: **with contraction off, the report's numbers agreed on every target
CI ran.** Contraction is one known source of difference. The library's `sqrt`, number
printing and evaluation order are not ruled out. If the report ever disagrees between
platforms, the fix is to compute its statistics in integers or fixed point, which is a
larger change with its own plan.

## libc++

Iterating an `unordered_map` gives a different order under libc++ than under libstdc++.
A review found that each of the 18 uses in the tree either feeds a sort or an
order-independent sum, and the canonical record is what proves it. It is identical with
clang 18 and libc++ 18 on Linux, and with Apple's libc++ on macOS.

## CI

`.github/workflows/ci.yml` runs on every pull request and every push to `main`:

| Job | Runner | Runs |
|---|---|---|
| `linux-gcc` | Ubuntu 24.04, x86-64 | `test`, `test-cli`, `test-lab`, `test-app`, `test-data`, `memmove-check`, `canonical`, the benchmarks starting, and the sanitizer build |
| `linux-clang-libcxx` | Ubuntu 24.04, x86-64 | `test`, `memmove-check`, `canonical` |
| `windows` | Windows, MSYS2 UCRT64 | what `linux-gcc` runs, except the sanitizers |
| `macos-arm64` | macOS 14, ARM64 | `test`, `test-app`, `test-data`, `canonical`, the benchmarks starting |
| `linux-arm64` | Ubuntu 24.04, ARM64 | `test`, `canonical`. It never fails the run. |
| `compare` | after all of them | each job's canonical record against `tests/canonical.txt`, naming the target and the line |

The check targets run one at a time, because `test-app` and `test-data` both write under
`runs/`. Running them in parallel failed `/compare maker smoke` locally. The benchmarks
run only to show they start. Shared runners cannot arbitrate service time, so their
timings are never read. The workflow installs a compiler, plus `diffutils` on MSYS2, and
nothing else.

Two things the Windows runner found that the development laptop had not:

- **Glob order.** `examples/data/*` expanded case-insensitively there, which reordered
  three tape lines whose values were identical. `tools/canonical.sh` now sets `LC_ALL=C`.
- **One intermittent failure, cause not established.** On the first Windows run,
  `test-cli` reported that two identical smoke runs wrote a different `suite.cfg`. Every
  run since has passed, on CI and under MSYS2 on the laptop. The check now prints the file
  and the differing lines, so a repeat will show what differed.

## Not done, and why

- **A canonical simulation server** (running experiments remotely). It would put a network
  between the user and the market, which the project rules out. Here "canonical" means
  the committed evidence and the pinned fingerprints, which anyone can re-run.
- **Comparable benchmark numbers across machines.** They measure one machine and are
  published that way.
- **Strategy portability.** A strategy is integer C++ written against
  `arena/strategy.hpp`. It is versioned by the suite and recorded by checksum, and
  `/reproduce` already says whether an installed strategy reproduced on another machine.
