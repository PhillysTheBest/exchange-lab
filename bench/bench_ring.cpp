// bench/bench_ring.cpp — what one hop through the SPSC ring costs, separated
// into the ring's own bookkeeping (push+pop on one thread) and the cost of
// actually moving a cache line between two cores (a cross-core ping-pong).
#include "px/histogram.hpp"
#include "px/platform.hpp"
#include "px/spsc_ring.hpp"
#include "px/tsc_clock.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

using namespace px;

namespace {

struct Msg {
    std::uint64_t seq;
    Tsc send_ts;
};

constexpr std::size_t kRingCapacity = 1u << 14;
constexpr std::uint64_t kWarmup = 20'000;
constexpr std::uint64_t kIters = 500'000;

// METHODOLOGY NOTE, kept on purpose: an earlier version let the producer push
// unpaced. With a 16k-deep ring and a consumer doing slightly more work per
// message than the producer, the ring saturated almost immediately and stayed
// full, so (recv_ts - send_ts) measured queueing delay — Little's-law backlog —
// not the cost of a handoff. It produced a tight, plausible-looking distribution
// around 2.6 ms that was simply the wrong quantity. This version ping-pongs over
// two rings, so exactly one message is ever in flight and the timed interval is
// one round trip. It is reported as a round trip, not halved into an assumed-
// symmetric one-way figure.
void bench_cross_core(const TscClock& clock) {
    auto to_consumer = std::make_unique<SpscRing<Msg, kRingCapacity>>();
    auto to_producer = std::make_unique<SpscRing<std::uint64_t, kRingCapacity>>();

    const auto cores = performance_core_ids();
    const bool pinned = cores.size() >= 3;
    if (!pinned) std::printf("(fewer than 3 performance cores reported: running unpinned)\n");

    LatencyRecorder rec(kIters);
    std::atomic<bool> order_violation{false};

    std::thread consumer([&] {
        if (pinned) pin_current_thread_to(cores[2]);
        std::uint64_t expected = 0;
        for (std::uint64_t i = 0; i < kWarmup + kIters; ++i) {
            Msg m;
            while (!to_consumer->try_pop(m)) cpu_relax();
            if (m.seq != expected) order_violation.store(true, std::memory_order_relaxed);
            expected = m.seq + 1;
            while (!to_producer->try_push(m.seq)) cpu_relax();
        }
    });

    if (pinned) pin_current_thread_to(cores[1]);
    for (std::uint64_t i = 0; i < kWarmup + kIters; ++i) {
        const Tsc t0 = tsc_begin();
        while (!to_consumer->try_push(Msg{i, t0})) cpu_relax();
        std::uint64_t ack;
        while (!to_producer->try_pop(ack)) cpu_relax();
        const Tsc t1 = tsc_end();
        if (i >= kWarmup) rec.record(t1 - t0);
    }
    consumer.join();

    std::printf("order preserved: %s\n", order_violation.load() ? "NO -- BUG" : "yes");
    print_percentiles("cross-core round trip", rec.summarize(clock));
}

void bench_same_thread(const TscClock& clock) {
    auto ring = std::make_unique<SpscRing<Msg, kRingCapacity>>();
    LatencyRecorder rec(kIters);
    for (std::uint64_t i = 0; i < kWarmup + kIters; ++i) {
        const Tsc t0 = tsc_begin();
        (void)ring->try_push(Msg{i, t0}); // drained every iteration: never full
        Msg out;
        (void)ring->try_pop(out);
        const Tsc t1 = tsc_end();
        if (i >= kWarmup) rec.record(t1 - t0);
    }
    print_percentiles("same-thread push+pop", rec.summarize(clock));
}

} // namespace

int main() {
    harden_process();
    const TscClock clock;
    std::printf("invariant TSC: %s, %.3f ticks/ns\n", clock.invariant_tsc() ? "yes" : "no", clock.ticks_per_ns());
    bench_same_thread(clock);
    bench_cross_core(clock);
    return 0;
}
