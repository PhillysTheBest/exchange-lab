// bench/bench_tick_to_trade.cpp — the headline measurement.
//
// Runs a seeded synthetic stream (100k warmup + 1M recorded orders by default)
// through the full three-thread pipeline (sim/pipeline.hpp) and reports the
// matching core's tick-to-trade latency — dequeue from the inbound ring to the
// last resulting event written to the outbound ring — for every recorded order:
// overall, per kind of work, the slowest individual orders, and a PASS/FAIL gate
// (default: p99.9 < 2000 ns, with zero outbound-ring stalls and zero heap
// allocations inside the recorded window). Nothing is discarded or trimmed.
//
//   bench_tick_to_trade [--orders=N] [--warmup=N] [--seed=S] [--instruments=K]
//                       [--gate-ns=NS] [--gate-pct=P] [--no-pin] [--csv=PATH]
#include "px/histogram.hpp"
#include "px/matching_engine.hpp"
#include "px/platform.hpp"
#include "px/tsc_clock.hpp"
#include "sim/alloc_counter.hpp"
#include "sim/order_stream.hpp"
#include "sim/pipeline.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <vector>

using namespace px;

namespace {

std::uint64_t g_allocs_at_start = 0;
std::uint64_t g_allocs_at_end = 0;

bool flag(const char* arg, const char* name, const char** value) {
    const std::size_t n = std::strlen(name);
    if (std::strncmp(arg, name, n) != 0 || arg[n] != '=') return false;
    *value = arg + n + 1;
    return true;
}

// The cost of the measurement itself: an empty tsc_begin()/tsc_end() bracket.
// Reported so readers can judge it; never subtracted from the results.
double timer_floor_ns(const TscClock& clock) {
    LatencyRecorder rec(200'000);
    for (int i = 0; i < 200'000; ++i) {
        const Tsc t0 = tsc_begin();
        const Tsc t1 = tsc_end();
        rec.record(t1 - t0);
    }
    return rec.percentile_ns(clock, 0.5);
}

} // namespace

int main(int argc, char** argv) {
    std::size_t orders = 1'000'000;
    std::size_t warmup = 100'000;
    std::uint64_t seed = 42;
    unsigned instruments = 4;
    double gate_ns = 2000.0;
    double gate_pct = 99.9;
    bool pin = true;
    const char* csv_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const char* v = nullptr;
        if (flag(argv[i], "--orders", &v)) orders = std::strtoull(v, nullptr, 10);
        else if (flag(argv[i], "--warmup", &v)) warmup = std::strtoull(v, nullptr, 10);
        else if (flag(argv[i], "--seed", &v)) seed = std::strtoull(v, nullptr, 10);
        else if (flag(argv[i], "--instruments", &v)) instruments = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        else if (flag(argv[i], "--gate-ns", &v)) gate_ns = std::strtod(v, nullptr);
        else if (flag(argv[i], "--gate-pct", &v)) gate_pct = std::strtod(v, nullptr);
        else if (flag(argv[i], "--csv", &v)) csv_path = v;
        else if (std::strcmp(argv[i], "--no-pin") == 0) pin = false;
        else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (orders == 0 || instruments == 0 || instruments > 64) {
        std::fprintf(stderr, "--orders must be > 0 and --instruments in 1..64\n");
        return 2;
    }

    harden_process();
    const TscClock clock;
    const double floor_ns = timer_floor_ns(clock);

    sim::StreamConfig scfg;
    scfg.seed = seed;
    scfg.orders = orders + warmup;
    scfg.instruments = static_cast<std::uint16_t>(instruments);
    sim::StreamMix mix;
    const auto stream = sim::generate_order_stream(scfg, &mix);

    auto engine = std::make_unique<MatchingEngine>();
    sim::add_instruments(*engine, scfg);

    sim::PipelineOptions opt;
    opt.warmup = warmup;
    opt.pin_threads = pin;
    opt.on_window_start = [] { g_allocs_at_start = sim::g_allocations.load(); };
    opt.on_window_end = [] { g_allocs_at_end = sim::g_allocations.load(); };
    const sim::PipelineResult res = sim::run_pipeline(*engine, stream, opt);
    const std::uint64_t window_allocs = g_allocs_at_end - g_allocs_at_start;

    // ---- report ---------------------------------------------------------------
    const auto pct = [&](std::size_t x) { return 100.0 * static_cast<double>(x) / static_cast<double>(stream.size()); };
    std::printf("machine    invariant TSC: %s, %.3f ticks/ns; timer floor (empty bracket, p50): %.0f ns, not subtracted\n",
                clock.invariant_tsc() ? "yes" : "NO", clock.ticks_per_ns(), floor_ns);
    std::printf("threads    gateway cpu %d, matcher cpu %d, publisher cpu %d%s\n", res.gateway_cpu, res.matcher_cpu,
                res.publisher_cpu, pin ? "" : " (pinning disabled)");
    std::printf("stream     seed %llu, %zu instruments, %zu requests = %zu warmup + %zu recorded\n",
                static_cast<unsigned long long>(seed), static_cast<std::size_t>(instruments), stream.size(), warmup,
                res.recorded);
    std::printf("mix        passive GTC %.1f%%, marketable GTC %.1f%%, marketable IOC %.1f%%, market %.1f%%, "
                "cancel %.1f%%, late cancel (order already filled) %.1f%%\n",
                pct(mix.passive), pct(mix.marketable_gtc), pct(mix.marketable_ioc), pct(mix.market), pct(mix.cancel),
                pct(mix.late_cancel));

    std::uint32_t max_depth = 0;
    for (InstrumentId i = 0; i < engine->instrument_count(); ++i) {
        max_depth = std::max({max_depth, engine->depth(i, Side::Buy), engine->depth(i, Side::Sell)});
    }
    std::printf("book end   %zu resting orders on %zu price levels, deepest side %u levels (capacity %u)\n",
                engine->resting_orders(), engine->live_levels(), max_depth, engine->config().levels_per_side);

    const double window_s = clock.to_ns(res.window_ticks) / 1e9;
    std::printf("throughput %.2f M orders/s through the matcher (%zu recorded orders in %.3f s, gateway unpaced)\n",
                static_cast<double>(res.recorded) / window_s / 1e6, res.recorded, window_s);

    std::printf("\ntick-to-trade: dequeue from the inbound ring -> last event written to the outbound ring\n");
    const Percentiles all = res.latency->summarize(clock);
    print_percentiles("all recorded orders", all);
    std::vector<std::vector<std::uint32_t>> by_class(sim::kOrderClasses);
    for (std::size_t i = 0; i < res.classes.size(); ++i) by_class[res.classes[i]].push_back(static_cast<std::uint32_t>(i));
    for (std::size_t c = 0; c < sim::kOrderClasses; ++c) {
        if (by_class[c].empty()) continue;
        char label[48];
        std::snprintf(label, sizeof label, "  %s", sim::kOrderClassNames[c]);
        print_percentiles(label, res.latency->summarize_indices(clock, by_class[c].data(), by_class[c].size()));
    }

    // The slowest individual orders, with what they were — the tail, unhidden.
    std::vector<std::uint32_t> order(res.recorded);
    std::iota(order.begin(), order.end(), 0u);
    const std::size_t worst = std::min<std::size_t>(5, order.size());
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(worst), order.end(),
                      [&](std::uint32_t a, std::uint32_t b) { return res.latency->at(a) > res.latency->at(b); });
    std::printf("slowest    ");
    for (std::size_t k = 0; k < worst; ++k) {
        std::printf("%s%.0f ns (%s)", k ? ", " : "", clock.to_ns(res.latency->at(order[k])),
                    sim::kOrderClassNames[res.classes[order[k]]]);
    }
    std::printf("\n");

