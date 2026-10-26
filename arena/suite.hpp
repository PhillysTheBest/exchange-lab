// arena/suite.hpp — the standard experiment: one fixed market, one seat, a named
// block of seeds. Defines the conditions and runs arms; reports nothing.
//
// The design, and the measurements behind every choice in it, are in
// docs/RUNNER.md. The short version:
//
//   THE SEAT. A candidate strategy is appended after the reference line-up, at
//   index 6, owner 7. An inert participant appended last is invisible: it takes
//   scheduler sequence numbers but pushes nothing for anyone else, so the other
//   participants' wakes, the engine's requests and the session hash are exactly
//   those of the market without it (arena/strategies/placebo.hpp). So the only
//   difference between a candidate's arm and the placebo arm is the candidate's
//   own orders. The seat is never informed, and its reaction delay is set by the
//   suite, not by the candidate, so a comparison measures strategy logic rather
//   than a delay the candidate chose for itself.
//
//   SIMULATED TIME, NOT REQUESTS. A session ends at 25 ms of simulated time,
//   with no request bound and no stall exit. A request bound would be spent
//   partly by the candidate's own orders, so a busier candidate would end
//   everyone's session earlier and be marked at a different closing value of
//   the fundamental. Under a time bound the fundamental's path and its closing
//   value are common to every arm on a seed, because the fundamental draws only
//   from its own seeded stream.
//
//   THE HEALTH GATE. The project's rule is that a book that has come loose
//   from the fundamental is not a market and nothing read off one counts. The
//   gate is that rule as an integer decision per session; an arm counts only
//   if it passes on every seed, and failing seeds are never dropped.
//
// Everything in this header is integer. Floating point appears only in
// arena/suite_report.hpp, after every session has finished, the same split
// arena/sweep.cpp draws. make test-cli greps this file to keep it that way.
#pragma once

#include "arena/session.hpp"
#include "arena/strategies/registry.hpp"
#include "arena/strategy.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace px::arena::suite {

// ---- seeds ----------------------------------------------------------------

// Seeds come in named blocks only. A free seed list would make the conditions
// whatever somebody typed, which is what the standard suite exists to prevent.
struct SeedBlock {
    const char* name;
    std::uint64_t first;
    std::uint32_t count;
    // A block kept for confirming a finished strategy once, never for tuning. Not
    // a condition: conditions_text() leaves it out, so it moves no fingerprint.
    bool confirmatory = false;
};

// `smoke` is a prefix of `standard`, so smoke numbers are a subset of standard
// ones. It shows that a strategy runs and trades; it cannot separate anything.
// `holdout` overlaps neither, and has as many seeds as `standard` so that a
// confirmation is as precise as the exploration it checks. No test, example or
// document runs a candidate on it: that is what keeps it a holdout.
inline constexpr SeedBlock kSeedBlocks[] = {
    {"standard", 42, 64},
    {"smoke", 42, 4},
    {"holdout", 1000000, 64, true},
};

[[nodiscard]] inline const SeedBlock* find_block(std::string_view name) noexcept {
    for (const SeedBlock& b : kSeedBlocks) {
        if (name == b.name) return &b;
    }
    return nullptr;
}

// ---- the health gate --------------------------------------------------------

struct HealthGate {
    // Mean |mid - V| at most this many ticks: one quoted maker spread.
    std::uint64_t max_mean_gap_ticks = 4;
    // The book one-sided at most this share of the fundamental's steps.
    std::uint64_t max_one_sided_pct = 5;
};

enum class EndedBy : std::uint8_t { Time = 0, Ceiling };

[[nodiscard]] inline const char* ended_by_name(EndedBy e) noexcept {
    return e == EndedBy::Time ? "time" : "ceiling";
}

enum class Health : std::uint8_t { Ok = 0, NoSamples, Gap, OneSided, BookFull, Wall, Ceiling, Check };

[[nodiscard]] inline const char* health_name(Health h) noexcept {
    switch (h) {
        case Health::Ok: return "ok";
        case Health::NoSamples: return "no_samples";
        case Health::Gap: return "gap";
        case Health::OneSided: return "one_sided";
        case Health::BookFull: return "book_full";
        case Health::Wall: return "wall";
        case Health::Ceiling: return "ceiling";
        case Health::Check: return "check";
    }
    return "?";
}

// Judges one session by cross-multiplication, never through
// SessionStats::mean_gap_ticks(), which is not an integer: a decision at the
// threshold must not depend on one platform's rounding. Returns the first
// failing reason; Check is set by the runner once the session's check has run.
[[nodiscard]] inline Health judge(const SessionStats& st, EndedBy ended, const HealthGate& g) noexcept {
    if (ended == EndedBy::Ceiling) return Health::Ceiling;
    if (st.gap_samples == 0) return Health::NoSamples;
    if (st.gap_sum_ticks > g.max_mean_gap_ticks * st.gap_samples) return Health::Gap;
    if (100 * st.gap_unpriced > g.max_one_sided_pct * (st.gap_samples + st.gap_unpriced)) return Health::OneSided;
    if (st.book_full_rejects != 0) return Health::BookFull;
    if (st.wall_hits != 0) return Health::Wall;
    return Health::Ok;
}

