// bench/bench_pipeline_stages.cpp — Stage 5, Section 2: where does the time
// in bench_pipeline_e2e.cpp's single ~1.5us number actually go?
//
// bench_pipeline_e2e.cpp answers "how long does the whole path take" through
// two real, pinned, cross-core threads (T1-equivalent -> ring A -> real
// Brain thread -> ring B), on purpose — that's the honest end-to-end number.
// It has never broken that number down stage by stage, because doing so
// cross-thread would mean comparing TSC timestamps taken on two different
// cores for each individual stage boundary, which is a strictly weaker
// measurement than a same-thread round trip: it's only valid at all because
// invariant TSC is hardware-synchronized across cores on a single package
// (true of this machine, confirmed by has_invariant_tsc()), and even then
// it's exposed to whatever scheduling jitter separates the two threads
// crossing that particular boundary.
//
// This benchmark takes the opposite, more precise approach for stage
// attribution specifically: SINGLE-THREADED, no ring hop, no cross-core
// comparison at all. It calls the exact same public building blocks
// pipeline::Brain::apply_book_event/dispatch_market_triggered call
// internally (parse_feed_message, Ingress::handle_message,
// ShadowBook::apply_delta, strategy::extract_features, Strategy::on_event,
// risk::RiskEngine::check), in the same order, on one thread, with a TSC
// bracket around each — reconstructing Brain's real per-event work without
// needing access to Brain's private methods (none of the building blocks
// above are Brain-private).
//
// SANITY CHECK this file's own numbers should satisfy: the five/six stage
// p50s below, summed, should land in the same neighborhood as
// bench_pipeline_e2e's own p50 MINUS roughly twice bench_ring's cross-core
// ping-pong cost (two ring hops that number pays and this one doesn't) —
// reported explicitly at the end, in the same "show your work" spirit as
// docs/PERFORMANCE.md's "methodology bug" section, not asserted and left
// unverified.
#include "pipeline/brain.hpp" // BookEvent/BookEventKind, for the switch this file's apply_to_book mirrors
#include "pipeline/ingress.hpp"
#include "px/histogram.hpp"
#include "px/platform.hpp"
#include "px/shadow_book.hpp"
#include "px/tsc_clock.hpp"
#include "risk_engine.hpp"
#include "strategy/market_state.hpp"
#include "strategy/order_tracker.hpp"
#include "strategy/strategy.hpp"
#include "strategy/trade_window.hpp"
#include "ticker_registry.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace px;
using namespace px::live;
using namespace px::pipeline;
using namespace px::strategy;

namespace {

constexpr std::uint64_t kWarmup = 5'000;
constexpr std::uint64_t kIters  = 100'000;

[[nodiscard]] std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Bench-only, same shape as bench_pipeline_e2e.cpp's AlwaysJoinStrategy:
// fires unconditionally once both sides of the book are known, so every
// timed iteration reaches the risk-check stage too (nothing here measures
// "how often does a real strategy decide to trade").
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

// Mirrors pipeline::Brain::apply_book_event's switch (pipeline/brain.cpp)
// exactly, minus the book_events_applied_ counter and dispatch call this
// file measures as separate, independent stages instead. Kept as its own
// function (not inlined into the timed loop) so it's visually obvious this
// is the same logic Brain runs, not a reinvented shortcut.
void apply_to_book(const BookEvent& e, ShadowBook& book, TradeWindow& trades) noexcept {
    switch (e.kind) {
        case BookEventKind::SnapshotBegin: book.clear(); return;
        case BookEventKind::LevelSet:      book.set_level(e.side, e.price, e.qty); return;
        case BookEventKind::LevelDelta:    book.apply_delta(e.side, e.price, e.qty_delta); return;
        case BookEventKind::Ticker:        return; // informational only, same as Brain's own handling
        case BookEventKind::Trade:         trades.record(e.side, e.price, e.qty, e.ts_ms); return;
        case BookEventKind::LifecycleStatus: return; // not exercised by this benchmark's synthetic frames
    }
}

} // namespace

