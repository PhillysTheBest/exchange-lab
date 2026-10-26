// arena/strategies/placebo.hpp — the empty seat: a strategy that never acts.
//
// It exists for the standard experiment runner (arena/suite.hpp, docs/RUNNER.md).
// Every candidate is run in one seat appended after the reference line-up, and
// the placebo is what that seat holds in the control arm. Its value is exactly
// zero on every seed, so "a candidate against the placebo" is "a candidate
// against doing nothing", and each reference participant's account in the
// placebo arm is its counterfactual for that seed.
//
// INVISIBLE ONLY WHEN LAST. The scheduler orders wakes by (time, sequence), and
// the sequence number is assigned when an item is pushed (arena/clock.hpp). An
// inert seat's wakes take sequence numbers, but processing them pushes nothing
// for anybody else, so every other participant's items come out in the same
// relative order and the engine sees the same requests. Appended after the
// line-up, the placebo therefore leaves the session hash exactly as it was:
// 2853d2127a3486c5 under test_arena's small session, with or without it.
// Inserted anywhere earlier it is not invisible, because every later
// participant's owner id (index + 1) and reset seed shift: at index 3 the same
// session hashes to 5a9a688a7ca5b9f8. test_placebo_is_inert holds both facts.
//
// Written against arena/strategy.hpp alone, like the worked example in fader.hpp.
#pragma once

#include "arena/strategy.hpp"

namespace px::arena::strategies {

struct Placebo {
    const char* name = "placebo";

    // Irrelevant: it is inert at any delay. The runner overrides the seat's
    // delay anyway.
    SimTime reaction_ns = 10'000;
    SimTime timer_ns = 0;

    void reset(std::uint64_t) {}

    ActionBatch on_event(const MarketView&) {
        ActionBatch out;
        return out;
    }
};

} // namespace px::arena::strategies