// ---- the suite ------------------------------------------------------------

struct SuiteDef {
    const char* name;
    std::uint32_t version;
    const char* lineup;          // a roster_from_spec() spec: the opposing market
    SimTime session_ns;          // simulated time per session
    SimTime seat_reaction_ns;    // the seat's delay, whatever the candidate declares
    std::uint64_t step_ceiling;  // a bound on a runaway strategy, not a stopping rule
    HealthGate health;
    RosterConfig roster;         // every reference participant's parameters
    const char* change = "";     // how the market differs from the default, in --set= names; printed only
    // A market calibrated from a tape (marketdata/market_file.hpp): its
    // calibration.* lines, which say which tape, as conditions. Empty for every
    // built-in suite, so their conditions and fingerprints do not move.
    std::string calibration = "";
};

// The one definition. A change to any of it, or to a reference strategy, a
// default, the session or the engine, moves the pinned fingerprint in
// test_arena: that forces a version bump instead of a silent change of
// conditions.
[[nodiscard]] inline SuiteDef standard_suite() {
    return SuiteDef{
        "standard",
        1,
        "maker,rival,informed,noise:2,momentum", // exactly build_roster(RosterConfig{})
        25'000'000,
        10'000, // the reference maker's delay
        2'000'000,
        HealthGate{},
        RosterConfig{},
        "",
    };
}

// ---- scenario families --------------------------------------------------------

// Each family is a suite of its own, with its own name and therefore its own
// fingerprint, placebo arm and health gate: `standard` is pinned and must not
// change in place. A family changes one thing about the market, so a result
// that holds in `standard` can be asked whether it needs that thing. Families
// are never pooled into one number; docs/REPORT.md has the reasoning and the
// measurements that admitted each.
[[nodiscard]] inline SuiteDef wide_suite() {
    SuiteDef d = standard_suite();
    d.name = "wide";
    d.roster.maker_half_spread_ticks = 4;
    d.change = "maker.half_spread_ticks=4";
    return d;
}

[[nodiscard]] inline SuiteDef busy_suite() {
    SuiteDef d = standard_suite();
    d.name = "busy";
    d.roster.noise_timer_ns = 5'000;
    d.change = "noise.timer_ns=5000";
    return d;
}

// There is no `null` family (no informed participant). It was built and
// measured: its placebo failed the health gate on 56 of the 64 standard seeds
// (median gap 8.39 ticks, worst 30.09), because without the informed trader
// nothing pulls the book back to the fundamental within 25 ms. A market that
// has come loose is not a market, so it is not shipped; docs/REPORT.md.
inline constexpr const char* kSuiteNames = "standard wide busy";

[[nodiscard]] inline bool find_suite(std::string_view name, SuiteDef& out) {
    if (name == "standard") out = standard_suite();
    else if (name == "wide") out = wide_suite();
    else if (name == "busy") out = busy_suite();
    else return false;
    return true;
}

// Session settings: the defaults, except that a session ends on simulated time
// alone. Session::check() accepts stall_limit_ns == 0 because max_time_ns > 0.
[[nodiscard]] inline SessionConfig suite_session_config(const SuiteDef& def, std::uint64_t seed) {
    SessionConfig c;
    c.seed = seed;
    c.max_requests = 0;
    c.max_time_ns = def.session_ns;
    c.stall_limit_ns = 0;
    return c;
}

// The roster config with anchor and tick taken from the session, exactly as
// Session::start(scfg, rcfg) does before it builds the default roster.
[[nodiscard]] inline RosterConfig suite_roster_config(const SuiteDef& def) {
    const SessionConfig sc = suite_session_config(def, 0);
    RosterConfig r = def.roster;
    r.anchor = sc.start_price;
    r.tick = sc.tick;
    return r;
}

// ---- the seat ---------------------------------------------------------------

// Wraps the candidate's handle and reports the suite's delay. The session
// reads a participant's delay once, through the handle, when it starts, so
// nothing in the session changes. The candidate's own `reaction_ns` member
// keeps the value it declared, which is recorded but not used; the delay it
// actually runs at is visible to it as now_ns - as_of_ns in every MarketView.
class SeatOverride final : public StrategyHandle {
public:
    SeatOverride(std::unique_ptr<StrategyHandle> inner, SimTime reaction_ns)
        : inner_(std::move(inner)), reaction_(reaction_ns), declared_(inner_->reaction_ns()) {}
    [[nodiscard]] const char* name() const noexcept override { return inner_->name(); }
    [[nodiscard]] SimTime reaction_ns() const noexcept override { return reaction_; }
    [[nodiscard]] SimTime timer_ns() const noexcept override { return inner_->timer_ns(); }
    void reset(std::uint64_t seed) override { inner_->reset(seed); }
    ActionBatch on_event(const MarketView& v) override { return inner_->on_event(v); }
    [[nodiscard]] SimTime declared_reaction_ns() const noexcept { return declared_; }

private:
    std::unique_ptr<StrategyHandle> inner_;
    SimTime reaction_;
    SimTime declared_;
};

enum class Origin : std::uint8_t { Reference = 0, Builtin, Installed };

