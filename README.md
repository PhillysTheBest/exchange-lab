# prediction-markets

Event-driven C++20 trading engine for prediction markets, and a vehicle for
measuring — not assuming — HFT-style latency techniques. Six classes
(`Order`, `OrderBook`, `Trade`, `Market`, `Trader`, `MatchingEngine`), five
operations (BUY, SELL, LIMIT, MARKET, CANCEL — matching itself, MATCH, is what
`OrderBook::submit` does), a Kalshi connectivity layer built on top of it, and
a deterministic execution-decision layer (`Strategy` — see Stage 3, below) on
top of that.

## Why this reads differently from a typical toy exchange

Two decisions shape everything else here:

- **Prices are integer cents 1–99, never floating point.** A prediction-market
  contract is a bounded probability that settles at 0 or 100. That bound is
  what makes the order book a **flat 101-entry array indexed directly by
  price** instead of a tree, and what makes YES/NO duality collapse to a
  [single function](include/px/normalize.hpp): `BUY NO @ p` is arithmetically
  identical to `SELL YES @ (100-p)`, so the engine only ever matches one asset.
- **Every latency number in this repo was measured, and one of them was wrong
  on the first attempt** — a benchmarking methodology bug, caught and fixed,
  described below rather than quietly edited away. That's the actual point of
  the exercise: cheap claims about nanoseconds are worthless without the
  measurement to back them, and the measurement itself needs scrutiny.

## Building

No CMake/Ninja in this environment by design — the toolchain here is
[MSYS2 UCRT64](https://www.msys2.org/) g++ 13.2 + `mingw32-make`, and the
Makefile targets both that and a native Linux g++/make with no changes.

```bash
mingw32-make test         # debug build (PX_DEBUG invariants + _GLIBCXX_ASSERTIONS), core tests
mingw32-make bench        # release build (-O3 -march=native -flto), core benchmarks
mingw32-make repl         # interactive exchange REPL
mingw32-make strategy_demo   # Phase 4 pipeline demo (see below) — fully local, no network
mingw32-make live         # Kalshi connectivity: kalshi_shadow + kalshi_pipeline (needs OpenSSL)
mingw32-make test-live    # feed schema, gateway dry-run, and replay-determinism tests (needs OpenSSL)
mingw32-make bench-live   # RSA-PSS signing latency benchmark (needs OpenSSL)
mingw32-make all          # test + bench + repl + strategy_demo
```

> **Windows-specific gotcha, found the hard way:** if you have more than one
> MSYS2 subsystem installed (`mingw64` alongside `ucrt64`), Windows' DLL search
> order can load a *different* `libcrypto`/`libssl` at runtime than the one
> `mingw32-make live` linked against, producing a baffling
> `DECODER routines::unsupported` error that has nothing to do with your key.
> Put the matching bin directory first when running a `live`-linked binary:
> `PATH="/c/msys64/ucrt64/bin:$PATH" ./build/release/kalshi_shadow.exe ...`

## Architecture

| Class | Responsibility |
|---|---|
| [`Order`](include/px/order.hpp) | POD, `alignas(64)` — exactly one cache line, index-linked, no pointers |
| [`Trade`](include/px/trade.hpp) | POD execution record, `alignas(64)`, TSC-timestamped |
| [`OrderBook`](include/px/order_book.hpp) | The array ladder + occupancy bitmasks + matching. Also doubles as the Kalshi shadow book (Phase 3) |
| [`Market`](include/px/market.hpp) | Metadata + lifecycle (PreOpen → Open ⇄ Halted → Resolved), owns one `OrderBook` |
| [`Trader`](include/px/trader.hpp) | Integer cash + flat per-market position array, O(1) pre-trade risk check |
| [`MatchingEngine`](include/px/matching_engine.hpp) | Single-threaded owner of all mutable state; ties the above together |

**The book.** `PriceLevel levels_[101]`, indexed directly by price (0 and 100
are permanent empty sentinels — also giving a MARKET sweep a natural
"one-past-the-edge" stop with no extra bounds check). Two 64-bit occupancy
masks (bid/ask) mirror which indices are non-empty, so best bid/ask is one
`countl_zero`/`countr_zero` over two words — not a scan of the ladder. Each
level is an intrusive FIFO of `Order` slots linked **by index, not pointer**
(4 bytes instead of 8), so insert and cancel are both O(1).

**Zero allocation on the hot path.** `Order` storage is a
[`Pool<Order, 65536>`](include/px/pool.hpp) — one contiguous slab, allocated
once, handing out/reclaiming `u32` slot indices via an intrusive free list.
Order-ID → slot lookup is a
[hand-rolled open-addressing flat map](include/px/flat_map.hpp) with
backward-shift deletion (Knuth / Wikipedia "Open addressing") — deliberately
not `std::unordered_map`, which allocates a node per entry and costs at least
one guaranteed cache miss per lookup. Both are large (`OrderBook` is ~5.7 MB by
design) — **always heap-allocate one**, never declare it as a stack local; see
the SIZE WARNING in [order_book.hpp](include/px/order_book.hpp).

**Matching.** Price-time priority. A MARKET order is translated internally to
a LIMIT at the extreme tick (1 or 99) with `TimeInForce` forced to `IOC`, so
there is exactly **one** matching code path for both order types. Self-trade
prevention cancels the resting order and keeps sweeping, rather than crossing
a trader against themselves.

**Risk model (Trader), stated simply because it's simple on purpose:** cash is
checked at order-entry time against the order's full notional (`price·qty` for
a buy, `(100-price)·qty` for a sell — the bound on possible loss for a
contract in [0,100]), but is *not* escrowed for the lifetime of a resting
order. Several large resting orders that individually pass the check could in
principle all fill at once and go cash-negative in aggregate. A production
system would hold per-order escrow, released on cancel/fill; that's the
natural next step, flagged rather than half-built. What **is** exact, and is
what the tests check: `Trader::apply_fill` moves cash and position between
exactly the two counterparties on every trade — zero-sum, nothing created or
destroyed, checked in [test_engine.cpp](tests/test_engine.cpp) across a
multi-trader sweep.

## Correctness

- **[Differential fuzzing](tests/fuzz_vs_reference.cpp)**: the array+bitmask
  `OrderBook` against a deliberately naive `std::map`-based
  [`ReferenceBook`](tests/reference_book.hpp) — chosen to be boringly obviously
  correct, not fast (tree + node-per-order + heap allocation throughout). This
  is a test oracle only; it never runs in a benchmark. 20,000 randomized
  submit/cancel commands, asserting identical fill quantities, reject reasons,
  trade-by-trade sequences, and best bid/ask after **every** command. Currently
  0 mismatches.
