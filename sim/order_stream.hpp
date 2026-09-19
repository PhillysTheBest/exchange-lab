// sim/order_stream.hpp — a seeded, fully pre-generated synthetic order flow.
//
// Built entirely before any timing starts, so generation cost (RNG, branching)
// never lands inside a measured interval. Only raw std::mt19937_64 output is used
// — never a std:: distribution, whose output is implementation-defined — so a
// seed produces the same stream on any conforming standard library.
//
// Model, per instrument: a mid price does a mean-reverting random walk that can
// never drift more than `max_mid_drift_ticks` from its start, which bounds how
// many distinct price levels can exist (well under EngineConfig::levels_per_side).
// Each command is one of
//   passive      GTC limit behind the mid, mostly within 20 ticks (two thirds of
//                those within 5); a configurable share spread further out, up to
//                `far_offset_max_ticks` (50 by default, 1500 in deep_profile())
//   marketable   limit 0-5 ticks through the mid, 70% GTC / 30% IOC: takes liquidity
//   market       IOC sweep of any depth
//   cancel       of an order that is really still resting — the generator replays
//                the stream through a private engine as it goes, so it knows,
//                like a real participant cancelling its own quotes. A small,
//                explicit share of cancels (`pct_late_cancel`) deliberately
//                targets an order that has just filled, as happens in practice
//                when a cancel races a fill; those are answered UnknownOrder.
// Resting orders are capped (`max_resting`): at the cap the next command is a
// cancel, which bounds the book's size.
#pragma once

#include "px/matching_engine.hpp"
#include "px/messages.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

