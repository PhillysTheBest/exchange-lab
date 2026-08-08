// strategy/strategy.hpp — the execution-decision interface. Replaces Stage 1's
// strategy/signal.hpp (deleted).
//
// Signal (Stage 1) only ever decided WHETHER to trade, only in response to an
// oracle event, and always via a marketable IOC order (ThresholdSignal always
// crossed the spread — see strategy/oracle_threshold_strategy.hpp for its
// port onto this interface). Strategy decides WHAT KIND of execution to take
// — Join a resting queue, Improve the current best price, Cross the spread,
// Cancel a resting order, or Wait — in response to a book update, an oracle
// event, or an update to one of the strategy's own orders. See
// strategy/market_state.hpp for what a Strategy actually sees to decide with.
#pragma once

#include "oracle_feed.hpp" // OracleEvent
#include "px/types.hpp"

#include <cstdint>

namespace px::strategy {

// What kind of order this intent describes. Kalshi's UUID order_id never
// appears here, or on any ring — see the plan doc's "Order-ID ring safety"
// section: `id` is always Brain's own OrderId, assigned when a NewOrder
// intent is created and reused verbatim as the target of a later
// CancelOrder intent. Gateway alone maps it to Kalshi's real order_id,
// privately, off any ring.
enum class IntentKind : std::uint8_t { NewOrder, CancelOrder };

struct OrderIntent {
    IntentKind kind = IntentKind::NewOrder;
    OrderId id = kInvalidOrderId; // NewOrder: Brain-assigned id for this new order.
                                  // CancelOrder: which of Brain's own orders to cancel.
    MarketId market = 0;
    TraderId trader = 0;               // offline MatchingEngine only; live Gateway ignores it
    Contract contract = Contract::Yes; // offline MatchingEngine only; live side is pre-normalized
    Side side = Side::Buy;
    OrderType type = OrderType::Limit; // offline MatchingEngine only — Kalshi's V2 REST body has
                                        // no "type" field at all (see live/kalshi_rest.cpp)
    TimeInForce tif = TimeInForce::IOC;
    Price price = 0;
    Qty qty = 0;
};

// What triggered a Strategy's evaluation. BookUpdate covers both order-book
// mutations and public trade notifications (see pipeline/events.hpp's
// BookEventKind::Trade) — every Strategy re-reads the full MarketState
// regardless of which specific thing changed, so a finer-grained trigger
// identity wouldn't change any strategy's actual decision, only add a case to
// switch on. OwnOrderUpdate fires after Brain's OrderTracker processes an
// Acked/Fill/Cancelled report for one of THIS strategy's own resting orders —
// without it, a state-machine strategy's exit transition would only fire on
// the next *incidental* book tick after a fill, which is fragile on a quiet book.
enum class TriggerKind : std::uint8_t { BookUpdate, OracleEvent, OwnOrderUpdate };

enum class ExecutionAction : std::uint8_t { None, Join, Improve, Cross, Cancel, Wait };

struct ExecutionDecision {
    ExecutionAction action = ExecutionAction::None;
    OrderIntent intent{};                    // valid iff action is Join/Improve/Cross
    OrderId cancel_target = kInvalidOrderId; // valid iff action is Cancel
};

// A strategy may (but needn't) model itself as moving through these states —
// shared vocabulary, not an interface requirement: Strategy itself doesn't
// mention it. A strategy that decides fresh every call from MarketState alone
// (e.g. OrderBookImbalance) simply doesn't use it.
enum class StrategyState : std::uint8_t { Flat, Quoting, Position, Exiting };

struct MarketState; // see strategy/market_state.hpp — kept out of this header so
                     // the Strategy interface itself doesn't pull in ShadowBook/
                     // OrderTracker/TradeWindow, none of which it needs directly.

class Strategy {
public:
    virtual ~Strategy() = default;

    // Called on every trigger this strategy is registered for (see
    // pipeline::Brain::add_strategy). `oracle` is non-null iff
    // trigger == TriggerKind::OracleEvent; a strategy that only reacts to
    // BookUpdate/OwnOrderUpdate can ignore it entirely.
    [[nodiscard]] virtual ExecutionDecision on_event(const MarketState& state, TriggerKind trigger,
                                                      const OracleEvent* oracle) noexcept = 0;

    // For bench labeling (bench/bench_strategy.cpp) and on_reject logging
    // (pipeline::Brain::on_reject) — not part of the decision path itself.
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

} // namespace px::strategy
