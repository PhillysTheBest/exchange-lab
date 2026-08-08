// tools/strategy_demo.cpp — closes the oracle-driven loop end-to-end, locally
// and safely: a synthetic NOAA oracle feed runs on its own thread and
// publishes into an OracleRing; the main thread drains it, evaluates
// OracleThresholdStrategy (the reference/demo-only port of Stage 1's
// ThresholdSignal onto the Strategy interface — see
// strategy/oracle_threshold_strategy.hpp) against a local demo market's live
// book, and acts on whatever ExecutionDecision comes back against that same
// local MatchingEngine — no network access, no external side effects, just
// the real wiring (thread -> ring -> strategy -> engine) a live deployment
// reuses with a real feed and Kalshi's real order routes slotted in behind
// the same interfaces.
//
// MarketState here is hand-built from the offline MatchingEngine's real
// OrderBook rather than via strategy::extract_features (which is tied to the
// live pipeline's ShadowBook — see pipeline/brain.cpp for that path). Depth/
// imbalance/recent-trade-flow/outstanding-orders are left at their defaults:
// OracleThresholdStrategy never reads them (it only ever reacts to
// TriggerKind::OracleEvent, using have_bid/best_bid/have_ask/best_ask), and
// this demo has no ShadowBook/OrderTracker/TradeWindow to source them from
// honestly.
#include "px/matching_engine.hpp"
#include "px/platform.hpp"
#include "strategy/market_state.hpp"
#include "strategy/oracle_feed.hpp"
#include "strategy/oracle_threshold_strategy.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <thread>

using namespace px;
using namespace px::strategy;

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

const char* action_name(ExecutionAction a) {
    switch (a) {
        case ExecutionAction::None:    return "NONE";
        case ExecutionAction::Join:    return "JOIN";
        case ExecutionAction::Improve: return "IMPROVE";
        case ExecutionAction::Cross:   return "CROSS";
        case ExecutionAction::Cancel:  return "CANCEL";
        case ExecutionAction::Wait:    return "WAIT";
    }
    return "?";
}
} // namespace

int main() {
    MatchingEngine eng;
    const MarketId m = eng.create_market("KXHIGHNY-DEMO");
    eng.open_market(m);
    const TraderId market_maker = eng.create_trader(1'000'000'00);
    const TraderId strategy_trader = eng.create_trader(1'000'000'00);

    // Seed resting liquidity so the strategy has something to trade against —
    // a wide market straddling the 90F threshold this demo watches.
    (void)eng.submit(m, market_maker, Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, 92, 50);
    (void)eng.submit(m, market_maker, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 8, 50);

    OracleRing ring;
    std::atomic<bool> stop_flag{false};
    // mean_f hovers near the 90F threshold so readings cross it in both
    // directions over the run instead of producing only one kind of signal.
    SyntheticNoaaFeed noaa("KNYC", /*mean_f=*/88.0, /*seed=*/42);
    std::thread feed_thread([&] { noaa.run(ring, stop_flag); });

    OracleThresholdStrategy strat(OracleSource::NOAA, "KNYC", /*threshold=*/90.0, m, strategy_trader,
                                   /*edge_cents=*/3, /*qty=*/5);

    std::signal(SIGINT, on_sigint);
    std::printf("running against a local demo market (Ctrl+C to stop, auto-stops after 40 events)...\n");

    int events_seen = 0, intents_fired = 0;
    Market* mkt = eng.market(m);
    Trader* t = eng.trader(strategy_trader);
    while (!g_stop && events_seen < 40) {
        OracleEvent e;
        if (!ring.try_pop(e)) { cpu_relax(); continue; }
        ++events_seen;

        MarketState state;
        state.market = m;
        state.have_bid = mkt->book().best_bid(state.best_bid);
        state.have_ask = mkt->book().best_ask(state.best_ask);
        if (state.have_bid && state.have_ask) state.spread = state.best_ask - state.best_bid;
        state.position = t->position(m);

        const ExecutionDecision decision = strat.on_event(state, TriggerKind::OracleEvent, &e);

        if (decision.action == ExecutionAction::None || decision.action == ExecutionAction::Wait) {
            std::printf("[oracle] %s=%.1f (%s)\n", e.key, e.value, action_name(decision.action));
            continue;
        }
        ++intents_fired;

        if (decision.action == ExecutionAction::Cancel) {
            const auto reject = eng.cancel(m, decision.cancel_target);
            std::printf("[oracle] %s=%.1f -> CANCEL order=%llu -> reject=%d\n", e.key, e.value,
                        static_cast<unsigned long long>(decision.cancel_target), static_cast<int>(reject));
            continue;
        }

        const OrderIntent& intent = decision.intent;
        const auto r = eng.submit(intent.market, intent.trader, intent.contract, intent.side, intent.type,
                                   intent.tif, intent.price, intent.qty);
        std::printf("[oracle] %s=%.1f -> %s %s %u @ %d  ->  filled=%u rested=%u reject=%d\n", e.key, e.value,
                    action_name(decision.action), intent.side == Side::Buy ? "BUY" : "SELL", intent.qty,
                    intent.price, r.filled_qty, r.rested_qty, static_cast<int>(r.reject));
    }

    stop_flag.store(true, std::memory_order_relaxed);
    feed_thread.join();

    std::printf("done: %d events, %d intents fired. strategy trader cash=%lld position=%lld\n", events_seen,
                intents_fired, static_cast<long long>(t->cash()), static_cast<long long>(t->position(m)));
    return 0;
}
