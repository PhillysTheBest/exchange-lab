# Performance report

A single-document account of every latency number this project has produced,
how each one was measured, what optimization it's attached to, and — just as
important — which comparisons this report deliberately does **not** make,
because they were never actually run. See [README.md](README.md) for the
system's architecture and correctness story; this document is about speed
and the discipline behind claiming it.

## How to read this

- **Every number below is p50/p90/p99/p99.9(/p99.99), never a mean.** A mean
  hides exactly the outliers — a mispredicted branch cascading into a TLB
  miss, an OS scheduler preemption — that determine whether a trading
  decision beats the market or arrives after it's already moved. See
  [`px/histogram.hpp`](include/px/histogram.hpp).
- **Timed with `rdtscp`** ([`px/tsc_clock.hpp`](include/px/tsc_clock.hpp)),
  calibrated once against `std::chrono::steady_clock`, with the
  invariant-TSC bit checked (CPUID leaf `0x80000007`, EDX bit 8) rather than
  assumed — every benchmark below prints `invariant TSC: yes` before its
  numbers, confirming the clock is trustworthy on this run.
- **Machine**: a mobile-TDP laptop chip (Intel i5-12450H, hybrid 4P+4E
  cores), Windows 11, no `isolcpus`, no tuned bare-metal box. Every benchmark
  that can pin does (`pin_current_thread_to` + `THREAD_PRIORITY_TIME_CRITICAL`
  — [`src/platform_win32.cpp`](src/platform_win32.cpp)), but this is a
  laptop, not a colo cage, and the numbers below should be read as "what a
  reasonable but unglamorous machine gets," not a performance ceiling.
- **All numbers in this document come from one coherent build/run** (`mingw32-make
  clean && mingw32-make all && mingw32-make test-live && mingw32-make live &&
  mingw32-make bench-live`, immediately in sequence), not stitched together
  from different sessions run under different conditions. They will drift
  run to run on any machine, this one included — re-run the `Makefile`
  targets listed with each section yourself rather than trusting numbers
  frozen in a document.

## Architecture at a glance

```
Offline:  Order/OrderBook/Trade/Market/Trader/MatchingEngine  (array ladder, O(1) match)
Live:     Kalshi --TLS/WS--> [T1 Ingress] --ring A--> [T2 Brain] --ring B--> [T3 Gateway] --REST--> Kalshi
                                                            ^                     |
                                                            +-------ring C--------+
Decision: Strategy::on_event(MarketState, TriggerKind, OracleEvent*) -> Join/Improve/Cross/Cancel/Wait
```

Full detail in [README.md](README.md)'s Architecture section and
[CLAUDE.md](CLAUDE.md).

## The optimization journey

Six named techniques, each adopted for a specific, statable reason. The
**Evidence** column is deliberately precise about what was and wasn't
isolated with a controlled measurement — a technique's contribution is
sometimes a clean before/after number, and sometimes it's "baked into" the
combined-system numbers in the next section with no individual attribution
claimed, because that specific A/B experiment was never run. Claiming a
number for the second kind would be exactly the sort of unverified assumption
this project's whole benchmarking philosophy exists to push back against.

| # | Technique | Problem it solves | Evidence |
|---|---|---|---|
| 1 | **Array price ladder + occupancy bitmasks** ([`order_book.hpp`](include/px/order_book.hpp)) instead of a sorted tree of price levels | Best bid/ask is a `countl_zero`/`countr_zero` over two 64-bit words, not a tree descent; a price is a direct array index, not a comparison-based lookup | Architectural choice, not independently isolated — see "The comparison we didn't make," below. Reflected only in the combined `submit`/`cancel` numbers in the next section |
| 2 | **Object pool, no hot-path `new`** ([`pool.hpp`](include/px/pool.hpp)) — one contiguous slab, allocated once, an intrusive free list handing out/reclaiming indices | Heap allocation (and its worst case — an allocator lock, a page fault) on every single order is the single most common reason a "fast" matching engine isn't | Architectural choice; not isolated against a `new`-per-order baseline. Reflected in `submit`/`cancel`'s combined numbers |
| 3 | **Hand-rolled open-addressing flat map** ([`flat_map.hpp`](include/px/flat_map.hpp)) for order-id → slot lookup, with backward-shift deletion | `std::unordered_map` allocates a node per entry — at least one guaranteed cache miss per lookup, on a lookup that happens on every cancel and every fill | Architectural choice; not isolated against `std::unordered_map`. Reflected in `cancel`'s combined number |
| 4 | **Lock-free SPSC ring** ([`spsc_ring.hpp`](include/px/spsc_ring.hpp)), LMAX-style monotonic cursors, cache-line-padded | Cross-thread handoff (T1→T2→T3, and back) without a mutex, without false sharing between producer/consumer cache lines | **Directly measured**, same-thread vs. cross-core, via `bench_ring` — see below. This is the one technique with its own clean, isolated before/after number |
| 5 | **CPU pinning + `THREAD_PRIORITY_TIME_CRITICAL`** ([`platform_win32.cpp`](src/platform_win32.cpp)), P-core selection + SMT-sibling skip on this machine's hybrid 4P+4E layout | Removes scheduler migration and P/E-core placement as a source of jitter | Always-on in every pinned benchmark below; never run head-to-head against an unpinned version on this machine, so no isolated number is claimed |
| 6 | **RSA-PSS signing moved off T2 onto its own thread (T3 Gateway)** | Signing is ~15,000–28,000× a book update with a multi-millisecond tail (measured, not assumed) — inline in T2 it would freeze the book mid-update every time an order goes out | **Directly measured** — `bench_sign` vs. `bench_engine`'s submit numbers. The other clean before/after number this report has |

