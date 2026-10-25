// bench/bench_matching_latency.cpp — the headline measurement.
//
// MATCHING LATENCY, as measured here: on the matching thread, from the instant a
// request has been dequeued from the inbound ring to the instant its last
// resulting event has been written to the outbound ring (sim/pipeline.hpp).
// This is the engine's service time per order. It is NOT wire-to-wire
// "tick-to-trade": no network, no parsing, and time spent queued in the inbound
// ring is excluded by definition. The 32-ish ns cost of the timestamps
// themselves is measured and printed, never subtracted.
//
// Fixed claims, checked the same way on every run (no tunable percentile):
//   PASS  p99.9 < 2000 ns, zero outbound-ring stalls, zero heap allocations in
//         the recorded window. The count above 2000 ns and the max are always
//         printed next to it.
//   With --repeat=K the same stream is replayed K times on fresh engines; every
//   replay must emit a bit-identical event stream, and each order's best-of-K
//   latency (timing noise only ever adds time) is reported beside the raw runs.
//
// Profiles:
//   thin  (default) passive orders mostly within 20 ticks of a drifting mid
//   deep  half the passive orders spread up to 1500 ticks away, 60k resting
//         orders: sides reach well over a thousand levels, so the level index's
//         long shifts run inside the measured window
//
//   bench_matching_latency [--profile=thin|deep] [--seed=S] [--orders=N]
//        [--warmup=N] [--instruments=K] [--repeat=K] [--rate=ORDERS_PER_SEC]
//        [--outliers=PATH] [--csv=PATH] [--no-pin]
#include "px/histogram.hpp"
#include "px/matching_engine.hpp"
#include "px/platform.hpp"
#include "px/cycle_clock.hpp"
#include "sim/alloc_counter.hpp"
#include "sim/order_stream.hpp"
#include "sim/pipeline.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

using namespace px;

