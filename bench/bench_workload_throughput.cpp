// bench/bench_workload_throughput.cpp — Stage 5, Sections 4/5/6: throughput/
// saturation, backpressure, and multi-market scaling, all driven by the same
// CLI-configurable tool against a real, closed-loop pipeline.
//
//   producer (this thread, paced) --wire JSON--> parse_feed_message
//     -> Ingress::handle_message -> ring A -> [real Brain thread: ShadowBook,
//     extract_features, PassiveMarketMaker::on_event, RiskEngine::check]
//     -> ring B -> [real Gateway thread: SimulatedExchangeSink stands in for
//     Kalshi's REST responses, same pattern tests/test_pipeline_e2e.cpp uses]
//     -> ring C -> back to Brain (fills/acks/cancels close the loop, so a
//     registered PassiveMarketMaker actually cycles Quoting/Position/Exiting
//     under load, not just sitting inert)
//
// Deliberately NOT the ping-pong one-event-at-a-time discipline
// bench_ring.cpp/bench_pipeline_e2e.cpp use — see workload/latency_watcher.hpp's
// header comment for why throughput/saturation/backpressure measurement needs
// the opposite regime (many events in flight, by design) and how latency is
// still measured honestly under it.
//
// USAGE:
//   bench_workload_throughput --rate=100000 --duration-sec=2 --markets=10
//     --profile=normal --market-dist=skewed --seed=42 --csv=perf/results/x.csv
//     --label=throughput_sweep
//
// --profile=adversarial is deliberately rejected here — see
// workload/generator.hpp's header comment: adversarial frames are sometimes
// dropped by design (seq gaps/duplicates/stale replays), which breaks this
// tool's 1:1 frame-sent -> book-event-applied assumption used for latency
// attribution. Adversarial correctness is tests/test_reconnect_recovery.cpp
// and tests/test_workload_generator.cpp's job, not this performance tool's.
#include "live/kalshi_gateway.hpp"
#include "pipeline/brain.hpp"
#include "pipeline/ingress.hpp"
#include "px/histogram.hpp"
#include "px/platform.hpp"
#include "px/tsc_clock.hpp"
#include "risk_engine.hpp"
#include "strategy/passive_market_maker.hpp"
#include "ticker_registry.hpp"
#include "workload/generator.hpp"
#include "workload/latency_watcher.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace px;
using namespace px::live;
using namespace px::pipeline;
using namespace px::strategy;
using namespace px::workload;

namespace {

// ---- SimulatedExchangeSink -------------------------------------------------
// Identical role to tests/test_pipeline_e2e.cpp's own (independently
// hand-rolled, per this codebase's established per-file convention — see
// that file's header comment) — stands in for Kalshi's REST responses so a
// real Gateway/Brain closed loop can run with no socket, no signing (the
// wire/signing path is proven elsewhere: bench_sign, test_gateway_dry_run).
class SimulatedExchangeSink final : public Sink {
public:
    explicit SimulatedExchangeSink(Qty qty_filled_at_entry) : qty_filled_at_entry_(qty_filled_at_entry) {}

    bool place(const OrderRequest& req, bool& did_send, std::string& response, std::string&) override {
        did_send = true;
        ++place_call_count;
        const std::string uuid = "sim-uuid-" + std::to_string(place_call_count);
        const Qty fill = (qty_filled_at_entry_ < req.qty) ? qty_filled_at_entry_ : req.qty;
        const Qty remaining = req.qty - fill;
        last_remaining_ = remaining;
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "HTTP/1.1 201 Created\r\n\r\n"
                      "{\"order_id\":\"%s\",\"client_order_id\":\"px-sim\","
                      "\"fill_count\":\"%u.00\",\"remaining_count\":\"%u.00\","
                      "\"average_fill_price\":\"%.4f\",\"ts_ms\":1}",
                      uuid.c_str(), fill, remaining, static_cast<double>(req.price) / 100.0);
        response = buf;
        return true;
    }

    bool cancel(const std::string& kalshi_order_id, bool& did_send, std::string& response, std::string&) override {
        did_send = true;
        ++cancel_call_count;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "HTTP/1.1 200 OK\r\n\r\n"
                      "{\"order_id\":\"%s\",\"client_order_id\":\"px-sim\",\"reduced_by\":\"%u.00\",\"ts_ms\":2}",
                      kalshi_order_id.c_str(), last_remaining_);
        response = buf;
        return true;
    }

    int place_call_count = 0;
    int cancel_call_count = 0;

