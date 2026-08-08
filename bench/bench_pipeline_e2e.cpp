// bench/bench_pipeline_e2e.cpp — the number every other benchmark in this
// project has been building toward: how long the FULL internal decision path
// actually takes, through the real running system, not a synthetic
// approximation of it.
//
//   WS text frame -> parse_feed_message -> Ingress::handle_message
//   -> ring A -> [T2 Brain, its own pinned thread: ShadowBook update,
//   extract_features, Strategy::on_event, RiskEngine::check] -> ring B
//   -> OrderIntent observed by this benchmark
//
// Two real, pinned threads and two real cross-core ring hops — not measured
// in isolation (bench_ring.cpp already does that) but as they actually
// contribute to this whole path, the same way test_pipeline_e2e.cpp proves
// correctness through the real system instead of a reimplemented shortcut.
//
// METHODOLOGY, same lesson bench_ring.cpp already paid for: one frame is fed
// in, and this benchmark blocks until its OrderIntent is observed on ring B
// before feeding the next — never more than one event in flight. Letting the
// producer run ahead would mean measuring queueing delay through two
// already-generously-sized rings, not the decision path's own cost — exactly
// the bug bench_ring.cpp's own file header describes finding the first time
// this project measured a ring under load.
//
// The strategy used here (AlwaysJoinStrategy) is bench-only and deliberately
// not one of the shipped ones: it fires unconditionally on every BookUpdate,
// with no outstanding-orders/position gating, so every single timed
// iteration produces exactly one OrderIntent to wait for — a controlled,
// worst-case-frequency probe of the dispatch machinery itself, not a claim
// about how often any real strategy actually decides to trade.
// RiskEngine's token bucket is deliberately oversized (see main()) for the
// same reason bench_ring.cpp doesn't want ring backpressure in its number:
// this benchmark measures decision-path latency, not
// Kalshi's-real-rate-limit compliance, which tests/test_risk.cpp already
// covers on its own.
#include "pipeline/brain.hpp"
#include "pipeline/ingress.hpp"
#include "px/histogram.hpp"
#include "px/platform.hpp"
#include "px/tsc_clock.hpp"
#include "risk_engine.hpp"
#include "strategy/strategy.hpp"
#include "ticker_registry.hpp"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

using namespace px;
using namespace px::live;
using namespace px::pipeline;
using namespace px::strategy;

namespace {

constexpr std::uint64_t kWarmup = 5'000;
constexpr std::uint64_t kIters  = 100'000;

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

} // namespace

int main() {
    const auto cores = performance_core_ids();
    const bool have_two_cores = cores.size() >= 2;
    std::printf("pinning: %s (of %zu candidate performance cores)\n",
                have_two_cores ? "bench thread + Brain thread, distinct cores" : "none available", cores.size());

    TscClock clock;
    std::printf("invariant TSC: %s, calibrated %.4f ticks/ns\n\n",
                clock.invariant_tsc() ? "yes" : "no (results may drift)", clock.ticks_per_ns());

    TickerRegistry registry;
    MarketId market;
    if (!registry.add("BENCH-E2E", market)) {
        std::fprintf(stderr, "registry.add failed\n");
        return 1;
    }
    KalshiConfig ws_cfg; // never connected — handle_message doesn't touch it
    Ingress ingress(ws_cfg, registry);

    // Oversized on purpose — see file header. 100,000+ iterations * 10
    // tokens/order comfortably fits without ever refilling.
    risk::RiskConfig risk_cfg;
    risk_cfg.bucket_capacity_tokens = 10'000'000;
    auto brain = std::make_unique<Brain>(risk_cfg); // SIZE WARNING — see pipeline/brain.hpp
    brain->add_strategy(market, std::make_unique<AlwaysJoinStrategy>(market, /*trader=*/0));

    RingA ring_a;
    RingB ring_b;
    RingC ring_c;   // unused — no exec reports in this benchmark, Gateway isn't part of this path
    OracleRing oracle_ring; // unused
    std::atomic<bool> stop{false};

    std::thread brain_thread([&] {
        if (have_two_cores) pin_current_thread_to(cores[1]);
        brain->run(ring_a, oracle_ring, ring_c, ring_b, stop);
    });
    if (have_two_cores) pin_current_thread_to(cores[0]);

    // Untimed setup: a real two-sided book via the real snapshot path.
    {
        FeedMsg snap;
        std::string err;
        if (!parse_feed_message(R"({"type":"orderbook_snapshot","sid":30,"seq":1,)"
                                 R"("msg":{"market_ticker":"BENCH-E2E","market_id":"m",)"
                                 R"("yes_dollars_fp":[["0.4000","100.00"]],)"
                                 R"("no_dollars_fp":[["0.4000","50.00"]]}})",
                                 snap, err)) {
            std::fprintf(stderr, "setup snapshot parse failed: %s\n", err.c_str());
            return 1;
        }
        ingress.handle_message(snap, ring_a);
        // Drain the setup Join this snapshot itself triggers (AlwaysJoinStrategy
        // fires the moment both sides are known) before the timed loop starts.
        OrderIntent discard;
        while (!ring_b.try_pop(discard)) cpu_relax();
    }

    LatencyHistogram hist;
    hist.reserve(kIters);
    long long seq = 2; // 1 was the snapshot above

    for (std::uint64_t i = 0; i < kWarmup + kIters; ++i) {
        // Ping-pong +1/-1 on the same price level: every call is a real,
        // freshly-parsed LevelDelta (never the identical byte string twice —
        // no shortcut through a cached parse), and the level's qty stays
        // valid (99..101-ish) forever, no drift to correct for.
        const long long delta = (i % 2 == 0) ? 1 : -1;
        char raw[256];
        std::snprintf(raw, sizeof(raw),
                      "{\"type\":\"orderbook_delta\",\"sid\":30,\"seq\":%lld,"
                      "\"msg\":{\"market_ticker\":\"BENCH-E2E\",\"market_id\":\"m\","
                      "\"price_dollars\":\"0.4000\",\"delta_fp\":\"%lld.00\",\"side\":\"yes\"}}",
                      seq, delta);
        ++seq;

        FeedMsg fm;
        std::string err;

        const Tsc t0 = read_tscp();
        if (!parse_feed_message(raw, fm, err)) {
            std::fprintf(stderr, "parse failed mid-benchmark: %s\n", err.c_str());
            return 1;
        }
        ingress.handle_message(fm, ring_a);

        OrderIntent intent;
        while (!ring_b.try_pop(intent)) cpu_relax();
        const Tsc t1 = read_tscp();

        if (i >= kWarmup) hist.record(clock.to_ns(t1 - t0));
    }

    LatencyHistogram::print(
        "e2e: WS frame -> parse -> shadow-book -> features -> strategy -> risk -> OrderIntent", hist.summarize());

    const auto p = hist.summarize();
    std::printf("\nfor comparison: bench_ring's cross-core ping-pong alone is ~300-600ns p50 on this "
                "machine, and this path crosses a ring TWICE (ring A in, ring B out) plus does real JSON "
                "parsing, a ShadowBook mutation, MarketState assembly, and a strategy decision in between "
                "-- p50=%.0fns / p99=%.0fns is the honest, measured cost of \"an out-of-band observation "
                "becomes a submitted order\" this project's README has been arguing for (see \"On what "
                "'sub-microsecond' actually buys here\"), not an assumed one.\n",
                p.p50, p.p99);

    stop.store(true, std::memory_order_relaxed);
    brain_thread.join();
    return 0;
}
