// strategy/trade_flow.hpp — reacts to the recent public trade tape
// (MarketState::recent_trades — see trade_window.hpp), not just static book
// depth. Genuinely different information from OrderBookImbalance: two books
// with identical current depth can have arrived there via very different
// recent trade flow, and this strategy is the one that can tell the difference.
#pragma once

#include "market_state.hpp"
#include "oracle_feed.hpp"
#include "px/types.hpp"
#include "strategy.hpp"

#include <cstddef>

namespace px::strategy {

class TradeFlow final : public Strategy {
public:
    TradeFlow(MarketId market, TraderId trader, std::size_t min_trade_count = 3,
              double flow_threshold = 0.7, Qty qty = 1) noexcept
        : market_(market), trader_(trader), min_trade_count_(min_trade_count),
          flow_threshold_(flow_threshold), qty_(qty) {}

    [[nodiscard]] ExecutionDecision on_event(const MarketState& state, TriggerKind trigger,
                                              const OracleEvent*) noexcept override {
        ExecutionDecision d;
        if (trigger != TriggerKind::BookUpdate) return d;
        if (!state.have_bid || !state.have_ask) return d;
        if (!state.outstanding_orders.empty()) return d;

        const auto& t = state.recent_trades;
        if (t.total < min_trade_count_) {
            d.action = ExecutionAction::Wait; // not enough recent flow to read anything into
            return d;
        }

        const double buy_fraction = static_cast<double>(t.taker_buy) / static_cast<double>(t.total);

        // Momentum-follow, not chase: recent flow dominated by takers buying
        // (lifting the ask repeatedly) suggests the book is trending up —
        // Join the bid to accumulate at the current best rather than Cross
        // and pay the spread on every single tick. A strong book imbalance in
        // the SAME direction is required too: a buy-heavy trade tape against
        // a book that's actually ask-heavy (sellers replenishing as fast as
        // they're hit) is exactly the "don't trust the tape alone" case this
        // feature combination exists to catch.
        if (buy_fraction >= flow_threshold_ && state.imbalance >= 0.5) {
            d.action = ExecutionAction::Join;
            d.intent = OrderIntent{IntentKind::NewOrder, kInvalidOrderId,   market_,
                                    trader_,              Contract::Yes,    Side::Buy,
                                    OrderType::Limit,     TimeInForce::GTC, state.best_bid,
                                    qty_};
        } else if (buy_fraction <= (1.0 - flow_threshold_) && state.imbalance <= 0.5) {
            d.action = ExecutionAction::Join;
            d.intent = OrderIntent{IntentKind::NewOrder, kInvalidOrderId,   market_,
                                    trader_,              Contract::Yes,    Side::Sell,
                                    OrderType::Limit,     TimeInForce::GTC, state.best_ask,
                                    qty_};
        } else {
            d.action = ExecutionAction::Wait;
        }
        return d;
    }

    [[nodiscard]] const char* name() const noexcept override { return "TradeFlow"; }

private:
    MarketId market_;
    TraderId trader_;
    std::size_t min_trade_count_;
    double flow_threshold_;
    Qty qty_;
};

} // namespace px::strategy
