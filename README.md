# px — a price-time-priority matching engine in C++20

A single-threaded exchange matching core fed by a lock-free single-producer/
single-consumer ring, with **no heap allocation after start-up**, int64 fixed-point
prices and a price-ordered level index. It comes with the measurement and test
machinery that backs every number below.

**Headline.** Measured on a laptop (i5-12450H, Windows 11, on battery, Balanced power
plan) over five 1M-order streams per profile. Matching latency here means the time from
dequeuing an order to writing its last resulting event to the outbound ring.

| Book | p50 | p99.9 | Under 2 µs | Slowest order |
|---|---|---|---|---|
| Deep (about 1,500 price levels per side) | 278 ns | 1.2 µs | 99.95% | 158 µs |
| Thin | 154 ns | 891 ns | 99.96% | — |

**Determinism.** Replays are bit-identical: the same input gives the same event stream,
on Windows and on Linux. On Windows no order is slow on every replay; each order's
best-of-5 time is under 1 µs. So the tail there comes from the machine, not the engine's
work. The sections below show how that was established rather than assumed, and the
one case where the engine itself goes over 2 µs.

## What it does

- **Order types:** limit (GTC or IOC), market (an IOC sweep with no price limit), and
  cancel by order id (owner only).
- **Several instruments per engine.** Each instrument has its own tick size, and order
  ids are unique across the whole exchange.
- **Price-time priority.**
  - The best price trades first, and within a price level the oldest order fills first.
  - A partial fill keeps the order's place in the queue.
  - Fills execute at the resting order's price.
- **Self-trade prevention:** when an incoming order would trade against a resting order
  from the same owner, the resting order is cancelled and the sweep continues.
- **Admission before matching.** Every rejection is decided before any state changes:
  bad instrument, quantity, price or tick, duplicate id, or a full book. An order is
  never partly executed and then rejected.
- **Output is a stream of 64-byte events:** `Ack`, `Trade`, `Cancelled` (user / IOC
  remainder / self-trade) and `Rejected` (reason), each numbered by one engine-wide
  sequence. See [messages.hpp](include/px/messages.hpp) for the exact protocol.

## Design

```
            inbound SpscRing<OrderRequest>              outbound SpscRing<ExecEvent>
 gateway ──────────────────────────────────► matcher ──────────────────────────────► publisher
 (thread)   64-byte POD slots, 65,536 deep   (thread,  64-byte POD slots, 131,072 deep  (thread)
                                              pinned)
```

- **Prices** are `int64` fixed point, where 1.0000 = 10,000 units. Each instrument
  has a tick size in those units. There is no floating point anywhere on the matching
  path.
- **Orders** are 32-byte records in a preallocated pool, addressed by 4-byte slot index.
  - Orders at one price form an intrusive doubly-linked FIFO, so a cancel is an O(1)
    unlink.
  - Price, side and instrument live once on the level, not on every order.
- **The price-ordered level index** ([sorted_levels.hpp](include/px/sorted_levels.hpp))
  is a fixed-capacity flat sorted array per side.
  - It stores keys (`price` for bids, `-price` for asks) and level slots, with the best
    price at the back.
  - Touching the best level is `pop_back` or an append. Other lookups binary-search one
    contiguous 16 KB key array rather than chasing tree pointers.
  - `std::map` was rejected because it allocates a node per level.
- **Order id → slot** is an open-addressing hash map with 16-byte entries, linear
  probing and backward-shift deletion, kept at most 50% full.
- **The SPSC ring** uses monotonic cursors, with each side caching the other's cursor.
  Cursors are padded to 128 bytes so Intel's adjacent-line prefetcher cannot make them
  share. Payloads are trivially copyable and exactly one cache line.
- **One thread** (the matcher) owns the engine, so the core has no locks, atomics or
  fences. Every capacity is set in `EngineConfig` and allocated, zero-filled and
  pre-faulted in the constructor.

### Worst cases are bounded, and measured

A side holds at most 2,048 price levels, and an order that would need a new level
beyond that is rejected (`BookFull`). So the most expensive book change is adding or
removing a price at the far end of a full side, which shifts the whole index (about
24 KB). `bench_engine` measures every operation single-threaded on a fresh book.