[[nodiscard]] inline const char* origin_name(Origin o) noexcept {
    switch (o) {
        case Origin::Reference: return "reference";
        case Origin::Builtin: return "builtin";
        case Origin::Installed: return "installed";
    }
    return "?";
}

// True when `name` is a token of kStrategyNames: compiled into every binary.
// Anything else that resolves is an installed strategy, which only binaries
// built with PX_ARENA_USER_STRATEGIES can reach.
[[nodiscard]] inline bool is_builtin(std::string_view name) noexcept {
    std::string_view rest = kStrategyNames;
    while (!rest.empty()) {
        const std::size_t sp = rest.find(' ');
        const std::string_view token = rest.substr(0, sp);
        if (!token.empty() && token == name) return true;
        if (sp == std::string_view::npos) break;
        rest = rest.substr(sp + 1);
    }
    return false;
}

inline constexpr std::size_t kMaxCandidates = 8;

[[nodiscard]] inline std::string known_candidates_text() {
    std::string s = std::string("built in: ") + kStrategyNames;
#if defined(PX_ARENA_USER_STRATEGIES)
    s += std::string("; installed: ") + (kUserStrategyNames[0] != '\0' ? kUserStrategyNames : "none");
#endif
    return s;
}

// Refuses a candidate list before anything runs, naming the cause.
[[nodiscard]] inline bool check_candidates(const std::vector<std::string>& names, std::string& why) {
    if (names.empty()) {
        why = "name at least one candidate";
        return false;
    }
    if (names.size() > kMaxCandidates) {
        why = "at most 8 candidates in one run";
        return false;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string& n = names[i];
        if (n.empty()) {
            why = "an empty candidate name";
            return false;
        }
        if (n == "informed") {
            why = "'informed' cannot be a candidate: the seat is never given the fundamental value";
            return false;
        }
        if (n == "placebo") {
            why = "'placebo' cannot be a candidate: it is always run, as the control";
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (names[j] == n) {
                why = "'" + n + "' is named twice";
                return false;
            }
        }
        Roster scratch;
        if (!add_named_strategy(n, RosterConfig{}, scratch)) {
            why = "unknown strategy '" + n + "'; " + known_candidates_text();
            return false;
        }
    }
    return true;
}

// The line-up's size, which is the seat's index.
[[nodiscard]] inline bool lineup_size(const SuiteDef& def, std::size_t& out, std::string& why) {
    Roster r;
    if (!roster_from_spec(def.lineup, suite_roster_config(def), r, why)) return false;
    out = r.size();
    return true;
}

// Seats an already-built handle: the line-up, then the handle appended last,
// wrapped so its delay is the suite's, and never informed. Tests use this
// overload to seat fixtures.
[[nodiscard]] inline bool build_arm_roster(const SuiteDef& def, std::unique_ptr<StrategyHandle> seat, Roster& out,
                                           std::string& why) {
    Roster r;
    if (!roster_from_spec(def.lineup, suite_roster_config(def), r, why)) return false;
    if (r.size() + 1 > Session::kMaxParticipants) {
        why = "the line-up leaves no room for a seat under the participant ceiling";
        return false;
    }
    if (seat) {
        r.strategies.push_back(std::make_unique<SeatOverride>(std::move(seat), def.seat_reaction_ns));
        r.informed.push_back(false);
    }
    out = std::move(r);
    return true;
}

// Seats a strategy by name, or nothing when `candidate` is empty (the market
// with no seat, against which the placebo arm is checked).
[[nodiscard]] inline bool build_arm_roster(const SuiteDef& def, std::string_view candidate, Roster& out,
                                           Origin& origin, std::string& why) {
    origin = Origin::Reference;
    if (candidate.empty()) return build_arm_roster(def, std::unique_ptr<StrategyHandle>{}, out, why);
    Roster one;
    if (!add_named_strategy(candidate, suite_roster_config(def), one) || one.strategies.size() != 1) {
        why = "unknown strategy '" + std::string(candidate) + "'; " + known_candidates_text();
        return false;
    }
    if (candidate != "placebo") origin = is_builtin(candidate) ? Origin::Builtin : Origin::Installed;
    return build_arm_roster(def, std::move(one.strategies.front()), out, why);
}

// ---- the records: one field per CSV column ----------------------------------

