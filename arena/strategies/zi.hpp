// arena/strategies/zi.hpp — zero intelligence: the floor a candidate has to clear.
//
// After Gode and Sunder (1993). Their zero-intelligence traders bid and ask at
// prices drawn uniformly at random, constrained only by a budget, and the market
// they made was still allocatively efficient: much of what looks like skill in a
// double auction is the auction. This is that trader. On its timer it withdraws
// the order it left resting, flips a coin for the side, and posts one limit order
// at a price drawn uniformly from `band_ticks` either side of the market. No
// view, no memory, no signal, and no use of its own fills. Its one constraint is
// the budget: it never trades past `max_position` either way, which is ZI-C's
// "no trade at a loss" in the only form a common-value market allows.
//
// ONE DEPARTURE, and why. Gode and Sunder drew from a fixed range because their
// traders' values were fixed. Here the fundamental is a random walk, about 16
// ticks in a 25 ms suite session, so a fixed range would soon be a strategy about
// a stale price, not one without intelligence. The band is centred on the mid
// this trader sees (the last trade, then the previous close, when there is none),
// which is the least it can know and still be in the market.
//
// WHAT IT IS FOR. Not to make money. A strategy that cannot beat it on the same
// seeds has shown nothing; one that loses more than it has learned to lose. Its
// orders that cross the touch pay the spread, and its resting orders are filled
// mostly when the market has already moved through them, so it is expected to
// lose, mostly in the `spread` column. How much is not predicted: docs/RUNNER.md
// reports what it does on the standard block.
//
// Integer only, like everything inside a session.
#pragma once

#include "arena/strategy.hpp"

namespace px::arena::strategies {

struct Zi {
    const char* name = "zi";

    SimTime reaction_ns = 10'000;
    SimTime timer_ns = 20'000;

    Qty qty = 10;
    std::int64_t band_ticks = 8;
    std::int64_t max_position = 200;
    Price anchor = 100 * kPriceScale;

    void reset(std::uint64_t seed) { rng_.reset(seed); }

    ActionBatch on_event(const MarketView& v) {
        ActionBatch out;
        if (v.reason != WakeReason::Timer || v.in_flight != 0) return out;
        // One order at a time: the new one replaces whatever it left out there.
        for (std::size_t i = 0; i < v.my_order_count; ++i) out.push(Action::cancel(v.my_orders[i].id));

        const Side side = (rng_.next() & 1) ? Side::Buy : Side::Sell;
        const bool room = side == Side::Buy ? v.position + qty <= max_position : v.position - qty >= -max_position;
        // The draw is made whether or not it is used, so the budget never shifts
        // the random sequence the later orders see.
        const std::int64_t offset = static_cast<std::int64_t>(rng_.below(static_cast<std::uint64_t>(2 * band_ticks + 1))) -
                                    band_ticks;
        if (!room) return out;

        Price ref = v.mid();
        if (ref == 0) ref = v.last_trade();
        if (ref == 0) ref = anchor;
        const Price price = snap_to_tick(ref + offset * v.tick, v.tick);
        if (price > 0) out.push(Action::limit(side, price, qty));
        return out;
    }

private:
    SimRng rng_{42};
};

} // namespace px::arena::strategies
