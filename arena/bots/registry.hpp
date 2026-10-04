// arena/bots/registry.hpp — the single list of participants in play.
//
// ADDING A BOT: write a header beside this one, include it, and add one line to
// build_roster(). If it has anything worth sweeping, add its fields to
// RosterConfig and one line each to set_roster_param(). That is the whole
// extension mechanism — no registration macro, no static initialiser ordering,
// no plugin loading. Strategies link into the binary.
//
// WHY THERE ARE TWO MAKERS. The swept maker quotes against a rival maker whose
// reaction delay is held fixed and fast. That is not decoration: with a single
// maker, slowing it down does not measure what its delay is worth, it measures
// how much the whole market depends on it. A lone stale maker *is* the price, so
// the uninformed flow keeps trading against its stale quotes and hands it money,
// and the first sweep duly showed a slower maker earning more. With a fast rival
// in the book the price stays near fair value whatever the swept maker does, and
// the question becomes the real one: what does my delay cost me against
// competitors who do not have it? Latency is worth something relative to
// somebody, and the rival is the somebody.
//
// WHO IS INFORMED. Exactly one entry may be marked informed, and build_roster()
// is the only place that decides. Marking it here rather than inside the bot
// keeps the information asymmetry visible in one file: a reader can see, without
// reading any strategy, that precisely one participant is given the fundamental
// value and the rest must infer price from the book.
//
// OWNER IDS are assigned by the session as (roster index + 1), so every
// participant is a distinct owner to the engine. That matters more than it
// looks: self-trade prevention *cancels* a resting order rather than trading it,
// so sharing one owner between bots would silently delete the maker's quotes
// instead of filling them, and the whole experiment would be measuring an
// artefact. Distinct owners confine self-trade prevention to a bot crossing
// itself, which the maker's spread makes impossible.
#pragma once

#include "arena/bot.hpp"
#include "arena/bots/fader.hpp"
#include "arena/bots/informed.hpp"
#include "arena/bots/market_maker.hpp"
#include "arena/bots/momentum.hpp"
#include "arena/bots/noise.hpp"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace px::arena {

struct RosterConfig {
    Price anchor = 100 * kPriceScale; // the previous close, known to everyone
    Price tick = 100;

    // The maker: the participant the headline experiment sweeps.
    SimTime maker_reaction_ns = 10'000;
    std::int64_t maker_half_spread_ticks = 2;
    std::int64_t maker_quote_qty = 50;
    // Big enough that the book stays two-sided. At 500 a maker ran out of
    // inventory room after ten net fills and withdrew one side, so 42% of the
    // session had no two-way market; at 2,000 that is 0.3%. The limit still
    // bites sometimes, which is the point of having one.
    std::int64_t maker_max_position = 2'000;
    // Inventory skew is not only risk management, it is the other half of price
    // discovery: if the estimate drifts below fair value the informed trader
    // buys, the maker goes short, and the lean pushes its quotes back up. Found
    // by sweeping, not by thinking — at 4 the mean |mid - V| was 11 ticks, at 40
    // it is 3. Expressed at the inventory limit, so it does not multiply with it.
    std::int64_t maker_skew_ticks_at_limit = 40;
    // The learning rate: how far one full quote's worth of aggressive flow moves
    // the estimate, in hundredths of a tick. There is an optimum and it is not
    // obvious — too low and the estimate never finds the price, too high and the
    // makers chase uninformed flow. Sweeping it puts the best tracking near 15
    // now that they read the whole tape rather than only their own fills, which
    // is roughly four times as much evidence per look.
    std::int64_t maker_impact_centiticks = 15;

    // The rival maker: same strategy, fixed fast delay, never swept. It is the
    // competition, and it is what keeps the book honest when the swept maker is
    // slow. Turn it off to see the degenerate single-maker market for yourself.
    bool rival_enabled = true;
    SimTime rival_reaction_ns = 1'000;
    std::int64_t rival_half_spread_ticks = 2;
    std::int64_t rival_quote_qty = 50;
    // -1 means "whatever the swept maker uses", which is the default and keeps
    // the two in step. Setting it separately is what makes a positive control
    // possible: freeze the swept maker's learning while the rival keeps anchoring
    // the book to the fundamental, so a stale quote can be picked off in a market
    // that is still correctly priced.
    std::int64_t rival_impact_centiticks = -1;

