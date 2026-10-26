// arena/strategies/momentum.hpp — buys strength, sells weakness.
//
// It watches the tape and takes when the last few prints have moved the same
// way. That makes it *partly* informed, but only second-hand: the information it
// trades on is the informed trader's footprint, already in the price. So it
// amplifies a move rather than starting one, which is the realistic thing for
// price impact to look like, and it gives the maker a second kind of adverse
// flow that is not simply "somebody knew".
//
// It is the smallest of the four strategies and it is here for texture. It is also the
// one to delete first if the roster ever needs to be shorter: three good
// participants beat ten mediocre ones, and this is the fourth.
#pragma once

#include "arena/strategy.hpp"

namespace px::arena::strategies {

struct Momentum {
    const char* name = "momentum";

    SimTime reaction_ns = 20'000;
    SimTime timer_ns = 0;

    std::int64_t run_to_trade = 3; // consecutive same-direction prints needed
    Qty qty = 20;
    std::int64_t max_position = 400;

    void reset(std::uint64_t) {
        last_ = 0;
        run_ = 0;
    }

    ActionBatch on_event(const MarketView& v) {
        ActionBatch out;
        const Price p = v.last_trade();
        if (p == 0) return out;
        if (last_ == 0) {
            last_ = p;
            return out;
        }
        if (p > last_) {
            run_ = run_ > 0 ? run_ + 1 : 1;
        } else if (p < last_) {
            run_ = run_ < 0 ? run_ - 1 : -1;
        }
        last_ = p;
        if (v.in_flight != 0) return out;

        if (run_ >= run_to_trade && v.position < max_position && v.best(Side::Sell) != 0) {
            out.push(Action::market(Side::Buy, qty));
            run_ = 0;
        } else if (run_ <= -run_to_trade && v.position > -max_position && v.best(Side::Buy) != 0) {
            out.push(Action::market(Side::Sell, qty));
            run_ = 0;
        }
        return out;
    }

private:
    Price last_ = 0;
    std::int64_t run_ = 0;
};

} // namespace px::arena::strategies