int main() {
    const auto cores = performance_core_ids();
    if (!cores.empty()) pin_current_thread_to(cores.front());
    std::printf("pinned to logical core %s (of %zu candidate performance cores)\n",
                cores.empty() ? "none" : std::to_string(cores.front()).c_str(), cores.size());

    TscClock clock;
    std::printf("invariant TSC: %s, calibrated %.4f ticks/ns\n\n",
                clock.invariant_tsc() ? "yes" : "no (results may drift)", clock.ticks_per_ns());

    TickerRegistry registry;
    MarketId market;
    if (!registry.add("BENCH-STAGES", market)) {
        std::fprintf(stderr, "registry.add failed\n");
        return 1;
    }
    KalshiConfig ws_cfg; // never connected — handle_message doesn't touch it
    Ingress ingress(ws_cfg, registry);
    RingA ring_a; // single-threaded use here, same as bench_ring.cpp's own same-thread sub-benchmark

    ShadowBook book;
    TradeWindow trades;
    OrderTracker tracker; // stack local — see its own SIZE NOTE; ~1.04MB at kMaxMarkets=1024, still fits
    risk::RiskConfig risk_cfg;
    risk_cfg.bucket_capacity_tokens = 10'000'000; // oversized on purpose — see bench_pipeline_e2e.cpp's
                                                    // identical note; this file measures decision-path
                                                    // cost, not rate-limit compliance
    risk::RiskEngine risk(risk_cfg);
    AlwaysJoinStrategy strat(market, /*trader=*/0);

    // Untimed setup: a real two-sided book via the real snapshot path, applied
    // by hand (no Brain thread here to do it) — pop every BookEvent the
    // snapshot produces (one SnapshotBegin, then one LevelSet per level) and
    // apply each with the same function the timed loop below uses.
    {
        FeedMsg snap;
        std::string err;
        if (!parse_feed_message(R"({"type":"orderbook_snapshot","sid":40,"seq":1,)"
                                 R"("msg":{"market_ticker":"BENCH-STAGES","market_id":"m",)"
                                 R"("yes_dollars_fp":[["0.4000","100.00"]],)"
                                 R"("no_dollars_fp":[["0.4000","50.00"]]}})",
                                 snap, err)) {
            std::fprintf(stderr, "setup snapshot parse failed: %s\n", err.c_str());
            return 1;
        }
        ingress.handle_message(snap, ring_a);
        BookEvent be;
        while (ring_a.try_pop(be)) apply_to_book(be, book, trades); // drains SnapshotBegin + every LevelSet
    }

    LatencyHistogram h_parse, h_ingress, h_book, h_features, h_strategy, h_risk;
    for (auto* h : {&h_parse, &h_ingress, &h_book, &h_features, &h_strategy, &h_risk}) h->reserve(kIters);

    long long seq = 2; // 1 was the snapshot above

    for (std::uint64_t i = 0; i < kWarmup + kIters; ++i) {
        const bool warm = (i >= kWarmup);

        // Same ping-pong +1/-1 payload bench_pipeline_e2e.cpp uses — a real,
        // freshly-parsed LevelDelta every iteration, level qty never drifts.
        const long long delta = (i % 2 == 0) ? 1 : -1;
        char raw[256];
        std::snprintf(raw, sizeof(raw),
                      "{\"type\":\"orderbook_delta\",\"sid\":40,\"seq\":%lld,"
                      "\"msg\":{\"market_ticker\":\"BENCH-STAGES\",\"market_id\":\"m\","
                      "\"price_dollars\":\"0.4000\",\"delta_fp\":\"%lld.00\",\"side\":\"yes\"}}",
                      seq, delta);
        ++seq;

        // Stage 1: parse — raw WS text -> FeedMsg.
        FeedMsg fm;
        std::string perr;
        Tsc t0 = read_tscp();
        const bool parsed = parse_feed_message(raw, fm, perr);
        Tsc t1 = read_tscp();
        if (!parsed) {
            std::fprintf(stderr, "parse failed mid-benchmark: %s\n", perr.c_str());
            return 1;
        }
        if (warm) h_parse.record(clock.to_ns(t1 - t0));

        // Stage 2: ingress translate — FeedMsg -> BookEvent (ticker already
        // resolved at registry.add time; this is seq-tracking + BookEvent
        // construction + a same-thread ring push/pop). bench_ring.cpp's own
        // same-thread push+pop number (~20-40ns) is included in this figure —
        // called out explicitly rather than pretending this isolates translate
        // cost alone.
        Tsc t2 = read_tscp();
        ingress.handle_message(fm, ring_a);
        BookEvent be;
        while (!ring_a.try_pop(be)) cpu_relax();
        Tsc t3 = read_tscp();
        if (warm) h_ingress.record(clock.to_ns(t3 - t2));

        // Stage 3: shadow-book update.
        Tsc t4 = read_tscp();
        apply_to_book(be, book, trades);
        Tsc t5 = read_tscp();
        if (warm) h_book.record(clock.to_ns(t5 - t4));

        // Stage 4: feature extraction.
        Tsc t6 = read_tscp();
        const MarketState state = extract_features(market, book, risk.position(market), tracker, trades, now_ns());
        Tsc t7 = read_tscp();
        if (warm) h_features.record(clock.to_ns(t7 - t6));

        // Stage 5: strategy decision.
        Tsc t8 = read_tscp();
        const ExecutionDecision decision = strat.on_event(state, TriggerKind::BookUpdate, nullptr);
        Tsc t9 = read_tscp();
        if (warm) h_strategy.record(clock.to_ns(t9 - t8));

        // Stage 6: risk check — only reachable, same as production
        // (pipeline/brain.cpp's handle_decision), when the strategy actually
        // decided to do something. AlwaysJoinStrategy fires every iteration
        // once both sides are known (true from the very first iteration,
        // thanks to the untimed snapshot setup above).
        if (decision.action != ExecutionAction::None) {
            Tsc t10 = read_tscp();
            const risk::Verdict v = risk.check(decision.intent.market, decision.intent.side, decision.intent.price,
                                                decision.intent.qty, now_ns());
            Tsc t11 = read_tscp();
            if (warm) h_risk.record(clock.to_ns(t11 - t10));
            (void)v; // bucket is oversized on purpose — see setup comment; verdict is always Accept here
        }
    }

    LatencyHistogram::print("stage 1: parse (WS text -> FeedMsg)", h_parse.summarize());
    LatencyHistogram::print("stage 2: ingress translate (FeedMsg -> BookEvent, incl. same-thread ring)",
                             h_ingress.summarize());
    LatencyHistogram::print("stage 3: shadow-book update (ShadowBook::apply_delta)", h_book.summarize());
    LatencyHistogram::print("stage 4: feature extraction (extract_features)", h_features.summarize());
    LatencyHistogram::print("stage 5: strategy decision (Strategy::on_event)", h_strategy.summarize());
    LatencyHistogram::print("stage 6: risk check (RiskEngine::check)", h_risk.summarize());

    const double sum_p50 = h_parse.summarize().p50 + h_ingress.summarize().p50 + h_book.summarize().p50 +
                            h_features.summarize().p50 + h_strategy.summarize().p50 + h_risk.summarize().p50;
    std::printf(
        "\nsum of stage p50s = %.1f ns -- compare against bench_pipeline_e2e's own p50 (see docs/PERFORMANCE.md) "
        "MINUS roughly twice bench_ring's cross-core ping-pong p50 (two ring hops this single-threaded "
        "benchmark doesn't pay): that difference is what this file adds up to, and the two numbers landing "
        "in the same neighborhood is this benchmark's own internal consistency check, not an assumed "
        "identity -- see this file's header comment.\n",
        sum_p50);

    return 0;
}