| Operation (single thread) | p50 | p99 |
|---|---|---|
| cancel an order at the best level | 42 ns | 67 ns |
| new order joining the best level | 89 ns | 343 ns |
| new order creating a new best level | 62 ns | 300 ns |
| IOC order taking 1 resting order | 69 ns | 347 ns |
| market order sweeping 10 levels | 234 ns | 487 ns |
| **worst case:** add a level at the far end of a 2,048-level side | 576 ns | 855 ns |
| **worst case:** remove the far-end level of a 2,048-level side | **480 ns** | 655 ns |

The far-end removal first measured **6,416 ns**. The cause was Windows UCRT's `memmove`,
not the data structure:

| 24 KB shift, one element | Time |
|---|---|
| `memmove` down (the erase direction, overlapping) | 7.9 µs |
| `memmove` up | 0.4 µs |
| plain loop the compiler vectorises, down | 0.6 µs |

The shifts are now explicit loops. `-fno-tree-loop-distribute-patterns` stops GCC from
turning them back into `memmove`, and the size is held in a local so that u32 slot
stores cannot alias it and block vectorisation. The event output was checked unchanged
before and after the fix, using the same seed-42 hash.

## How latency is measured

- **What is timed.** Timing is on the matcher thread only. `lfence; rdtsc` runs right
  after an order is dequeued, and `rdtscp; lfence` runs after its last event has been
  written to the outbound ring. Both are `asm volatile` with a memory clobber, so LTO
  cannot move work across them. Both stamps come from the same core, so no cross-core
  clock comparison is involved.
- **What is not timed.** Time spent waiting in the inbound ring is excluded: this is
  the engine's *service time*, not wire-to-wire tick-to-trade. There is no network, no
  parsing and no NIC.
- **Timer floor.** The empty timestamp bracket costs about 32 ns. It is printed with
  every run and never subtracted.
- **Workload** ([order_stream.hpp](sim/order_stream.hpp)). A seeded, pre-generated
  stream over 4 instruments:
  - 50% passive GTC, 15% marketable limits, 5% market orders, 28% cancels, and 1.5%
    late cancels of orders that have already filled.
  - The generator replays its own stream through a shadow engine, so cancels target
    orders that are really resting.
  - **Thin profile:** passive orders mostly within 20 ticks of the mid.
  - **Deep profile:** half the passive orders spread up to 1,500 ticks away, with up
    to 60k resting orders, which gives sides of about 1,500 levels.
- **Setup.**
  - Threads are pinned to separate performance cores; the matcher gets the last one,
    away from CPU0's interrupts.
  - The process runs at HIGH priority with EcoQoS power throttling disabled.
  - All memory is pre-faulted.
  - Warmups of 100k orders (thin) and 300k orders (deep) are processed but not
    recorded.
- **The gate is fixed:** p99.9 < 2 µs, zero outbound-ring stalls, and zero heap
  allocations in the recorded window. Allocations are counted by a replaced global
  `operator new`. The number of orders over 2 µs and the slowest order are always
  printed next to the result. Nothing is trimmed.
- **Replays.** With `--repeat=K` the same stream is replayed on fresh engines. Every
  event stream must be bit-identical. Each order's *best of K* then estimates its true
  engine cost, because timing noise only ever adds time.
- **Open-loop pacing.** With `--rate=N`, orders are sent on a fixed schedule, so the
  matcher idles between orders with cooler caches.

## Results

**Windows 11, i5-12450H, on battery, Balanced power plan, g++ 13.2 `-O3 -march=native
-flto`, commit `8055854`.** Raw output, CSVs and every slow order are in
[results/windows/](results/windows/). The table is regenerated by
`bench/run_evidence.sh windows && python bench/summarize_results.py results/windows`.
Each cell is the median across runs, with the range in brackets.

| Run | p50 ns | p99 ns | p99.9 ns | p99.99 ns | slowest order | over 2 µs | gate |
|---|---|---|---|---|---|---|---|
| thin, 5 seeds × 1M | 154 (149–165) | 576 (556–583) | 891 (848–893) | 7,976 | 113,903 ns | 0.040% | **PASS** ×5 |
| deep, 5 seeds × 1M | 278 (255–283) | 792 (728–800) | 1,196 (1,055–1,230) | 9,324 | 157,908 ns | 0.054% | **PASS** ×5 |
| thin, paced 50k/s, 3 replays | 263 (260–266) | 668 (662–704) | 4,387 (4,004–5,231) | 12,346 | 41,610 ns | 0.189% | **FAIL** ×3 |
| deep, paced 50k/s, 3 replays | 402 (399–404) | 1,180 (1,163–1,216) | 7,460 (7,267–7,564) | 18,611 | 80,488 ns | 0.314% | **FAIL** ×3 |