struct SessionRow {
    std::string suite;
    std::uint32_t version = 0;
    std::string block;
    std::uint64_t seed = 0;
    std::string arm;
    Origin origin = Origin::Reference;
    std::uint32_t seat = 0;
    SimTime seat_reaction_ns = 0;
    SimTime declared_reaction_ns = 0;
    SimTime timer_ns = 0;
    EndedBy ended_by = EndedBy::Time;
    std::uint64_t steps = 0;
    std::uint64_t requests = 0;
    std::uint64_t events = 0;
    std::uint64_t trades = 0;
    std::uint64_t volume = 0;
    std::uint64_t rejects = 0;
    std::uint64_t self_trade_cancels = 0;
    std::uint64_t book_full_rejects = 0;
    std::uint64_t action_drops = 0;
    std::uint64_t sink_growths = 0;
    std::uint64_t fair_steps = 0;
    std::uint64_t wall_hits = 0;
    SimTime elapsed_ns = 0;
    Price v_final = 0;
    std::uint64_t gap_samples = 0;
    std::uint64_t gap_sum_ticks = 0;
    std::uint64_t gap_max_ticks = 0;
    std::uint64_t gap_unpriced = 0;
    std::int64_t cash_sum = 0;
    std::int64_t position_sum = 0;
    std::int64_t value_sum = 0;
    std::uint64_t seat_orders = 0;
    std::uint64_t seat_fills = 0;
    std::int64_t seat_value = 0; // price units, marked at v_final
    // The seat's path, sampled from outside the session each time the
    // fundamental moves and once at the end: read-only, so no hash moves.
    std::uint64_t seat_path_samples = 0;
    std::uint64_t seat_abs_position_sum = 0;
    std::uint64_t seat_max_abs_position = 0;
    std::int64_t seat_max_drawdown = 0; // price units: the largest fall from the running peak, which starts at 0
    bool inert = false;
    Health health = Health::Ok;
    std::uint64_t session_hash = 0;
    std::uint64_t event_hash = 0;
    std::uint64_t event_count = 0;
    std::string check_kind; // "no_seat" for the placebo, "replay" for candidates
    std::uint64_t check_hash = 0;
    bool check_ok = true;

    [[nodiscard]] bool healthy() const noexcept { return health == Health::Ok; }
};

struct ParticipantRow {
    std::string suite;
    std::uint32_t version = 0;
    std::string block;
    std::uint64_t seed = 0;
    std::string arm;
    std::uint32_t index = 0;
    std::uint32_t owner = 0;
    bool seat = false; // role: "seat" or "reference"
    std::string name;
    bool informed = false;
    SimTime reaction_ns = 0; // the delay it ran at
    SimTime timer_ns = 0;
    std::uint64_t wakes = 0;
    Account account;
    std::int64_t value = 0; // cash + position * v_final - fees
};

struct ArmRecord {
    SessionRow session;
    std::vector<ParticipantRow> parts;
};

// Runs one arm on one seed. The runner owns the step loop rather than calling
// Session::run(), which is the same loop without the ceiling, so that a later
// step has a place to sample a path. Returns false only if the session refuses
// to start, which build_arm_roster() makes unreachable.
[[nodiscard]] inline bool run_arm(const SuiteDef& def, const SeedBlock& block, std::uint64_t seed,
                                  std::string_view arm, Origin origin, Roster roster, ArmRecord& out,
                                  std::string& why) {
    const std::size_t n = roster.size();
    std::size_t lineup = 0;
    if (!lineup_size(def, lineup, why)) return false;
    const bool has_seat = n == lineup + 1;

    std::vector<SimTime> timers(n);
    for (std::size_t i = 0; i < n; ++i) timers[i] = roster.strategies[i]->timer_ns();
    SimTime declared = 0;
    if (has_seat) {
        const auto* seat = dynamic_cast<const SeatOverride*>(roster.strategies.back().get());
        declared = seat != nullptr ? seat->declared_reaction_ns() : roster.strategies.back()->reaction_ns();
    }

    Session s;
    const Session::StartError bad = s.start(suite_session_config(def, seed), std::move(roster));
    if (bad != Session::StartError::None) {
        why = Session::explain(bad);
        return false;
    }
    std::uint64_t steps = 0;
    bool ceiling = false;
    std::uint64_t path_samples = 0, abs_position_sum = 0, max_abs_position = 0;
    std::int64_t peak = 0, max_drawdown = 0;
    auto sample = [&] {
        const Account& a = s.ledger().at(lineup);
        const std::uint64_t pos = static_cast<std::uint64_t>(a.position < 0 ? -a.position : a.position);
        const std::int64_t value = a.value(s.fair_value().value());
        ++path_samples;
        abs_position_sum += pos;
        if (pos > max_abs_position) max_abs_position = pos;
        if (value > peak) peak = value;
        if (peak - value > max_drawdown) max_drawdown = peak - value;
    };
    std::uint64_t fair_seen = s.stats().fair_steps;
    while (s.step()) {
        if (has_seat && s.stats().fair_steps != fair_seen) {
            fair_seen = s.stats().fair_steps;
            sample();
        }
        if (++steps >= def.step_ceiling) {
            ceiling = true;
            break;
        }
    }
    s.finish();
    if (has_seat) sample();

    const SessionStats& st = s.stats();
    const Price v = s.fair_value().value();
    SessionRow& r = out.session;
    r = SessionRow{};
    r.suite = def.name;
    r.version = def.version;
    r.block = block.name;
    r.seed = seed;
    r.arm = std::string(arm);
    r.origin = origin;
    r.seat = static_cast<std::uint32_t>(lineup);
    r.seat_reaction_ns = has_seat ? s.reaction_of(lineup) : 0;
    r.declared_reaction_ns = declared;
    r.timer_ns = has_seat ? timers[lineup] : 0;
    r.ended_by = ceiling ? EndedBy::Ceiling : EndedBy::Time;
    r.steps = steps;
    r.requests = st.requests;
    r.events = st.events;
    r.trades = st.trades;
    r.volume = st.volume;
    r.rejects = st.rejects;
    r.self_trade_cancels = st.self_trade_cancels;
    r.book_full_rejects = st.book_full_rejects;
    r.action_drops = st.action_drops;
    r.sink_growths = st.sink_growths;
    r.fair_steps = st.fair_steps;
    r.wall_hits = st.wall_hits;
    r.elapsed_ns = st.elapsed_ns;
    r.v_final = v;
    r.gap_samples = st.gap_samples;
    r.gap_sum_ticks = st.gap_sum_ticks;
    r.gap_max_ticks = st.gap_max_ticks;
    r.gap_unpriced = st.gap_unpriced;
    r.cash_sum = s.ledger().cash_sum();
    r.position_sum = s.ledger().position_sum();
    r.value_sum = s.ledger().value_sum(v);
    if (has_seat) {
        const Account& a = s.ledger().at(lineup);
        r.seat_orders = a.orders_sent;
        r.seat_fills = a.fills;
        r.seat_value = a.value(v);
        r.inert = a.orders_sent == 0;
        r.seat_path_samples = path_samples;
        r.seat_abs_position_sum = abs_position_sum;
        r.seat_max_abs_position = max_abs_position;
        r.seat_max_drawdown = max_drawdown;
    }
    r.health = judge(st, r.ended_by, def.health);
    r.session_hash = s.hash();
    r.event_hash = s.event_hash();
    r.event_count = s.event_count();

    out.parts.clear();
    out.parts.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        ParticipantRow p;
        p.suite = def.name;
        p.version = def.version;
        p.block = block.name;
        p.seed = seed;
        p.arm = std::string(arm);
        p.index = static_cast<std::uint32_t>(i);
        p.owner = static_cast<std::uint32_t>(i + 1);
        p.seat = has_seat && i == lineup;
        p.name = s.name_of(i);
        p.informed = s.informed(i);
        p.reaction_ns = s.reaction_of(i);
        p.timer_ns = timers[i];
        p.wakes = s.wakes_of(i);
        p.account = s.ledger().at(i);
        p.value = p.account.value(v);
        out.parts.push_back(std::move(p));
    }
    return true;
}

