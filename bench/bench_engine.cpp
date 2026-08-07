// bench/bench_engine.cpp — pinned, warmed latency benchmarks for the matching
// path: resting a fresh order, a single full cross, a deep multi-level sweep,
// cancel, and sustained mixed-load throughput. Every result is a percentile
// distribution (p50/p90/p99/p99.9/p99.99/max), never a mean — see histogram.hpp
// for why. Build with `mingw32-make bench` (release: -O3 -march=native -flto).
#include "px/histogram.hpp"
#include "px/matching_engine.hpp"
#include "px/platform.hpp"
#include "px/tsc_clock.hpp"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace px;

namespace {

constexpr int kWarmup = 10'000;
constexpr int kIters  = 200'000;

void run_bench(const char* label, const TscClock& clock, LatencyHistogram& hist) {
    LatencyHistogram::Percentiles p = hist.summarize();
    (void)clock;
    LatencyHistogram::print(label, p);
}

// Benchmark 1: rest a fresh GTC limit order that never crosses anything. Each
// timed submit is immediately followed by an untimed cancel, so resting order
// count stays ~1 throughout instead of growing across 200k+ iterations.
void bench_rest_new_order(MatchingEngine& eng, MarketId m, TraderId trader, const TscClock& clock) {
    LatencyHistogram hist;
    hist.reserve(kIters);
    for (int i = 0; i < kWarmup + kIters; ++i) {
        const Tsc t0 = read_tscp();
        const auto r = eng.submit(m, trader, Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, kMaxPrice, 1);
        const Tsc t1 = read_tscp();
        if (i >= kWarmup) hist.record(clock.to_ns(t1 - t0));
        (void)eng.cancel(m, r.order_id); // untimed cleanup
    }
    run_bench("submit: rest new order", clock, hist);
}

// Benchmark 2: a single resting order fully crossed by an exactly-matching
// marketable order. Untimed setup rests the maker order; the timed call is the
// taker's submit(), which crosses, settles both accounts, and emits one Trade.
void bench_full_cross(MatchingEngine& eng, MarketId m, TraderId maker, TraderId taker, const TscClock& clock) {
    LatencyHistogram hist;
    hist.reserve(kIters);
    for (int i = 0; i < kWarmup + kIters; ++i) {
        (void)eng.submit(m, maker, Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, 50, 1);
        const Tsc t0 = read_tscp();
        const auto r = eng.submit(m, taker, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 50, 1);
        const Tsc t1 = read_tscp();
        if (i >= kWarmup) hist.record(clock.to_ns(t1 - t0));
        (void)r;
    }
    run_bench("submit: single full cross", clock, hist);
}

// Benchmark 3: a deep sweep — 50 resting one-lot orders spread across 50 price
// levels, taken out by a single MARKET order in one submit() call. Exercises
// the ladder/mask walk across many levels, not just one.
void bench_deep_sweep(MatchingEngine& eng, MarketId m, TraderId maker, TraderId taker, const TscClock& clock) {
    constexpr int kDepth = 50;
    LatencyHistogram hist;
    hist.reserve(kIters);
    for (int i = 0; i < kWarmup + kIters; ++i) {
        for (Price p = 1; p <= kDepth; ++p) {
            (void)eng.submit(m, maker, Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, p, 1);
        }
        const Tsc t0 = read_tscp();
        const auto r = eng.submit(m, taker, Contract::Yes, Side::Buy, OrderType::Market, TimeInForce::IOC, 0, kDepth);
        const Tsc t1 = read_tscp();
        if (i >= kWarmup) hist.record(clock.to_ns(t1 - t0));
        (void)r;
    }
    run_bench("submit: 50-level market sweep", clock, hist);
}

// Benchmark 4: cancel of a resting order.
void bench_cancel(MatchingEngine& eng, MarketId m, TraderId trader, const TscClock& clock) {
    LatencyHistogram hist;
    hist.reserve(kIters);
    for (int i = 0; i < kWarmup + kIters; ++i) {
        const auto r = eng.submit(m, trader, Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, kMaxPrice, 1);
        const Tsc t0 = read_tscp();
        const RejectReason rr = eng.cancel(m, r.order_id);
        const Tsc t1 = read_tscp();
        if (i >= kWarmup) hist.record(clock.to_ns(t1 - t0));
        (void)rr;
    }
    run_bench("cancel: resting order", clock, hist);
}

// Benchmark 5: sustained mixed load (70% rest, 20% cross, 10% cancel) — reports
// both the per-op percentile distribution and aggregate throughput.
void bench_mixed_throughput(MatchingEngine& eng, MarketId m, TraderId a, TraderId b, const TscClock& clock) {
    std::mt19937_64 rng(7);
    std::uniform_int_distribution<int> action(0, 9);
    std::uniform_int_distribution<int> price_dist(kMinPrice, kMaxPrice);
    std::vector<OrderId> live;
    live.reserve(4096);

    LatencyHistogram hist;
    hist.reserve(kIters);
    const Tsc wall_start = read_tscp();
    for (int i = 0; i < kWarmup + kIters; ++i) {
        const int roll = action(rng);
        const Tsc t0 = read_tscp();
        if (roll < 2 && !live.empty()) {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const std::size_t idx = pick(rng);
            (void)eng.cancel(m, live[idx]);
            live[idx] = live.back();
            live.pop_back();
        } else {
            const bool taker_side = roll >= 8; // aims at the opposite book's inside price
            const TraderId who = (roll % 2 == 0) ? a : b;
            const Side side = taker_side ? Side::Buy : Side::Sell;
            const Price price = taker_side ? kMaxPrice : static_cast<Price>(price_dist(rng));
            const auto r = eng.submit(m, who, Contract::Yes, side, OrderType::Limit, TimeInForce::GTC, price, 1);
            if (r.rested_qty > 0 && live.size() < 60000) live.push_back(r.order_id);
        }
        const Tsc t1 = read_tscp();
        if (i >= kWarmup) hist.record(clock.to_ns(t1 - t0));
    }
    const Tsc wall_end = read_tscp();

    run_bench("mixed load (rest/cross/cancel)", clock, hist);
    const double total_ns = clock.to_ns(wall_end - wall_start);
    const double ops_per_sec = static_cast<double>(kWarmup + kIters) / (total_ns / 1e9);
    std::printf("%-28s  %.0f ops/sec (%d ops in %.2f ms)\n",
                "  throughput", ops_per_sec, kWarmup + kIters, total_ns / 1e6);
}

} // namespace

int main() {
    const auto cores = performance_core_ids();
    bool pinned = false;
    if (!cores.empty()) pinned = pin_current_thread_to(cores.front());
    std::printf("pinned to logical core %s (of %zu candidate performance cores)\n",
                pinned ? std::to_string(cores.front()).c_str() : "none", cores.size());

    TscClock clock;
    std::printf("invariant TSC: %s, calibrated %.4f ticks/ns\n\n",
                clock.invariant_tsc() ? "yes" : "no (results may drift)", clock.ticks_per_ns());

    MatchingEngine eng;
    const MarketId m = eng.create_market("BENCH-MKT");
    eng.open_market(m);
    const TraderId maker = eng.create_trader(1'000'000'00);
    const TraderId taker = eng.create_trader(1'000'000'00);

    bench_rest_new_order(eng, m, maker, clock);
    bench_full_cross(eng, m, maker, taker, clock);
    bench_deep_sweep(eng, m, maker, taker, clock);
    bench_cancel(eng, m, maker, clock);
    bench_mixed_throughput(eng, m, maker, taker, clock);

    return 0;
}
