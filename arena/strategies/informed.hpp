// arena/strategies/informed.hpp — the one participant that knows something.
//
// It can see the fundamental value (MarketView::signal) and nobody else can. Its
// whole strategy is the obvious one: when a resting quote is on the wrong side
// of the truth by more than its edge, take it.
//
// WHY THE SESSION NEEDS IT. Without an informed trader the latency experiment
// measures nothing. In a market of participants who all infer price from each
// other, "the slow maker lost money" is a statement about whatever the fast strategies
// were written to do. Here the maker's loss has a cause outside the order book:
// the value moved, and somebody saw it first. The maker's reaction delay then
// decides how long its stale quote sits there to be taken — which is a mechanism
// a reader can check, and the reason the curve has a predicted shape rather than
// a discovered one.
//
// It is deliberately not clever. It does not predict, size up, hide, or walk the
// book. A cleverer informed trader would make the curve steeper, which is why
// `edge_ticks` and `qty` are sweepable: showing the shape survives changing them
// is the robustness check that keeps the result honest.
//
// It takes with an IOC limit at the price it saw, never a market order, so it
// can never sweep further than the level it meant to take. If the quote has
// already moved, its order simply does not fill — that race is the experiment.
#pragma once

#include "arena/strategy.hpp"

namespace px::arena::strategies {

struct Informed {
    const char* name = "informed";

    SimTime reaction_ns = 1'000;
    SimTime timer_ns = 0; // purely signal- and event-driven

    std::int64_t edge_ticks = 1; // how wrong a quote must be before it is worth taking
    Qty qty = 50;
    std::int64_t max_position = 1'000;

    void reset(std::uint64_t) {}

    ActionBatch on_event(const MarketView& v) {
        ActionBatch out;
        if (v.signal == 0) return out; // not informed in this session: do nothing
        if (v.in_flight != 0) return out;

        const Price edge = edge_ticks * v.tick;
        const Price ask = v.best(Side::Sell);
        const Price bid = v.best(Side::Buy);

        if (ask != 0 && v.signal > ask + edge && v.position < max_position) {
            out.push(Action::limit(Side::Buy, ask, take_qty(v, Side::Buy), Tif::IOC));
        } else if (bid != 0 && v.signal < bid - edge && v.position > -max_position) {
            out.push(Action::limit(Side::Sell, bid, take_qty(v, Side::Sell), Tif::IOC));
        }
        return out;
    }

private:
    // Never take more than the level showed, and never past the position limit.
    [[nodiscard]] Qty take_qty(const MarketView& v, Side side) const {
        const std::uint64_t shown = v.size_at_best(side == Side::Buy ? Side::Sell : Side::Buy);
        std::uint64_t want = qty;
        if (shown != 0 && shown < want) want = shown;
        const std::int64_t room = side == Side::Buy ? max_position - v.position : max_position + v.position;
        if (room <= 0) return 0;
        if (static_cast<std::uint64_t>(room) < want) want = static_cast<std::uint64_t>(room);
        return static_cast<Qty>(want);
    }
};

} // namespace px::arena::strategies