- **Throughput.** Unpaced, the gateway pushes as fast as the matcher drains: 3.09M
  orders/s on the thin book and 2.40M on the deep book, median across seeds. At the end
  of the deep runs the book holds 60,000 resting orders on sides up to 1,491 levels.
- **Replays of seed 42, five per profile.** Every event stream is bit-identical.

  | Profile | Best-of-5 p50 | Best-of-5 p99.9 | Best-of-5 slowest order | Orders over 2 µs in all 5 runs |
  |---|---|---|---|---|
  | thin | 111 ns | 493 ns | **889 ns** | **0** |
  | deep | 156 ns | 653 ns | **943 ns** | **0** |

  Paced replays behave the same way. Their best-of-3 slowest order is 1,154 ns (thin)
  and 1,921 ns (deep), and no order is slow in all three runs.
- **Ring hop.** A same-thread push and pop costs 32 ns (p50). A cross-core ping-pong
  round trip costs 424 ns (p50) and 506 ns (p99).

### Linux, in a WSL2 virtual machine on the same laptop

g++ 13.3, same commands; raw output in [results/wsl2/](results/wsl2/).

| Run | p50 ns | p99 ns | p99.9 ns | p99.99 ns | slowest order | over 2 µs | gate |
|---|---|---|---|---|---|---|---|
| thin, 5 seeds × 1M | 223 (171–227) | 621 (594–645) | 920 (881–1,017) | 24,878 | 4.3 ms | 0.048% | **PASS** ×5 |
| deep, 5 seeds × 1M | 289 (260–300) | 840 (803–863) | 1,457 (1,317–1,506) | 40,020 | 1.6 ms | 0.065% | **PASS** ×5 |
| thin, paced 50k/s, 3 replays | 345 (303–351) | 1,042 (966–1,065) | 6,876 (6,697–7,396) | 58,284 | 2.7 ms | 0.276% | **FAIL** ×3 |
| deep, paced 50k/s, 3 replays | 477 (451–492) | 1,700 (1,606–1,773) | 10,358 (9,589–10,894) | 72,052 | 1.6 ms | 0.665% | **FAIL** ×3 |

- **Same output.** The event hashes are identical to Windows (`63bb035a395bae79` thin,
  `75ec1ab46ce5260a` deep), so the engine's output does not depend on the OS or C
  runtime.
- **Heavier tail.** The hypervisor can deschedule a virtual CPU for milliseconds, so
  these numbers say nothing about bare-metal Linux with `isolcpus`, which was not
  available.
- **Best-of-5 replays.** The slowest order is 897 ns (thin) and 1,159 ns (deep).

### What the tail is, and what it isn't

- **Where it comes from.** On Windows, every order that took more than 2 µs was fast on
  some other replay of the identical book state. The engine's own work there stays
  under about 1 µs unpaced, and under 2 µs paced. The slow tail is the machine taking
  the core away. Windows cannot isolate a core from interrupts and deferred procedure
  calls; Linux can, with `isolcpus`.
- **The one limit the evidence found.** On the deep book, paced, inside the VM, 294
  orders (0.03%) were over 2 µs in *all three* replays. Their best times were 2.0–3.3 µs
  (median 2.2 µs). They are all cancels (225) or new resting orders (69), which are
  exactly the operations that open or close a price level. On a side of about 1,500
  levels, that shifts up to 18 KB of the level index. With caches cooled by 20 µs idle
  gaps, and TLB misses that cost more under nested paging, that exceeds 2 µs. A
  tick-indexed ladder would make those operations O(1); this design deliberately trades
  that for a price-ordered map with no fixed price band.

- **Unpaced runs.** The slow orders arrive in bursts: the median gap between them is
  0.03–0.08 ms, consistent with one disturbance polluting the caches for several
  consecutive orders.
- **Paced runs.** An idle-loop "hiccup meter" on the matcher core finds that it loses
  about 6 µs every 2.00 ms, roughly 550 times a second. But only 3–6% of the slow paced
  orders start within 50 µs after such a hiccup. So those interruptions do **not**
  explain most of the paced tail, and its specific cause is not established.
  Frequency and power-state changes, running on battery, and activity on the
  hyperthread sibling are candidates, not findings.