    const std::size_t over = res.latency->count_above(clock.from_ns(gate_ns));
    std::printf("over gate  %zu of %zu orders took longer than %.0f ns (%.4f%%)\n", over, res.recorded, gate_ns,
                100.0 * static_cast<double>(over) / static_cast<double>(res.recorded));
    std::printf("hygiene    outbound-ring stalls: %llu (orders affected: %llu); heap allocations in recorded window: %llu\n",
                static_cast<unsigned long long>(res.outbound_stalls), static_cast<unsigned long long>(res.stalled_orders),
                static_cast<unsigned long long>(window_allocs));
    std::printf("output     %llu events, event_hash=%016llx\n", static_cast<unsigned long long>(res.events),
                static_cast<unsigned long long>(res.event_hash));

    const double at_gate = res.latency->percentile_ns(clock, gate_pct / 100.0);
    const bool pass = at_gate < gate_ns && res.outbound_stalls == 0 && window_allocs == 0;
    std::printf("\nRESULT: %s p%g=%.0f ns %s %.0f ns (N=%zu, stalls=%llu, allocations=%llu)\n", pass ? "PASS" : "FAIL",
                gate_pct, at_gate, at_gate < gate_ns ? "<" : ">=", gate_ns, res.recorded,
                static_cast<unsigned long long>(res.outbound_stalls), static_cast<unsigned long long>(window_allocs));

    if (csv_path) {
        std::FILE* f = open_csv_for_append(csv_path);
        char label[64];
        std::snprintf(label, sizeof label, "tick_to_trade_seed%llu", static_cast<unsigned long long>(seed));
        append_csv(f, label, all);
        if (f) std::fclose(f);
    }
    return pass ? 0 : 1;
}
