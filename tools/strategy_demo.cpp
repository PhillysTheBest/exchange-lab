// tools/strategy_demo.cpp — closes the Phase 4 loop end-to-end, locally and
// safely: a synthetic NOAA oracle feed runs on its own thread and publishes
// into an OracleRing; the main thread drains it, evaluates a ThresholdSignal
// against a local demo market's live book, and submits any resulting
// OrderIntent to that same local MatchingEngine — no network access, no
// external side effects, just the real wiring (thread -> ring -> signal ->
// engine) that a live deployment would reuse with a real feed and a real
// Kalshi order route slotted in behind the same interfaces.
#include "px/matching_engine.hpp"
#include "px/platform.hpp"
#include "strategy/oracle_feed.hpp"
#include "strategy/signal.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <thread>

using namespace px;
using namespace px::strategy;

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }
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

    ThresholdSignal sig(OracleSource::NOAA, "KNYC", /*threshold=*/90.0, m, strategy_trader,
                         /*edge_cents=*/3, /*qty=*/5);

    std::signal(SIGINT, on_sigint);
    std::printf("running against a local demo market (Ctrl+C to stop, auto-stops after 40 events)...\n");

    int events_seen = 0, intents_fired = 0;
    Market* mkt = eng.market(m);
    while (!g_stop && events_seen < 40) {
        OracleEvent e;
        if (!ring.try_pop(e)) { cpu_relax(); continue; }
        ++events_seen;

        Price bb = 0, ba = 100;
        if (!mkt->book().best_bid(bb)) bb = 0;
        if (!mkt->book().best_ask(ba)) ba = 100;

        OrderIntent intent;
        if (sig.evaluate(e, bb, ba, intent)) {
            ++intents_fired;
            const auto r = eng.submit(intent.market, intent.trader, intent.contract, intent.side,
                                       intent.type, intent.tif, intent.price, intent.qty);
            std::printf("[oracle] %s=%.1f -> INTENT %s %u @ %d  ->  filled=%u rested=%u reject=%d\n",
                        e.key, e.value, intent.side == Side::Buy ? "BUY" : "SELL", intent.qty, intent.price,
                        r.filled_qty, r.rested_qty, static_cast<int>(r.reject));
        } else {
            std::printf("[oracle] %s=%.1f (no signal)\n", e.key, e.value);
        }
    }

    stop_flag.store(true, std::memory_order_relaxed);
    feed_thread.join();

    Trader* t = eng.trader(strategy_trader);
    std::printf("done: %d events, %d intents fired. strategy trader cash=%lld position=%lld\n",
                events_seen, intents_fired, static_cast<long long>(t->cash()),
                static_cast<long long>(t->position(m)));
    return 0;
}
