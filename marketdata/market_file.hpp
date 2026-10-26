// marketdata/market_file.hpp — a calibrated market, as markets/NAME.cfg holds it.
//
// `calibrate NAME` (marketdata/calibrate.cpp) writes one; the lab programs read
// it with --market=NAME (suite_lab, sweep_lab, arena_lab), and nothing else
// does: the evidence programs refuse the flag, so no calibrated market can reach
// a published number or a pinned hash.
//
// A calibrated market is the standard suite's market with at most four
// parameters moved, so the file says little that a run needs: which tape it was
// matched to (the calibration.* lines) and what it sets (the set.* lines). The
// rest is the evidence that it may be used at all, which the reader insists on:
// the known-answer control held on the build that made it, and the market passed
// the health gate on every seed of the standard block. A run on a calibrated
// market records the calibration.* lines among its conditions and the values
// among its roster lines, so it can be re-run from its own suite.cfg without the
// tape or this file (docs/REPRODUCE.md).
//
// Integer and text only, like the tape.
#pragma once

#include "arena/suite.hpp"
#include "marketdata/tape.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace px::data {

// The only parameters a calibrated market may set, in the order the file lists
// them. Calibration moves nothing else, and a file that tries is refused: in
// particular, nothing docs/ARENA.md calls load-bearing can be set this way.
inline constexpr const char* kMarketParamNames =
    "maker.half_spread_ticks rival.half_spread_ticks noise.qty noise.timer_ns";

// Names a market cannot take: a run directory or --suite= would read them as a suite.
inline constexpr const char* kReservedMarketNames =
    "standard wide busy all sensitivity delay-5us delay-20us thin no-momentum no-rival";

inline constexpr int kMarketVersion = 1;

[[nodiscard]] inline bool market_name_ok(std::string_view name, std::string& why) {
    if (!valid_name(name)) {
        why = "'" + std::string(name) + "' is not a market name: lower-case letters, digits, - _ and ., starting with a "
              "letter or digit";
        return false;
    }
    for (const std::string& r : arena::suite::split_names(kReservedMarketNames)) {
        if (name == r) {
            why = "a market cannot be called '" + r + "', which names a suite (" + kReservedMarketNames +
                  "); import the file again under another name: /data add FILE name=...";
            return false;
        }
    }
    return true;
}

struct MarketFile {
    std::string name;
    std::string path;        // where it was read from; empty when rebuilt from a run's record
    std::string calibration; // the calibration.* lines in file order, each ending in a newline
    std::vector<std::pair<std::string, std::int64_t>> sets; // kMarketParamNames, in that order
};

// Lines of `text` whose key starts with `prefix`, as they stand.
[[nodiscard]] inline std::string lines_with_prefix(std::string_view text, std::string_view prefix) {
    std::string out;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with(prefix) && line.find('=') != std::string_view::npos) {
            out += line;
            out += '\n';
        }
    }
    return out;
}

// The calibration.* lines a market must have, so a run's conditions say which tape it came from.
inline constexpr const char* kCalibrationKeys =
    "calibration.version calibration.market calibration.base calibration.tape calibration.tape_cksum "
    "calibration.source_cksum calibration.depth calibration.matched";

// Checks what any market needs, from a file or a run's record alike.
[[nodiscard]] inline bool check_market(MarketFile& m, std::string& why) {
    for (const std::string& k : arena::suite::split_names(kCalibrationKeys)) {
        if (arena::suite::cfg_value(m.calibration, k).empty()) {
            why = "no " + k + " line: not a market this build reads";
            return false;
        }
    }
    if (arena::suite::cfg_value(m.calibration, "calibration.version") != std::to_string(kMarketVersion)) {
        why = "calibration version " + arena::suite::cfg_value(m.calibration, "calibration.version") +
              ", this build reads version " + std::to_string(kMarketVersion);
        return false;
    }
    if (arena::suite::cfg_value(m.calibration, "calibration.base") != "standard") {
        why = "calibrated from the suite '" + arena::suite::cfg_value(m.calibration, "calibration.base") +
              "'; this build calibrates the standard suite only";
        return false;
    }
    m.name = arena::suite::cfg_value(m.calibration, "calibration.market");
    return market_name_ok(m.name, why);
}

