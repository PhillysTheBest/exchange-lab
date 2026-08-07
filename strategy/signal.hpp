// strategy/signal.hpp — turns (oracle event, current book) into an order intent.
//
// A Signal never touches MatchingEngine or the network directly — it only
// produces an OrderIntent, a plain description of what it would like to do.
// Something else (tools/strategy_demo.cpp locally; a real kalshi_rest call in a
// live deployment) decides whether/how to actually act on it. That separation
// is what makes a Signal unit-testable without a live market and reusable
// against either the offline matching engine or the Kalshi shadow book.
#pragma once

#include "oracle_feed.hpp"
#include "px/types.hpp"

#include <string>
#include <utility>

namespace px::strategy {

struct OrderIntent {
    MarketId market = 0;
    TraderId trader = 0;
    Contract contract = Contract::Yes;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::IOC;
    Price price = 0;
    Qty qty = 0;
};

class Signal {
public:
    virtual ~Signal() = default;

    // Called for each oracle event with the book's current best bid/ask
    // (kMinPrice-1 / kMaxPrice+1, i.e. 0 / 100, if that side is empty — never a
    // sentinel the caller has to special-case against valid prices). Returns
    // true and fills `out` iff the signal wants to trade on this event.
    [[nodiscard]] virtual bool evaluate(const OracleEvent& event, Price best_bid, Price best_ask,
                                         OrderIntent& out) noexcept = 0;
};

// A concrete, deliberately simple example: model the true YES probability as a
// step function of a NOAA reading against a threshold (e.g. "will the high hit
// 90°F"), and buy YES when the book is offering it cheaper than that model
// price by more than `edge_cents`, or buy NO (equivalently sell YES) when the
// book's YES side is priced richer than the model by the same margin. This is
// a template for wiring a real model in, not a claim that step-function
// weather modeling is itself a good trading strategy.
class ThresholdSignal final : public Signal {
public:
    ThresholdSignal(OracleSource watch_source, std::string watch_key, double threshold,
                     MarketId market, TraderId trader, Price edge_cents = 3, Qty qty = 1) noexcept
        : watch_source_(watch_source), watch_key_(std::move(watch_key)), threshold_(threshold),
          market_(market), trader_(trader), edge_cents_(edge_cents), qty_(qty) {}

    [[nodiscard]] bool evaluate(const OracleEvent& event, Price best_bid, Price best_ask,
                                 OrderIntent& out) noexcept override {
        if (event.source != watch_source_ || watch_key_ != event.key) return false;

        // Model: comfortably past the threshold -> treat YES as ~fair at the
        // high end; comfortably under -> ~fair at the low end. The dead zone in
        // between deliberately produces no signal rather than guessing.
        Price model_price;
        if (event.value >= threshold_ + 1.0) model_price = 95;
        else if (event.value <= threshold_ - 1.0) model_price = 5;
        else return false;

        if (model_price >= 90 && best_ask <= kMaxPrice && best_ask + edge_cents_ <= model_price) {
            out = {market_, trader_, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::IOC, best_ask, qty_};
            return true;
        }
        if (model_price <= 10 && best_bid >= kMinPrice && best_bid >= model_price + edge_cents_) {
            out = {market_, trader_, Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::IOC, best_bid, qty_};
            return true;
        }
        return false;
    }

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