// ---- a whole run --------------------------------------------------------------

struct CandidateInfo {
    std::string name;
    Origin origin = Origin::Builtin;
    SimTime declared_reaction_ns = 0;
    SimTime timer_ns = 0;
    std::string digest; // an installed strategy's header checksum (cksum CRC), empty for a built-in
};

// Which build ran: recorded so a run can be traced and re-run, never
// fingerprinted, because the same conditions on two builds must stay comparable.
struct BuildInfo {
    std::string commit;   // git rev-parse --short=12 HEAD at build time, or "unknown"
    bool dirty = false;   // tracked files differed from that commit
    std::string compiler; // __VERSION__
    std::string platform; // windows or linux
    std::string flags;    // the release flags
};

struct RunInfo {
    std::string binary;          // "suite" or "suite_lab"
    std::string user_strategies; // the installed names this binary can see, or "none"
    std::vector<CandidateInfo> candidates;
    BuildInfo build;             // empty commit: not recorded (tests)
    std::string market_file;     // the markets/NAME.cfg a calibrated suite was read from, or empty
};

// Exactly what the three raw files hold, so a report is a function of this
// alone and a later step can rebuild one from disk.
struct RunRecord {
    SuiteDef def = standard_suite();
    SeedBlock block = kSeedBlocks[0];
    RunInfo info;
    std::vector<SessionRow> sessions;   // seed-major: placebo, then candidates as named
    std::vector<ParticipantRow> parts;  // the same order, one row per participant
};

// The arms of a run in row order: the placebo, then the candidates as named.
[[nodiscard]] inline std::vector<std::string> arm_names(const RunRecord& run) {
    std::vector<std::string> arms{"placebo"};
    for (const CandidateInfo& c : run.info.candidates) arms.push_back(c.name);
    return arms;
}

struct ArmSummary {
    std::string arm;
    std::uint32_t seeds = 0;
    std::uint32_t healthy = 0;
    std::uint32_t inert = 0;
    std::uint32_t check_failures = 0;
    std::vector<std::pair<std::uint64_t, Health>> failing; // seed, reason, in seed order
    [[nodiscard]] bool admissible() const noexcept { return seeds > 0 && failing.empty(); }
};

[[nodiscard]] inline std::vector<ArmSummary> summarize_arms(const RunRecord& run) {
    std::vector<ArmSummary> out;
    for (const std::string& arm : arm_names(run)) {
        ArmSummary a;
        a.arm = arm;
        for (const SessionRow& r : run.sessions) {
            if (r.arm != arm) continue;
            ++a.seeds;
            if (r.healthy()) ++a.healthy;
            else a.failing.emplace_back(r.seed, r.health);
            if (r.inert) ++a.inert;
            if (!r.check_ok) ++a.check_failures;
        }
        out.push_back(std::move(a));
    }
    return out;
}

