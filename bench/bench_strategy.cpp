// bench/bench_strategy.cpp — latency of a Strategy's own decision function,
// and of assembling the MarketState it decides from, measured rather than
// assumed to be "obviously cheap enough to run inline on T2."
//
// pipeline/brain.hpp's file header cites RSA-PSS signing as ~15,000-30,000x a
// ShadowBook level update as the reason signing lives on its own thread (T3)
// instead of inline in T2's book-update loop. Strategy::on_event and
// extract_features are the other things T2 runs inline, once per registered
// strategy on every trigger (pipeline/brain.cpp's dispatch_market_triggered)
// — this benchmark is what backs the equivalent claim for THEM: that they're
// closer to a book update than to a signing call, so leaving them inline is
// the same kind of justified choice, not an unexamined one.
//
// Each Strategy is benched via a Strategy& parameter (bench_one), the same
// base-class interface Brain actually calls through — not a claim that this
// definitely defeats devirtualization under -flto's whole-program view (not
// separately verified here), just that the call is written the way
// production calls it rather than through the concrete type directly.
// name() (see strategy/strategy.hpp's own comment pointing at this file)
// labels each row.
#include "px/histogram.hpp"
#include "px/platform.hpp"
#include "px/tsc_clock.hpp"
#include "strategy/market_state.hpp"
#include "strategy/oracle_threshold_strategy.hpp"
#include "strategy/order_book_imbalance.hpp"
#include "strategy/passive_market_maker.hpp"
#include "strategy/strategy.hpp"
#include "strategy/trade_flow.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <vector>

using namespace px;
using namespace px::strategy;

namespace {

constexpr int kWarmup = 10'000;
constexpr int kIters  = 200'000;

// A representative two-sided, moderately bid-skewed book with recent
// buy-heavy trade flow and no resting orders of our own — the "there's
// something to decide" case, not the (more common in practice, but
// computationally trivial) early-return case every strategy also has. Shared
// across PassiveMarketMaker/OrderBookImbalance/TradeFlow so the three
// benchmarks are directly comparable against identical input.
[[nodiscard]] MarketState make_book_update_state() {
    MarketState s;
    s.market = 0;
    s.have_bid = true;
    s.have_ask = true;
    s.best_bid = 45;
    s.best_ask = 55;
    s.spread = 10;
    s.bid_depth = 60;
    s.ask_depth = 40;
    s.imbalance = 0.6; // bid_depth / (bid_depth + ask_depth)
    s.position = 0;
    s.recent_trades = TradeCounts{/*total=*/5, /*taker_buy=*/4, /*taker_sell=*/1};
    s.outstanding_orders = {}; // empty span — no resting orders of our own
    return s;
}

// PassiveMarketMaker's other hot path: an open position that needs managing
// (Cross), reachable on either BookUpdate or OwnOrderUpdate — see its file
// header for why that responsiveness is the entire reason
// TriggerKind::OwnOrderUpdate exists.
[[nodiscard]] MarketState make_position_state() {
    MarketState s = make_book_update_state();
    s.position = 8;
    return s;
}

[[nodiscard]] OracleEvent make_matching_oracle_event() {
    OracleEvent e;
    e.source = OracleSource::NOAA;
    e.set_key("KNYC");
    e.value = 95.0; // comfortably past OracleThresholdStrategy's default 90.0 threshold + 1.0 dead zone
    return e;
}

// Runs `n` on_event calls through the base-class pointer, discarding nothing
// (every decision's action is folded into `sink` so -O3/-flto can't prove the
// loop body's result is unobserved and remove it, the same concern
// bench_engine.cpp's real submit()/cancel() side effects sidestep for free —
// Strategy::on_event has no side effects of its own to lean on instead).
void bench_one(Strategy& strat, const char* scenario, const MarketState& state, TriggerKind trigger,
                const OracleEvent* oracle, const TscClock& clock, unsigned& sink) {
    LatencyHistogram hist;
    hist.reserve(kIters);
    for (int i = 0; i < kWarmup + kIters; ++i) {
        const Tsc t0 = read_tscp();
        const ExecutionDecision d = strat.on_event(state, trigger, oracle);
        const Tsc t1 = read_tscp();
        if (i >= kWarmup) hist.record(clock.to_ns(t1 - t0));
        sink += static_cast<unsigned>(d.action);
    }
    char label[64];
    std::snprintf(label, sizeof(label), "%s (%s)", strat.name(), scenario);
    LatencyHistogram::print(label, hist.summarize());
}

void bench_extract_features(const TscClock& clock, unsigned& sink) {
    // Stack locals, deliberately — unlike OrderBook (~5.7 MB) and Brain
    // (~780 KB), ShadowBook/TradeWindow carry no SIZE WARNING (~440 B and a
    // fixed 64-entry ring, respectively); see tests/test_market_state.cpp,
    // which stack-allocates the identical pair for the same reason.
    ShadowBook book;
    book.set_level(Side::Buy, 45, 60);
    book.set_level(Side::Sell, 55, 40);
    OrderTracker tracker;
    TradeWindow trades;
    for (int i = 0; i < 4; ++i) trades.record(Side::Buy, 45, 1, i);
    trades.record(Side::Sell, 55, 1, 4);

    LatencyHistogram hist;
    hist.reserve(kIters);
    for (int i = 0; i < kWarmup + kIters; ++i) {
        const Tsc t0 = read_tscp();
        const MarketState s = extract_features(/*market=*/0, book, /*position=*/0, tracker, trades, /*now_ms=*/5);
        const Tsc t1 = read_tscp();
        if (i >= kWarmup) hist.record(clock.to_ns(t1 - t0));
        sink += static_cast<unsigned>(s.best_bid) + static_cast<unsigned>(s.best_ask);
    }
    LatencyHistogram::print("extract_features (MarketState assembly)", hist.summarize());
}

} // namespace

