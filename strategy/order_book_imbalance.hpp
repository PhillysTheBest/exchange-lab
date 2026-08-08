// strategy/order_book_imbalance.hpp — the most direct realization of the
// source planning doc's core ask: order-book state alone, not an oracle
// event, drives the decision.
//
// Reads MarketState::imbalance (bid_depth / (bid_depth+ask_depth) at BBO —
// see market_state.hpp) and picks among three responses depending on how
// skewed the book is: heavily skewed -> Cross (the skew is strong enough to
// act on now, pay the spread); moderately skewed -> Join (add liquidity on
// the stronger side rather than pay for immediacy); balanced -> Wait.
#pragma once

#include "market_state.hpp"
#include "oracle_feed.hpp"
#include "px/types.hpp"
#include "strategy.hpp"

namespace px::strategy {

class OrderBookImbalance final : public Strategy {
public:
    OrderBookImbalance(MarketId market, TraderId trader, double aggressive_threshold = 0.75,
                        double passive_threshold = 0.60, Qty qty = 1) noexcept
        : market_(market), trader_(trader), aggressive_threshold_(aggressive_threshold),
          passive_threshold_(passive_threshold), qty_(qty) {}

    [[nodiscard]] ExecutionDecision on_event(const MarketState& state, TriggerKind trigger,
                                              const OracleEvent*) noexcept override {
        ExecutionDecision d;
        if (trigger != TriggerKind::BookUpdate) return d; // book-driven only — no oracle, no own-order logic
        if (!state.have_bid || !state.have_ask) return d; // need a real two-sided market to read

        // Already resting an order — don't pile on. Keeps this
        // demonstration's exposure easy to reason about; RiskEngine's own
        // exposure check (risk/risk_engine.hpp) is the real backstop, this is
        // just this strategy staying simple about it.
        if (!state.outstanding_orders.empty()) return d;

        if (state.imbalance >= aggressive_threshold_) {
            // Heavily bid-skewed: buyers are winning the book -> take the ask now.
            d.action = ExecutionAction::Cross;
            d.intent = OrderIntent{IntentKind::NewOrder, kInvalidOrderId,   market_,
                                    trader_,              Contract::Yes,    Side::Buy,
                                    OrderType::Limit,     TimeInForce::IOC, state.best_ask,
                                    qty_};
        } else if (state.imbalance <= (1.0 - aggressive_threshold_)) {
            d.action = ExecutionAction::Cross;
            d.intent = OrderIntent{IntentKind::NewOrder, kInvalidOrderId,   market_,
                                    trader_,              Contract::Yes,    Side::Sell,
                                    OrderType::Limit,     TimeInForce::IOC, state.best_bid,
                                    qty_};
        } else if (state.imbalance >= passive_threshold_) {
            // Moderately bid-skewed: join the bid rather than pay the spread.
            d.action = ExecutionAction::Join;
            d.intent = OrderIntent{IntentKind::NewOrder, kInvalidOrderId,   market_,
                                    trader_,              Contract::Yes,    Side::Buy,
                                    OrderType::Limit,     TimeInForce::GTC, state.best_bid,
                                    qty_};
        } else if (state.imbalance <= (1.0 - passive_threshold_)) {
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

    [[nodiscard]] const char* name() const noexcept override { return "OrderBookImbalance"; }

private:
    MarketId market_;
    TraderId trader_;
    double aggressive_threshold_;
    double passive_threshold_;
    Qty qty_;
};

} // namespace px::strategy
