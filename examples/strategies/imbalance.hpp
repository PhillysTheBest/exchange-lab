// examples/strategies/imbalance.hpp — a strategy written from outside the tree.
//
// Install it the way anyone would install their own:
//
//     make strategy STRATEGY=examples/strategies/imbalance.hpp
//     build/release/sweep_lab --strategies=maker,rival,informed,noise:2,imbalance --seeds=8
//
// THE CONVENTION a strategy header follows, and the only one:
//   * the file is <name>.hpp, where <name> is lowercase letters, digits and
//     underscores, and is what --strategies= calls it;
//   * it defines struct <Name> (the file name in CamelCase, so imbalance.hpp
//     defines Imbalance) in namespace px::arena::strategies;
//   * that struct provides the five members arena/strategy.hpp lists, and
//     nothing in it may use floating point (see arena/clock.hpp for why).
// It includes arena/strategy.hpp and nothing else from the project: the engine,
// the session and the scheduler are out of reach by construction.
//
// The strategy: take liquidity when the touch is lopsided. If the size resting
// at the best bid is several times the size at the best ask, buying pressure is
// queued up and the ask is thin, so it lifts the ask; the mirror case sells. It
// is uninformed (v.signal is 0 for it) and is an example, not a recommendation.
#pragma once

#include "arena/strategy.hpp"

namespace px::arena::strategies {

struct Imbalance {
    const char* name = "imbalance";

    SimTime reaction_ns = 20'000;
    SimTime timer_ns = 0; // event-driven: a touch change is what it reacts to

    std::uint64_t ratio = 3; // how lopsided the touch must be to act on
    Qty qty = 10;
    std::int64_t max_position = 200;

    void reset(std::uint64_t) {}

    ActionBatch on_event(const MarketView& v) {
        ActionBatch out;
        if (v.in_flight != 0) return out;
        const std::uint64_t bid = v.size_at_best(Side::Buy);
        const std::uint64_t ask = v.size_at_best(Side::Sell);
        if (bid == 0 || ask == 0) return out;

        if (bid >= ratio * ask && v.position < max_position) {
            out.push(Action::market(Side::Buy, qty));
        } else if (ask >= ratio * bid && v.position > -max_position) {
            out.push(Action::market(Side::Sell, qty));
        }
        return out;
    }
};

} // namespace px::arena::strategies
