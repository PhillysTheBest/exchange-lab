// arena/bots/fader.hpp — the worked example: a bot written from the contract.
//
// This exists to keep `docs/ARENA.md`'s claim that someone can add a strategy
// without reading the engine from being a claim. Everything it uses is in
// arena/bot.hpp: a view in, a batch of actions out. It never includes the
// engine, the session, the scheduler or the book mirror directly, and it could
// not reach them if it tried.
//
// The strategy, such as it is: lean against the recent move. It keeps a slow
// average of the traded price and takes the other side when the last print is
// far enough from that average, which makes it a counterparty for the momentum
// bot and a supplier of adverse flow to whichever maker is quoting the extreme.
// It is uninformed — it cannot see the fundamental value, so `v.signal` is
// always 0 for it — and it is not meant to be good.
//
// Integer only, like everything inside a session: no floating point, so replay
// stays bit-identical across platforms (see arena/clock.hpp).
#pragma once

#include "arena/bot.hpp"

namespace px::arena::bots {

struct Fader {
    const char* name = "fader";

    SimTime reaction_ns = 30'000;
    SimTime timer_ns = 0; // purely event-driven: it reacts to prints

    std::int64_t trigger_ticks = 6; // how far from its average is worth fading
    Qty qty = 15;
    std::int64_t max_position = 300;

    void reset(std::uint64_t) {
        average_ = 0;
        seen_ = 0;
    }

    ActionBatch on_event(const MarketView& v) {
        ActionBatch out;
        const Price last = v.last_trade();
        if (last == 0) return out;

        // A slow integer average of where the market has been trading. Needs a
        // few prints before it means anything.
        if (seen_ < 8) {
            average_ = average_ == 0 ? last : average_ + (last - average_) / 2;
            ++seen_;
            return out;
        }
        const Price reference = average_;
        average_ += (last - average_) / 8;

        if (v.in_flight != 0) return out;

        const Price trigger = trigger_ticks * v.tick;
        // Price ran up: sell into it, if there is a bid to sell to and room to
        // go shorter.
        if (last > reference + trigger && v.best(Side::Buy) != 0 && v.position > -max_position) {
            out.push(Action::market(Side::Sell, qty));
        } else if (last < reference - trigger && v.best(Side::Sell) != 0 && v.position < max_position) {
            out.push(Action::market(Side::Buy, qty));
        }
        return out;
    }

private:
    Price average_ = 0;
    int seen_ = 0;
};

} // namespace px::arena::bots
