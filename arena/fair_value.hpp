// arena/fair_value.hpp — the fundamental value, and the only information in a
// session that does not come out of the order book.
//
// WHY THIS EXISTS. Without it the headline experiment is circular. If the only
// participants are strategies trading off each other's quotes, then "the slow market
// maker got picked off" is true by construction: it happens because a sniper was
// written to do it, and the resulting curve measures that sniper's parameters,
// not the value of being fast. Latency only has a price when somebody else knows
// something, so the session gives one participant real information and makes the
// market maker earn its spread against traders who have none.
//
// That is Glosten and Milgrom's setting, which is the point: the market maker
// collects the spread from uninformed flow and pays it back to an informed
// trader, and its reaction delay decides how much of the second it suffers. The
// qualitative shape is therefore predicted in advance rather than discovered
// afterwards, and a curve that came out flat — or upside down — would be a
// finding about the implementation, not a disappointing plot.
//
// THE PROCESS. A jump process on the tick grid: at integer intervals drawn from
// the session RNG, V moves one tick up or down with equal probability. No
// floating point, no libm: see the determinism rules in arena/clock.hpp.
//
// IT MUST BE A MARTINGALE, and the first version of this file was not. A pull
// back toward the opening price looks harmless and bounds the walk, but it pays
// a market maker for holding inventory: price deviates, the maker's skew leaves
// it leaning the right way, and the drift hands it the move back. That effect
// swamped adverse selection completely — the first sweep showed *positive*
// markouts rising with delay, which is to say the slower maker looked better,
// which is nonsense. With a fair coin, the best estimate of V at the markout
// horizon is V now, so a fill's markout measures only what the filler did not
// know, which is the thing being studied.
//
// The walk still cannot escape: `band_ticks` is a reflecting wall, far enough
// out that a session never reaches it, present only so that no price can run
// negative or off the engine's tick grid. If a session ever touches the wall
// the walk stops being a martingale there, so `wall_hits` is counted and
// reported rather than hidden.
//
// Who may look at V:
//   * the informed strategy, which is the entire mechanism;
//   * the accounting, which marks inventory and computes fill markouts against
//     it — V is the only defensible mark in a closed world, and marking against
//     the mid would let a participant improve its own P&L by moving the mid;
//   * the renderer, which draws it as the line the book is chasing.
// The market maker, the noise traders and the momentum strategy may not, and the
// session does not pass it to them.
#pragma once

#include "arena/clock.hpp"
#include "px/types.hpp"

#include <cstdint>

namespace px::arena {

struct FairValueConfig {
    Price start = 100 * kPriceScale;
    Price tick = 100;
    SimTime step_mean_ns = 100'000; // mean time between jumps
    // A reflecting wall, not a spring: far enough out that a session should
    // never see it. See the martingale note above.
    std::int64_t band_ticks = 2'000;
};

class FairValue {
public:
    FairValue() = default;

    void reset(const FairValueConfig& cfg, std::uint64_t seed) {
        cfg_ = cfg;
        rng_.reset(seed);
        v_ = cfg.start;
        steps_ = 0;
        last_step_at_ = 0;
        wall_hits_ = 0;
        up_ = true;
    }

    [[nodiscard]] Price value() const noexcept { return v_; }
    [[nodiscard]] std::uint64_t steps() const noexcept { return steps_; }
    [[nodiscard]] SimTime last_step_at() const noexcept { return last_step_at_; }
    // Which way the most recent jump went: the renderer draws it, and nothing in
    // a strategy's decision path is allowed to read it.
    [[nodiscard]] bool last_was_up() const noexcept { return up_; }

    [[nodiscard]] SimTime next_interval() noexcept { return rng_.interval(cfg_.step_mean_ns); }

    [[nodiscard]] std::uint64_t wall_hits() const noexcept { return wall_hits_; }

    // One jump: a fair coin, so that E[V(t+h)] == V(t) and a markout measures
    // information rather than drift. The only exception is the reflecting wall,
    // which a session is not expected to reach; every time it does is counted.
    void step(SimTime at) noexcept {
        const std::int64_t dev = (v_ - cfg_.start) / cfg_.tick;
        const std::int64_t band = cfg_.band_ticks <= 0 ? 1 : cfg_.band_ticks;
        if (dev >= band) {
            up_ = false;
            ++wall_hits_;
        } else if (dev <= -band) {
            up_ = true;
            ++wall_hits_;
        } else {
            up_ = (rng_.next() & 1) != 0;
        }
        v_ += up_ ? cfg_.tick : -cfg_.tick;
        last_step_at_ = at;
        ++steps_;
    }

private:
    FairValueConfig cfg_{};
    SimRng rng_{42};
    Price v_ = 100 * kPriceScale;
    std::uint64_t steps_ = 0;
    SimTime last_step_at_ = 0;
    std::uint64_t wall_hits_ = 0;
    bool up_ = true;
};

} // namespace px::arena