// How a run ended, as the process exit code: 2 for usage (decided before
// anything runs), 3 when the reference market itself is not admissible on this
// build, 1 when a candidate failed replay, 0 otherwise whatever the verdicts.
struct Outcome {
    bool reference_admissible = true;
    bool closed_identity = true; // cash, position and value each sum to zero in every arm
    bool candidates_replay = true;
    [[nodiscard]] int exit_code() const noexcept {
        if (!reference_admissible || !closed_identity) return 3;
        if (!candidates_replay) return 1;
        return 0;
    }
};

[[nodiscard]] inline Outcome outcome_of(const RunRecord& run) {
    Outcome o;
    for (const SessionRow& r : run.sessions) {
        if (r.cash_sum != 0 || r.position_sum != 0 || r.value_sum != 0) o.closed_identity = false;
        if (r.arm == "placebo") {
            if (!r.healthy()) o.reference_admissible = false;
        } else if (!r.check_ok) {
            o.candidates_replay = false;
        }
    }
    return o;
}

// ---- text ---------------------------------------------------------------------

inline void append_u(std::string& s, std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%llu", static_cast<unsigned long long>(v));
    s += b;
}
inline void append_i(std::string& s, std::int64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%lld", static_cast<long long>(v));
    s += b;
}
inline void append_hex(std::string& s, std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    s += b;
}
inline void append_kv(std::string& s, std::string_view k, std::int64_t v) {
    s += k;
    s += '=';
    append_i(s, v);
    s += '\n';
}
inline void append_kv(std::string& s, std::string_view k, std::string_view v) {
    s += k;
    s += '=';
    s += v;
    s += '\n';
}

