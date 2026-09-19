// sim/pipeline.hpp — the three-thread exchange pipeline and its measurement.
//
//   [gateway] --inbound SpscRing<OrderRequest>--> [matcher] --outbound SpscRing<ExecEvent>--> [publisher]
//
// gateway    pushes a pre-generated request stream as fast as the ring accepts it
//            (the matcher never waits: throughput-bound, caches stay hot), or,
//            with pace_orders_per_sec, on a fixed open-loop schedule (the matcher
//            idles between orders, so each one starts with colder caches).
// matcher    the ONLY thread that touches the MatchingEngine. Pops a request,
//            runs process(), and pushes every resulting event onto the outbound
//            ring from inside the EventSink.
// publisher  drains events and folds them into an EventHasher — standing in for
//            a market-data / drop-copy consumer, and proving the stream's content.
//
// WHAT IS TIMED (matching latency): on the matcher thread only, from the instant a
// request has been dequeued (tsc_begin() right after try_pop returns) to the
// instant its last resulting event has been written to the outbound ring
// (tsc_end() after process() returns). Both stamps come from the same core's TSC,
// so no cross-core clock comparison is involved. Time a request spends waiting in
// the inbound ring is NOT included: this is the engine's service time per order.
// If the outbound ring is ever full, the matcher spins inside the timed region;
// that is counted (outbound_stalls) rather than hidden, and a clean run has zero.
#pragma once

#include "px/histogram.hpp"
#include "px/matching_engine.hpp"
#include "px/messages.hpp"
#include "px/platform.hpp"
#include "px/spsc_ring.hpp"
#include "px/tsc_clock.hpp"

#include <x86intrin.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace px::sim {

using InboundRing = SpscRing<OrderRequest, std::size_t{1} << 16>;   // 4 MB
using OutboundRing = SpscRing<ExecEvent, std::size_t{1} << 17>;     // 8 MB

// What kind of work a request turned out to be — latency is reported per class,
// because a 12-maker sweep and a passive join are different amounts of work.
enum class OrderClass : std::uint8_t { Reject = 0, Cancel, Passive, Cross1, Cross2to4, Cross5plus };
inline constexpr std::size_t kOrderClasses = 6;
inline constexpr const char* kOrderClassNames[kOrderClasses] = {
    "reject", "cancel", "new, no fill", "new, 1 fill", "new, 2-4 fills", "new, 5+ fills"};

struct PipelineOptions {
    std::size_t warmup = 0; // leading requests that are processed but not recorded
    bool pin_threads = true;
    // Open-loop pacing: 0 = the gateway pushes as fast as the ring accepts.
    // Otherwise recorded requests are sent on a fixed schedule (warmup ones
    // unpaced), so the matcher idles between orders with cold caches, and a late
    // send never delays the schedule of the ones after it.
    double pace_orders_per_sec = 0;
    double ticks_per_ns = 0; // TSC rate, required when pacing
    // Keep each recorded order's dequeue timestamp (for outlier forensics).
    bool record_start_times = false;
    // Idle hiccup meter: while the matcher polls an empty inbound ring inside the
    // recorded window, a gap between consecutive polls longer than the threshold
    // means the core was taken away (interrupt, DPC, preemption). Up to
    // `hiccup_capacity` of them are kept.
    std::size_t hiccup_capacity = 0;
    Tsc hiccup_threshold_ticks = 0;
    // Called on the matcher thread at the start and end of the recorded window
    // (used by the benchmark to count heap allocations inside it).
    void (*on_window_start)() = nullptr;
    void (*on_window_end)() = nullptr;
};

