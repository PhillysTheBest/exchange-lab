// strategy/passive_market_maker.hpp — the strategy that actually demonstrates
// the Flat -> Quoting -> Position -> Exiting -> Flat state machine, and the
// only one of the three that reacts to TriggerKind::OwnOrderUpdate (a fill or
// a resting quote drifting off the current best) as well as BookUpdate.
//
// Quotes both sides passively (Join) when flat and the spread is wide enough
// to be worth quoting; cancels a resting quote once the book moves away from
// it (own quote no longer at the current best); crosses (aggressively,
// Cross) to reduce an open position the moment one exists, on whichever
// trigger notices it first — BookUpdate or OwnOrderUpdate — rather than
// waiting for the next incidental book tick. That responsiveness is the
// entire reason TriggerKind::OwnOrderUpdate exists (see strategy.hpp).
#pragma once

#include "market_state.hpp"
#include "oracle_feed.hpp"
#include "px/types.hpp"
#include "strategy.hpp"

namespace px::strategy {

class PassiveMarketMaker final : public Strategy {
public:
    PassiveMarketMaker(MarketId market, TraderId trader, Price min_spread = 4, Qty quote_qty = 5,
                        Position max_position = 20) noexcept
        : market_(market), trader_(trader), min_spread_(min_spread), quote_qty_(quote_qty),
          max_position_(max_position) {}

    [[nodiscard]] ExecutionDecision on_event(const MarketState& state, TriggerKind trigger,
                                              const OracleEvent*) noexcept override {
        ExecutionDecision d;
        if (trigger == TriggerKind::OracleEvent) return d; // book/own-order driven only

        // Position management takes priority over quoting on EITHER trigger,
        // and fires on whichever one notices the position first — a fill
        // arriving via OwnOrderUpdate should not have to wait for the next
        // incidental book tick to start being managed (see the file header).
        if (state.position != 0) {
            state_ = StrategyState::Exiting;
            if (state.position > 0 && state.have_bid) {
                d.action = ExecutionAction::Cross;
                d.intent = OrderIntent{IntentKind::NewOrder,
                                        kInvalidOrderId,
                                        market_,
                                        trader_,
                                        Contract::Yes,
                                        Side::Sell,
                                        OrderType::Limit,
                                        TimeInForce::IOC,
                                        state.best_bid,
                                        static_cast<Qty>(state.position)};
            } else if (state.position < 0 && state.have_ask) {
                d.action = ExecutionAction::Cross;
                d.intent = OrderIntent{IntentKind::NewOrder,
                                        kInvalidOrderId,
                                        market_,
                                        trader_,
                                        Contract::Yes,
                                        Side::Buy,
                                        OrderType::Limit,
                                        TimeInForce::IOC,
                                        state.best_ask,
                                        static_cast<Qty>(-state.position)};
            }
            // Else: position exists but the side we'd need to trade against
            // is empty right now — nothing safe to do this call, try again
            // next trigger.
            return d;
        }
        if (state_ == StrategyState::Exiting) state_ = StrategyState::Flat; // position just closed out

        if (trigger == TriggerKind::OwnOrderUpdate) {
            // No position (checked above) — this was a cancel/ack completing.
            // Nothing further to decide here; the next BookUpdate re-evaluates
            // quoting from scratch with fresh book context.
            if (state.outstanding_orders.empty()) state_ = StrategyState::Flat;
            return d;
        }

        // trigger == BookUpdate, flat, managing (or starting) two-sided quotes.
        if (!state.have_bid || !state.have_ask) return d;

        // A resting quote that's no longer at the current best has been left
        // behind by the book — cancel it before anything else (one action
        // per call: the next trigger re-quotes that side once the cancel is
        // acked, via OwnOrderUpdate).
        bool have_bid_quote = false, have_ask_quote = false;
        for (const auto& o : state.outstanding_orders) {
            if (o.side == Side::Buy) {
                have_bid_quote = true;
                if (o.price != state.best_bid) {
                    d.action = ExecutionAction::Cancel;
                    d.cancel_target = o.id;
                    return d;
                }
            } else {
                have_ask_quote = true;
                if (o.price != state.best_ask) {
                    d.action = ExecutionAction::Cancel;
                    d.cancel_target = o.id;
                    return d;
                }
            }
        }

        if (state.spread < min_spread_) {
            d.action = ExecutionAction::Wait; // not worth quoting a market this tight
            return d;
        }

        // Fill in whichever side is missing, capped by this strategy's own
        // self-imposed exposure limit (max_position_) — a strategy-level
        // guard layered on top of, not a replacement for, RiskEngine's own
        // exposure check (see risk/risk_engine.hpp).
        if (!have_bid_quote && state.position + static_cast<Position>(quote_qty_) <= max_position_) {
            state_ = StrategyState::Quoting;
            d.action = ExecutionAction::Join;
            d.intent = OrderIntent{IntentKind::NewOrder,
                                    kInvalidOrderId,
                                    market_,
                                    trader_,
                                    Contract::Yes,
                                    Side::Buy,
                                    OrderType::Limit,
                                    TimeInForce::GTC,
                                    state.best_bid,
                                    quote_qty_};
            return d;
        }
        if (!have_ask_quote && state.position - static_cast<Position>(quote_qty_) >= -max_position_) {
            state_ = StrategyState::Quoting;
            d.action = ExecutionAction::Join;
            d.intent = OrderIntent{IntentKind::NewOrder,
                                    kInvalidOrderId,
                                    market_,
                                    trader_,
                                    Contract::Yes,
                                    Side::Sell,
                                    OrderType::Limit,
                                    TimeInForce::GTC,
                                    state.best_ask,
                                    quote_qty_};
            return d;
        }

        d.action = ExecutionAction::Wait; // both sides already quoted at the current best
        return d;
    }

    [[nodiscard]] const char* name() const noexcept override { return "PassiveMarketMaker"; }

    // For tests/logging — not part of the decision path itself.
    [[nodiscard]] StrategyState state() const noexcept { return state_; }

private:
    MarketId market_;
    TraderId trader_;
    Price min_spread_;
    Qty quote_qty_;
    Position max_position_;
    StrategyState state_ = StrategyState::Flat;
};

} // namespace px::strategy
