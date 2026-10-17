// arena/controls.hpp — the evaluator's own controls: answers known in advance,
// which the runner and the report must reproduce (docs/CONTROLS.md).
//
// Each control is a function of finished records and returns whether it held,
// with what it expected and what it measured. They are separate functions, not
// one verdict, so test_arena can plant a defect and show which control catches
// it. Like suite_report.hpp, this runs after every session has finished, so
// floating point is allowed here and nowhere earlier.
#pragma once

#include "arena/suite.hpp"
#include "arena/suite_report.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace px::arena::suite {

struct ControlResult {
    std::string name;
    std::string expected;
    std::string measured;
    bool pass = false;
};

// The empty seat makes and loses nothing, on every seed.
[[nodiscard]] inline ControlResult control_placebo_zero(const RunRecord& run) {
    ControlResult c{"placebo is zero", "seat value exactly 0 on every seed", "", true};
    std::uint32_t seeds = 0, zero = 0;
    for (const SessionRow& r : run.sessions) {
        if (r.arm != "placebo") continue;
        ++seeds;
        if (r.seat_value == 0 && r.seat_orders == 0 && r.seat_fills == 0) ++zero;
    }
    c.pass = seeds > 0 && zero == seeds;
    c.measured = std::to_string(zero) + "/" + std::to_string(seeds) + " seeds";
    return c;
}

// The placebo's sessions are the market with no seat at all.
[[nodiscard]] inline ControlResult control_placebo_inert(const RunRecord& run) {
    ControlResult c{"placebo is inert", "same session hash as the no-seat market on every seed", "", true};
    std::uint32_t seeds = 0, same = 0;
    for (const SessionRow& r : run.sessions) {
        if (r.arm != "placebo") continue;
        ++seeds;
        if (r.check_kind == "no_seat" && r.check_ok && r.check_hash == r.session_hash) ++same;
    }
    c.pass = seeds > 0 && same == seeds;
    c.measured = std::to_string(same) + "/" + std::to_string(seeds) + " seeds";
    return c;
}

// Every arm on a seed closes at the same fundamental: the time bound at work.
// A request bound breaks this (docs/RUNNER.md), and pairing with it.
[[nodiscard]] inline ControlResult control_common_close(const RunRecord& run) {
    ControlResult c{"common close", "every arm closes at the placebo's fundamental, every seed", "", true};
    std::uint32_t rows = 0, same = 0;
    for (const SessionRow& r : run.sessions) {
        if (r.arm == "placebo") continue;
        for (const SessionRow& p : run.sessions) {
            if (p.arm == "placebo" && p.seed == r.seed) {
                ++rows;
                if (p.v_final == r.v_final && p.fair_steps == r.fair_steps) ++same;
            }
        }
    }
    c.pass = rows > 0 && same == rows;
    c.measured = std::to_string(same) + "/" + std::to_string(rows) + " arm-seeds";
    return c;
}

// Every candidate session gave the same hash when run again from fresh instances.
[[nodiscard]] inline ControlResult control_replay(const RunRecord& run) {
    ControlResult c{"replay", "every candidate session identical on replay", "", true};
    std::uint32_t rows = 0, same = 0;
    for (const SessionRow& r : run.sessions) {
        if (r.arm == "placebo") continue;
        ++rows;
        if (r.check_kind == "replay" && r.check_ok && r.check_hash == r.session_hash) ++same;
    }
    c.pass = rows > 0 && same == rows;
    c.measured = std::to_string(same) + "/" + std::to_string(rows) + " sessions";
    return c;
}

// Cash, position and value sum to zero in every arm, and each reference
// participant's change against the placebo sums to minus the seat's value.
[[nodiscard]] inline ControlResult control_identities(const RunRecord& run) {
    ControlResult c{"closed identities", "market closed in every arm; attribution sums to -seat value", "", true};
    bool attribution = true;
    (void)attribute(run, attribution);
    const bool closed = outcome_of(run).closed_identity;
    c.pass = closed && attribution;
    c.measured = std::string(closed ? "closed" : "NOT closed") + ", attribution " + (attribution ? "exact" : "BROKEN");
    return c;
}

