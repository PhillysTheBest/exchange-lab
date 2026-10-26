// arena/strategies/noise.hpp — uninformed flow, and the maker's only source of income.
//
// It trades for reasons the session does not model: on its timer it either posts
// a limit near where it last saw the market, takes a small amount at the touch,
// or cancels something it left behind. It has no view, no signal and no memory
// beyond its own resting orders, so on average it loses the spread — which is
// exactly what pays the market maker.
//
// Without it the session is a maker and a sniper, the maker never earns
// anything, and the only possible result is that being slow is bad. The noise
// traders are what make the trade-off a trade-off: a wider spread earns more per
// uninformed fill, and a faster reaction loses less per informed one.
//
// `anchor` is the previous close. Knowing where an instrument traded yesterday
// is not information about today, and something has to break the symmetry at the
// open or an empty book has no price at all.
#pragma once

#include "arena/strategy.hpp"

namespace px::arena::strategies {

struct Noise {
    const char* name = "noise";

    SimTime reaction_ns = 50'000;
    SimTime timer_ns = 5'000; // its arrival rate: how busy the session is

    Price anchor = 100 * kPriceScale;
    Qty qty = 25;
    unsigned pct_market = 25; // of its actions, this share take rather than post
    unsigned pct_cancel = 20; // and this share tidy up instead
    std::int64_t max_offset_ticks = 5;
    std::int64_t max_position = 2'000;
    // How many quotes it is willing to leave resting. Without a cap it would
    // post forever and only tidy up a fifth of the time, so the book would fill
    // with abandoned orders until the engine started rejecting BookFull — a
    // session artefact, not a market.
    std::size_t max_live_orders = 6;

    void reset(std::uint64_t seed) { rng_.reset(seed); }

    ActionBatch on_event(const MarketView& v) {
        ActionBatch out;
        // Only acts on its own heartbeat: reacting to the book would make it a
        // strategy, and it is meant to be flow.
        if (v.reason != WakeReason::Timer) return out;

        const Side side = (rng_.next() & 1) ? Side::Buy : Side::Sell;
        const auto roll = rng_.below(100);

        // Tidy up when asked to by the dice, or when it is already resting as
        // much as it is willing to leave out there.
        const bool too_many = v.my_order_count >= max_live_orders;
        if (v.my_order_count != 0 && (too_many || roll < pct_cancel)) {
            const std::size_t pick = static_cast<std::size_t>(rng_.below(v.my_order_count));
            out.push(Action::cancel(v.my_orders[pick].id));
            return out;
        }

        const bool room = side == Side::Buy ? v.position < max_position : v.position > -max_position;
        if (!room) return out;

        if (roll < pct_cancel + pct_market) {
            if (v.best(opposite(side)) != 0) out.push(Action::market(side, qty));
            return out;
        }

        Price ref = v.mid();
        if (ref == 0) ref = v.last_trade();
        if (ref == 0) ref = anchor;
        const std::int64_t offset = 1 + static_cast<std::int64_t>(rng_.below(
                                            static_cast<std::uint64_t>(max_offset_ticks)));
        const Price price = snap_to_tick(side == Side::Buy ? ref - offset * v.tick : ref + offset * v.tick, v.tick);
        if (price > 0) out.push(Action::limit(side, price, qty));
        return out;
    }

private:
    SimRng rng_{42};
};

} // namespace px::arena::strategies