int main() {
    const auto cores = performance_core_ids();
    bool pinned = false;
    if (!cores.empty()) pinned = pin_current_thread_to(cores.front());
    std::printf("pinned to logical core %s (of %zu candidate performance cores)\n",
                pinned ? std::to_string(cores.front()).c_str() : "none", cores.size());

    TscClock clock;
    std::printf("invariant TSC: %s, calibrated %.4f ticks/ns\n\n",
                clock.invariant_tsc() ? "yes" : "no (results may drift)", clock.ticks_per_ns());

    unsigned sink = 0;

    bench_extract_features(clock, sink);

    const MarketState book_state = make_book_update_state();
    const MarketState position_state = make_position_state();
    const OracleEvent oracle_event = make_matching_oracle_event();

    PassiveMarketMaker pmm(/*market=*/0, /*trader=*/0);
    bench_one(pmm, "quoting", book_state, TriggerKind::BookUpdate, nullptr, clock, sink);
    PassiveMarketMaker pmm_position(/*market=*/0, /*trader=*/0);
    bench_one(pmm_position, "managing position", position_state, TriggerKind::BookUpdate, nullptr, clock, sink);

    OrderBookImbalance obi(/*market=*/0, /*trader=*/0);
    bench_one(obi, "book-driven", book_state, TriggerKind::BookUpdate, nullptr, clock, sink);

    TradeFlow tf(/*market=*/0, /*trader=*/0);
    bench_one(tf, "trade-flow-driven", book_state, TriggerKind::BookUpdate, nullptr, clock, sink);

    OracleThresholdStrategy ots(OracleSource::NOAA, "KNYC", /*threshold=*/90.0, /*market=*/0, /*trader=*/0);
    bench_one(ots, "oracle-driven", book_state, TriggerKind::OracleEvent, &oracle_event, clock, sink);

    // Prevents the compiler from treating every loop above as dead code —
    // never actually read for its value, but its existence in the object
    // file is what forces every on_event()/extract_features() call to
    // actually happen. Real content, not decoration.
    std::printf("\n(checksum, ignore: %u)\n", sink);

    std::printf("\nfor comparison: a ShadowBook level update is on the order of 50-100 ns (see "
                "bench_engine's order-submit numbers), and RSA-PSS signing is ~818,000 ns at p50 "
                "(see bench_sign) -- if every number above stays within roughly 1-2 orders of "
                "magnitude of a book update, that's the measured basis for leaving strategy "
                "dispatch inline on T2 (pipeline/brain.cpp) rather than giving it its own thread "
                "the way signing needed one.\n");

    // Brain currently calls extract_features once per registered strategy on
    // a market (pipeline/brain.cpp's dispatch_market_triggered), not once per
    // trigger with the result shared — a real, deliberate simplification
    // (fewer moving parts than threading a shared MarketState through the
    // loop) rather than an oversight, and this benchmark is what makes the
    // "cheap enough that it doesn't matter" side of that trade-off a measured
    // claim instead of an assumed one: at a handful of strategies per market,
    // a few redundant extract_features calls per event cost nanoseconds, not
    // the microseconds that would actually justify restructuring the loop.
    return 0;
}