private:
    Qty qty_filled_at_entry_;
    Qty last_remaining_ = 0;
};

struct Args {
    double rate_per_sec = 10'000.0;
    double duration_sec = 2.0;
    std::size_t markets = 1;
    Profile profile = Profile::Normal;
    MarketDist market_dist = MarketDist::Skewed;
    std::uint64_t seed = 42;
    std::string csv_path;
    std::string label = "run";
};

[[nodiscard]] bool starts_with(const char* s, const char* prefix) noexcept {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

[[nodiscard]] bool parse_args(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (starts_with(a, "--rate=")) {
            out.rate_per_sec = std::atof(a + 7);
        } else if (starts_with(a, "--duration-sec=")) {
            out.duration_sec = std::atof(a + 15);
        } else if (starts_with(a, "--markets=")) {
            out.markets = static_cast<std::size_t>(std::atoll(a + 10));
        } else if (starts_with(a, "--profile=")) {
            const std::string v = a + 10;
            if (v == "normal") out.profile = Profile::Normal;
            else if (v == "bursty") out.profile = Profile::Bursty;
            else if (v == "adversarial") {
                std::fprintf(stderr,
                              "--profile=adversarial is not supported by this tool -- see this file's header "
                              "comment; use tests/test_reconnect_recovery.cpp or "
                              "tests/test_workload_generator.cpp instead.\n");
                return false;
            } else {
                std::fprintf(stderr, "unknown --profile=%s\n", v.c_str());
                return false;
            }
        } else if (starts_with(a, "--market-dist=")) {
            const std::string v = a + 14;
            if (v == "uniform") out.market_dist = MarketDist::Uniform;
            else if (v == "skewed") out.market_dist = MarketDist::Skewed;
            else {
                std::fprintf(stderr, "unknown --market-dist=%s\n", v.c_str());
                return false;
            }
        } else if (starts_with(a, "--seed=")) {
            out.seed = static_cast<std::uint64_t>(std::atoll(a + 7));
        } else if (starts_with(a, "--csv=")) {
            out.csv_path = a + 6;
        } else if (starts_with(a, "--label=")) {
            out.label = a + 8;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a);
            return false;
        }
    }
    if (out.markets == 0 || out.markets > kMaxMarkets) {
        std::fprintf(stderr, "--markets must be in [1, %zu]\n", kMaxMarkets);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return 1;

    const auto cores = performance_core_ids();
    TscClock clock;
    std::printf("invariant TSC: %s, calibrated %.4f ticks/ns; %zu candidate performance cores\n",
                clock.invariant_tsc() ? "yes" : "no (results may drift)", clock.ticks_per_ns(), cores.size());

    TickerRegistry registry;
    std::vector<MarketId> market_ids(args.markets);
    for (std::size_t i = 0; i < args.markets; ++i) {
        const std::string ticker = "WL-MKT-" + std::to_string(i);
        if (!registry.add(ticker, market_ids[i])) {
            std::fprintf(stderr, "registry.add failed for %s\n", ticker.c_str());
            return 1;
        }
    }

    KalshiConfig ws_cfg;
    Ingress ingress(ws_cfg, registry);

    risk::RiskConfig risk_cfg; // Kalshi Basic-tier defaults — see risk_engine.hpp
    auto brain = std::make_unique<Brain>(risk_cfg); // heap — see pipeline/brain.hpp's SIZE WARNING
    for (std::size_t i = 0; i < args.markets; ++i) {
        brain->add_strategy(market_ids[i], std::make_unique<PassiveMarketMaker>(market_ids[i], /*trader=*/0));
    }

    auto sink = std::make_unique<SimulatedExchangeSink>(/*qty_filled_at_entry=*/0);
    SimulatedExchangeSink* sink_ptr = sink.get(); // observed after threads join, below -- see test_pipeline_e2e.cpp's
                                                    // identical pattern for why this outlives the Gateway safely
    Gateway gateway(std::move(sink), registry);

    RingA ring_a;
    RingB ring_b;
    RingC ring_c;
    OracleRing oracle_ring; // unused — this tool drives market data only, see file header
    std::atomic<bool> stop{false};

    std::thread brain_thread([&] {
        if (cores.size() >= 2) pin_current_thread_to(cores[1]);
        brain->run(ring_a, oracle_ring, ring_c, ring_b, stop);
    });
    std::thread gateway_thread([&] {
        if (cores.size() >= 3) pin_current_thread_to(cores[2]);
        gateway.run(ring_b, ring_c, stop);
    });
    if (cores.size() >= 1) pin_current_thread_to(cores[0]); // producer/main thread

    GeneratorConfig gen_cfg;
    gen_cfg.seed = args.seed;
    gen_cfg.num_markets = args.markets;
    gen_cfg.profile = args.profile;
    gen_cfg.market_dist = args.market_dist;
    Generator gen(gen_cfg);

    // Untimed setup: seed every market's initial snapshot up front so every
    // subsequent next_frame() call in the timed loop below is known to
    // produce exactly one BookEvent (Normal/Bursty only — see this file's
    // header comment) — the property LatencyWatcher's 1:1 seq indexing
    // needs to be valid.
    std::string raw; // reused across every generate call in this file — see GeneratedFrame's header comment
    for (std::size_t i = 0; i < args.markets; ++i) {
        (void)gen.make_initial_snapshot(i, raw);
        FeedMsg fm;
        std::string err;
        if (!parse_feed_message(raw, fm, err)) {
            std::fprintf(stderr, "setup snapshot parse failed for market %zu: %s\n", i, err.c_str());
            return 1;
        }
        ingress.handle_message(fm, ring_a);
    }
    const std::uint64_t expected_after_setup =
        static_cast<std::uint64_t>(args.markets) * static_cast<std::uint64_t>(Generator::kBookEventsPerInitialSnapshot);
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (brain->book_events_applied() < expected_after_setup && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
    }

    // ---- the timed measurement window --------------------------------------
    LatencyWatcher watcher(1u << 20);
    LatencyHistogram hist;
    std::atomic<bool> watcher_stop{false};
    std::thread watcher_thread([&] {
        if (cores.size() >= 4) pin_current_thread_to(cores[3]);
        watcher.watch([&] { return brain->book_events_applied(); }, std::numeric_limits<std::uint64_t>::max(),
                       watcher_stop, clock, hist);
    });

    const std::uint64_t rss_start = current_rss_bytes();
    const std::uint64_t cpu_start = process_cpu_time_ns();

    const double interval_ns = 1'000'000'000.0 / args.rate_per_sec;
    std::uint64_t next_counter = expected_after_setup; // raw Brain::book_events_applied() value this frame expects
    std::uint64_t frames_sent = 0;
    std::size_t ring_occupancy_max = 0;
    double ring_occupancy_sum = 0.0;
    std::uint64_t ring_occupancy_samples = 0;

    const Tsc run_start = read_tscp();
    Tsc last_send = run_start;
    const double duration_ns = args.duration_sec * 1e9;

    while (clock.to_ns(read_tscp() - run_start) < duration_ns) {
        const double actual_interval_ns = interval_ns * gen.rate_multiplier();
        // Busy-wait pacing (TSC-timed) -- matches this project's existing
        // no-blocking-primitives-on-a-timed-path convention rather than
        // std::this_thread::sleep_for, which is not precise enough at the
        // higher rate points this sweep targets.
        while (clock.to_ns(read_tscp() - last_send) < actual_interval_ns) cpu_relax();
        last_send = read_tscp();

        (void)gen.next_frame(raw);
        FeedMsg fm;
        std::string err;
        if (!parse_feed_message(raw, fm, err)) {
            std::fprintf(stderr, "parse failed mid-run: %s -- %s\n", raw.c_str(), err.c_str());
            return 1;
        }
        watcher.record_send(next_counter);
        ingress.handle_message(fm, ring_a);
        ++next_counter;
        ++frames_sent;

        if ((frames_sent & 0xFF) == 0) { // sample every 256 frames -- cheap, frequent enough to see bursts
            const std::size_t occ = ring_a.size_approx();
            ring_occupancy_max = std::max(ring_occupancy_max, occ);
            ring_occupancy_sum += static_cast<double>(occ);
            ++ring_occupancy_samples;

            // Stage 5, Section 9: periodic RSS sample, once per wall-clock
            // second, for the long-running-session growth check (plateau
            // vs. unbounded growth) -- --duration-sec=60+ runs print ~1
            // line/sec. Piggybacked on the existing every-256-frames check
            // rather than its own per-iteration TSC read, so it doesn't add
            // hot-loop overhead that would bias Section 4/6's own numbers.
            const double elapsed_so_far = clock.to_ns(read_tscp() - run_start) / 1e9;
            static long last_second_logged = -1;
            const auto second_bucket = static_cast<long>(elapsed_so_far);
            if (second_bucket != last_second_logged) {
                last_second_logged = second_bucket;
                std::printf("  t=%lds rss=%.2fMB frames_sent=%llu\n", second_bucket,
                            current_rss_bytes() / (1024.0 * 1024.0), static_cast<unsigned long long>(frames_sent));
            }
        }
    }

    const double elapsed_sec = clock.to_ns(read_tscp() - run_start) / 1e9;

    // Drain grace period: let whatever's still in flight resolve before
    // stopping the watcher/Brain/Gateway threads, so the tail of the run
    // isn't systematically under-counted in the latency histogram.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::uint64_t rss_end = current_rss_bytes();
    const std::uint64_t cpu_end = process_cpu_time_ns();

    watcher_stop.store(true, std::memory_order_relaxed);
    watcher_thread.join();
    stop.store(true, std::memory_order_relaxed);
    brain_thread.join();
    gateway_thread.join();

    const auto p = hist.summarize();
    const double achieved_rate = static_cast<double>(frames_sent) / elapsed_sec;
    const double cpu_cores_used = static_cast<double>(cpu_end - cpu_start) / (elapsed_sec * 1e9);
    const double ring_occ_mean = (ring_occupancy_samples > 0) ? (ring_occupancy_sum / ring_occupancy_samples) : 0.0;

    std::printf("\n== %s ==\n", args.label.c_str());
    std::printf("target_rate=%.0f/s achieved_rate=%.0f/s markets=%zu profile=%s dist=%s duration=%.2fs\n",
                args.rate_per_sec, achieved_rate, args.markets,
                args.profile == Profile::Bursty ? "bursty" : "normal",
                args.market_dist == MarketDist::Skewed ? "skewed" : "uniform", elapsed_sec);
    LatencyHistogram::print("event send -> Brain-applied latency", p);
    std::printf(
        "watcher gaps (coalesced completions)=%zu (0 means every single completion got its own timestamp)\n",
        watcher.gaps());
    std::printf("ring A occupancy: max=%zu mean=%.1f (capacity=%zu)\n", ring_occupancy_max, ring_occ_mean,
                RingA::capacity());
    std::printf("cpu cores used (kernel+user / wall time)=%.2f  rss: start=%.1fMB end=%.1fMB\n", cpu_cores_used,
                rss_start / (1024.0 * 1024.0), rss_end / (1024.0 * 1024.0));
    std::printf("gateway: %d places, %d cancels sent (real orders, from the registered PassiveMarketMaker(s) "
                "actually trading under this workload)\n",
                sink_ptr->place_call_count, sink_ptr->cancel_call_count);

    if (!args.csv_path.empty()) {
        std::FILE* f = std::fopen(args.csv_path.c_str(), "a");
        if (f) {
            static bool header_written_this_process = false;
            // Header is written once per process invocation if the file is
            // empty -- perf/run_sweeps.py always calls this tool once per
            // sweep point in a fresh process, so this keeps the header exactly
            // once at the very start of a sweep's CSV without needing a
            // separate "write header" mode.
            std::fseek(f, 0, SEEK_END);
            if (std::ftell(f) == 0 && !header_written_this_process) {
                std::fprintf(f,
                              "label,target_rate,achieved_rate,markets,profile,market_dist,seed,duration_sec,"
                              "p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns,mean_ns,watcher_gaps,"
                              "ring_occ_max,ring_occ_mean,ring_capacity,cpu_cores_used,rss_start_bytes,rss_end_bytes,"
                              "frames_sent,book_events_applied\n");
                header_written_this_process = true;
            }
            std::fprintf(f,
                          "%s,%.0f,%.1f,%zu,%s,%s,%llu,%.3f,"
                          "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%zu,"
                          "%zu,%.1f,%zu,%.3f,%llu,%llu,"
                          "%llu,%llu\n",
                          args.label.c_str(), args.rate_per_sec, achieved_rate, args.markets,
                          args.profile == Profile::Bursty ? "bursty" : "normal",
                          args.market_dist == MarketDist::Skewed ? "skewed" : "uniform",
                          static_cast<unsigned long long>(args.seed), args.duration_sec, p.p50, p.p90, p.p99, p.p999,
                          p.p9999, p.max, p.mean, watcher.gaps(), ring_occupancy_max, ring_occ_mean, RingA::capacity(),
                          cpu_cores_used, static_cast<unsigned long long>(rss_start),
                          static_cast<unsigned long long>(rss_end), static_cast<unsigned long long>(frames_sent),
                          static_cast<unsigned long long>(brain->book_events_applied()));
            std::fclose(f);
        } else {
            std::fprintf(stderr, "could not open --csv=%s for append\n", args.csv_path.c_str());
        }
    }

    return 0;
}