// Splits a whitespace-separated name list such as kRosterParamNames.
[[nodiscard]] inline std::vector<std::string> split_names(std::string_view list) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : list) {
        if (c == ' ' || c == '\n' || c == '\t') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

inline constexpr const char* kSessionsHeader =
    "suite,version,block,seed,arm,origin,seat,seat_reaction_ns,declared_reaction_ns,timer_ns,"
    "ended_by,steps,requests,events,trades,volume,rejects,self_trade_cancels,book_full_rejects,"
    "action_drops,sink_growths,fair_steps,wall_hits,elapsed_ns,v_final,"
    "gap_samples,gap_sum_ticks,gap_max_ticks,gap_unpriced,"
    "cash_sum,position_sum,value_sum,"
    "seat_orders,seat_fills,seat_value,inert,healthy,health_reason,"
    "session_hash,event_hash,event_count,check_kind,check_hash,check_ok,"
    "seat_path_samples,seat_abs_position_sum,seat_max_abs_position,seat_max_drawdown\n";

inline constexpr const char* kParticipantsHeader =
    "suite,version,block,seed,arm,index,owner,role,name,informed,reaction_ns,timer_ns,wakes,"
    "orders_sent,rejects,self_trade_cancels,fills,maker_fills,taker_fills,filled_qty,"
    "position,cash,fees,value,edge_sum,spread_sum,adverse_sum,markout_n,adverse_fills,unresolved_fills\n";

inline void append_session_row(std::string& s, const SessionRow& r) {
    s += r.suite; s += ',';
    append_u(s, r.version); s += ',';
    s += r.block; s += ',';
    append_u(s, r.seed); s += ',';
    s += r.arm; s += ',';
    s += origin_name(r.origin); s += ',';
    append_u(s, r.seat); s += ',';
    append_i(s, r.seat_reaction_ns); s += ',';
    append_i(s, r.declared_reaction_ns); s += ',';
    append_i(s, r.timer_ns); s += ',';
    s += ended_by_name(r.ended_by); s += ',';
    append_u(s, r.steps); s += ',';
    append_u(s, r.requests); s += ',';
    append_u(s, r.events); s += ',';
    append_u(s, r.trades); s += ',';
    append_u(s, r.volume); s += ',';
    append_u(s, r.rejects); s += ',';
    append_u(s, r.self_trade_cancels); s += ',';
    append_u(s, r.book_full_rejects); s += ',';
    append_u(s, r.action_drops); s += ',';
    append_u(s, r.sink_growths); s += ',';
    append_u(s, r.fair_steps); s += ',';
    append_u(s, r.wall_hits); s += ',';
    append_i(s, r.elapsed_ns); s += ',';
    append_i(s, r.v_final); s += ',';
    append_u(s, r.gap_samples); s += ',';
    append_u(s, r.gap_sum_ticks); s += ',';
    append_u(s, r.gap_max_ticks); s += ',';
    append_u(s, r.gap_unpriced); s += ',';
    append_i(s, r.cash_sum); s += ',';
    append_i(s, r.position_sum); s += ',';
    append_i(s, r.value_sum); s += ',';
    append_u(s, r.seat_orders); s += ',';
    append_u(s, r.seat_fills); s += ',';
    append_i(s, r.seat_value); s += ',';
    append_u(s, r.inert ? 1 : 0); s += ',';
    append_u(s, r.healthy() ? 1 : 0); s += ',';
    s += health_name(r.health); s += ',';
    append_hex(s, r.session_hash); s += ',';
    append_hex(s, r.event_hash); s += ',';
    append_u(s, r.event_count); s += ',';
    s += r.check_kind; s += ',';
    append_hex(s, r.check_hash); s += ',';
    append_u(s, r.check_ok ? 1 : 0); s += ',';
    append_u(s, r.seat_path_samples); s += ',';
    append_u(s, r.seat_abs_position_sum); s += ',';
    append_u(s, r.seat_max_abs_position); s += ',';
    append_i(s, r.seat_max_drawdown);
    s += '\n';
}

inline void append_participant_rows(std::string& s, const std::vector<ParticipantRow>& rows) {
    for (const ParticipantRow& p : rows) {
        const Account& a = p.account;
        s += p.suite; s += ',';
        append_u(s, p.version); s += ',';
        s += p.block; s += ',';
        append_u(s, p.seed); s += ',';
        s += p.arm; s += ',';
        append_u(s, p.index); s += ',';
        append_u(s, p.owner); s += ',';
        s += p.seat ? "seat" : "reference"; s += ',';
        s += p.name; s += ',';
        append_u(s, p.informed ? 1 : 0); s += ',';
        append_i(s, p.reaction_ns); s += ',';
        append_i(s, p.timer_ns); s += ',';
        append_u(s, p.wakes); s += ',';
        append_u(s, a.orders_sent); s += ',';
        append_u(s, a.rejects); s += ',';
        append_u(s, a.self_trade_cancels); s += ',';
        append_u(s, a.fills); s += ',';
        append_u(s, a.maker_fills); s += ',';
        append_u(s, a.taker_fills); s += ',';
        append_u(s, a.filled_qty); s += ',';
        append_i(s, a.position); s += ',';
        append_i(s, a.cash); s += ',';
        append_i(s, a.fees); s += ',';
        append_i(s, p.value); s += ',';
        append_i(s, a.edge_sum); s += ',';
        append_i(s, a.spread_sum); s += ',';
        append_i(s, a.adverse_sum); s += ',';
        append_u(s, a.markout_n); s += ',';
        append_u(s, a.adverse_fills); s += ',';
        append_u(s, a.unresolved_fills);
        s += '\n';
    }
}

// The conditions: everything a candidate faced that is not already in the
// placebo hashes. These lines, and only these, are fingerprinted.
[[nodiscard]] inline std::string conditions_text(const SuiteDef& def, const SeedBlock& block) {
    std::string s;
    std::size_t seat = 0;
    std::string why;
    if (!lineup_size(def, seat, why)) seat = 0;
    const SessionConfig sc = suite_session_config(def, block.first);
    append_kv(s, "suite.name", def.name);
    append_kv(s, "suite.version", static_cast<std::int64_t>(def.version));
    append_kv(s, "suite.lineup", def.lineup);
    append_kv(s, "suite.session_ns", def.session_ns);
    append_kv(s, "suite.step_ceiling", static_cast<std::int64_t>(def.step_ceiling));
    append_kv(s, "seat.index", static_cast<std::int64_t>(seat));
    append_kv(s, "seat.owner", static_cast<std::int64_t>(seat + 1));
    append_kv(s, "seat.reaction_ns", def.seat_reaction_ns);
    append_kv(s, "seat.informed", std::int64_t{0});
    append_kv(s, "health.max_mean_gap_ticks", static_cast<std::int64_t>(def.health.max_mean_gap_ticks));
    append_kv(s, "health.max_one_sided_pct", static_cast<std::int64_t>(def.health.max_one_sided_pct));
    append_kv(s, "health.book_full_rejects", std::int64_t{0});
    append_kv(s, "health.wall_hits", std::int64_t{0});
    append_kv(s, "block.name", block.name);
    append_kv(s, "block.first_seed", static_cast<std::int64_t>(block.first));
    append_kv(s, "block.seeds", static_cast<std::int64_t>(block.count));
    append_kv(s, "market.tick", sc.tick);
    append_kv(s, "market.start_price", sc.start_price);
    for (const std::string& k : split_names(Session::kSessionParamNames)) {
        if (k == "session.seed") continue;
        std::int64_t v = 0;
        if (Session::get_param(sc, k, v)) append_kv(s, k, v);
    }
    s += def.calibration;
    return s;
}

// Every roster field under the names --set= takes, so a run can be re-created.
[[nodiscard]] inline std::string roster_text(const SuiteDef& def) {
    std::string s;
    const RosterConfig rc = suite_roster_config(def);
    for (const std::string& k : split_names(kRosterParamNames)) {
        std::int64_t v = 0;
        if (get_roster_param(rc, k, v)) append_kv(s, k, v);
    }
    return s;
}

[[nodiscard]] inline std::string suite_cfg_text(const SuiteDef& def, const SeedBlock& block, const RunInfo& info) {
    std::string s;
    s += "# The conditions of one standard-suite run. Simulated, not measured.\n";
    s += "# Conditions: fingerprinted, with every placebo session hash (outcome.cfg).\n";
    s += conditions_text(def, block);
    s += "# Every reference participant's parameters, under the names --set= takes.\n";
    s += roster_text(def);
    s += "# What this invocation ran. Not conditions.\n";
    append_kv(s, "run.binary", info.binary);
    append_kv(s, "run.user_strategies", info.user_strategies);
    std::string names;
    for (std::size_t i = 0; i < info.candidates.size(); ++i) {
        if (i != 0) names += ',';
        names += info.candidates[i].name;
    }
    append_kv(s, "run.candidates", names);
    for (const CandidateInfo& c : info.candidates) {
        const std::string k = "run.candidate." + c.name;
        append_kv(s, k + ".origin", origin_name(c.origin));
        append_kv(s, k + ".declared_reaction_ns", c.declared_reaction_ns);
        append_kv(s, k + ".timer_ns", c.timer_ns);
        if (!c.digest.empty()) append_kv(s, k + ".digest", c.digest);
    }
    if (!info.market_file.empty()) append_kv(s, "run.market_file", info.market_file);
    if (!info.build.commit.empty()) {
        s += "# Which build ran it. Not conditions: `suite --reproduce` re-runs on any build.\n";
        append_kv(s, "build.commit", info.build.commit);
        append_kv(s, "build.dirty", std::int64_t{info.build.dirty ? 1 : 0});
        append_kv(s, "build.compiler", info.build.compiler);
        append_kv(s, "build.platform", info.build.platform);
        append_kv(s, "build.flags", info.build.flags);
    }
    return s;
}

// The lines of a suite.cfg that are conditions: everything except comments and
// the run.* and build.* lines, which say what ran rather than what it faced.
[[nodiscard]] inline std::vector<std::string> condition_lines(std::string_view cfg) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < cfg.size()) {
        std::size_t end = cfg.find('\n', at);
        if (end == std::string_view::npos) end = cfg.size();
        const std::string_view line = cfg.substr(at, end - at);
        at = end + 1;
        if (line.empty() || line[0] == '#' || line.starts_with("run.") || line.starts_with("build.")) continue;
        out.emplace_back(line);
    }
    return out;
}