namespace px::sim {

struct StreamConfig {
    std::uint64_t seed = 42;
    std::size_t orders = 1'000'000;
    std::uint16_t instruments = 4;
    Price tick = 100;                     // $0.01
    Price start_mid = 100 * kPriceScale;  // $100.00
    std::int64_t max_mid_drift_ticks = 500;
    // Passive offsets from the mid: (100 - pct_far_passive)% land within 20 ticks
    // (two thirds of those within 5), the rest spread over 21..far_offset_max_ticks.
    unsigned pct_far_passive = 10;
    std::int64_t far_offset_max_ticks = 50;
    std::uint32_t owners = 64;
    std::size_t max_resting = 20'000;
    unsigned pct_passive = 50;
    unsigned pct_marketable = 15;
    unsigned pct_market = 5;              // cancels are the remaining 30%
    unsigned pct_late_cancel = 5;         // of cancels: target an already-filled order
};

struct StreamMix {
    std::size_t passive = 0, marketable_gtc = 0, marketable_ioc = 0, market = 0, cancel = 0, late_cancel = 0;
};

// The deep-book profile: half the passive orders spread up to 1500 ticks from a
// mid that drifts at most 200 ticks, and up to 60k resting orders, so each side
// holds well over a thousand price levels (every price stays within 1700 ticks
// of the start, inside the default 2048 levels per side).
[[nodiscard]] inline StreamConfig deep_profile(StreamConfig c) {
    c.pct_far_passive = 50;
    c.far_offset_max_ticks = 1500;
    c.max_mid_drift_ticks = 200;
    c.max_resting = 60'000;
    return c;
}

// Registers the stream's instruments ("SYM00", "SYM01", ...) on an engine.
inline void add_instruments(MatchingEngine& engine, const StreamConfig& cfg) {
    for (std::uint16_t i = 0; i < cfg.instruments; ++i) {
        char sym[16];
        std::snprintf(sym, sizeof sym, "SYM%02u", static_cast<unsigned>(i));
        engine.add_instrument(sym, cfg.tick);
    }
}

[[nodiscard]] inline std::vector<OrderRequest> generate_order_stream(const StreamConfig& cfg,
                                                                     StreamMix* mix_out = nullptr) {
    std::mt19937_64 rng(cfg.seed);
    std::vector<OrderRequest> out;
    out.reserve(cfg.orders);

    auto shadow = std::make_unique<MatchingEngine>();
    add_instruments(*shadow, cfg);
    const EventSink discard(nullptr, [](void*, const ExecEvent&) {});

    struct Known {
        OrderId id;
        OwnerId owner;
        InstrumentId instr;
    };
    std::vector<Known> resting;          // GTC orders not yet cancelled by us (some may have filled)
    std::vector<Known> filled(256, Known{0, 0, 0}); // ring of recently filled ids, for late cancels
    std::size_t filled_next = 0, filled_count = 0;
    std::vector<Price> mid(cfg.instruments, cfg.start_mid);
    StreamMix mix;

    const std::int64_t drift = cfg.max_mid_drift_ticks;
    const unsigned passive_end = cfg.pct_passive;
    const unsigned marketable_end = passive_end + cfg.pct_marketable;
    const unsigned market_end = marketable_end + cfg.pct_market;
    const unsigned mid_end = 100 - cfg.pct_far_passive;
    const unsigned near_end = mid_end * 2 / 3;
    const auto far_span = static_cast<std::uint64_t>(cfg.far_offset_max_ticks - 20);

    // Pops a uniformly random entry of `resting` that is still live, moving the
    // filled ones it meets into the recently-filled ring. False if none is left.
    const auto take_live = [&](Known& chosen) {
        while (!resting.empty()) {
            const std::size_t j = rng() % resting.size();
            const Known k = resting[j];
            resting[j] = resting.back();
            resting.pop_back();
            if (shadow->is_live(k.id)) {
                chosen = k;
                return true;
            }
            filled[filled_next] = k;
            filled_next = (filled_next + 1) % filled.size();
            if (filled_count < filled.size()) ++filled_count;
        }
        return false;
    };

    for (std::size_t i = 0; i < cfg.orders; ++i) {
        const OrderId id = static_cast<OrderId>(i + 1);
        const auto instr = static_cast<InstrumentId>(rng() % cfg.instruments);

        // Mean-reverting walk: the further the mid has drifted up, the likelier
        // the next step is down, reaching certainty at +/- drift ticks.
        Price& m = mid[instr];
        if (rng() % 100 < 10) {
            const std::int64_t dev = (m - cfg.start_mid) / cfg.tick;
            const bool up = static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(2 * drift)) < drift - dev;
            m += up ? cfg.tick : -cfg.tick;
        }

        const auto owner = static_cast<OwnerId>(rng() % cfg.owners);
        const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
        const auto qty = static_cast<Qty>(1 + rng() % 500);

        unsigned roll = static_cast<unsigned>(rng() % 100);
        if (shadow->resting_orders() >= cfg.max_resting) roll = 99; // at the cap: cancel

        OrderRequest req;
        bool built = false;
        if (roll >= market_end) {
            if (filled_count > 0 && rng() % 100 < cfg.pct_late_cancel) {
                const Known k = filled[rng() % filled_count];
                req = cancel_order(k.id, k.instr, k.owner);
                ++mix.late_cancel;
                built = true;
            } else {
                Known k{};
                if (take_live(k)) {
                    req = cancel_order(k.id, k.instr, k.owner);
                    ++mix.cancel;
                    built = true;
                } else {
                    roll = 0; // nothing resting yet: add liquidity instead
                }
            }
        }
        if (!built) {
            if (roll < passive_end) {
                const auto r = rng() % 100;
                const std::int64_t offset = r < near_end ? 1 + static_cast<std::int64_t>(rng() % 5)
                                          : r < mid_end  ? 6 + static_cast<std::int64_t>(rng() % 15)
                                                         : 21 + static_cast<std::int64_t>(rng() % far_span);
                const Price px = side == Side::Buy ? m - offset * cfg.tick : m + offset * cfg.tick;
                req = limit_order(id, instr, owner, side, px, qty, Tif::GTC);
                ++mix.passive;
            } else if (roll < marketable_end) {
                const auto through = static_cast<std::int64_t>(rng() % 6);
                const Price px = side == Side::Buy ? m + through * cfg.tick : m - through * cfg.tick;
                const Tif tif = rng() % 10 < 7 ? Tif::GTC : Tif::IOC;
                req = limit_order(id, instr, owner, side, px, qty, tif);
                ++(tif == Tif::GTC ? mix.marketable_gtc : mix.marketable_ioc);
            } else {
                req = market_order(id, instr, owner, side, qty);
                ++mix.market;
            }
        }

        shadow->process(req, discard);
        if (req.type == ReqType::New && req.ord == OrdType::Limit && req.tif == Tif::GTC) {
            resting.push_back({req.order_id, req.owner, req.instr});
        }
        out.push_back(req);
    }
    if (mix_out) *mix_out = mix;
    return out;
}

} // namespace px::sim