struct PipelineResult {
    std::uint64_t event_hash = 0;
    std::uint64_t events = 0;
    std::uint64_t outbound_stalls = 0;       // spins on a full outbound ring, whole run
    std::uint64_t stalled_orders = 0;        // recorded orders during which that happened
    Tsc window_start = 0;                    // TSC at the first recorded dequeue
    Tsc window_ticks = 0;                    // first recorded dequeue -> last recorded order done
    std::size_t recorded = 0;
    std::array<std::uint64_t, kOrderClasses> class_counts{};
    int gateway_cpu = -1, matcher_cpu = -1, publisher_cpu = -1;
    std::unique_ptr<LatencyRecorder> latency; // one sample per recorded request, in stream order
    std::vector<std::uint8_t> classes;        // OrderClass per sample
    std::vector<Tsc> start_tsc;               // dequeue TSC per sample (if record_start_times)
    struct Hiccup {
        Tsc start;
        Tsc duration;
    };
    std::vector<Hiccup> hiccups;              // first hiccup_capacity idle gaps, in time order
    std::uint64_t hiccup_count = 0;           // all of them, including any past capacity
};

namespace detail {

struct MatcherCtx {
    OutboundRing* out = nullptr;
    std::uint64_t stalls = 0;
    std::uint32_t trades = 0;
    bool rejected = false;
};

inline void push_event(void* c, const ExecEvent& e) {
    auto* ctx = static_cast<MatcherCtx*>(c);
    ctx->trades += e.type == EventType::Trade;
    ctx->rejected |= e.type == EventType::Rejected;
    while (!ctx->out->try_push(e)) {
        ++ctx->stalls;
        cpu_relax();
    }
}

[[nodiscard]] inline OrderClass classify(const OrderRequest& req, const MatcherCtx& ctx) noexcept {
    if (ctx.rejected) return OrderClass::Reject;
    if (req.type == ReqType::Cancel) return OrderClass::Cancel;
    if (ctx.trades == 0) return OrderClass::Passive;
    if (ctx.trades == 1) return OrderClass::Cross1;
    if (ctx.trades <= 4) return OrderClass::Cross2to4;
    return OrderClass::Cross5plus;
}

} // namespace detail

// Runs `stream` through `engine` on the calling thread with no rings at all and
// returns the hash of the emitted event stream — the reference the threaded
// pipeline's output is compared against.
[[nodiscard]] inline EventHasher run_direct(MatchingEngine& engine, const std::vector<OrderRequest>& stream) {
    EventHasher h;
    const EventSink sink(&h, [](void* c, const ExecEvent& e) { static_cast<EventHasher*>(c)->add(e); });
    for (const OrderRequest& r : stream) engine.process(r, sink);
    return h;
}