### The comparison we didn't make

[`tests/reference_book.hpp`](tests/reference_book.hpp) — the deliberately
naive `std::map`+`std::deque`+heap-allocation-throughout book used to
differentially fuzz `OrderBook` for *correctness* (see
[`fuzz_vs_reference.cpp`](tests/fuzz_vs_reference.cpp), 20,000 iterations, 0
mismatches) — carries its own header comment stating plainly: **"This is
explicitly not the A/B performance twin... that was declined."** This report
honors that decision rather than quietly overriding it to manufacture a
flattering "N× faster than a naive book" headline number. A naive
tree-plus-heap-allocation book losing a speed contest to an array-plus-pool
one would be true and also nearly information-free — the interesting
engineering claim in this project was never "an array beats a tree," it's
the actual measured numbers below, on the actual matching semantics this
book implements.

## Measured results

### Core matching engine — `mingw32-make bench` (`bench_engine`)

```
submit: rest new order        p50=   165.1ns  p90=   224.0ns  p99=   247.2ns  p99.9=   441.2ns
submit: single full cross     p50=   123.4ns  p90=   130.2ns  p99=   174.7ns  p99.9=   226.0ns
submit: 50-level market sweep p50=    52.1ns  p90=  1688.9ns  p99=  2185.3ns  p99.9=  3445.1ns
cancel: resting order         p50=    79.7ns  p90=   118.2ns  p99=   167.5ns  p99.9=   379.0ns
mixed load (rest/cross/cancel)  p50= 158.7ns  p90=   269.3ns  p99=   504.5ns  p99.9=   840.6ns
  throughput: 3,695,814 ops/sec (mixed load)
```

The 50-level sweep's p50/p99 gap (52ns vs. 2,185ns — over 40×) is kept
deliberately visible rather than smoothed into a single "sweep is fast"
number: sweeping more levels is genuinely more work, and reporting only the
best case would be exactly the misleading, cherry-picked claim this whole
methodology exists to avoid.

### Lock-free ring — `mingw32-make bench` (`bench_ring`)

```
same-thread push+pop (no core hop)   p50=    40.5ns   p99=    52.1ns   p99.9=    59.7ns
cross-core round trip (ping-pong)    p50=   627.9ns   p99=   842.3ns   p99.9=  3110.6ns
```

**This is technique #4's isolated number.** ~40ns is the ring's own
bookkeeping (cursor read/write, capacity check); ~628–842ns is what a real
cache-line bounce between two physical cores actually costs on this machine
— the ~15× gap between them is the attributable cost of cross-core
communication, not ring overhead. Measured via a strict one-message-in-flight
ping-pong protocol; see "The methodology bug," below, for why that protocol
exists at all.

### Strategy dispatch — `mingw32-make bench` (`bench_strategy`)

```
extract_features (MarketState assembly)   p50=  34.9ns   p99=  46.9ns
PassiveMarketMaker (quoting)              p50=  41.7ns   p99=  54.5ns
PassiveMarketMaker (managing position)    p50=  42.5ns   p99=  54.5ns
OrderBookImbalance (book-driven)          p50=  43.3ns   p99=  49.3ns
TradeFlow (trade-flow-driven)             p50=  52.9ns   p99=  65.3ns
OracleThresholdStrategy (oracle-driven)   p50=  56.9ns   p99=  66.1ns
```

Every strategy's decision function costs roughly the same order of magnitude
as a single `ShadowBook` level update (~50–165ns, from `bench_engine`
above) — the measured basis for Brain running strategy dispatch inline on
T2 rather than giving it its own thread.

### RSA-PSS signing — `mingw32-make bench-live` (`bench_sign`)

```
RSA-2048 PSS sign_request()   p50= 1,386,790.8ns (~1.39ms)   p99= 2,103,180.0ns (~2.10ms)   max= 6,836,338.9ns
```

**Technique #6's isolated number**, calling the real production
`sign_request` against a throwaway locally-generated key. At p50, that's
roughly **8,400×** a `ShadowBook` update (~165ns) and **~33,000×** a
strategy's own decision cost (~42ns) — the entire, measured justification
for T3 existing as its own thread rather than inline in T2.

### The full internal decision path, end to end — `mingw32-make bench-live` (`bench_pipeline_e2e`)

