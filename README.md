# px — a price-time-priority matching engine in C++20

A single-threaded exchange matching core fed by a lock-free single-producer/
single-consumer ring, with **no heap allocation after start-up**, int64 fixed-point
prices and a price-ordered level index. It comes with the measurement and test
machinery that backs every number below — and with [the arena](#the-arena), a seeded
market of strategies trading inside the engine so you can watch it work and ask what a
participant's reaction delay is worth.

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
- **The arena** ([test_arena.cpp](tests/test_arena.cpp)): 66 checks. A participant's
  book, rebuilt from the event stream alone, is held against the engine's own ladder
  after every one of 200,000 requests — the same differential shape as the fuzzer, with
  the engine as oracle. Plus the scheduler's total order under heavy timestamp
  collision, queue position by hand through a cancel and a partial fill, bit-identical
  replay from a seed, and the closed-world identity: cash, position and marked value
  each sum to **exactly zero** across participants, in integers.

## The arena

A seeded market of strategies trading inside the engine, each with a reaction delay you
control, rendered live in the terminal. Design, and the four modelling mistakes it took
to get there, in [docs/ARENA.md](docs/ARENA.md).

> **Every number in this section is simulated.** A session invents its own nanoseconds
> so that a participant's reaction delay can be a controlled variable rather than a
> property of the machine. The latency tables above are *measured*, on hardware. These
> are not, and the two must not be read together. Nothing in `arena/` is even called
> latency: a strategy has a `reaction_ns`.

Six participants, each a distinct owner to the engine. A **maker** quotes both sides and
infers price from order flow; a **rival** maker does the same at a fixed fast delay; an
**informed** trader can see a fundamental value nobody else can and takes quotes that are
on the wrong side of it; two **noise** traders and a **momentum** strategy supply the rest of
the flow. That is Glosten and Milgrom's setting: the maker earns the spread from
uninformed flow and pays it back to the informed trader.

Queue position is not an engine feature. The engine exposes no per-order query and none
was added — `arena/book_mirror.hpp` reconstructs every level and every FIFO from the
`ExecEvent` stream, which is the only thing a real venue sends out. So a strategy provably
sees no more than a real participant would.

<img src="docs/arena.svg" alt="The arena: order book, participants, reaction-delay sliders, session and tape, drawn in a terminal" width="100%">

That picture is generated, not screenshotted: `arena --still=60000000 --seed=42` runs a
session headless to one simulated instant and prints the frame, and
`bench/frame_to_svg.py` turns it into the SVG above. Regenerate it and diff it — a
screenshot is a thing somebody took once and nobody can check.

Keys: `space` pause, `+`/`-` speed, `1`–`9` focus a participant, `q` quit, and `f` to
re-read your `--script` file from disk and restart. **None of them can change what
happens.** The viewer only ever reads the session, so a run watched at 512×, paused
halfway and resumed has the same session hash as the same seed run headless — and `f`
restarts rather than injecting, for the same reason: a participant appearing at the
instant a human pressed a key would be a wall-clock event, and the session could never
be reproduced again.

### What is 10 microseconds worth?

**In this market, 10 µs costs less than 24 seeds can resolve; 100 µs and 1 ms both cost
real money.** Over 24 seeds of 200,000 requests each, with every point run on the same
seeds so the difference can be taken seed by seed:

| Maker's reaction delay | Paired difference vs the 1 µs maker | Standard error | Smallest effect 24 seeds could resolve | Separated? |
|---|---|---|---|---|
| 10 µs | −39.96 | ±56.19 | ±157 | **no — underpowered** |
| 100 µs | −601.83 | ±93.05 | ±261 | yes |
| 1 ms | **−3117.13** | ±74.71 | ±209 | yes |

All figures are currency units of marked value over one 200,000-request session, and
`bench/summarize_arena.py` prints this table from the committed CSVs.

Paired differences are reported because the levels are unreadable: a maker's value is
dominated by what its leftover inventory happened to be worth, and the spread of levels
across seeds swamps the effect. Every point runs on the same seeds and the seed fixes the
path of the fundamental, so the same-seed difference cancels most of that noise.

**The standard error matters, and reporting it changed one of these answers.** At 10 µs
the effect is smaller than what 24 seeds can resolve, so the honest statement is *not
established* — "underpowered" and "nothing there" are different claims and only one of
them is earned here. At 100 µs the effect is about six standard errors from zero, so it
*is* separated, even though the full min-to-max range across seeds still straddles zero.
A range test over 24 draws is far more conservative than a paired standard error; this
table reports both so a reader can see which test they are being shown.

### And the control says it is not adverse selection

The obvious story — the slow maker's stale quotes get picked off — is **wrong here**, and
the null run is what catches it. `sweep --null` removes the informed trader, so there is
no information in the session and speed should be worth nothing. It is worth almost
exactly the same:

| Run | 1 µs → 1 ms, paired |
|---|---|
| With an informed trader | −3075.10 |
| **Nobody informed** | **−2948.30** |

96% of the effect survives with nothing to be adversely selected by. Splitting each fill
into the spread it earned and the price move afterwards (`edge = spread + adverse`, in
integers) shows where the money actually went:

| Maker's delay | Fills | Spread earned | Adverse selection | Value |
|---|---|---|---|---|
| 1 µs | 10,342 | +3,457 | +3.25 | +3,945 |
| 1 ms | 1,874 | +392 | +10.46 | +760 |

The cost of being slow is a **liquidity-provision** effect, not an information one. A
1 ms maker cannot keep a quote standing: it gets 5.5× fewer fills and earns 8.8× less
spread. The post-fill column moves by about **$7 out of a $3,185 swing** — and in the
*helpful* direction — with a paired range that straddles zero at every point.

**That column is empty for a structural reason, and a second control tried and failed to
calibrate it.** A null run cannot distinguish a measure that works from one that is
always zero, so the other control sets out to create the effect on purpose: freeze a
maker's learning and its inventory skew and its quotes never move at all, which is a
stale quoter by construction. Three variants were tried and **all three fail the health
line** — a maker in this session is not a probe you can insert without perturbing what it
measures, because jointly with the rival it *is* the price-discovery mechanism. Make it
stale enough to be reliably picked off and the book stops tracking the fundamental, so
the measure loses its reference. So the post-fill column's **sensitivity is not
established**, and that is published rather than glossed.

What the reading rests on instead is analytic, and solid: the fundamental is a martingale,
so the expected post-fill move is zero for *every* participant regardless of what it
traded. Only a participant that **predicts** the fundamental could make that column
non-zero, and none here does — the informed trader reacts to V's current value, it does
not forecast the next one. So the honest reading is **the post-fill channel is empty
because the model requires it**, not that nothing adverse occurred; a stale quote's loss
shows up in what the code calls `spread`, being filled at a price the fundamental has
already left behind. [docs/ARENA.md](docs/ARENA.md) works through it, and
`results/arena-windows/stale.*` holds the failed control, because a control that failed
is still evidence about the instrument.

That is the honest answer, and it is the one the evidence supports rather than the one
the design was hoping for.

### And the ledger says where the money went

The session is closed, so every gain is some named participant's loss and the values must
sum to zero. That turns the claim above from an inference into an audit — if the slow
maker were being adversely selected, the informed trader would be richer for it:

| Participant | 1 µs maker | 1 ms maker | Transfer |
|---|---|---|---|
| maker | +3856.91 | +739.77 | **−3117.13** |
| rival | +3800.71 | +7466.36 | **+3665.65** |
| informed | +1744.25 | +1624.82 | −119.43 |
| noise | −5139.77 | −5346.70 | −206.93 |
| noise | −4221.65 | −4305.66 | −84.01 |
| momentum | −40.45 | −178.59 | −138.15 |
| **sum** | **+0.00** | **+0.00** | |

**The maker's loss goes to its competitor, and the informed trader ends up slightly worse
off.** The −3117.13 here is the same number as the paired mean in the table above, arrived
at from the other direction. Slowing one maker down does not feed the trader with
information; it hands uninformed flow to the maker that is still quoting.

The sums are the check on the attribution rather than decoration: they are exactly zero in
integers, so nothing is unaccounted for. `summarize_arena.py` prints this table, and keys
it on participant index rather than name — two of the strategies are both called `noise`, and
grouping by name averages them into one participant and silently breaks the identity.

### What this instrument cannot show

The null result is partly a property of the model, not only of the market, and the limits
are worth stating before someone mistakes them for findings. There is **one** informed
trader with a one-tick threshold; the fundamental moves on a single timescale; there is no
second venue, no quote fade, no cancel-replace race, and no queue position worth winning
because the book is thin enough that priority rarely binds. Those are exactly the
mechanisms through which ten microseconds pays for itself in a real market. So a reader
should take "10 µs is not resolvable here" as a statement about this instrument at this
seed count, not about trading.

What the arena does establish is narrower and checkable: that the *uptime* cost of being
slow dominates the *selection* cost by roughly three orders of magnitude in a market with
one informed participant — and that an experiment can be built where the control is
capable of overturning the headline, which this one did.

### Known limits

Found by testing the arena, fixed, and pinned by regression tests in
[test_arena.cpp](tests/test_arena.cpp):

- **At most 64 participants per session.** A session marks which strategies an event concerned
  in a fixed array. Past that bound the marks were silently dropped, so those strategies never
  got an own-event wake — with 104 identical noise traders, the first 64 took 151.9 fills
  each and the rest took 55.9: wrong numbers that looked entirely plausible. Rosters above
  the ceiling are now refused at construction. The ceiling is a hard limit rather than a
  resizable buffer on purpose: changing how wakes are marked would change wake *ordering*,
  which moves every session hash and would invalidate the committed evidence.
- **A session that goes quiet now ends and says so.** A roster with nothing but a market
  maker used to hang: once its quotes are where it wants them it submits nothing further,
  so the request count stalls while the fundamental keeps rescheduling itself. A session
  now ends after `stall_limit_ns` of *simulated* time with no request — simulated, so it
  triggers identically on every machine and every replay — and reports `stalled`, which
  the sweep surfaces as a warning rather than averaging the run in silently.
- **The renderer makes no measured claim.** `ui.hpp` is covered for its invariants (one
  cell per code point, clipping at every edge, a deterministic composed frame pinned by
  hash) rather than for how it looks, which is a matter of taste and not of evidence.

### What else is in `results/arena-windows/`

- **Market health**, printed above every result: the mean `|mid − fundamental|` in ticks.
  8–12 ticks in the headline runs against a 4-tick spread, so the book tracks a value no
  participant can see. In the null run it is 41–47 ticks, because without the informed
  trader nothing anchors price to value at all — which is itself the mechanism showing up.
- **`no-rival`**: the degenerate market the design notes warn about. With one maker and no
  competition, its spread *per fill* **rises** with delay (+3,665 → +5,985), because a lone
  stale quote is the price and the uninformed flow keeps paying it. Published so the
  warning is checkable rather than a story.
- **Four robustness sweeps** — informed aggression, maker spread, arrival rate and
  learning rate — published whatever they say. A shape that only exists at one set of
  parameters is not a finding.
- **Replay**: every session in the headline and null runs was executed twice and the
  session hashes compared. All identical.

Regenerate the whole directory with `bash bench/run_arena_evidence.sh windows`, or
`BUILD=build-linux bash bench/run_arena_evidence.sh wsl2` for the Linux tree.
`python bench/summarize_arena.py results/arena-windows` prints every table quoted above,
so a number here that the script does not print has no evidence behind it.

### The same answer on both platforms, to the last digit

Unlike the engine's benchmarks, these results do not depend on the machine, the power
plan or the load. `results/arena-wsl2/` is the whole suite run again under Linux, and
**all eight sweeps come out identical** — every paired difference, every adverse and
spread figure, every book-to-fundamental gap, and every session hash:

| | Windows (UCRT) | Linux (glibc) |
|---|---|---|
| headline, seed 42, four points | `aebce54544c10ca5 35859f327b69af22 800430353d000562 9f777b5eb3508703` | identical |
| 1 ms paired difference | −3075.1000 | −3075.1000 |

That is what the no-floating-point rule buys. A session is integer arithmetic over a
seeded RNG, so there is no libm, no `long double` spill and no ordering difference left
to diverge; the arena's numbers are a property of the code rather than of the computer
that ran it. The evidence directories differ only in `about.txt`, which records which
compiler and which binary produced them.

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
mingw32-make arena      # the arena: the sweep runner, the suite and the viewer
build/release/arena                    # watch a market happen
build/release/sweep --seeds=24 --replay-check   # the reaction-delay sweep
build/release/sweep --null             # the control: nobody informed
build/release/sweep --strategies=maker,rival,informed,noise:3   # compose the line-up
build/release/arena --strategies=maker,informed,noise:2         # and watch it
```

### Add your own strategy

A strategy is one C++ header, written against `arena/strategy.hpp` only: a view of
the market in, a batch of orders out. `examples/strategies/imbalance.hpp` is a
complete one and states the convention. Installing it builds three lab binaries that
can see it; `sweep`, `arena`, `suite` and the tests cannot, so the published numbers
above stay out of its reach.

```bash
make strategy STRATEGY=examples/strategies/imbalance.hpp
make compare CANDIDATES=imbalance          # score it
build/release/arena_lab --strategies=maker,rival,informed,noise:2,momentum,imbalance   # watch it
```

`make compare` puts the strategy in one seat of the default market on 64 fixed seeds,
beside a placebo arm in which the same seat never acts, replays every session, and
prints the paired difference with its standard error. `CANDIDATES=mine,other` puts two
strategies through identical conditions with one command. The conditions and every
raw per-session result are written to `runs/`, never to `results/`: a suite session
lasts 25 ms of simulated time rather than 200,000 requests, so its numbers are not
comparable with the ones above. [`docs/RUNNER.md`](docs/RUNNER.md) is the design. An
evaluation report, validated controls and one-command reproducibility are the next
steps and are not built yet.

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

**Built:** matching, the event protocol, SPSC rings, the threaded pipeline, the
measurement and test tooling, and the arena — a seeded simulated market with a
reaction-delay model, participant-side book reconstruction, marked P&L and a terminal
renderer.

**Not built (out of scope):**
- order modify or amend (cancel and re-enter instead);
- stop orders, icebergs, auctions;
- a network gateway or FIX/ITCH/OUCH encoding;
- journaling and recovery;
- the full LMAX multi-consumer graph (one SPSC ring into one core, by design);
- per-participant risk limits;
- anything in the arena that would need a network, a package manager or a browser.

## Layout

| Path | Contents |
|---|---|
| `include/px/` | the engine: types, messages, pools, hash map, level index, matching engine, ring, TSC clock, stats, platform |
| `src/` | engine and platform implementations (Windows and Linux) |
| `sim/` | seeded order-stream generator, three-thread pipeline and timing, allocation counter |
| `arena/` | the simulated market: clock and scheduler, fundamental value, book mirror, accounting, the strategy contract, `strategies/`, the sweep runner, the standard suite, the renderer and the viewer |
| `tests/` | unit, scenario, fuzz (with the reference engine), allocation, determinism and arena tests |
| `bench/` | `bench_matching_latency`, `bench_engine`, `bench_ring`, and the evidence scripts |
| `results/` | raw benchmark evidence with the machine state it was captured on, and the arena's sweeps |
| `tools/` | the interactive REPL and a demo script |
| `docs/` | [`ARENA.md`](docs/ARENA.md): the arena's design, and the four modelling mistakes it took to get there; [`RUNNER.md`](docs/RUNNER.md): the standard experiment runner behind `make compare` |