// One strategy under two labels must be identical, seed by seed. This is the
// control that catches misaligned pairing.
[[nodiscard]] inline ControlResult control_aa(const RunRecord& run, const std::string& a, const std::string& b) {
    ControlResult c{"A/A", a + " and " + b + " identical on every seed", "", true};
    const Paired p = paired(seat_values(run, a), seat_values(run, b));
    c.pass = p.n > 0 && p.identical;
    c.measured = p.identical ? "identical on " + std::to_string(p.n) + " seeds"
                             : fmt("%+.2f", to_currency(p.mean)) + " +- " + fmt("%.2f", to_currency(p.se)) +
                                   ", not identical";
    return c;
}

// Two arms that differ by nothing that should matter must not separate. The
// interval it prints is the noise floor: a difference smaller than this
// should not be read as skill.
[[nodiscard]] inline ControlResult control_noise_floor(const RunRecord& run, const std::string& a, const std::string& b,
                                                        const std::string& what) {
    ControlResult c{"noise floor", what + ": not separated", "", true};
    const Paired p = paired(seat_values(run, a), seat_values(run, b));
    c.pass = p.n > 1 && !p.separated();
    c.measured = fmt("%+.2f", to_currency(p.mean)) + " +- " + fmt("%.2f", to_currency(p.se)) + " (" +
                 fmt("%+.2f", to_currency(p.lo)) + " .. " + fmt("%+.2f", to_currency(p.hi)) + ")";
    return c;
}

// The positive control: a strategy whose result is known in sign and location
// (arena/strategies/roundtrip.hpp). Its loss must separate from zero, it must
// be in spread, and adverse and carry must not separate.
[[nodiscard]] inline ControlResult control_positive(const RunRecord& run, const std::string& arm) {
    ControlResult c{"positive control", arm + " loses, separated; the loss is in spread; adverse and carry are not", "",
                    true};
    const std::vector<MetricRow> rows = evaluate(run);
    auto level = [&](const char* key) -> const Paired* {
        for (const MetricRow& r : rows) {
            if (r.baseline.empty() && r.arm == arm && std::string(kMetrics[r.metric].key) == key) return &r.stats;
        }
        return nullptr;
    };
    const Paired* value = level("value");
    const Paired* spread = level("spread_earned");
    const Paired* adverse = level("adverse");
    const Paired* carry = level("carry");
    if (value == nullptr || spread == nullptr || adverse == nullptr || carry == nullptr) {
        c.pass = false;
        c.measured = arm + " is not in the run";
        return c;
    }
    c.pass = value->hi < 0 && spread->hi < 0 && !adverse->separated() && !carry->separated();
    c.measured = "value " + fmt("%+.2f", value->mean) + " +- " + fmt("%.2f", value->se) + ", spread " +
                 fmt("%+.2f", spread->mean) + ", adverse " + fmt("%+.2f", adverse->mean) + " +- " +
                 fmt("%.2f", adverse->se) + ", carry " + fmt("%+.2f", carry->mean) + " +- " + fmt("%.2f", carry->se);
    return c;
}

[[nodiscard]] inline std::string controls_text(const std::vector<ControlResult>& cs) {
    std::string s;
    char b[512];
    std::size_t passed = 0;
    for (const ControlResult& c : cs) {
        std::snprintf(b, sizeof b, "  %-4s %-18s expected: %s\n                          measured: %s\n",
                      c.pass ? "ok" : "FAIL", c.name.c_str(), c.expected.c_str(), c.measured.c_str());
        s += b;
        if (c.pass) ++passed;
    }
    std::snprintf(b, sizeof b, "%zu/%zu controls held\n", passed, cs.size());
    s += b;
    return s;
}

} // namespace px::arena::suite
