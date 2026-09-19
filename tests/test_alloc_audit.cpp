// tests/test_alloc_audit.cpp — proves the "zero allocation on the hot path"
// claim instead of asserting it. Global operator new is replaced with a counting
// version (sim/alloc_counter.hpp); everything is constructed first, then the
// counter is sampled around (1) 1.1M direct process() calls whose events are
// pushed through an SPSC ring and drained, and (2) the recorded window of the
// full three-thread pipeline. Both must perform exactly zero allocations.
#include "px/matching_engine.hpp"
#include "px/spsc_ring.hpp"
#include "sim/alloc_counter.hpp"
#include "sim/order_stream.hpp"
#include "sim/pipeline.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <memory>

using namespace px;

namespace {

std::uint64_t g_window_start = 0;
std::uint64_t g_window_end = 0;

struct DirectCtx {
    SpscRing<ExecEvent, 1024>* ring;
    std::uint64_t drained = 0;
};

} // namespace

int main() {
    // The counter itself must work, or "0 allocations" would prove nothing.
    {
        const std::uint64_t before = sim::g_allocations.load();
        auto plain = std::make_unique<int>(1);
        auto aligned = std::make_unique<Order>();
        PX_CHECK(sim::g_allocations.load() - before == 2);
    }

    sim::StreamConfig cfg;
    cfg.orders = 1'100'000;
    cfg.seed = 99;
    const auto stream = sim::generate_order_stream(cfg);

    // (1) Direct: engine + ring on one thread.
    {
        auto engine = std::make_unique<MatchingEngine>();
        sim::add_instruments(*engine, cfg);
        auto ring = std::make_unique<SpscRing<ExecEvent, 1024>>();
        DirectCtx ctx{ring.get()};
        const EventSink sink(&ctx, [](void* c, const ExecEvent& e) {
            auto* d = static_cast<DirectCtx*>(c);
            if (!d->ring->try_push(e)) {
                ExecEvent tmp;
                while (d->ring->try_pop(tmp)) ++d->drained;
                (void)d->ring->try_push(e);
            }
        });

        const std::uint64_t before = sim::g_allocations.load();
        for (const OrderRequest& r : stream) engine->process(r, sink);
        ExecEvent tmp;
        while (ring->try_pop(tmp)) ++ctx.drained;
        const std::uint64_t allocs = sim::g_allocations.load() - before;

        std::printf("direct: %llu allocations over %zu process() calls (%llu events through the ring)\n",
                    static_cast<unsigned long long>(allocs), stream.size(),
                    static_cast<unsigned long long>(ctx.drained));
        PX_CHECK(allocs == 0);
        PX_CHECK(ctx.drained > stream.size()); // every request emits at least one event
    }

    // (2) The threaded pipeline's recorded window, all three threads included.
    {
        auto engine = std::make_unique<MatchingEngine>();
        sim::add_instruments(*engine, cfg);
        sim::PipelineOptions opt;
        opt.warmup = 100'000;
        opt.on_window_start = [] { g_window_start = sim::g_allocations.load(); };
        opt.on_window_end = [] { g_window_end = sim::g_allocations.load(); };
        const sim::PipelineResult res = sim::run_pipeline(*engine, stream, opt);
        const std::uint64_t allocs = g_window_end - g_window_start;
        std::printf("pipeline: %llu allocations across all threads while %zu orders were recorded\n",
                    static_cast<unsigned long long>(allocs), res.recorded);
        PX_CHECK(res.recorded == 1'000'000);
        PX_CHECK(allocs == 0);
    }
    PX_TEST_REPORT_AND_RETURN();
}
