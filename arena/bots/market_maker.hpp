// arena/bots/market_maker.hpp — quotes both sides, learns from who trades with
// it, and manages inventory.
//
// This is the participant the headline experiment is about. It has no
// information: it cannot see the fundamental value, so everything it believes
// about price it has inferred from its own fills.
//
// HOW IT LEARNS, AND WHY IT IS THE ONLY WAY THAT WORKS. A dealer's quotes are
// its belief about value. The update is Glosten and Milgrom's, and it is about
// *direction*, not price: a trade where the aggressor bought is evidence that
// value is higher than I thought, so I raise my estimate. Uninformed flow
// arrives on both sides and cancels out; informed flow arrives on one side and
// pushes the estimate toward the truth. So the price discovers the fundamental
// without any participant being told what it is, which is the result the whole
// session depends on.
//
// It learns from the WHOLE TAPE, not only from its own fills, because the feed
// states the aggressor's side of every trade. That matters more than it sounds:
// learning only from its own fills, the restoring force toward fair value is
// proportional to the share of informed volume that happened to hit this maker,
// while the random walk from uninformed fills is not. Sessions drifted — the mid
// sat twenty-six ticks from the fundamental against a four-tick spread, and the
// adverse-selection signal disappeared into the mispricing. Reading the public
// tape makes every maker see the informed trader's footprint, which is both how
// real price discovery works and what holds the book near the fundamental.
//
// The first version of this file instead chased the last traded price — follow
// the tape, which sounds sensible and is catastrophic. The last trade is the
// maker's own quote, so the estimate was a function of itself: a pure random
// walk with nothing anchoring it. Over a session the book wandered thousands of
// ticks from fair value, the informed trader ran into its position limit trying
// to drag it back and then stopped correcting at all, and every number
// downstream became a measurement of that drift. The direction update has no
// such loop, because what it reads — which side got filled — is the one thing
// the maker cannot manufacture by quoting.
//
// Its defences, which are the only two a real maker has:
//   * update beliefs on every fill, so a quote that is wrong gets fixed;
//   * lean against inventory, because being run over on one side shows up as a
//     position before it shows up as a loss.
// Both only happen when the bot is awake, so a bot that is awake more often
// defends better. Nothing in here reads reaction_ns: the advantage of being fast
// is mechanical, not asserted.
//
// It will not quote while it has an order in flight. A maker that re-quoted
// before seeing its own acknowledgement would double its exposure every reaction
// window, which is a bug in a real system and would be an artefact here.
#pragma once

#include "arena/bot.hpp"

namespace px::arena::bots {

struct MarketMaker {
    const char* name = "maker";

    SimTime reaction_ns = 10'000;
    SimTime timer_ns = 50'000; // a slow heartbeat, so it re-quotes in a dead market

    Price anchor = 100 * kPriceScale; // the previous close: public knowledge
    std::int64_t half_spread_ticks = 2;
    Qty quote_qty = 50;
    std::int64_t max_position = 500;
    // How far the quotes lean when inventory is at the limit, in ticks. Stated
    // this way rather than per unit held, because per-unit skew multiplies with
    // max_position: raising the limit from 500 to 2,000 to keep the book
    // two-sided also quadrupled the lean and pushed the mid eleven ticks off the
    // fundamental. Tying it to the limit keeps the two knobs independent.
    std::int64_t skew_ticks_at_limit = 4;
    // How far one full-size fill moves the estimate, in hundredths of a tick.
    // This is the learning rate: too low and the maker never finds the price,
    // too high and it chases its own uninformed flow.
    std::int64_t impact_centiticks = 60;

    void reset(std::uint64_t) {
        est_ = anchor;
        last_volume_ = 0;
        seen_flow_ = false;
    }

    ActionBatch on_event(const MarketView& v) {
        ActionBatch out;
        learn(v);
        // Wait to see its own acknowledgements before touching the market again.
        if (v.in_flight != 0) return out;

        const Price tick = v.tick;
        const Price skew = max_position == 0 ? 0 : v.position * skew_ticks_at_limit * tick / max_position;
        const Price centre = est_ - skew;
        // Symmetric around the centre. An earlier version added a tick to the
        // ask "to be safe", which quietly handed the maker a free tick on every
        // sale and showed up as an edge the experiment never meant to grant.
        const Price want_bid = snap_to_tick(centre - half_spread_ticks * tick, tick);
        const Price want_ask = snap_to_tick(centre + half_spread_ticks * tick, tick);

        // Inventory limits: stop adding to the side that is already too big.
        const bool quote_bid = v.position < max_position && want_bid > 0;
        const bool quote_ask = v.position > -max_position && want_ask > want_bid;

        // What is resting now, as far as its own feed has told it.
        OrderId live_bid = 0, live_ask = 0;
        Price at_bid = 0, at_ask = 0;
        for (std::size_t i = 0; i < v.my_order_count; ++i) {
            const BookMirror::OrderRef& o = v.my_orders[i];
            if (o.side == Side::Buy) {
                live_bid = o.id;
                at_bid = o.price;
            } else {
                live_ask = o.id;
                at_ask = o.price;
            }
        }

        // Cancel-replace only what actually moved: a quote that is still where
        // it belongs keeps its place in the queue, which is the thing worth most
        // to a maker.
        if (live_bid != 0 && (!quote_bid || at_bid != want_bid)) out.push(Action::cancel(live_bid));
        if (live_ask != 0 && (!quote_ask || at_ask != want_ask)) out.push(Action::cancel(live_ask));
        if (quote_bid && (live_bid == 0 || at_bid != want_bid)) {
            out.push(Action::limit(Side::Buy, want_bid, quote_qty));
        }
        if (quote_ask && (live_ask == 0 || at_ask != want_ask)) {
            out.push(Action::limit(Side::Sell, want_ask, quote_qty));
        }
        return out;
    }

private:
    // The Bayesian update, read off the order-flow imbalance the maker missed
    // while it was not looking: buyers aggressing means value is probably higher.
    void learn(const MarketView& v) {
        if (v.book == nullptr) return;
        const std::int64_t volume = v.book->signed_volume();
        if (!seen_flow_) {
            last_volume_ = volume;
            seen_flow_ = true;
            return;
        }
        const std::int64_t dv = volume - last_volume_;
        last_volume_ = volume;
        if (dv == 0 || quote_qty == 0) return;
        const std::int64_t magnitude = dv < 0 ? -dv : dv;
        // Scaled by how much of a full quote's worth traded, and capped at three
        // quotes so one sweep cannot throw the estimate across the book.
        std::int64_t units = magnitude * 100 / static_cast<std::int64_t>(quote_qty);
        if (units > 300) units = 300;
        const Price move = v.tick * impact_centiticks * units / 10'000;
        est_ += dv > 0 ? move : -move;
    }

    Price est_ = 100 * kPriceScale;
    std::int64_t last_volume_ = 0;
    bool seen_flow_ = false;
};

} // namespace px::arena::bots
