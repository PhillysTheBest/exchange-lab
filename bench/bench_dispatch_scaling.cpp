// bench/bench_dispatch_scaling.cpp — Stage 5, Section 6/10: isolates
// pipeline::Brain::dispatch_market_triggered's cost as the TOTAL number of
// registered strategies grows, independent of parse/generator noise.
//
// WHY THIS EXISTS: bench/bench_workload_throughput.cpp's market-count sweep
// (perf/results/market_scaling_sweep.csv) showed essentially flat latency
// from 1 to 1000 markets — but that measurement's noise floor (real JSON
// generation + parsing, ~900ns-ish and highly variable) is large enough to
// plausibly hide a genuine but smaller O(N) effect from
// dispatch_market_triggered's linear scan over ALL registered strategies
// (see pipeline/brain.cpp — confirmed by reading the code, not assumed).
// This benchmark uses bench_pipeline_e2e.cpp's exact one-event-at-a-time
// ping-pong discipline (same file, same reasoning: isolate per-op cost from
// queueing/generation noise) so any real O(N) cost has nowhere to hide.
//
// METHOD: register N-1 "NeverFireStrategy" instances on N-1 dummy markets
// that never receive any traffic (so dispatch_market_triggered's scan must
// walk past all of them, comparing MarketId, before ever reaching the one
// real match) plus one AlwaysJoinStrategy on the single market that actually
// receives the ping-ponged deltas. Sweeps N across the same order of
// magnitude as the market-count sweep (1, 10, 50, 100, 500, 1000) and prints
// one latency histogram per N — a real O(N) cost shows up as p50 growing
// with N; a flat p50 across N is this benchmark's own evidence the scan
// genuinely doesn't matter at these scales, not an assumption.
#include "pipeline/brain.hpp"
#include "pipeline/ingress.hpp"
#include "px/histogram.hpp"
#include "px/platform.hpp"
#include "px/tsc_clock.hpp"
#include "risk_engine.hpp"
#include "strategy/strategy.hpp"
#include "ticker_registry.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace px;
using namespace px::live;
using namespace px::pipeline;
using namespace px::strategy;