- **The honest claim for this machine:** unpaced, p99.9 < 2 µs on both platforms;
  paced at 50k orders/s, it is not.

## Correctness

- **Differential fuzzing** ([fuzz_vs_reference.cpp](tests/fuzz_vs_reference.cpp)).
  - 7 seeded runs, 924k events, **0 mismatches** against a deliberately naive
    `std::map` + `std::deque` reference engine that implements the same event protocol
    and admission rule.
  - Profiles cover a shallow band (level churn at the touch, self-trades, duplicate and
    invalid ids, foreign cancels), a deep book (sides up to 1,236 levels, with about
    2,000 cancels deep in the book and far-end inserts), and 64-level sides pinned at
    capacity (17k `BookFull`).
  - After every command, the events, top of book and resting count must match; whole
    ladders are compared periodically.
  - Coverage of those paths is asserted, not assumed. A planted one-character crossing
    bug is caught at command 24.
- **Scenario tests** ([test_engine.cpp](tests/test_engine.cpp)): 213 hand-checked checks
  covering priority, partial fills, market and IOC remainders, cancels, duplicate ids
  rejected before trading, tick validation, `BookFull`, self-trade prevention,
  instrument isolation and ordering. A full `check_invariants()` runs after every
  command.
- **Zero allocation** ([test_alloc_audit.cpp](tests/test_alloc_audit.cpp)): 0
  allocations across 1.1M `process()` calls, and 0 across all three threads during the
  pipeline's recorded window. The test first proves the counter itself counts.
- **Determinism** ([test_determinism.cpp](tests/test_determinism.cpp)): a direct
  single-thread run and two threaded pipeline runs produce identical event hashes and
  identical final books. The hashes are also identical between Windows (UCRT) and
  Linux (glibc).
- **Sanitizers.** The whole suite passes under AddressSanitizer and UBSan on Linux
  (WSL2, g++ 13.3). The build is warning-free under `-Wall -Wextra -Wpedantic` on both
  platforms.
- **Containers and ring.** The pool, hash map and level index are checked against
  standard-library models over millions of random operations. The SPSC ring is checked
  with 10M sequenced messages across two pinned threads.

## Build and run

The build needs g++ 13 or newer and make: MSYS2 UCRT64 `mingw32-make` on Windows, or
plain `make` on Linux.

```bash
mingw32-make test       # all tests (debug, PX_DEBUG invariants)
mingw32-make latency    # the 1M-order matching-latency benchmark
mingw32-make bench      # every benchmark
build/release/bench_matching_latency --profile=deep --repeat=5
build/release/bench_matching_latency --rate=50000 --outliers=slow.csv
mingw32-make repl && build/release/repl < tools/repl_demo.txt
```

A minimal REPL session:

```
> sell ACME 70 10.01 as 4
  ack     #3 sell 70 @ 10.01
> buy ACME 150 10.02 as 6
  ack     #5 buy 150 @ 10.02
  trade   #5 x #3: 70 @ 10.01  (#5 has 80 left, #3 has 0 left)
  trade   #5 x #1: 80 @ 10.02  (#5 has 0 left, #1 has 20 left)
```

## Scope

**Built:** matching, the event protocol, SPSC rings, the threaded pipeline, and the
measurement and test tooling.

**Not built (out of scope):**
- order modify or amend (cancel and re-enter instead);
- stop orders, icebergs, auctions;
- a network gateway or FIX/ITCH/OUCH encoding;
- journaling and recovery;
- the full LMAX multi-consumer graph (one SPSC ring into one core, by design);
- per-participant risk limits.

## Layout

| Path | Contents |
|---|---|
| `include/px/` | the engine: types, messages, pools, hash map, level index, matching engine, ring, TSC clock, stats, platform |
| `src/` | engine and platform implementations (Windows and Linux) |
| `sim/` | seeded order-stream generator, three-thread pipeline and timing, allocation counter |
| `tests/` | unit, scenario, fuzz (with the reference engine), allocation and determinism tests |
| `bench/` | `bench_matching_latency`, `bench_engine`, `bench_ring`, and the evidence scripts |
| `results/` | raw benchmark evidence with the machine state it was captured on |
| `tools/` | the interactive REPL and a demo script |