namespace {

constexpr double kGateNs = 2000.0;
constexpr double kGatePct = 99.9;
constexpr double kHiccupNs = 1000.0; // an idle poll-to-poll gap this long means the core was taken away

std::uint64_t g_allocs_at_start = 0;
std::uint64_t g_allocs_at_end = 0;

bool flag(const char* arg, const char* name, const char** value) {
    const std::size_t n = std::strlen(name);
    if (std::strncmp(arg, name, n) != 0 || arg[n] != '=') return false;
    *value = arg + n + 1;
    return true;
}

double timer_floor_ns(const TscClock& clock) {
    LatencyRecorder rec(200'000);
    for (int i = 0; i < 200'000; ++i) {
        const Tsc t0 = tsc_begin();
        const Tsc t1 = tsc_end();
        rec.record(t1 - t0);
    }
    return rec.percentile_ns(clock, 0.5);
}

double median_of(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

struct RunSummary {
    Percentiles all;
    double at_gate = 0;
    std::size_t over_gate = 0;
    std::uint64_t stalls = 0;
    std::uint64_t allocs = 0;
    std::uint64_t hash = 0;
    std::uint64_t events = 0;
    double mops = 0;
    bool pass = false;
};

} // namespace

int main(int argc, char** argv) {
    std::string profile = "thin";
    std::size_t orders = 1'000'000;
    std::size_t warmup = 0; // 0 = profile default
    std::uint64_t seed = 42;
    unsigned instruments = 4;
    unsigned repeat = 1;
    double rate = 0;
    bool pin = true;
    const char* csv_path = nullptr;
    const char* outliers_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const char* v = nullptr;
        if (flag(argv[i], "--profile", &v)) profile = v;
        else if (flag(argv[i], "--orders", &v)) orders = std::strtoull(v, nullptr, 10);
        else if (flag(argv[i], "--warmup", &v)) warmup = std::strtoull(v, nullptr, 10);
        else if (flag(argv[i], "--seed", &v)) seed = std::strtoull(v, nullptr, 10);
        else if (flag(argv[i], "--instruments", &v)) instruments = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        else if (flag(argv[i], "--repeat", &v)) repeat = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        else if (flag(argv[i], "--rate", &v)) rate = std::strtod(v, nullptr);
        else if (flag(argv[i], "--csv", &v)) csv_path = v;
        else if (flag(argv[i], "--outliers", &v)) outliers_path = v;
        else if (std::strcmp(argv[i], "--no-pin") == 0) pin = false;
        else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (orders == 0 || instruments == 0 || instruments > 64 || repeat == 0 || rate < 0 ||
        (profile != "thin" && profile != "deep")) {
        std::fprintf(stderr, "bad arguments (profile thin|deep, orders > 0, instruments 1..64, repeat >= 1)\n");
        return 2;
    }

    sim::StreamConfig scfg;
    scfg.seed = seed;
    scfg.instruments = static_cast<std::uint16_t>(instruments);
    if (profile == "deep") {
        scfg = sim::deep_profile(scfg);
        if (warmup == 0) warmup = 300'000;
    } else if (warmup == 0) {
        warmup = 100'000;
    }
    scfg.orders = orders + warmup;

    harden_process();
    const TscClock clock;
    const double floor_ns = timer_floor_ns(clock);

    sim::StreamMix mix;
    const auto stream = sim::generate_order_stream(scfg, &mix);

    std::printf("MATCHING LATENCY: dequeue from the inbound ring -> last event written to the outbound ring\n");
    std::printf("machine    counter %s, constant rate: %s, %.3f ticks/ns, resolution %.2f ns; timer floor %.0f ns "
                "(p50 of an empty bracket), not subtracted\n",
                kCounterName, clock.invariant_tsc() ? "yes" : "NO", clock.ticks_per_ns(), clock.resolution_ns(), floor_ns);
    const auto pct = [&](std::size_t x) { return 100.0 * static_cast<double>(x) / static_cast<double>(stream.size()); };
    std::printf("stream     profile %s, seed %llu, %u instruments, %zu requests = %zu warmup + %zu recorded, %s\n",
                profile.c_str(), static_cast<unsigned long long>(seed), instruments, stream.size(), warmup, orders,
                rate > 0 ? "paced" : "gateway unpaced");
    if (rate > 0) std::printf("pacing     open loop, recorded orders sent on a fixed schedule of %.0f orders/s\n", rate);
    std::printf("mix        passive GTC %.1f%%, marketable GTC %.1f%%, marketable IOC %.1f%%, market %.1f%%, "
                "cancel %.1f%%, late cancel (order already filled) %.1f%%\n",
                pct(mix.passive), pct(mix.marketable_gtc), pct(mix.marketable_ioc), pct(mix.market), pct(mix.cancel),
                pct(mix.late_cancel));

    std::vector<std::uint32_t> best_of(orders, std::numeric_limits<std::uint32_t>::max()); // per-order best-of-K ticks
    std::vector<RunSummary> runs;
    std::vector<std::uint8_t> classes;
    std::unique_ptr<MatchingEngine> last_engine;
    std::FILE* outliers = nullptr;
    if (outliers_path) {
        outliers = std::fopen(outliers_path, "w");
        if (outliers) std::fprintf(outliers, "run,index,time_ms,class,latency_ns\n");
    }
    std::FILE* csv = nullptr;
    if (csv_path) {
        csv = std::fopen(csv_path, "a");
        if (csv) {
            std::fseek(csv, 0, SEEK_END);
            if (std::ftell(csv) == 0) {
                std::fprintf(csv, "profile,seed,rate,run,n,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns,over_2us,"
                                  "stalls,allocs,mops,hiccups,deepest_side,resting,event_hash\n");
            }
        }
    }

    for (unsigned run = 0; run < repeat; ++run) {
        auto engine = std::make_unique<MatchingEngine>();
        sim::add_instruments(*engine, scfg);

        sim::PipelineOptions opt;
        opt.warmup = warmup;
        opt.pin_threads = pin;
        opt.pace_orders_per_sec = rate;
        opt.ticks_per_ns = clock.ticks_per_ns();
        opt.record_start_times = true;
        opt.hiccup_capacity = 1'000'000;
        opt.hiccup_threshold_ticks = clock.from_ns(kHiccupNs);
        opt.on_window_start = [] { g_allocs_at_start = sim::g_allocations.load(); };
        opt.on_window_end = [] { g_allocs_at_end = sim::g_allocations.load(); };
        const sim::PipelineResult res = sim::run_pipeline(*engine, stream, opt);

        RunSummary s;
        s.all = res.latency->summarize(clock);
        s.at_gate = res.latency->percentile_ns(clock, kGatePct / 100.0);
        s.over_gate = res.latency->count_above(clock.from_ns(kGateNs));
        s.stalls = res.outbound_stalls;
        s.allocs = g_allocs_at_end - g_allocs_at_start;
        s.hash = res.event_hash;
        s.events = res.events;
        s.mops = static_cast<double>(res.recorded) / (clock.to_ns(res.window_ticks) / 1e9) / 1e6;
        s.pass = s.at_gate < kGateNs && s.stalls == 0 && s.allocs == 0;
        runs.push_back(s);
        for (std::size_t i = 0; i < res.recorded; ++i) best_of[i] = std::min(best_of[i], res.latency->at(i));

        std::uint32_t deepest = 0;
        for (InstrumentId i = 0; i < engine->instrument_count(); ++i) {
            deepest = std::max({deepest, engine->depth(i, Side::Buy), engine->depth(i, Side::Sell)});
        }

        if (run == 0) {
            classes = res.classes;
            std::printf("threads    gateway cpu %d, matcher cpu %d, publisher cpu %d%s\n", res.gateway_cpu,
                        res.matcher_cpu, res.publisher_cpu, pin ? "" : " (pinning disabled)");
            std::printf("affinity   gateway %s, matcher %s, publisher %s\n", affinity_name(res.gateway_affinity),
                        affinity_name(res.matcher_affinity), affinity_name(res.publisher_affinity));
            std::printf("book       at the end: %zu resting orders on %zu levels, deepest side %u of %u levels\n",
                        engine->resting_orders(), engine->live_levels(), deepest, engine->config().levels_per_side);
            std::printf("\nrun 1 of %u, every recorded order, by kind of work:\n", repeat);
            print_percentiles("all recorded orders", s.all);
            std::vector<std::vector<std::uint32_t>> by_class(sim::kOrderClasses);
            for (std::size_t i = 0; i < res.classes.size(); ++i) {
                by_class[res.classes[i]].push_back(static_cast<std::uint32_t>(i));
            }
            for (std::size_t c = 0; c < sim::kOrderClasses; ++c) {
                if (by_class[c].empty()) continue;
                char label[48];
                std::snprintf(label, sizeof label, "  %s", sim::kOrderClassNames[c]);
                print_percentiles(label, res.latency->summarize_indices(clock, by_class[c].data(), by_class[c].size()));
            }
        }

        // ---- outlier forensics: where in time do the slow orders fall? ----
        std::vector<double> outlier_ms;
        for (std::size_t i = 0; i < res.recorded; ++i) {
            if (res.latency->at(i) <= clock.from_ns(kGateNs)) continue;
            const double t_ms = clock.to_ns(res.start_tsc[i] - res.window_start) / 1e6;
            outlier_ms.push_back(t_ms);
            if (outliers) {
                std::fprintf(outliers, "%u,%zu,%.4f,\"%s\",%.0f\n", run + 1, i, t_ms, sim::kOrderClassNames[res.classes[i]],
                             clock.to_ns(res.latency->at(i)));
            }
        }
        std::vector<double> gaps;
        for (std::size_t k = 1; k < outlier_ms.size(); ++k) gaps.push_back(outlier_ms[k] - outlier_ms[k - 1]);
        // How many slow orders were dequeued within 50 us after the matcher core
        // lost at least 1 us while idle? (Hiccups are only observable while
        // idle, so this is meaningful for paced runs.)
        std::size_t after_hiccup = 0;
        {
            std::size_t h = 0;
            const Tsc window = clock.from_ns(50'000);
            for (std::size_t i = 0; i < res.recorded; ++i) {
                if (res.latency->at(i) <= clock.from_ns(kGateNs)) continue;
                const Tsc t = res.start_tsc[i];
                while (h + 1 < res.hiccups.size() && res.hiccups[h + 1].start + res.hiccups[h + 1].duration <= t) ++h;
                if (!res.hiccups.empty()) {
                    const Tsc end = res.hiccups[h].start + res.hiccups[h].duration;
                    after_hiccup += end <= t && t - end <= window;
                }
            }
        }
        std::vector<double> hiccup_gaps, hiccup_lens;
        for (std::size_t k = 0; k < res.hiccups.size(); ++k) {
            hiccup_lens.push_back(clock.to_ns(res.hiccups[k].duration) / 1e3);
            if (k > 0) hiccup_gaps.push_back(clock.to_ns(res.hiccups[k].start - res.hiccups[k - 1].start) / 1e6);
        }
        const double window_s = clock.to_ns(res.window_ticks) / 1e9;
        std::printf("run %u      p50 %.0f  p99 %.0f  p99.9 %.0f  p99.99 %.0f  max %.0f ns | %zu over 2 us (%.1f/s, median gap "
                    "%.2f ms) | idle hiccups >1 us: %llu (%.0f/s, median %.1f us, median gap %.2f ms) | %.2f M orders/s | "
                    "hash %016llx\n",
                    run + 1, s.all.p50, s.all.p99, s.all.p999, s.all.p9999, s.all.max, s.over_gate,
                    static_cast<double>(s.over_gate) / window_s, median_of(gaps),
                    static_cast<unsigned long long>(res.hiccup_count), static_cast<double>(res.hiccup_count) / window_s,
                    median_of(hiccup_lens), median_of(hiccup_gaps), s.mops, static_cast<unsigned long long>(s.hash));
        if (res.hiccup_count > 0) {
            std::printf("           %zu of the %zu orders over 2 us were dequeued within 50 us after an idle hiccup\n",
                        after_hiccup, s.over_gate);
        }

        if (csv) {
            std::fprintf(csv, "%s,%llu,%.0f,%u,%zu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%zu,%llu,%llu,%.3f,%llu,%u,%zu,%016llx\n",
                         profile.c_str(), static_cast<unsigned long long>(seed), rate, run + 1, s.all.n, s.all.p50,
                         s.all.p90, s.all.p99, s.all.p999, s.all.p9999, s.all.max, s.over_gate,
                         static_cast<unsigned long long>(s.stalls), static_cast<unsigned long long>(s.allocs), s.mops,
                         static_cast<unsigned long long>(res.hiccup_count), deepest, engine->resting_orders(),
                         static_cast<unsigned long long>(s.hash));
        }
        last_engine = std::move(engine);
    }
    if (outliers) std::fclose(outliers);
    if (csv) std::fclose(csv);

    // ---- replay determinism and best-of-K ----
    bool identical = true;
    for (const RunSummary& r : runs) identical &= r.hash == runs.front().hash && r.events == runs.front().events;
    std::size_t slow_every_time = 0;
    std::uint32_t worst_best = 0;
    std::size_t worst_best_index = 0;
    for (std::size_t i = 0; i < best_of.size(); ++i) {
        if (best_of[i] > worst_best) {
            worst_best = best_of[i];
            worst_best_index = i;
        }
        slow_every_time += best_of[i] > clock.from_ns(kGateNs);
    }
    if (repeat > 1) {
        LatencyRecorder best(best_of.size());
        for (const std::uint32_t t : best_of) best.record(t);
        std::printf("\nreplays    %u runs of the same stream on fresh engines: event streams %s (hash %016llx, %llu events)\n",
                    repeat, identical ? "bit-identical" : "DIFFER", static_cast<unsigned long long>(runs.front().hash),
                    static_cast<unsigned long long>(runs.front().events));
        print_percentiles("best of K, per order", best.summarize(clock));
        std::printf("best of K  slowest order: %.0f ns (#%zu, %s); orders over 2 us in every one of the %u runs: %zu\n",
                    clock.to_ns(worst_best), worst_best_index, sim::kOrderClassNames[classes[worst_best_index]], repeat,
                    slow_every_time);
    }

    bool all_pass = identical;
    for (const RunSummary& r : runs) all_pass &= r.pass;
    const RunSummary& w = *std::max_element(runs.begin(), runs.end(),
                                            [](const RunSummary& a, const RunSummary& b) { return a.at_gate < b.at_gate; });
    std::size_t over_total = 0;
    std::uint64_t stalls_total = 0, allocs_total = 0;
    double worst_max = 0;
    for (const RunSummary& r : runs) {
        over_total += r.over_gate;
        stalls_total += r.stalls;
        allocs_total += r.allocs;
        worst_max = std::max(worst_max, r.all.max);
    }
    std::printf("\nRESULT: %s  worst run p99.9=%.0f ns (gate < %.0f ns), %zu of %zu orders over 2 us across %u run(s), "
                "worst single order %.0f ns, stalls=%llu, allocations=%llu%s\n",
                all_pass ? "PASS" : "FAIL", w.at_gate, kGateNs, over_total, orders * repeat, repeat, worst_max,
                static_cast<unsigned long long>(stalls_total), static_cast<unsigned long long>(allocs_total),
                repeat > 1 ? (identical ? ", replays bit-identical" : ", REPLAYS DIFFER") : "");
    return all_pass ? 0 : 1;
}