```
WS frame -> parse -> shadow-book -> features -> strategy -> risk -> OrderIntent
  p50= 3,129.5ns (~3.13µs)   p90= 3,255.7ns   p99= 4,177.7ns   p99.9= 18,656.6ns   max= 408,299.1ns
```

The headline number. Through two real, pinned threads and two real
cross-core ring hops (T1-equivalent parse → ring A → T2 Brain → ring B →
observed), pacing one event through at a time (see below): **~3.1µs at p50**
to turn a raw WS text frame into a routed `OrderIntent` — real JSON parsing,
a real `ShadowBook` mutation, a real `MarketState` assembly, and a real
strategy decision, twice across a cross-core ring. That's roughly **440×**
under RSA-PSS signing's own p50 (1.39ms / 3.13µs), the same order-of-magnitude
gap that justifies every other architectural split in this system, now
measured for the specific path an out-of-band observation actually travels
before becoming an order.

## The methodology bug (retold, because it's the reason any of the above is trustworthy)

The first version of `bench_ring` let its producer push as fast as it could,
unpaced. With a 16,384-deep ring and a consumer doing marginally more work
per message than the producer, the ring saturated near-full almost
immediately and stayed there — so the measured `recv_ts - send_ts` was
capturing **queueing delay** (Little's-law backlog dwell time), not the cost
of one cross-core handoff. It produced a tight, entirely plausible-looking
distribution clustered around 2.6 **milliseconds**. Cross-checked against a
same-thread, zero-cross-core-traffic control (p50 ≈ 13–40ns, no comparable
pattern), the number didn't hold up, and the fix — a ping-pong protocol,
producer sends one message and busy-waits for the consumer's ack before
sending the next, so exactly one exchange is ever in flight — is what
produced the ~628ns number reported above instead.

This isn't a footnote kept for color. It's why `bench_pipeline_e2e.cpp`
(this report's headline number) uses the *identical* one-event-at-a-time
discipline, deliberately, rather than a naive "push N frames, drain N
intents, divide" loop that would have silently re-introduced the exact same
queueing-delay artifact one architectural layer up. A benchmark's own
methodology is part of the system under test, not a neutral instrument —
this project has now caught itself getting that wrong once and corrected for
it a second time before it happened again.

## What adversarial testing actually found

Percentile numbers prove a system is *fast*; they say nothing about whether
it's *right* under conditions nobody happened to script by hand. Four real
bugs were found this way across this project, each fixed in the same commit
range as the test that found it — not a hypothetical list, an actual one:

1. **`ShadowBook::set_level`'s side-flip fix, incomplete.** A price level
   flipping from one side to the other *at a zero quantity* still corrupted
   the shared `qty_`/mask-bit invariant. Found by
   [`tests/test_replay_fuzz.cpp`](tests/test_replay_fuzz.cpp)'s biased
   adversarial fuzzing (828,000 checks, 4,000 events, 2 markets) through a
   real `Brain` thread.
2. **`OrderTracker::on_acked` zombie entries.** A fully-resolved
   (IOC-killed or fully filled) order was promoted into the acked table
   anyway, and nothing ever removed it — silently disabling
   `OrderBookImbalance`/`TradeFlow` (both gate re-entry on
   `outstanding_orders.empty()`) after their very first trade. Found by
   [`tests/test_pipeline_e2e.cpp`](tests/test_pipeline_e2e.cpp), which
   proves the fix with a real second `Cross` after the first one fully
   resolves — the same test fails (times out) against the pre-fix code, on
   purpose, so the regression test is provably not vacuous.
3. **`OrderTracker::on_fill` double-counting a synchronous partial fill.**
   `on_fill` independently subtracted from `remaining_qty`, even though
   `on_acked` — called immediately before it, for the same event — had
   already set `remaining_qty` from Kalshi's own authoritative number. A
   10-lot order reporting "6 remaining, 4 filled" tracked as 2 remaining.
   Found by [`tests/test_hostile_events.cpp`](tests/test_hostile_events.cpp).
4. **`SeqTracker`'s doc comment didn't match its code.** It promised "won't
   trust the stream again until the next Snapshot" after a gap; the code
   actually accepted the very next delta unconditionally. Found by the same
   file, alongside a matching unbounded-growth fix in
   [`live/kalshi_gateway.hpp`](live/kalshi_gateway.hpp)'s Kalshi-UUID map.

Full narrative for each in [README.md](README.md)'s Stage 3/Stage 4
sections.

## Conclusion

Every number in this report is attached to a specific, re-runnable `Makefile`
target, a stated methodology, and — where a technique's isolated
contribution was never actually measured — an explicit admission of that
instead of a plausible-sounding number standing in for it. The one metric
that ties the whole system together, ~3.1µs p50 from a raw exchange message
to a routed trading decision, is cheap enough that this project's own
argument holds: the wire isn't where the edge is, the decision path is, and
now that claim has a number attached to it instead of an assumption.