- **Debug-only invariants** ([order_book.hpp](include/px/order_book.hpp)'s
  `check_invariants()`, compiled under `PX_DEBUG`): masks agree with the
  ladder, each level's `total_qty` equals the sum of its resting orders' qty,
  the book is never crossed.
- **[Unit tests](tests/test_book.cpp)**: price-time priority, partial fills
  retaining queue position, MARKET sweep + IOC remainder (never rests, even if
  requested GTC), cancel of a partially-filled order, self-trade prevention,
  tick bounds, and `normalize`/`denormalize` round-tripping — checked both
  algebraically and end-to-end (a `SELL NO @30` genuinely crosses a resting
  `BUY YES @70`, not just the formula in isolation).
- **[Engine-level tests](tests/test_engine.cpp)**: market lifecycle gating
  orders correctly, the risk check rejecting *before* an order is ever
  created, cash/position conservation across a 5-trader sweep, and cancel
  correctly scoped to its own market.

No sanitizer runtime is installed in this MSYS2/mingw-w64 toolchain
(`-lasan`/`-lubsan` aren't available) — debug builds lean on
`_GLIBCXX_ASSERTIONS` (libstdc++ container/iterator bounds checks) plus the
`PX_DEBUG` invariants instead. On a Linux box with sanitizers installed, add
`-fsanitize=address,undefined` to `DEBUG_FLAGS` in the Makefile.

## Measuring latency (and a bug found while doing it)

[`histogram.hpp`](include/px/histogram.hpp) reports **p50/p90/p99/p99.9/p99.99/max
— never a mean.** In this domain the tail is the product: a mean hides exactly
the outliers (a mispredicted branch cascading into a TLB miss, an OS scheduler
preemption) that determine whether an order beats the market or arrives after
it's already moved. Timing uses `rdtscp` ([tsc_clock.hpp](include/px/tsc_clock.hpp)),
calibrated once against `std::chrono::steady_clock`, with the invariant-TSC bit
(CPUID leaf `0x80000007`, EDX bit 8) checked rather than assumed.

**The bug.** The first version of the cross-core ring benchmark let the
producer push as fast as it could, unpaced. With a 16,384-deep ring and a
consumer doing marginally more work per message than the producer, the ring
saturated near-full almost immediately and stayed there — so
`recv_ts - send_ts` was measuring **queueing delay** (Little's-law backlog
dwell time), not the cost of one core-to-core handoff. It produced a tight,
entirely plausible-looking distribution clustered around **2.6 milliseconds**.
Cross-checked against a single-threaded, zero-cross-core-traffic control (raw
back-to-back `rdtsc` deltas: p50 ≈ 13 ns, no comparable pattern), the number
didn't hold up. The fix ([bench_ring.cpp](bench/bench_ring.cpp)) was a
ping-pong protocol over two rings — producer sends one message and busy-waits
for the consumer's ack before sending the next, so exactly one exchange is
ever in flight and the timed interval is the round trip, nothing else:

```
same-thread push+pop (no core hop)       p50=   38.9ns   p99=    52.1ns   p99.9=    95.4ns
cross-core round trip (ping-pong)        p50=  411.9ns   p99=   798.6ns   p99.9=  4789.4ns
```

That gap — ~40 ns of pure ring mechanics vs. ~410 ns once a real cache-line
bounce between two physical cores is involved — is the actual, attributable
cost of cross-core communication on this machine. (It's also a plausible
number for a mobile-TDP laptop chip without `isolcpus`, not a tuned bare-metal
box — see the honesty section below.)

Engine benchmarks ([bench_engine.cpp](bench/bench_engine.cpp)), pinned to a
P-core, `-O3 -march=native -flto`:

```
submit: rest new order                p50=  129.4ns   p99=   413.5ns   p99.9=   580.6ns
submit: single full cross             p50=   66.5ns   p99=    87.0ns   p99.9=   102.6ns
submit: 50-level market sweep         p50=   27.6ns   p99=   999.0ns   p99.9=  1398.9ns
cancel: resting order                 p50=   44.1ns   p99=   228.0ns   p99.9=   342.6ns
mixed load (rest/cross/cancel)        p50=   87.8ns   p99=   460.8ns   p99.9=   878.8ns
  throughput: ~4.6M ops/sec
```

The 50-level sweep's p50/p99 gap (28 ns vs. ~1000 ns) is left in deliberately,
not smoothed over: it's a live example of exactly why this project reports
percentiles instead of a single number. Quoting the p50 alone ("28 ns to sweep
50 price levels!") would be a misleading, cherry-picked claim; the p99 is a far
more honest characterization of the operation's real cost, and the two
together are more informative than either alone. Exact numbers will vary run
to run — re-run `mingw32-make bench` on your own machine rather than trusting
numbers pasted into a README.

## Latency techniques — mapped honestly, not aspirationally

| Technique | Status here |
|---|---|
| Lock-free SPSC ring ([spsc_ring.hpp](include/px/spsc_ring.hpp)) | **Built.** LMAX-style: monotonic head/tail cursors, each side caching its last-seen view of the other's cursor so a comfortably-ahead producer touches the consumer's cache line on ~none of its pushes. Cache-line-padded, single-writer-per-line — no false sharing |
| CPU pinning ([platform_win32.cpp](src/platform_win32.cpp)) | **Built, adapted for Windows.** `SetThreadAffinityMask` + `THREAD_PRIORITY_TIME_CRITICAL`; `GetLogicalProcessorInformationEx` + `EfficiencyClass` to find P-cores and skip SMT siblings on this machine's hybrid i5-12450H (4P+4E) |
| Object pools / no hot-path `new` | **Built.** [`pool.hpp`](include/px/pool.hpp) — preallocated slab + intrusive free list |
| Cache-line padding / false-sharing removal | **Built.** `alignas(64)` throughout; verified `hardware_destructive_interference_size == 64` on this toolchain |
| Branch-friendly single matching path, `-O3 -march=native -flto` | **Built** |
| `isolcpus` | **Documented only.** No Windows equivalent exists; nearest available is affinity + priority, and this project won't touch boot configuration |
| NUMA / `libnuma` | **Seam only.** This machine is single-socket — there is no NUMA win to claim here, so none is claimed |
| Kernel bypass (OpenOnload / ef_vi / DPDK) | **Not attempted.** Wrong OS (Windows) and no Solarflare/Mellanox NIC. [`platform_linux.cpp`](src/platform_linux.cpp) is real, `pthread_setaffinity_np`-based code — not a comment — written for a box that *could* run these, but it has not been compiled or run here (no POSIX headers on this toolchain; confirmed via a syntax-only pass, zero syntax errors, only missing-header errors) |
| FIX gateway | **Documented only.** Institutional access |

**On what "sub-microsecond" actually buys here.** Kalshi's Basic API tier is
roughly 10 order placements per second ([rate limits](https://docs.kalshi.com/getting_started/rate_limits)) —
microseconds on the *send* path are meaningless against a 100 ms budget
between writes. The internal decision path (parse → shadow-book update →
signal → order intent) is where every technique above actually pays, and
that's the honest target: when an out-of-band event lands, every participant
is roughly equidistant from the exchange in wire time, so the edge is in
converting observation into a submitted order first — not in the wire itself.

## Phase 3 — Kalshi connectivity (`live/`)

TLS ([tls_socket.cpp](live/tls_socket.cpp)) + hand-rolled RFC 6455 WebSocket
framing ([ws_client.cpp](live/ws_client.cpp)) + RSA-PSS request signing
([kalshi_auth.cpp](live/kalshi_auth.cpp)) over Winsock2 + OpenSSL — all three
verified to actually work, not just compile, during this project:

- **Signing**: independently cross-verified with the `openssl` CLI
  (`openssl dgst -sha256 -verify ... -sigopt rsa_padding_mode:pss`) against a
  locally-generated test key — confirms the signed string
  (`timestamp_ms + method + path`) and RSA-PSS/SHA-256 parameters are correct,
  not just that the code runs without error.
- **TLS + certificate verification**: tested against a real server
  (`www.google.com:443`) with full chain and hostname verification against the
  actual **Windows root certificate store** (pulled into OpenSSL's
  `X509_STORE` via CryptoAPI in [win_root_certs.cpp](live/win_root_certs.cpp) —
  isolated in its own translation unit because `wincrypt.h` and OpenSSL's
  headers define conflicting macros if included together). Certificate
  verification is never disabled — this talks to a real financial exchange.
- **WebSocket handshake + framing**: tested against a public echo service,
  round-tripping a real message through the masked-frame send path and the
  frame-reassembly receive path. This is also how a real bug got caught: the
  server sent `sec-websocket-accept:` in lowercase, and the initial exact-case
  header search missed it — HTTP header *names* are case-insensitive per
  RFC 7230 §3.2, values are not, and the fix (search a lowercased copy, extract
  from the original) respects that distinction.
- **JSON field extraction** ([json_scan.hpp](live/json_scan.hpp)): not a
  general parser — a specialized scanner for Kalshi's flat, shallow message
  fields, unit-tested offline against realistic `ticker`/`orderbook_snapshot`
  payloads. No JSON library is installed in this toolchain, and a general
  parser would be more machinery than the actual message shapes need.

**Correction, made in Stage 2 (below):** this section originally flagged the
`orderbook_delta` wire schema as a best-effort, unverified guess. It was
guessed wrong — see "the schema was wrong" in the Stage 2 section for what
broke and how it was fixed and verified against Kalshi's published AsyncAPI
spec. **Still not run against Kalshi itself** — no API credentials were
available or sought at any point in this project.

**Safety posture, structural, not just documented:**
`place_order(..., dry_run=true, ...)` — the default everywhere it's called —
builds and signs the full request but **never constructs a `TlsSocket`**;
verified by pointing it at a nonexistent host and confirming it returns
instantly with no DNS/connection attempt. Key material is read from
`KALSHI_API_KEY_ID`/`KALSHI_PRIVATE_KEY_PATH` environment variables only,
never a CLI argument (visible in process listings/shell history) or source
file; `.gitignore` covers `*.pem`/`*.key`/`.env`. Demo environment is the
default host everywhere; production requires explicitly setting `KALSHI_ENV=prod`.

```bash
export KALSHI_API_KEY_ID=your_key_id
export KALSHI_PRIVATE_KEY_PATH=/path/to/your_key.pem
PATH="/c/msys64/ucrt64/bin:$PATH" ./build/release/kalshi_shadow.exe KXHIGHNY-25AUG08-B89.5
```

## Phase 4 — oracle feeds and signal hooks (`strategy/`)

A prediction market moves on the real-world event it's a bet on, not on order
flow — [`OracleFeed`](strategy/oracle_feed.hpp) is the pluggable interface for
out-of-band sources (NOAA, SEC EDGAR, FAA, ...), each running on its own
thread and publishing into a lock-free ring;
[`Signal`](strategy/signal.hpp) turns an event plus the current book into an
`OrderIntent`, which something else (a real order route, or nothing at all)
decides whether to act on.

**This project ships only synthetic/simulated feeds** — nothing here calls out
to NOAA, EDGAR, or the FAA. `mingw32-make strategy_demo` runs the whole
pipeline locally and safely end to end: a synthetic NOAA thread publishes
readings hovering near a 90°F threshold, the main thread drains them against a
local demo market's live book, `ThresholdSignal` fires BUY/SELL intents when
the reading crosses the threshold with enough edge over the book, and those
intents are actually submitted to a local `MatchingEngine` — closing the loop
with real settlement, verified arithmetically (10 sells × 5 @ 8¢ + 1 buy × 5 @
92¢ nets exactly the −60¢ cash change and −45 position the engine reports).
Wiring a real feed is implementing `OracleFeed::run` against that source's
actual API and pushing into the same ring — everything downstream is unchanged.

**Correction, made in Stage 3 (below):** `Signal`/`ThresholdSignal`, as this
section originally described them, are superseded — `strategy/signal.hpp` is
deleted. `OracleThresholdStrategy` (`strategy/oracle_threshold_strategy.hpp`)
is the direct port of `ThresholdSignal` onto the `Strategy` interface Stage 3
introduces, kept deliberately reference/demo-only rather than promoted to a
peer of the three strategies that stage is actually about — see the Stage 3
section for why, and for what a `Strategy` can do that a `Signal` couldn't.

## Stage 2 — the live pipeline (`pipeline/`, `risk/`, `live/kalshi_gateway.*`)

Stage 1 built the *pieces* of a live connection (TLS, WebSocket, signing, a
shadow-mode `OrderBook`) but never assembled them into a running pipeline, and
one of those pieces was wrong. Stage 2 is both a correctness fix and an
architectural assembly:

```
KALSHI --TLS/WS--> [T1 Ingress]  --ring A-->  [T2 Brain]  --ring B-->  [T3 Egress]
                    blocking recv              ShadowBook               RSA-PSS sign
                    byte-scan -> BookEvent      + Signal                + REST send
                    ticker -> MarketId          + RiskEngine.check            |
                                                     ^                        |
                                                     +--------ring C----------+
                                                         ExecReport (reject/429)
```

All three rings are the same [`SpscRing`](include/px/spsc_ring.hpp) Stage 1
already built and measured, reused unchanged. [`Ingress`](pipeline/ingress.hpp)
is T1, [`Brain`](pipeline/brain.hpp) is T2 — the only thread that touches
`ShadowBook`/`RiskEngine` state, mirroring `MatchingEngine`'s single-writer
discipline from Stage 1 — and [`Gateway`](live/kalshi_gateway.hpp) is T3.
`ShadowBook` itself ([shadow_book.hpp](include/px/shadow_book.hpp)) is a
purpose-built ~440-byte type, not the ~5.7 MB `OrderBook` reused from Stage 1:
tracking even 20 markets in the full offline book (which carries an unused
`Order` slab + id-map in shadow mode) would be 114 MB, well past L3; the slim
ladder-plus-two-bitmasks version puts 20 markets under 9 KB.

### The schema was wrong

[`kalshi_feed.cpp`](live/kalshi_feed.cpp) was originally written against a
guessed schema, flagged as unverified at the time (see the correction note in
Phase 3, above). Checked against Kalshi's published AsyncAPI spec, it was
wrong in a way that would have made it parse **nothing while reporting
success**: every price/quantity field Kalshi actually sends is a *quoted
decimal string* (`"price_dollars": "0.0800"`), not the bare integers the code
assumed, and the existing `json_extract_int`/`json_extract_number` scanners
stop at the opening `"`. Fixed with new scanner primitives
([`json_scan.hpp`](live/json_scan.hpp): `parse_decimal_scaled`,
`json_extract_dollars_as_cents`, `json_extract_dollar_pair_array`) and a full
rewrite of the message decoder, now covered by 51
[golden-frame tests](tests/test_feed_parse.cpp) built from the real schema —
including the AsyncAPI spec's own example (`"0.0800"` → 8 cents, checked to
round rather than truncate any real sub-cent digit) and a deliberate `seq` gap
that must trigger a resync request.

One thing is flagged rather than guessed a second time: whether `seq` is
scoped per subscription or per market within a shared subscription isn't
documented anywhere found. [`SeqTracker`](live/kalshi_feed.hpp) tracks it per
watched ticker specifically because that fails toward an occasional
*unnecessary* resync (cheap, idempotent) rather than a *missed* gap (silent
book corruption) if the assumption turns out backwards.

### RiskEngine — three checks, integer-only, inline

Not a thread: a `RiskEngine::check(...)` call in T2, immediately before an
accepted `OrderIntent` reaches ring B ([`risk_engine.hpp`](risk/risk_engine.hpp)).
Because a contract always settles at 0 or 100 cents, max loss is bounded and
known at order-entry time — there's no margin model beyond that bound, so this
does exactly three things:

1. **Token bucket** (survival). Kalshi's Basic tier is ~10 order placements/sec
   ([rate limits](https://docs.kalshi.com/getting_started/rate_limits)); a 429
   arriving on ring C drains the local bucket to resync with reality rather
   than continuing to optimistically spend against a budget that's already
   gone.
2. **Max exposure**: `|position| × 100¢` per market, checked against the
   *projected* position an order would create.
3. **Fat-finger bounds**: reject outside `[5, 95]¢` regardless of what the
   strategy signals — checked first, before the other two, so a broken
   strategy is caught even with an exhausted rate-limit bucket.

58 unit tests ([test_risk.cpp](tests/test_risk.cpp)), including one that
specifically exercises the integer token bucket's carry-remainder logic: ten
3 ms refill steps (not a divisor of the 10 ms/token rate) must still total
exactly 3 tokens, which only happens if fractional remainders survive across
separate `check()` calls instead of being truncated away each time.

### Gateway (T3) — sink-parameterized, dry-run by default

[`live/kalshi_gateway.hpp`](live/kalshi_gateway.hpp): a `Sink` chosen **once,
at construction** — `DryRunSink` (default: builds and signs the full request,
never opens a socket) or `LiveSink` (real send, still only ever to whatever
host the config names — demo by default). Gateway never branches on this
per-order; "can this send real orders" is a structural property, not a
runtime decision inside the dumb pipe the design calls for.

**Measured, not assumed, why signing lives on its own thread**
([bench_sign.cpp](bench/bench_sign.cpp), calling the real production
`sign_request`, not a reimplementation):

```
RSA-2048 PSS sign_request()   p50=  818µs   p99= 2123µs   max= 3537µs
ShadowBook level update       ~50-100ns (bench_engine's order-submit numbers)
```

Signing costs roughly **11,000×–28,000×** a book update, with a multi-millisecond
tail. Inline in T2 that would freeze book updates for over a millisecond while
the market moved. At ~1,200 signs/sec headroom against Kalshi's ~10/sec write
limit, this was never a throughput problem — it's purely jitter, which is the
entire justification for a separate thread, now with a number behind it.

**Fill tracking gap, flagged rather than faked:** Kalshi's create-order REST
response does not carry fill information — confirmed against the AsyncAPI
spec, which documents fills as arriving asynchronously over a *separate*,
privately-authenticated `fill` WebSocket channel. Building that listener
(straightforward reuse of `KalshiFeed`'s connect/subscribe machinery against
channel `"fill"` instead of `"orderbook_delta"`) is real, scoped-out work, not
attempted here. What Gateway *does* parse from the REST response — the HTTP
status line — needs no schema guessing: `429` → `RateLimited`, other non-2xx →
`Reject`, `2xx` → nothing pushed to ring C (accepted is not filled).
`RiskEngine::record_fill` is consequently never invoked by this stage's live
path — a real, documented limitation, not a bug in `record_fill` itself (its
own unit tests exercise it directly).

### A second real bug, found writing the tests for the first fix

Loading a Kalshi private key ([`kalshi_auth.cpp`](live/kalshi_auth.cpp)) used
`fopen()` + OpenSSL's `PEM_read_PrivateKey(FILE*, ...)`. On this toolchain
(OpenSSL 3.2 via MSYS2 UCRT64) that fails outright —
`error:1E08010C:DECODER routines::unsupported` — because a `FILE*` opened by
one CRT and handed across a DLL boundary into OpenSSL, built against a
different CRT, doesn't behave the way its layout suggests. This would have
silently broken *every* real Kalshi connection attempt; caught here only
because [test_gateway_dry_run.cpp](tests/test_gateway_dry_run.cpp) needed to
generate and load a throwaway test key and the load failed outright. Fixed by
switching to `BIO_new_file` + `PEM_read_bio_PrivateKey`, which lets OpenSSL
open the file with its own linked CRT instead of crossing the boundary with a
raw handle — verified with a minimal standalone repro before touching the real
code, then confirmed fixed by the same test going green.

### Verification

- **Golden-frame tests** (51 checks) — schema correctness, described above.
- **Risk engine unit tests** (58 checks) — bucket exhaustion/refill/carry
  logic, exposure boundary (long and short), fat-finger bounds, check
  ordering, the 429-drain hook.
- **Gateway dry-run structural test** (27 checks) — `DryRunSink` proven to
  never send by pointing it at `invalid.invalid` (an RFC 6761
  guaranteed-non-resolving domain) and confirming success anyway; `LiveSink`
  against the identical host proven to genuinely fail, so the first result
  isn't vacuous. Plus HTTP status-line parsing and `Gateway::run`'s dispatch
  (429→ring C, 2xx→nothing) exercised end-to-end through real threads and
  real rings.
- **Replay determinism** (108 checks,
  [test_replay_determinism.cpp](tests/test_replay_determinism.cpp)) — a fixed
  frame sequence, pushed through the real `Ingress::handle_message` into a
  real `Brain` running on its own thread, converges to an exact, hand-verified
  final `ShadowBook` state every time, and two fully independent runs produce
  bit-identical results. Scoped deliberately to the book-update path only —
  the oracle/signal/risk path genuinely depends on wall-clock time
  (`RiskEngine`'s token bucket), so claiming byte-identical determinism there
  would itself be the kind of unverified claim this project avoids.

```bash
mingw32-make test-live
mingw32-make bench-live
```

### Running the pipeline

```bash
# Zero network, deterministic — drives T1 from a file of raw WS-message JSON
# lines (see tests/frames/smoke.jsonl for the shape) through the real decode
# path, T2 and T3 running as real threads throughout:
./build/release/kalshi_pipeline.exe --replay tests/frames/smoke.jsonl KNYC-DEMO

# Real connection, still dry-run (every order built, signed, logged, never sent):
export KALSHI_API_KEY_ID=your_key_id
export KALSHI_PRIVATE_KEY_PATH=/path/to/your_key.pem
PATH="/c/msys64/ucrt64/bin:$PATH" ./build/release/kalshi_pipeline.exe KXHIGHNY-25AUG08-B89.5

# Real connection AND real sends, still only to Kalshi's demo host:
./build/release/kalshi_pipeline.exe --demo KXHIGHNY-25AUG08-B89.5
```

## Stage 3 — the Strategy Engine (`strategy/`, `pipeline/brain.*`)

Stage 1's `Signal` only ever answered one question — *should I trade, right
now, in response to an oracle event* — and always via a marketable IOC order.
Stage 2 wired that into the live pipeline but didn't change what it could
decide. Stage 3 replaces it with [`Strategy`](strategy/strategy.hpp): an
interface that decides *what kind* of execution to take —
`Join`/`Improve`/`Cross`/`Cancel`/`Wait` — in response to any of three
triggers (`BookUpdate`, `OracleEvent`, `OwnOrderUpdate`), not only an oracle
tick.

### What a Strategy sees

[`market_state.hpp`](strategy/market_state.hpp)'s `extract_features()`
assembles a `MarketState` (spread, BBO depth, imbalance, this account's own
position, recent trade flow, this account's own acked resting orders) fresh
on every dispatch — a pure function over already-owned state (`ShadowBook` +
`OrderTracker` + `TradeWindow`), independently unit-tested with no
pipeline/thread/network involved
([tests/test_market_state.cpp](tests/test_market_state.cpp)).

[`order_tracker.hpp`](strategy/order_tracker.hpp)'s `orders_for()` exposes
**acked orders only**. This is the structural fix for a real race: without
it, a strategy could decide to cancel an order Gateway hasn't confirmed yet —
racing its own not-yet-arrived ack. With it, that order simply doesn't exist
from a `Strategy`'s point of view until Gateway says so, so the invalid
decision can't be constructed in the first place, not just detected and
rejected after the fact.

### Four strategies, one demo-only

- [`PassiveMarketMaker`](strategy/passive_market_maker.hpp) — the one with an
  explicit Flat → Quoting → Position → Exiting state machine. Quotes both
  sides passively when flat and the spread is wide enough; crosses
  aggressively the moment a position exists, on *whichever* trigger notices
  it first (`BookUpdate` or `OwnOrderUpdate`) rather than waiting for the next
  incidental book tick — the entire reason `OwnOrderUpdate` exists as its own
  trigger kind.
- [`OrderBookImbalance`](strategy/order_book_imbalance.hpp) — reads
  `MarketState::imbalance` alone; heavily skewed crosses now, moderately
  skewed joins the stronger side, balanced waits.
- [`TradeFlow`](strategy/trade_flow.hpp) — reads the recent trade tape
  (`MarketState::recent_trades`), not static depth. Genuinely different
  information from `OrderBookImbalance`: two books with identical current
  depth can have arrived there via very different recent flow.
- [`OracleThresholdStrategy`](strategy/oracle_threshold_strategy.hpp) — the
  straight port of Stage 1's `ThresholdSignal` onto this interface. Kept
  explicitly reference/demo-only (see the correction note in the Phase 4
  section above) so `strategy/oracle_feed.hpp` and `tools/strategy_demo.cpp`
  stay exercised and `TriggerKind::OracleEvent` has a real consumer — not a
  fourth peer of the execution-intelligence strategies above.

`tools/kalshi_pipeline.cpp` registers the first three against the live
pipeline; `tools/strategy_demo.cpp` (Phase 4's demo, ported onto the new
interface) still runs `OracleThresholdStrategy` locally against a synthetic
NOAA feed with no network involved — same verified arithmetic as before (10
sells × 5 @ 8¢ + 1 buy × 5 @ 92¢ nets exactly −60¢ cash / −45 position).

### Brain: dispatch, not just decode

[`pipeline/brain.hpp`](pipeline/brain.hpp)'s `dispatch_market_triggered` now
fires on any book/trade mutation on a market (not only oracle events, as
Stage 1 built it) against every strategy registered on that market, and
`apply_exec_report` fires `OwnOrderUpdate` immediately when one of this
account's own orders is acked/filled/cancelled. `OrderIntent` gained an
`IntentKind` (`NewOrder`/`CancelOrder`) so a `Cancel` decision can travel ring
B alongside new orders — Kalshi's own UUID `order_id` still never appears on
any ring (same "Gateway keeps that mapping privately" reasoning Stage 2
already established); a `Cancel` targets Brain's own integer `OrderId`, the
same one `OrderTracker` already keys on. `RiskEngine` gained a matching
`check_cancel` so a cancel-and-replace loop still pays the rate limiter, not
just new orders (see [risk/risk_engine.hpp](risk/risk_engine.hpp) for the
honestly-flagged assumption behind `tokens_per_cancel`'s default, and for the
"resting exposure from GTC orders isn't tracked between fills" gap
`PassiveMarketMaker` specifically can trigger by quoting both sides at once).

Kalshi's `trade` channel is now decoded too
([`live/kalshi_feed.cpp`](live/kalshi_feed.cpp)) and collapsed into the same
`BookEvent` type as book mutations (`BookEventKind::Trade`) rather than given
its own ring — every strategy re-reads the full `MarketState` regardless of
which specific thing changed, so a separate identity would buy nothing;
[`trade_window.hpp`](strategy/trade_window.hpp) is the fixed-capacity,
exchange-time-bounded ring `TradeFlow` reads it from.

### A bug the fuzz test found, not assumed absent

[`ShadowBook::set_level`](include/px/shadow_book.hpp) shares one `qty_` slot
across both sides of a price level (a normalized book is never simultaneously
a bid and an ask at the same price). An earlier version of this file's own
side-flip fix handled a level flipping sides *to a nonzero quantity*
correctly, but missed the mirror case: side A holds real resting quantity,
then side B "sets" the same price to **zero** — the shared `qty_` slot gets
overwritten to 0 (correct), but side A's occupancy mask bit was never touched
(not correct) — `qty_` says empty, the mask says occupied.
[`tests/test_replay_fuzz.cpp`](tests/test_replay_fuzz.cpp) found this by
construction, not inspection: it pushes an adversarial, randomized stream of
`BookEvent`s directly onto ring A (biased toward reusing recently-touched
prices, to maximize how often exactly this side-flip pattern occurs) through
a real `Brain` thread, differentially checks the resulting `ShadowBook`
against an independently-written reference model of the same documented
spec, and calls `ShadowBook::check_invariants()` after every single event —
the same "after every single command" discipline
[`fuzz_vs_reference.cpp`](tests/fuzz_vs_reference.cpp) already holds the
offline `OrderBook` to. The fix: unconditionally clear the *other* side's
mask bit before deciding this side's own, not only on the positive-quantity
branch. 828,000 checks now pass across 4,000 adversarial events split across
two markets.

### Measured, not assumed: strategy dispatch stays inline on T2

The same reasoning that put RSA-PSS signing on its own thread (T3) — measure
the cost, don't guess — applies to what Brain now also runs inline on T2:
[`bench/bench_strategy.cpp`](bench/bench_strategy.cpp) times
`extract_features` and each strategy's `on_event`, called through the same
`Strategy&` interface Brain calls them through:

```
extract_features (MarketState assembly)   p50=  32.5ns   p99=  36.1ns
PassiveMarketMaker (quoting)              p50=  18.4ns   p99=  34.5ns
PassiveMarketMaker (managing position)    p50=  18.4ns   p99=  19.6ns
OrderBookImbalance (book-driven)          p50=  18.8ns   p99=  19.6ns
TradeFlow (trade-flow-driven)             p50=  23.6ns   p99=  28.4ns
OracleThresholdStrategy (oracle-driven)   p50=  25.2ns   p99=  29.3ns
```

Every number here sits at or below a `ShadowBook` level update's own
~50-100ns (`bench_engine`), and well over three orders of magnitude under
RSA-PSS signing's measured hundreds-of-microseconds p50 (`bench_sign`) —
comfortably inside the same "cheap enough to run inline" territory a book
update already occupies, unlike signing. (Exact numbers will vary run to
run — re-run `mingw32-make bench` on your own machine rather than trusting
numbers pasted into a README, same caveat the engine benchmarks above carry.)

`Brain::dispatch_market_triggered` calls `extract_features` once per
registered strategy on a market rather than computing it once and sharing it
across all of them — a deliberate simplification (fewer moving parts than
threading a shared `MarketState` through the loop), not an oversight; this
benchmark is what makes "cheap enough that the redundancy doesn't matter" a
measured claim instead of an assumed one.

```bash
mingw32-make test    # now also builds/runs test_market_state, test_strategies,
                      # and test_replay_fuzz alongside the Stage 1 core tests
mingw32-make bench    # now also builds/runs bench_strategy
```

## Stage 4 — proving the whole thing, adversarially, end to end

Stages 1–3 each proved their own layer correct in isolation. Stage 4 asks a
different question: does the *assembled* system — Ingress through Strategy
through Risk through Gateway, real threads, real rings — actually hold up
under real timing and real hostile input, not just each piece's own unit
tests? See [PERFORMANCE.md](PERFORMANCE.md) for the full report (the
optimization history, every measured number in one place, and the
methodology behind each one); this section is the summary.

### The dry-run loop, proven closed — including cancellation

[`tests/test_pipeline_e2e.cpp`](tests/test_pipeline_e2e.cpp) runs Ingress,
a real `Brain` thread, and a real `Gateway` thread together, driven by
scripted WS frames through the actual production parse path, closing the
loop with a `SimulatedExchangeSink` — a Sink standing in for a well-behaved
exchange's REST responses (no network, no credentials), because the real
`DryRunSink` **cannot** close this loop: `Gateway::handle_new_order` returns
immediately after `if (!did_send) return;`, since dry-running against
nothing means there's no response to parse. That's correct and intentional,
but it also means Acked/Fill/Cancelled/`OwnOrderUpdate` — and cancellation
itself, which `OrderTracker`'s whole design depends on an order having
actually been acked first — are silently unexercised by just running
`kalshi_pipeline.exe` in its default mode. Two scenarios prove the full
BUY/SELL/CANCEL/ACK/FILL/`OwnOrderUpdate` set closes correctly, including
Gateway correctly resolving Brain's own `OrderId` back to the same Kalshi
UUID a cancel needs to target.

### Strategy behavior, not just strategy speed

`tests/test_strategies.cpp` grew from 59 to 76 checks: exact-threshold
boundary tests (a value sitting precisely on a decision boundary must land on
the "acts" side, not fall through via an off-by-one), degenerate-input tests
(a one-sided or empty book must return `None`, not crash or silently read
garbage), and narrative sequences (the same strategy instance, watching one
market's depth evolve step by step, crossing from Wait into Join into Cross
in the right order — not three disconnected snapshots). The header now also
states explicitly which decision categories aren't reachable and why:
`Improve` is defined in the `Strategy` interface but no shipped strategy
produces it, and `TradeFlow` deliberately never crosses (see its own file
header) — both facts, not gaps in test coverage.

### Adversarial input, extended past the book into the whole pipeline

`tests/test_replay_fuzz.cpp` (Stage 3) already found one real bug this way.
[`tests/test_hostile_events.cpp`](tests/test_hostile_events.cpp) extends the
same methodology — real threads, real rings, adversarial input, not a
reimplemented shortcut — to duplicate/reordered feed messages, partial
fills, cancel/fill races, connection failure, risk rejection, and an
unrecognized message type. Two categories are explicitly **not** covered,
and said so rather than silently skipped: reconnect (there is no reconnect
logic — `Ingress::run` has no retry loop at all, a gap this section flags,
not fakes) and market halt/resolution-*aware* trading (Kalshi's
`market_lifecycle_v2` decodes to `Unknown` and is safely ignored, which is
different from — and short of — any strategy actually reacting to a halt).

**Three more real bugs, found by writing this test, not by inspection:**

1. `live/kalshi_feed.hpp`'s `SeqTracker` doc comment promised "won't trust
   the stream again until the next Snapshot" after a gap — the code didn't
   deliver on it. The very next delta after a gap was accepted
   unconditionally, silently re-baselining off of it; a duplicate or
   reordered delta arriving in that window would have been applied with zero
   verification. Fixed with an explicit `desynced_` state that drops
   everything until a real Snapshot arrives.
2. & 3. `strategy/order_tracker.hpp` had two independent zombie/double-count
   bugs in the fill-at-entry path — see the Stage 3 section above for the
   first (a fully-resolved order left a permanent entry, silently disabling
   `OrderBookImbalance`/`TradeFlow` after their first trade) and
   [`live/kalshi_gateway.hpp`](live/kalshi_gateway.hpp)'s matching
   unbounded-map-growth fix. Stage 4 found a second, sharper one: `on_fill`
   independently subtracted the fill amount from `remaining_qty`, even
   though `on_acked` (called immediately before it, for the exact same
   synchronous event) had already set `remaining_qty` to Kalshi's own
   authoritative post-fill number — double-counting the identical fill. A
   10-lot order reported as "6 remaining, 4 filled" was ending up tracked as
   2 remaining. Fixed by making `on_fill` accumulate `filled_qty` only,
   never re-derive `remaining_qty` — every remaining-quantity update in this
   codebase now comes from an authoritative Kalshi-reported number
   (`on_acked`'s `remaining_count`, `on_cancelled`'s `reduced_by`), never a
   second, locally-computed one.

### The number this whole project has been building toward

[`bench/bench_pipeline_e2e.cpp`](bench/bench_pipeline_e2e.cpp) times the
complete internal decision path — WS text frame → `parse_feed_message` →
`Ingress::handle_message` → ring A → **T2 Brain** (ShadowBook update,
`extract_features`, `Strategy::on_event`, `RiskEngine::check`) → ring B →
`OrderIntent` observed — through two real, pinned threads and two real
cross-core ring hops, using the same ping-pong-one-event-at-a-time discipline
`bench_ring.cpp` already established (see PERFORMANCE.md for why that
discipline matters — it's the same methodology bug, avoided this time):

```
e2e: WS frame -> ... -> OrderIntent   p50= 3129ns   p99= 4178ns
```

That's the honest, measured answer to "how much latency does the internal
decision path actually add": roughly 3.1µs at p50 to turn a raw exchange
message into a routed trading decision — crossing two rings, parsing real
JSON, mutating a book, assembling features, and running a strategy — five
orders of magnitude under RSA-PSS signing's ~1.4ms p50 (`bench_sign`), which
is exactly the argument this project has made from the start for why the
decision path, not the wire, is where the real edge lives (see "On what
'sub-microsecond' actually buys here," above).

```bash
mingw32-make test-live   # now also builds/runs test_pipeline_e2e, test_hostile_events, test_lifecycle
mingw32-make bench-live  # now also builds/runs bench_pipeline_e2e
```

### Closing the last two documented gaps: reconnect and market lifecycle

Everything above proved the assembled system correct and measured under the
assumption of a single, unbroken connection to an always-open market — two
gaps this project had explicitly flagged as absent, not silently ignored
(see the SCOPE note in [`tests/test_hostile_events.cpp`](tests/test_hostile_events.cpp)'s
original version). Both are now built.

**Reconnect.** [`Ingress::run`](pipeline/ingress.cpp) no longer exits the
instant a connect/subscribe/receive fails once — it retries with capped
exponential backoff (1s, 2s, 4s, ... up to 30s, resetting the moment a
connection is healthy again) until `stop_flag` is set, never giving up and
going silently dark. A fresh [`KalshiFeed`](live/kalshi_feed.hpp) is
constructed for every attempt rather than reset in place — not a style
choice: `TlsSocket` explicitly deletes its copy constructor/assignment
without declaring a move constructor/assignment, which (per normal C++
rules) suppresses the *implicit* move operations too, not just copy. That
makes `KalshiFeed` — which holds a `WebSocketClient`, which holds a
`TlsSocket`, both by value — neither copyable nor movable, so "reset for a
reconnect" can only honestly mean "construct a new one." After a successful
reconnect, every watched ticker gets an explicit fresh-snapshot request via
`request_resync` — reusing the *exact* mechanism a seq gap already
triggers, rather than assuming a bare `subscribe()` delivers one on its own
(an assumption this project's own schema-verification discipline wouldn't
let stand unconfirmed). That snapshot resynchronizes state through
already-tested paths with nothing new to get wrong: `SnapshotBegin` already
clears the `ShadowBook` (`pipeline/brain.cpp`), and a `Snapshot` already
unconditionally re-baselines `SeqTracker` (`kalshi_feed.hpp`). Proven in
[`tests/test_hostile_events.cpp`](tests/test_hostile_events.cpp): retries
actually happen (not give-up-after-one-failure), the backoff schedule is
real (not a tight retry loop), and `stop_flag` interrupts a backoff
promptly rather than being waited out.

**Market lifecycle.** Checked against Kalshi's real AsyncAPI spec before
writing a line of mapping code — same discipline as every other schema fact
in this project — `market_lifecycle_v2`'s `event_type` enum has **no
`"halted"` value**: `created`, `activated`, `deactivated`,
`close_date_updated`, `determined`, `settled`,
`price_level_structure_updated`, `metadata_updated`. This project maps
`activated` → `Open`, `deactivated` → `Halted` (the closest real analog —
"not tradeable right now," duration unstated), and `determined`/`settled` →
`Resolved`, reusing `px::MarketStatus` — the *offline* engine's own
`PreOpen`/`Open`/`Halted`/`Resolved` vocabulary
([`px/market.hpp`](include/px/market.hpp)) — rather than inventing a parallel
enum for the live path. `Brain` tracks one `MarketStatus` per market,
**defaulting to `Open`, not `PreOpen`**: Kalshi's lifecycle channel reports
state *changes* from subscription time onward, not a backfill of current
status, so a market already open before this pipeline connected would never
get an explicit `activated` event — defaulting to `PreOpen` would misread
silence as "not tradeable" and block every order this system would otherwise
send, a strictly worse failure mode than the one line of code that avoids it.
`Brain::handle_decision` gates on it before `RiskEngine` even runs: `Halted`
blocks new orders but still allows `Cancel` (risk-reducing, the same
reasoning `RiskEngine::check_cancel` already extends to fat-finger/exposure);
`Resolved` blocks everything, cancels included — nothing is left to trade.
Proven in [`tests/test_lifecycle.cpp`](tests/test_lifecycle.cpp) (68 checks)
through the real `PassiveMarketMaker` reacting to real book movement — not a
scripted stand-in — across the full schema-mapping, `Ingress`-decoding, and
`Brain`-gating layers.

```bash
mingw32-make test-live   # now also builds/runs test_lifecycle
```

## Stage 5 — proving and measuring it as a system

Stages 1-4 built the thing and tested it adversarially. Stage 5 asked a
different question: not "does it work," but "how does it behave under
realistic load, where does time actually go, and does the architecture's own
claims about itself hold up when measured instead of argued for." Full
numbers, methodology, and an expanded Limitations section are in
[PERFORMANCE.md](PERFORMANCE.md); this section is the narrative.

**Profiling found the hot path isn't where intuition would put it.** A new
per-stage benchmark ([`bench/bench_pipeline_stages.cpp`](bench/bench_pipeline_stages.cpp))
broke the single-event decision path into six independently-timed stages for
the first time. Parsing the raw WS text frame turned out to be ~80-85% of
the total cost — not the strategy decision, not the risk check, not the
shadow-book mutation, all of which are an order of magnitude cheaper. The
mechanism traces to [`live/json_scan.hpp`](live/json_scan.hpp) re-scanning
the same message from byte zero for every field it reads, independently.
Identified precisely, deliberately **not** patched this stage (see
PERFORMANCE.md's Section 10) — that file carries this project's highest
verification bar for a reason, and a scan-strategy change deserves its own
dedicated pass with full regression coverage, not a rushed fix squeezed in
alongside eleven other things.

**A synthetic workload generator caught its own measurement bug before it
could publish a wrong number** — the same "the methodology is part of the
system under test" lesson this project already learned once with
[`bench_ring.cpp`](bench/bench_ring.cpp) (see "Measuring latency," above),
learned independently a second time: the new deterministic workload
generator ([`workload/generator.hpp`](workload/generator.hpp)) initially
returned a freshly-heap-allocated JSON string on every call, and that
allocation cost (~1166ns, comparable to parsing itself) was mostly what a
first throughput sweep measured as "the pipeline's ceiling." Fixed (a
reused, caller-owned buffer instead), and the measured ceiling roughly
doubled once the harness stopped getting in its own way.

**Multi-market scaling looked flat at the surface, and wasn't quite,
underneath.** A full-pipeline sweep from 1 to 1000 registered markets showed
no visible latency growth — but that measurement's own noise floor is
larger than a real, smaller effect this project's static-analysis pass had
already flagged: `Brain::dispatch_market_triggered` scanned every
registered strategy on every single book update, not just the ones on the
market that actually changed. A second, purpose-built micro-benchmark
([`bench/bench_dispatch_scaling.cpp`](bench/bench_dispatch_scaling.cpp)),
using the same one-event-at-a-time discipline `bench_pipeline_e2e.cpp`
already established, found it: a real, if modest, cost that grows with
total registered strategy count. Fixed by indexing strategies per market
instead of scanning all of them — accepted only after re-measuring the same
benchmark and confirming the growth disappeared, and re-running the entire
existing correctness suite unchanged.

**Failure testing found a real gap in order tracking, not just confirmed
what was already believed.** Driving a broken exchange-response sink
through a real `Brain`/`Gateway` pair
([`tests/test_reconnect_recovery.cpp`](tests/test_reconnect_recovery.cpp))
showed that once a market's 32-slot pending-order table fills up — for any
reason, a Gateway failure or simply enough legitimate orders — every order
placed after that point still gets sent to the real exchange, but becomes
permanently untrackable locally: invisible to the acked-only view every
`Strategy` sees, and never targetable by a future cancel. The system never
lies about an order's state (verified directly — nothing was ever falsely
reported as confirmed-live), but it also never learns the truth once
tracking capacity runs out. No fix ships for this: building real order
reconciliation against exchange truth would mean a new REST endpoint this
project doesn't have, and adding one wasn't this stage's job — it's
documented as a known limitation instead of silently left untested or
quietly patched around.

**Everything else, in one line each**: a reproducible baseline exists and
is checked into `perf/baseline/`; `gprof` was attempted and produced no
usable output on this Windows/MinGW toolchain (documented, not hidden); the
existing lock-free ring's already-in-place spin-wait backpressure policy is
now written down and justified instead of merely implicit; a hot-path
allocation audit ([`tests/test_alloc_audit.cpp`](tests/test_alloc_audit.cpp))
found zero heap allocations across 10,000 iterations of the real per-event
decision path, in both debug and release builds; and a regression harness
now diffs fresh benchmark runs against a frozen baseline with tolerances
calibrated against this machine's own measured run-to-run noise, not guessed
numbers.

```bash
mingw32-make bench-perf        # builds the Stage 5 throughput/scaling/dispatch-cost tools
python perf/run_sweeps.py all  # runs every sweep, writes perf/results/*.csv
python perf/plot_results.py    # turns those into perf/results/*.png
mingw32-make perf-regression   # diffs a fresh run against perf/baseline/
```

## Trying it yourself

```bash
mingw32-make repl
```

```
> market create KXTEST-26AUG08
> market open 0
> trader create 100000
> trader create 100000
> sell 0 0 yes 60 10
> buy 0 1 yes 65 4
> book 0
> trades
```