// Reads a markets/NAME.cfg. Refuses anything but a complete market that is
// admissible: the control held, every seed passed the health gate, and it sets
// exactly the parameters calibration may set.
[[nodiscard]] inline bool parse_market(std::string_view text, MarketFile& out, std::string& why) {
    out = MarketFile{};
    using arena::suite::cfg_value;
    if (cfg_value(text, "status") != "complete") {
        why = "incomplete (no status=complete): calibrate it again";
        return false;
    }
    out.calibration = lines_with_prefix(text, "calibration.");
    if (!check_market(out, why)) return false;
    if (cfg_value(text, "control.held") != "1") {
        why = "the known-answer control did not hold when it was made, so it is not a calibrated market";
        return false;
    }
    std::int64_t seeds = 0, passed = 0;
    if (!parse_int(cfg_value(text, "health.seeds"), seeds) || !parse_int(cfg_value(text, "health.passed"), passed) ||
        seeds <= 0 || passed != seeds) {
        why = "it did not pass the health gate on every seed, so it is not a market";
        return false;
    }
    const std::string sets = lines_with_prefix(text, "set.");
    const std::vector<std::string> names = arena::suite::split_names(kMarketParamNames);
    std::size_t count = 0;
    for (std::size_t at = 0; at < sets.size(); ++count) {
        const std::size_t end = sets.find('\n', at);
        const std::string line = sets.substr(at + 4, end - at - 4);
        at = end + 1;
        const std::string key = line.substr(0, line.find('='));
        bool allowed = false;
        for (const std::string& n : names) allowed = allowed || n == key;
        if (!allowed) {
            why = "it sets " + key + ", which calibration never moves (it may set " + kMarketParamNames + ")";
            return false;
        }
    }
    for (const std::string& n : names) {
        std::int64_t v = 0;
        const std::string raw = cfg_value(text, "set." + n);
        if (!parse_int(raw, v) || v <= 0) {
            why = "set." + n + (raw.empty() ? " is missing" : "=" + raw + " is not a positive whole number");
            return false;
        }
        out.sets.emplace_back(n, v);
    }
    if (count != names.size()) {
        why = "a parameter is set twice";
        return false;
    }
    return true;
}

// `arg` is a market's name, read from `dir`/NAME.cfg, or a path to a .cfg file.
[[nodiscard]] inline bool read_market(const std::string& arg, const std::string& dir, MarketFile& out,
                                      std::string& why) {
    const bool is_path = arg.ends_with(".cfg") || arg.find('/') != std::string::npos ||
                         arg.find('\\') != std::string::npos;
    if (!is_path && !market_name_ok(arg, why)) return false;
    const std::string path = is_path ? arg : dir + "/" + arg + ".cfg";
    std::string text;
    if (!read_file(path, text)) {
        why = is_path ? "cannot read " + path
                      : "no market " + arg + " in " + dir + "/: make one from a tape with /calibrate " + arg;
        return false;
    }
    if (!parse_market(text, out, why)) {
        if (!is_path) why = path + ": " + why; // a path was given, and the caller says it
        return false;
    }
    if (!is_path && out.name != arg) {
        why = path + " holds the market '" + out.name + "', not '" + arg + "'";
        return false;
    }
    out.path = path;
    return true;
}

// The market a run was made on, from the run's own suite.cfg: its name, its
// calibration.* lines, and the values from its roster lines. Whatever else the
// run faced is checked afterwards, line by line, against the market this builds.
[[nodiscard]] inline bool market_from_record(std::string_view suite_cfg, MarketFile& out, std::string& why) {
    out = MarketFile{};
    out.calibration = lines_with_prefix(suite_cfg, "calibration.");
    if (!check_market(out, why)) return false;
    if (arena::suite::cfg_value(suite_cfg, "suite.name") != out.name) {
        why = "the run's suite is not its calibrated market";
        return false;
    }
    for (const std::string& n : arena::suite::split_names(kMarketParamNames)) {
        std::int64_t v = 0;
        if (!parse_int(arena::suite::cfg_value(suite_cfg, n), v)) {
            why = "the run records no " + n;
            return false;
        }
        out.sets.emplace_back(n, v);
    }
    return true;
}

// SuiteDef names its suite and its change as C strings; a calibrated one keeps
// them here for the life of the program. Never freed, and only ever a handful.
[[nodiscard]] inline const char* keep_text(const std::string& s) {
    static std::deque<std::string> kept;
    for (const std::string& k : kept)
        if (k == s) return k.c_str();
    kept.push_back(s);
    return kept.back().c_str();
}

// The standard suite with the market's values: a suite of its own, with its own
// name and fingerprint, and the calibration.* lines among its conditions.
[[nodiscard]] inline bool market_suite(const MarketFile& m, arena::suite::SuiteDef& out, std::string& why) {
    arena::suite::SuiteDef d = arena::suite::standard_suite();
    const arena::RosterConfig before = d.roster;
    std::string change;
    for (const auto& [k, v] : m.sets) {
        if (!arena::set_roster_param(d.roster, k, v)) {
            why = "this build has no parameter " + k;
            return false;
        }
        std::int64_t was = 0;
        if (arena::get_roster_param(before, k, was) && was != v) {
            if (!change.empty()) change += ", ";
            change += k + "=" + std::to_string(v);
        }
    }
    d.name = keep_text(m.name);
    d.change = keep_text(change);
    d.calibration = m.calibration;
    out = d;
    return true;
}

} // namespace px::data