namespace {

constexpr std::uint64_t kWarmup = 2'000;
constexpr std::uint64_t kIters  = 50'000;

// Identical to bench_pipeline_e2e.cpp's own bench-only strategy.
class AlwaysJoinStrategy final : public Strategy {
public:
    AlwaysJoinStrategy(MarketId market, TraderId trader) noexcept : market_(market), trader_(trader) {}
    [[nodiscard]] ExecutionDecision on_event(const MarketState& state, TriggerKind trigger,
                                              const OracleEvent*) noexcept override {
        ExecutionDecision d;
        if (trigger != TriggerKind::BookUpdate || !state.have_bid || !state.have_ask) return d;
        d.action = ExecutionAction::Join;
        d.intent = OrderIntent{IntentKind::NewOrder, kInvalidOrderId,   market_,
                                trader_,              Contract::Yes,    Side::Buy,
                                OrderType::Limit,     TimeInForce::GTC, state.best_bid,
                                1};
        return d;
    }
    [[nodiscard]] const char* name() const noexcept override { return "AlwaysJoinStrategy(bench)"; }

private:
    MarketId market_;
    TraderId trader_;
};

// Registered on every dummy market this benchmark sweeps in — never fires
// (never sees a BookUpdate at all, since its market never receives any
// event), so its only cost is being one more entry
// dispatch_market_triggered's scan has to walk past on every REAL dispatch.
class NeverFireStrategy final : public Strategy {
public:
    [[nodiscard]] ExecutionDecision on_event(const MarketState&, TriggerKind, const OracleEvent*) noexcept override {
        return {};
    }
    [[nodiscard]] const char* name() const noexcept override { return "NeverFireStrategy(bench)"; }
};

void run_one(std::size_t num_dummy_strategies, const std::vector<unsigned>& cores, TscClock& clock) {
    TickerRegistry registry;
    MarketId real_market;
    if (!registry.add("DISPATCH-BENCH", real_market)) {
        std::fprintf(stderr, "registry.add failed\n");
        return;
    }
    KalshiConfig ws_cfg;
    Ingress ingress(ws_cfg, registry);

    risk::RiskConfig risk_cfg;
    risk_cfg.bucket_capacity_tokens = 10'000'000; // oversized on purpose — see bench_pipeline_e2e.cpp
    auto brain = std::make_unique<Brain>(risk_cfg);

    // Dummy markets get ids starting just past the real one, never touched
    // by Ingress/TickerRegistry — Brain's per-market arrays are flat and
    // kMaxMarkets-sized, so an unregistered MarketId is perfectly valid to
    // pass to add_strategy directly (see pipeline/brain.hpp).
    for (std::size_t i = 0; i < num_dummy_strategies; ++i) {
        brain->add_strategy(static_cast<MarketId>(real_market + 1 + i), std::make_unique<NeverFireStrategy>());
    }
    brain->add_strategy(real_market, std::make_unique<AlwaysJoinStrategy>(real_market, /*trader=*/0));

    RingA ring_a;
    RingB ring_b;
    RingC ring_c;
    OracleRing oracle_ring;
    std::atomic<bool> stop{false};

    const bool have_two_cores = cores.size() >= 2;
    std::thread brain_thread([&] {
        if (have_two_cores) pin_current_thread_to(cores[1]);
        brain->run(ring_a, oracle_ring, ring_c, ring_b, stop);
    });
    if (have_two_cores) pin_current_thread_to(cores[0]);

    {
        FeedMsg snap;
        std::string err;
        if (!parse_feed_message(R"({"type":"orderbook_snapshot","sid":50,"seq":1,)"
                                 R"("msg":{"market_ticker":"DISPATCH-BENCH","market_id":"m",)"
                                 R"("yes_dollars_fp":[["0.4000","100.00"]],)"
                                 R"("no_dollars_fp":[["0.4000","50.00"]]}})",
                                 snap, err)) {
            std::fprintf(stderr, "setup snapshot parse failed: %s\n", err.c_str());
            return;
        }
        ingress.handle_message(snap, ring_a);
        OrderIntent discard;
        while (!ring_b.try_pop(discard)) cpu_relax();
    }

    LatencyHistogram hist;
    hist.reserve(kIters);
    long long seq = 2;

    for (std::uint64_t i = 0; i < kWarmup + kIters; ++i) {
        const long long delta = (i % 2 == 0) ? 1 : -1;
        char raw[256];
        std::snprintf(raw, sizeof(raw),
                      "{\"type\":\"orderbook_delta\",\"sid\":50,\"seq\":%lld,"
                      "\"msg\":{\"market_ticker\":\"DISPATCH-BENCH\",\"market_id\":\"m\","
                      "\"price_dollars\":\"0.4000\",\"delta_fp\":\"%lld.00\",\"side\":\"yes\"}}",
                      seq, delta);
        ++seq;

        FeedMsg fm;
        std::string err;
        const Tsc t0 = read_tscp();
        if (!parse_feed_message(raw, fm, err)) {
            std::fprintf(stderr, "parse failed mid-benchmark: %s\n", err.c_str());
            return;
        }
        ingress.handle_message(fm, ring_a);

        OrderIntent intent;
        while (!ring_b.try_pop(intent)) cpu_relax();
        const Tsc t1 = read_tscp();

        if (i >= kWarmup) hist.record(clock.to_ns(t1 - t0));
    }

    char label[96];
    std::snprintf(label, sizeof(label), "N=%5zu registered strategies (1 real + %zu never-fire)",
                  num_dummy_strategies + 1, num_dummy_strategies);
    LatencyHistogram::print(label, hist.summarize());

    stop.store(true, std::memory_order_relaxed);
    brain_thread.join();
}

} // namespace

int main() {
    const auto cores = performance_core_ids();
    TscClock clock;
    std::printf("invariant TSC: %s, calibrated %.4f ticks/ns\n\n",
                clock.invariant_tsc() ? "yes" : "no (results may drift)", clock.ticks_per_ns());

    // -> 1, 10, 50, 100, 500, 1000 total registered strategies (real + dummy)
    for (int n : {0, 9, 49, 99, 499, 999}) {
        run_one(static_cast<std::size_t>(n), cores, clock);
    }

    std::printf(
        "\nThis is the same e2e path bench_pipeline_e2e.cpp measures (WS frame -> ... -> OrderIntent), with N-1 "
        "extra never-firing strategies registered on markets that never receive traffic -- any growth in p50/p99 "
        "across the N values above is dispatch_market_triggered's O(N)-in-total-registered-strategies scan showing "
        "up; a flat curve is this benchmark's own evidence it doesn't matter at these scales, not an assumption. "
        "See PERFORMANCE.md's Section 10 optimization log for the accept/reject call this feeds.\n");
    return 0;
}
