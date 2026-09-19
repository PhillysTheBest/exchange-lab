// bench/bench_engine.cpp — single-thread cost of each kind of engine operation,
// no rings involved. Every case gets a FRESH engine with its own book, and every
// timed request is paired with an untimed one that restores the book, so each
// sample measures the same operation against the same book shape. Includes the
// sorted-level index's worst case: adding or removing a level at the far end of
// a nearly full side, which moves the whole array.
#include "px/histogram.hpp"
#include "px/matching_engine.hpp"
#include "px/platform.hpp"
#include "px/tsc_clock.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>

using namespace px;

namespace {

constexpr Price kTick = 100;
constexpr Price kMid = 100 * kPriceScale;
constexpr OwnerId kTaker = 999'999;
constexpr int kWarmup = 10'000;

struct Case {
    std::unique_ptr<MatchingEngine> eng = std::make_unique<MatchingEngine>();
    std::uint64_t events = 0;
    EventSink sink{&events, [](void* c, const ExecEvent&) { ++*static_cast<std::uint64_t*>(c); }};
    OrderId next_id = 1;

    Case() { eng->add_instrument("BENCH", kTick); }

    OrderId rest(Side side, Price px, Qty qty, OwnerId owner = 1) {
        const OrderId id = next_id++;
        eng->process(limit_order(id, 0, owner, side, px, qty), sink);
        return id;
    }
    // `levels` levels per side starting `gap` ticks from the mid, `per_level` orders each.
    void book(int levels, int per_level, Qty qty, int gap = 1) {
        for (int l = 0; l < levels; ++l) {
            for (int k = 0; k < per_level; ++k) {
                rest(Side::Buy, kMid - (gap + l) * kTick, qty, static_cast<OwnerId>(1 + k));
                rest(Side::Sell, kMid + (gap + l) * kTick, qty, static_cast<OwnerId>(1 + k));
            }
        }
    }
    Tsc timed(const OrderRequest& r) {
        const Tsc t0 = tsc_begin();
        eng->process(r, sink);
        const Tsc t1 = tsc_end();
        return t1 - t0;
    }
};

template <typename Fn>
void run(const char* label, int iters, const TscClock& clock, Fn&& one_iteration) {
    LatencyRecorder rec(static_cast<std::size_t>(iters));
    for (int i = 0; i < kWarmup + iters; ++i) {
        const Tsc t = one_iteration();
        if (i >= kWarmup) rec.record(t);
    }
    print_percentiles(label, rec.summarize(clock));
}

} // namespace

int main() {
    harden_process();
    const auto cores = performance_core_ids();
    if (!cores.empty()) pin_current_thread_to(cores.back());
    const TscClock clock;
    std::printf("single-thread engine cost per request (tsc bracket around process(); book restored untimed)\n");

    {
        Case c;
        c.book(100, 10, 100);
        run("new order, joins best level", 200'000, clock, [&] {
            const OrderId id = c.next_id++;
            const Tsc t = c.timed(limit_order(id, 0, kTaker, Side::Buy, kMid - kTick, 10));
            c.eng->process(cancel_order(id, 0, kTaker), c.sink);
            return t;
        });
    }
    {
        Case c;
        c.book(100, 10, 100, /*gap=*/11);
        run("new order, new best level", 200'000, clock, [&] {
            const OrderId id = c.next_id++;
            const Tsc t = c.timed(limit_order(id, 0, kTaker, Side::Buy, kMid, 10));
            c.eng->process(cancel_order(id, 0, kTaker), c.sink);
            return t;
        });
    }
    {
        Case c;
        c.book(100, 10, 100);
        run("cancel, from best level", 200'000, clock, [&] {
            const OrderId id = c.rest(Side::Buy, kMid - kTick, 10, kTaker);
            return c.timed(cancel_order(id, 0, kTaker));
        });
    }
    {
        Case c;
        c.book(100, 10, 100, /*gap=*/2);
        for (int k = 0; k < 1000; ++k) c.rest(Side::Sell, kMid + kTick, 1); // best ask: many 1-lots
        run("IOC take, 1 fill", 200'000, clock, [&] {
            const Tsc t = c.timed(limit_order(c.next_id++, 0, kTaker, Side::Buy, kMid + kTick, 1, Tif::IOC));
            c.rest(Side::Sell, kMid + kTick, 1); // replace the consumed maker at the tail
            return t;
        });
    }
    {
        Case c;
        c.book(100, 10, 100, /*gap=*/11);
        const auto refill = [&] {
            for (int l = 1; l <= 10; ++l) c.rest(Side::Sell, kMid + l * kTick, 1);
        };
        refill();
        run("market sweep, 10 levels", 200'000, clock, [&] {
            const Tsc t = c.timed(market_order(c.next_id++, 0, kTaker, Side::Buy, 10));
            refill();
            return t;
        });
    }
    {
        Case c;
        for (int l = 1; l <= 2047; ++l) c.rest(Side::Buy, kMid - l * kTick, 10);
        run("worst case: add far-end level", 100'000, clock, [&] {
            const OrderId id = c.next_id++;
            const Tsc t = c.timed(limit_order(id, 0, kTaker, Side::Buy, kMid - 2048 * kTick, 10));
            c.eng->process(cancel_order(id, 0, kTaker), c.sink);
            return t;
        });
        run("worst case: drop far-end level", 100'000, clock, [&] {
            const OrderId id = c.rest(Side::Buy, kMid - 2048 * kTick, 10, kTaker);
            return c.timed(cancel_order(id, 0, kTaker));
        });
        std::printf("(worst-case side: %u of %u levels)\n", c.eng->depth(0, Side::Buy) + 1,
                    c.eng->config().levels_per_side);
    }
    return 0;
}