[[nodiscard]] inline PipelineResult run_pipeline(MatchingEngine& engine, const std::vector<OrderRequest>& stream,
                                                 const PipelineOptions& opt = {}) {
    const std::size_t n = stream.size();
    const std::size_t warmup = opt.warmup < n ? opt.warmup : n;

    PipelineResult res;
    res.recorded = n - warmup;
    // Everything the measured threads touch is allocated (and zero-filled, which
    // pre-faults it) here, before any thread starts.
    res.latency = std::make_unique<LatencyRecorder>(res.recorded);
    res.classes.assign(res.recorded, 0);
    if (opt.record_start_times) res.start_tsc.assign(res.recorded, 0);
    res.hiccups.assign(opt.hiccup_capacity, {}); // filled, not reserved: a first touch would itself be a hiccup
    std::size_t hiccups_kept = 0;
    auto inbound = std::make_unique<InboundRing>();
    auto outbound = std::make_unique<OutboundRing>();

    // CPU0 services most interrupts on Windows, so prefer the higher P-cores and
    // give the matcher the last one.
    const std::vector<unsigned> cores = opt.pin_threads ? performance_core_ids() : std::vector<unsigned>{};
    if (cores.size() >= 4) {
        res.gateway_cpu = static_cast<int>(cores[1]);
        res.publisher_cpu = static_cast<int>(cores[2]);
        res.matcher_cpu = static_cast<int>(cores[3]);
    } else if (cores.size() == 3) {
        res.gateway_cpu = static_cast<int>(cores[0]);
        res.publisher_cpu = static_cast<int>(cores[1]);
        res.matcher_cpu = static_cast<int>(cores[2]);
    }

    std::atomic<int> ready{0};
    const auto wait_for_all = [&ready] {
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (ready.load(std::memory_order_acquire) < 3) cpu_relax();
    };

    EventHasher hasher;
    std::thread publisher([&] {
        if (res.publisher_cpu >= 0) pin_current_thread_to(static_cast<unsigned>(res.publisher_cpu));
        wait_for_all();
        for (;;) {
            ExecEvent e;
            if (!outbound->try_pop(e)) {
                cpu_relax();
                continue;
            }
            if (e.type == EventType::EndOfStream) break;
            hasher.add(e);
        }
    });

    std::thread gateway([&] {
        if (res.gateway_cpu >= 0) pin_current_thread_to(static_cast<unsigned>(res.gateway_cpu));
        wait_for_all();
        const bool paced = opt.pace_orders_per_sec > 0 && opt.ticks_per_ns > 0;
        const double interval = paced ? opt.ticks_per_ns * 1e9 / opt.pace_orders_per_sec : 0;
        Tsc schedule_start = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (paced && i >= warmup) {
                if (i == warmup) schedule_start = __rdtsc();
                const Tsc due = schedule_start + static_cast<Tsc>(static_cast<double>(i - warmup) * interval);
                while (__rdtsc() < due) cpu_relax();
            }
            while (!inbound->try_push(stream[i])) cpu_relax();
        }
    });

    std::thread matcher([&] {
        if (res.matcher_cpu >= 0) pin_current_thread_to(static_cast<unsigned>(res.matcher_cpu));
        detail::MatcherCtx ctx;
        ctx.out = outbound.get();
        const EventSink sink(&ctx, &detail::push_event);
        LatencyRecorder& lat = *res.latency;
        wait_for_all();

        Tsc window_start = 0;
        const bool meter = opt.hiccup_capacity > 0 && opt.hiccup_threshold_ticks > 0;
        for (std::size_t i = 0; i < n; ++i) {
            OrderRequest req;
            if (!inbound->try_pop(req)) {
                const bool in_window = meter && i > warmup;
                Tsc prev = __rdtsc();
                do {
                    cpu_relax();
                    if (in_window) {
                        const Tsc now = __rdtsc();
                        if (now - prev > opt.hiccup_threshold_ticks) {
                            if (hiccups_kept < opt.hiccup_capacity) res.hiccups[hiccups_kept++] = {prev, now - prev};
                            ++res.hiccup_count;
                        }
                        prev = now;
                    }
                } while (!inbound->try_pop(req));
            }
            if (i == warmup) {
                if (opt.on_window_start) opt.on_window_start();
                window_start = tsc_begin();
                res.window_start = window_start;
            }
            ctx.trades = 0;
            ctx.rejected = false;
            const std::uint64_t stalls_before = ctx.stalls;

            const Tsc t0 = tsc_begin();
            engine.process(req, sink);
            const Tsc t1 = tsc_end();

            if (i >= warmup) {
                lat.record(t1 - t0);
                if (opt.record_start_times) res.start_tsc[i - warmup] = t0;
                const OrderClass cls = detail::classify(req, ctx);
                res.classes[i - warmup] = static_cast<std::uint8_t>(cls);
                ++res.class_counts[static_cast<std::size_t>(cls)];
                res.stalled_orders += ctx.stalls != stalls_before;
            }
        }
        if (res.recorded > 0) {
            res.window_ticks = tsc_end() - window_start;
            if (opt.on_window_end) opt.on_window_end();
        }
        res.outbound_stalls = ctx.stalls;

        ExecEvent eos;
        eos.type = EventType::EndOfStream;
        while (!outbound->try_push(eos)) cpu_relax();
    });

    gateway.join();
    matcher.join();
    publisher.join();
    res.hiccups.resize(hiccups_kept);
    res.event_hash = hasher.value();
    res.events = hasher.count();
    return res;
}

} // namespace px::sim
