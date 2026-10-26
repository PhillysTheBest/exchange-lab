// arena/strategies/roundtrip.hpp — the evaluator's positive control.
//
// On a fixed timer it buys `qty` at market, and on the next wake sells the same
// `qty` at market, for ever: no view of the market enters any decision except
// whether the side it is about to hit has an order on it. So its result is known
// before it runs. Every fill crosses the book, and the reference makers quote
// `half_spread_ticks` (2) either side of their estimate of the fundamental, so
// each unit it trades should cost about 2 ticks of spread; its inventory is
// never more than `qty` and is flat every other wake, so adverse markout and
// carry should be indistinguishable from zero. docs/CONTROLS.md checks that the
// report finds exactly that.
//
// Integer only, like everything inside a session.
#pragma once

#include "arena/strategy.hpp"

namespace px::arena::strategies {

struct Roundtrip {
    const char* name = "roundtrip";

    SimTime reaction_ns = 10'000;
    SimTime timer_ns = 50'000;

    Qty qty = 10;

    void reset(std::uint64_t) { long_ = false; }

    ActionBatch on_event(const MarketView& v) {
        ActionBatch out;
        if (v.reason != WakeReason::Timer || v.in_flight != 0) return out;
        const Side side = long_ ? Side::Sell : Side::Buy;
        // A market order needs something to hit: the resting side opposite.
        if (v.best(side == Side::Buy ? Side::Sell : Side::Buy) == 0) return out;
        out.push(Action::market(side, qty));
        long_ = !long_;
        return out;
    }

private:
    bool long_ = false;
};

} // namespace px::arena::strategies