// One key of a suite.cfg, or empty.
[[nodiscard]] inline std::string cfg_value(std::string_view cfg, std::string_view key) {
    std::size_t at = 0;
    while (at < cfg.size()) {
        std::size_t end = cfg.find('\n', at);
        if (end == std::string_view::npos) end = cfg.size();
        const std::string_view line = cfg.substr(at, end - at);
        at = end + 1;
        if (line.size() > key.size() && line.starts_with(key) && line[key.size()] == '=') {
            return std::string(line.substr(key.size() + 1));
        }
    }
    return {};
}

[[nodiscard]] inline std::uint64_t fnv1a(std::uint64_t h, std::string_view s) noexcept {
    for (const char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

// FNV-1a over the conditions, then one line per seed with the placebo arm's
// session hash. It moves if anything about what a candidate faced moves: the
// engine, the session, a reference strategy, a default, the suite's settings
// or the block. Two results are comparable only if their fingerprints match.
[[nodiscard]] inline std::uint64_t suite_fingerprint(const SuiteDef& def, const SeedBlock& block,
                                                     const std::vector<std::pair<std::uint64_t, std::uint64_t>>& placebo) {
    std::uint64_t h = fnv1a(0xcbf29ce484222325ULL, conditions_text(def, block));
    for (const auto& [seed, hash] : placebo) {
        std::string line = "seed=";
        append_u(line, seed);
        line += " placebo=";
        append_hex(line, hash);
        line += '\n';
        h = fnv1a(h, line);
    }
    return h;
}

[[nodiscard]] inline std::vector<std::pair<std::uint64_t, std::uint64_t>> placebo_hashes(const RunRecord& run) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    for (const SessionRow& r : run.sessions) {
        if (r.arm == "placebo") out.emplace_back(r.seed, r.session_hash);
    }
    return out;
}

[[nodiscard]] inline std::string outcome_cfg_text(const RunRecord& run, std::uint64_t fingerprint,
                                                  std::uint64_t sessions_run, const Outcome& o) {
    std::string s;
    append_kv(s, "status", "complete");
    std::string fp;
    append_hex(fp, fingerprint);
    append_kv(s, "fingerprint", fp);
    append_kv(s, "sessions_run", static_cast<std::int64_t>(sessions_run));
    append_kv(s, "rows", static_cast<std::int64_t>(run.sessions.size()));
    bool checks = true;
    for (const SessionRow& r : run.sessions) checks = checks && r.check_ok;
    append_kv(s, "checks", checks ? "identical" : "different");
    append_kv(s, "closed_identity", std::int64_t{o.closed_identity ? 1 : 0});
    append_kv(s, "reference_admissible", std::int64_t{o.reference_admissible ? 1 : 0});
    for (const ArmSummary& a : summarize_arms(run)) {
        const std::string k = "arm." + a.arm;
        append_kv(s, k + ".admissible", std::int64_t{a.admissible() ? 1 : 0});
        std::string failing;
        for (const auto& [seed, why] : a.failing) {
            if (!failing.empty()) failing += ',';
            append_u(failing, seed);
            failing += ':';
            failing += health_name(why);
        }
        append_kv(s, k + ".failing_seeds", failing.empty() ? std::string("-") : failing);
        append_kv(s, k + ".inert_seeds", static_cast<std::int64_t>(a.inert));
        append_kv(s, k + ".check_failures", static_cast<std::int64_t>(a.check_failures));
    }
    append_kv(s, "exit", static_cast<std::int64_t>(o.exit_code()));
    return s;
}

} // namespace px::arena::suite
