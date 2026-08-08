// strategy/oracle_threshold_strategy.hpp — reference/demo-only port of Stage
// 1's ThresholdSignal onto the Strategy interface.
//
// Kept specifically so strategy/oracle_feed.hpp and tools/strategy_demo.cpp
// don't become dead code once strategy/signal.hpp is deleted, and so the
// TriggerKind::OracleEvent path has at least one real consumer exercising it
// — not a fourth showcase of execution intelligence alongside
// PassiveMarketMaker/OrderBookImbalance/TradeFlow, which are what Stage 3 is
// actually about. Same model as before: a step-function fair-value estimate
// from a threshold crossing, always Crossing the spread with IOC — no
// Join/Improve/Cancel/Wait reasoning, no state machine. A template for wiring
// a real model in, not a claim that step-function weather modeling is itself
// a good trading strategy (same disclaimer the original carried).
#pragma once

#include "market_state.hpp"
#include "oracle_feed.hpp"
#include "px/types.hpp"
#include "strategy.hpp"

#include <string>
#include <utility>

namespace px::strategy {

class OracleThresholdStrategy final : public Strategy {
public:
    OracleThresholdStrategy(OracleSource watch_source, std::string watch_key, double threshold,
                             MarketId market, TraderId trader, Price edge_cents = 3, Qty qty = 1) noexcept
        : watch_source_(watch_source), watch_key_(std::move(watch_key)), threshold_(threshold),
          market_(market), trader_(trader), edge_cents_(edge_cents), qty_(qty) {}

    [[nodiscard]] ExecutionDecision on_event(const MarketState& state, TriggerKind trigger,
                                              const OracleEvent* oracle) noexcept override {
        ExecutionDecision d;
        if (trigger != TriggerKind::OracleEvent || oracle == nullptr) return d;
        if (oracle->source != watch_source_ || watch_key_ != oracle->key) return d;

        // Model: comfortably past the threshold -> treat YES as ~fair at the
        // high end; comfortably under -> ~fair at the low end. The dead zone
        // in between deliberately produces no signal rather than guessing.
        Price model_price;
        if (oracle->value >= threshold_ + 1.0) model_price = 95;
        else if (oracle->value <= threshold_ - 1.0) model_price = 5;
        else return d;

        if (model_price >= 90 && state.have_ask && state.best_ask + edge_cents_ <= model_price) {
            d.action = ExecutionAction::Cross;
            d.intent = OrderIntent{IntentKind::NewOrder, kInvalidOrderId,        market_,
                                    trader_,              Contract::Yes,         Side::Buy,
                                    OrderType::Limit,     TimeInForce::IOC,      state.best_ask,
                                    qty_};
            return d;
        }
        if (model_price <= 10 && state.have_bid && state.best_bid >= model_price + edge_cents_) {
            d.action = ExecutionAction::Cross;
            d.intent = OrderIntent{IntentKind::NewOrder, kInvalidOrderId,        market_,
                                    trader_,              Contract::Yes,         Side::Sell,
                                    OrderType::Limit,     TimeInForce::IOC,      state.best_bid,
                                    qty_};
            return d;
        }
        return d;
    }

    [[nodiscard]] const char* name() const noexcept override { return "OracleThresholdStrategy"; }

private:
    OracleSource watch_source_;
    std::string watch_key_;
    double threshold_;
    MarketId market_;
    TraderId trader_;
    Price edge_cents_;
    Qty qty_;
};

} // namespace px::strategy