    // The informed trader: turn it off for the null run.
    bool informed_enabled = true;
    SimTime informed_reaction_ns = 1'000;
    std::int64_t informed_edge_ticks = 1;
    std::int64_t informed_qty = 20;
    // Big enough that it is not the binding constraint on price discovery. At
    // 1,000 it ran out of room correcting the book, stopped trading, and the
    // price came loose from the fundamental: the mean |mid - V| was 26 ticks
    // with a 4-tick spread. At 5,000 it is 1 tick. The informed trader is what
    // ties the market to the value nobody can see, so starving it of capacity
    // breaks the session rather than making it conservative.
    std::int64_t informed_max_position = 5'000;

    // Uninformed flow.
    int noise_count = 2;
    SimTime noise_reaction_ns = 50'000;
    SimTime noise_timer_ns = 10'000;
    std::int64_t noise_qty = 25;
    std::int64_t noise_pct_market = 25;

    bool momentum_enabled = true;
    SimTime momentum_reaction_ns = 20'000;
    std::int64_t momentum_qty = 20;

    // The worked example from docs/ARENA.md. Off by default, so adding it did
    // not move a single published number.
    bool fader_enabled = false;
    SimTime fader_reaction_ns = 30'000;
    std::int64_t fader_qty = 15;
    std::int64_t fader_trigger_ticks = 6;
};

struct Roster {
    std::vector<std::unique_ptr<BotHandle>> bots;
    std::vector<bool> informed; // parallel to `bots`; at most one is true
    int maker_index = -1;       // the participant the sweep is usually about
    int rival_index = -1;       // its competition, held fixed

    [[nodiscard]] std::size_t size() const noexcept { return bots.size(); }
};

[[nodiscard]] inline Roster build_roster(const RosterConfig& cfg) {
    Roster r;

    bots::MarketMaker maker;
    maker.reaction_ns = cfg.maker_reaction_ns;
    maker.anchor = cfg.anchor;
    maker.half_spread_ticks = cfg.maker_half_spread_ticks;
    maker.quote_qty = static_cast<Qty>(cfg.maker_quote_qty);
    maker.max_position = cfg.maker_max_position;
    maker.skew_ticks_at_limit = cfg.maker_skew_ticks_at_limit;
    maker.impact_centiticks = cfg.maker_impact_centiticks;
    r.maker_index = static_cast<int>(r.bots.size());
    r.bots.push_back(make_bot(maker));
    r.informed.push_back(false);

    if (cfg.rival_enabled) {
        bots::MarketMaker rival;
        rival.name = "rival";
        rival.reaction_ns = cfg.rival_reaction_ns;
        rival.anchor = cfg.anchor;
        rival.half_spread_ticks = cfg.rival_half_spread_ticks;
        rival.quote_qty = static_cast<Qty>(cfg.rival_quote_qty);
        rival.max_position = cfg.maker_max_position;
        rival.skew_ticks_at_limit = cfg.maker_skew_ticks_at_limit;
        rival.impact_centiticks = cfg.rival_impact_centiticks < 0 ? cfg.maker_impact_centiticks
                                                                 : cfg.rival_impact_centiticks;
        r.rival_index = static_cast<int>(r.bots.size());
        r.bots.push_back(make_bot(rival));
        r.informed.push_back(false);
    }

    if (cfg.informed_enabled) {
        bots::Informed informed;
        informed.reaction_ns = cfg.informed_reaction_ns;
        informed.edge_ticks = cfg.informed_edge_ticks;
        informed.qty = static_cast<Qty>(cfg.informed_qty);
        informed.max_position = cfg.informed_max_position;
        r.bots.push_back(make_bot(informed));
        r.informed.push_back(true);
    }

    for (int i = 0; i < cfg.noise_count; ++i) {
        bots::Noise noise;
        noise.reaction_ns = cfg.noise_reaction_ns;
        // Stagger the heartbeats so the traders do not all arrive in lockstep.
        noise.timer_ns = cfg.noise_timer_ns + i * (cfg.noise_timer_ns / 8 + 1);
        noise.anchor = cfg.anchor;
        noise.qty = static_cast<Qty>(cfg.noise_qty);
        noise.pct_market = static_cast<unsigned>(cfg.noise_pct_market);
        r.bots.push_back(make_bot(noise));
        r.informed.push_back(false);
    }

    if (cfg.fader_enabled) {
        bots::Fader fader;
        fader.reaction_ns = cfg.fader_reaction_ns;
        fader.qty = static_cast<Qty>(cfg.fader_qty);
        fader.trigger_ticks = cfg.fader_trigger_ticks;
        r.bots.push_back(make_bot(fader));
        r.informed.push_back(false);
    }

    if (cfg.momentum_enabled) {
        bots::Momentum momentum;
        momentum.reaction_ns = cfg.momentum_reaction_ns;
        momentum.qty = static_cast<Qty>(cfg.momentum_qty);
        r.bots.push_back(make_bot(momentum));
        r.informed.push_back(false);
    }

    return r;
}

