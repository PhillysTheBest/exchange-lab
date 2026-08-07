// bench/bench_ring.cpp — correctness + latency for the SPSC ring buffer under
// real cross-core traffic. Two threads, pinned to distinct performance cores
// where the platform reports them: a producer stamping each message with a TSC
// read, a consumer computing (receive_ts - send_ts) into a histogram. A
// same-thread round-trip benchmark runs alongside it, so the two numbers
// together separate "cost of the ring's own bookkeeping" from "cost of actually
// bouncing a cache line between two cores" — the real payoff of measuring
// instead of asserting a latency number.
#include "px/histogram.hpp"
#include "px/platform.hpp"
#include "px/spsc_ring.hpp"
#include "px/tsc_clock.hpp"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <thread>

using namespace px;

namespace {

struct Msg {
    std::uint64_t seq;
    Tsc send_ts;
};

constexpr std::size_t kRingCapacity = 1u << 14; // 16384
constexpr std::uint64_t kWarmup = 20'000;
constexpr std::uint64_t kIters  = 500'000;

// IMPORTANT METHODOLOGY NOTE, kept rather than silently "fixed away": an earlier
// version of this benchmark let the producer push as fast as it could with no
// pacing. With a 16384-deep ring and a consumer doing strictly more work per
// message (a TSC read, a division, a histogram push) than the producer (a TSC
// read, a store), the ring saturates near-full almost immediately and stays
// there — so (recv_ts - send_ts) mostly measures *queueing delay* (Little's-law
// backlog dwell time), not the cost of one core-to-core handoff. It produced a
// tight, plausible-looking distribution around ~2.6ms that was simply wrong for
// what it claimed to measure. This version uses a ping-pong protocol over two
// rings — the producer sends one message and busy-waits for the consumer's ack
// before sending the next — so exactly one exchange is ever in flight and the
// timed interval is the round trip, nothing else. Reported as a round trip
// (not divided by two into an assumed-symmetric one-way number) precisely to
// avoid smuggling an unverified assumption into the result.
void bench_cross_core(const TscClock& clock) {
    SpscRing<Msg, kRingCapacity> to_consumer;
    SpscRing<std::uint64_t, kRingCapacity> to_producer;

    const auto cores = performance_core_ids();
    const bool have_two_cores = cores.size() >= 2;
    if (!have_two_cores) {
        std::printf("(only %zu performance core id(s) available — running unpinned)\n", cores.size());
    }

    LatencyHistogram hist;
    hist.reserve(kIters);
    std::atomic<bool> order_violation{false};

    std::thread consumer([&] {
        if (have_two_cores) pin_current_thread_to(cores[1]);
        std::uint64_t expected = 0;
        for (std::uint64_t i = 0; i < kWarmup + kIters; ++i) {
            Msg m;
            while (!to_consumer.try_pop(m)) cpu_relax();
            if (m.seq != expected) order_violation.store(true, std::memory_order_relaxed);
            expected = m.seq + 1;
            while (!to_producer.try_push(m.seq)) cpu_relax(); // ack
        }
    });

    if (have_two_cores) pin_current_thread_to(cores[0]);
    for (std::uint64_t i = 0; i < kWarmup + kIters; ++i) {
        const Tsc t0 = read_tscp();
        const Msg m{i, t0};
        while (!to_consumer.try_push(m)) cpu_relax();

        std::uint64_t ack;
        while (!to_producer.try_pop(ack)) cpu_relax();
        const Tsc t1 = read_tscp();

        if (i >= kWarmup) hist.record(clock.to_ns(t1 - t0));
    }

    consumer.join();

    std::printf("order preserved: %s\n", order_violation.load() ? "NO -- BUG" : "yes");
    std::printf("rings empty after run: %s\n",
                (to_consumer.size_approx() == 0 && to_producer.size_approx() == 0) ? "yes" : "no");

    LatencyHistogram::print("cross-core round trip (ping-pong)", hist.summarize());
}

void bench_same_thread_roundtrip(const TscClock& clock) {
    SpscRing<Msg, kRingCapacity> ring;
    LatencyHistogram hist;
    hist.reserve(kIters);

    for (std::uint64_t i = 0; i < kWarmup + kIters; ++i) {
        const Tsc t0 = read_tscp();
        Msg m{i, t0};
        (void)ring.try_push(m); // ring is drained every iteration, so this never fails
        Msg out;
        (void)ring.try_pop(out);
        const Tsc t1 = read_tscp();
        if (i >= kWarmup) hist.record(clock.to_ns(t1 - t0));
    }

    LatencyHistogram::print("same-thread push+pop (no core hop)", hist.summarize());
}

} // namespace

int main() {
    TscClock clock;
    std::printf("invariant TSC: %s, calibrated %.4f ticks/ns\n\n",
                clock.invariant_tsc() ? "yes" : "no (results may drift)", clock.ticks_per_ns());

    bench_same_thread_roundtrip(clock);
    std::printf("\n");
    bench_cross_core(clock);
    return 0;
}
