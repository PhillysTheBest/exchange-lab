// strategy/market_state.hpp — everything a Strategy sees to decide with,
// assembled fresh on every trigger. Pure, allocation-free, unit-testable
// standalone (see tests/test_market_state.cpp) with no ring/thread/network
// dependency — extract_features takes plain references to already-owned
// state (Brain owns one ShadowBook + one OrderTracker + one TradeWindow per
// market, the same pattern it already used for ShadowBook alone in Stage 2).
#pragma once

#include "order_tracker.hpp"
#include "px/shadow_book.hpp"
#include "px/types.hpp"
#include "trade_window.hpp"

#include <cstdint>
#include <span>

namespace px::strategy {

struct MarketState {
    MarketId market = 0;

    bool have_bid = false;
    bool have_ask = false;
    Price best_bid = 0;
    Price best_ask = 0;
    Price spread = 0; // best_ask - best_bid; meaningful only if have_bid && have_ask

    // At BBO only — deliberately not a windowed sum over N cents, which would
    // need an arbitrary, unjustified magic constant. A strategy that wants
    // deeper book context can still read ShadowBook::level_qty itself; this
    // is the single most legible, defensible depth figure to standardize on.
    Qty bid_depth = 0;
    Qty ask_depth = 0;

    // bid_depth / (bid_depth + ask_depth). 0.5 (neither side favored) when
    // both are zero or absent — a defined, documented convention rather than
    // NaN or an arbitrary default a caller might not think to check for.
    double imbalance = 0.5;

    Position position = 0; // this account's current net YES position in the market

    TradeCounts recent_trades{}; // last kTradeWindowMs of exchange time — see trade_window.hpp

    // Acked-only — see strategy/order_tracker.hpp's class comment for why a
    // Strategy structurally cannot see (and therefore cannot target a Cancel
    // at) an order Gateway hasn't confirmed yet.
    std::span<const RestingOrder> outstanding_orders;
};

[[nodiscard]] inline MarketState extract_features(MarketId market, const ShadowBook& book, Position position,
                                                    const OrderTracker& tracker, const TradeWindow& trades,
                                                    std::int64_t now_ms) noexcept {
    MarketState s;
    s.market = market;
    s.have_bid = book.best_bid(s.best_bid);
    s.have_ask = book.best_ask(s.best_ask);
    if (s.have_bid && s.have_ask) s.spread = s.best_ask - s.best_bid;
    if (s.have_bid) s.bid_depth = book.level_qty(s.best_bid);
    if (s.have_ask) s.ask_depth = book.level_qty(s.best_ask);

    const auto total_depth = static_cast<double>(s.bid_depth) + static_cast<double>(s.ask_depth);
    s.imbalance = (total_depth > 0.0) ? (static_cast<double>(s.bid_depth) / total_depth) : 0.5;

    s.position = position;
    s.recent_trades = trades.recent_counts(now_ms);
    s.outstanding_orders = tracker.orders_for(market);
    return s;
}

} // namespace px::strategy