// Sets one roster field by name, for the sweep runner. Returns false for an
// unknown name so that a typo on the command line is an error rather than a
// silently ignored flag.
[[nodiscard]] inline bool set_roster_param(RosterConfig& c, std::string_view key, std::int64_t value) {
    if (key == "maker.reaction_ns") c.maker_reaction_ns = value;
    else if (key == "maker.half_spread_ticks") c.maker_half_spread_ticks = value;
    else if (key == "maker.quote_qty") c.maker_quote_qty = value;
    else if (key == "maker.max_position") c.maker_max_position = value;
    else if (key == "maker.skew_ticks_at_limit") c.maker_skew_ticks_at_limit = value;
    else if (key == "maker.impact_centiticks") c.maker_impact_centiticks = value;
    else if (key == "rival.enabled") c.rival_enabled = value != 0;
    else if (key == "rival.reaction_ns") c.rival_reaction_ns = value;
    else if (key == "rival.half_spread_ticks") c.rival_half_spread_ticks = value;
    else if (key == "rival.quote_qty") c.rival_quote_qty = value;
    else if (key == "rival.impact_centiticks") c.rival_impact_centiticks = value;
    else if (key == "informed.enabled") c.informed_enabled = value != 0;
    else if (key == "informed.reaction_ns") c.informed_reaction_ns = value;
    else if (key == "informed.edge_ticks") c.informed_edge_ticks = value;
    else if (key == "informed.qty") c.informed_qty = value;
    else if (key == "informed.max_position") c.informed_max_position = value;
    else if (key == "noise.count") c.noise_count = static_cast<int>(value);
    else if (key == "noise.reaction_ns") c.noise_reaction_ns = value;
    else if (key == "noise.timer_ns") c.noise_timer_ns = value;
    else if (key == "noise.qty") c.noise_qty = value;
    else if (key == "noise.pct_market") c.noise_pct_market = value;
    else if (key == "momentum.enabled") c.momentum_enabled = value != 0;
    else if (key == "momentum.reaction_ns") c.momentum_reaction_ns = value;
    else if (key == "momentum.qty") c.momentum_qty = value;
    else if (key == "fader.enabled") c.fader_enabled = value != 0;
    else if (key == "fader.reaction_ns") c.fader_reaction_ns = value;
    else if (key == "fader.qty") c.fader_qty = value;
    else if (key == "fader.trigger_ticks") c.fader_trigger_ticks = value;
    else return false;
    return true;
}

// Every name set_roster_param() accepts, for --help. Kept in step with the
// function above by test_arena, which asserts that each name listed here is
// actually settable — otherwise this comment rots the first time a bot changes.
inline constexpr const char* kRosterParamNames =
    "maker.reaction_ns maker.half_spread_ticks maker.quote_qty maker.max_position\n"
    "  maker.skew_ticks_at_limit maker.impact_centiticks rival.enabled rival.reaction_ns\n"
    "  rival.half_spread_ticks rival.quote_qty rival.impact_centiticks informed.enabled\n"
    "  informed.reaction_ns informed.edge_ticks informed.qty informed.max_position\n"
    "  noise.count\n"
    "  noise.reaction_ns noise.timer_ns noise.qty noise.pct_market momentum.enabled\n"
    "  momentum.reaction_ns momentum.qty fader.enabled fader.reaction_ns fader.qty\n"
    "  fader.trigger_ticks";

} // namespace px::arena
