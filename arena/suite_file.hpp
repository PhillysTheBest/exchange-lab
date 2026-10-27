// arena/suite_file.hpp — a suite of one's own, read from suites/NAME.cfg.
//
// The standard suite is fixed in arena/suite.hpp so that "standard v1" always
// names one set of conditions. A researcher with a different question writes the
// conditions down instead, in a file, and the file becomes a suite of its own:
// its own name, version, fingerprint, placebo arm and seed block.
//
//   # exchange-lab suite file (docs/RUNNER.md, "A suite of your own")
//   suitefile.format=1
//   suite.name=slow_seat
//   suite.version=1
//   suite.lineup=maker,rival,informed,noise:2,momentum
//   suite.session_ns=25000000
//   seat.reaction_ns=100000
//   block.first_seed=42
//   block.seeds=64
//   set.maker.half_spread_ticks=2        (any number of these: the names --set= takes)
//
// WHAT A FILE CANNOT SET, because the evaluator's guarantees rest on it: the seat
// (appended last, never informed), the placebo arm, the health gate and the step
// ceiling. A file that names one is refused, and so is any key this reader does
// not know, so a typo is a message and not a silently standard condition.
//
// THE FINGERPRINT covers every line. The conditions already hold the name, the
// version, the line-up, the session length, the seat's delay and the block; the
// reference parameters reach the fingerprint only through the placebo's session
// hashes, which a parameter of an absent participant never moves. So a file
// suite also writes its whole definition among its conditions as suitefile.*
// lines, sorted, and two files that differ anywhere differ in fingerprint.
//
// ADMISSION, as for a calibrated market: the placebo must pass the health gate on
// every seed of the block before any candidate runs (admit_suite, below).
//
// Read only by the lab programs; `suite` refuses a file. Integer throughout.
#pragma once

#include "arena/suite.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace px::arena::suite {

inline constexpr int kSuiteFileFormat = 1;

// Names a file suite cannot take: a built-in suite's, a block's, a grid's. The
// name of a calibrated market in markets/ is refused too, by the program, which
// knows where markets are.
inline constexpr const char* kReservedSuiteNames =
    "standard wide busy smoke holdout all sensitivity delay-5us delay-20us thin no-momentum no-rival file";

struct SuiteFile {
    std::string name;
    std::uint32_t version = 0;
    std::string lineup;
    SimTime session_ns = 0;
    SimTime seat_reaction_ns = 0;
    std::uint64_t first_seed = 0;
    std::uint32_t seeds = 0;
    std::vector<std::pair<std::string, std::int64_t>> sets; // sorted by name
};

namespace detail {

[[nodiscard]] inline bool suite_int(std::string_view s, std::int64_t& out) noexcept {
    const bool neg = !s.empty() && s[0] == '-';
    const std::string_view d = neg ? s.substr(1) : s;
    if (d.empty() || d.size() > 18) return false;
    std::int64_t v = 0;
    for (const char c : d) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = neg ? -v : v;
    return true;
}

[[nodiscard]] inline bool suite_name_ok(std::string_view n) noexcept {
    if (n.empty() || n.size() > 64) return false;
    if (!((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= '0' && n[0] <= '9'))) return false;
    for (const char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    return true;
}

// SuiteDef names its suite and its line-up as C strings; a file suite keeps them
// here for the life of the program. Never freed, and only ever a handful.
[[nodiscard]] inline const char* keep(const std::string& s) {
    static std::deque<std::string> kept;
    for (const std::string& k : kept)
        if (k == s) return k.c_str();
    kept.push_back(s);
    return kept.back().c_str();
}

} // namespace detail

// Reads a suite file's text. Refuses, naming the line, any key it does not know,
// any key the runner fixes, a repeated key, a missing one, or a value out of bounds.
[[nodiscard]] inline bool parse_suite_file(std::string_view text, SuiteFile& out, std::string& why) {
    out = SuiteFile{};
    std::vector<std::string> seen;
    bool have_format = false;
    std::size_t at = 0, line_no = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line[0] == '#') continue;
        const std::string where = "line " + std::to_string(line_no) + ", '" + std::string(line) + "'";
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            why = where + ": not key=value";
            return false;
        }
        const std::string key(line.substr(0, eq));
        const std::string_view value = line.substr(eq + 1);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
            why = where + ": " + key + " is given twice";
            return false;
        }
        seen.push_back(key);
        std::int64_t v = 0;
        auto number = [&](std::int64_t lo, std::int64_t hi, const char* what) {
            if (!detail::suite_int(value, v) || v < lo || v > hi) {
                why = where + ": " + what + " must be a whole number from " + std::to_string(lo) + " to " +
                      std::to_string(hi);
                return false;
            }
            return true;
        };
        if (key == "suitefile.format") {
            if (!number(kSuiteFileFormat, kSuiteFileFormat, "the format")) return false;
            have_format = true;
        } else if (key == "suite.name") {
            if (!detail::suite_name_ok(value)) {
                why = where + ": a suite name is lower-case letters, digits, - _ and ., starting with a letter or digit";
                return false;
            }
            out.name = std::string(value);
        } else if (key == "suite.version") {
            if (!number(1, 1'000'000, "the version")) return false;
            out.version = static_cast<std::uint32_t>(v);
        } else if (key == "suite.lineup") {
            if (value.empty()) {
                why = where + ": the line-up is empty";
                return false;
            }
            out.lineup = std::string(value);
        } else if (key == "suite.session_ns") {
            if (!number(1'000'000, 1'000'000'000, "suite.session_ns")) return false;
            out.session_ns = v;
        } else if (key == "seat.reaction_ns") {
            if (!number(1, 10'000'000, "seat.reaction_ns")) return false;
            out.seat_reaction_ns = v;
        } else if (key == "block.first_seed") {
            if (!number(0, INT64_MAX / 2, "block.first_seed")) return false;
            out.first_seed = static_cast<std::uint64_t>(v);
        } else if (key == "block.seeds") {
            if (!number(2, 1024, "block.seeds")) return false;
            out.seeds = static_cast<std::uint32_t>(v);
        } else if (key.starts_with("set.")) {
            const std::string param = key.substr(4);
            std::int64_t was = 0;
            if (!get_roster_param(RosterConfig{}, param, was)) {
                why = where + ": " + param + " is not a reference parameter (the names --set= takes for the line-up)";
                return false;
            }
            if (!detail::suite_int(value, v)) {
                why = where + ": the value is not a whole number";
                return false;
            }
            out.sets.emplace_back(param, v);
        } else if (key.starts_with("health.") || key == "suite.step_ceiling" || key.starts_with("seat.") ||
                   key == "block.name") {
            why = where + ": " + key + " is fixed by the runner. The seat (appended last, never informed), the " +
                  "placebo arm, the health gate and the step ceiling are the same in every suite";
            return false;
        } else if (key.starts_with("session.") || key.starts_with("fair.") || key.starts_with("market.")) {
            why = where + ": " + key + " is not settable by a suite file: the session's settings and the " +
                  "fundamental stay the standard market's";
            return false;
        } else {
            why = where + ": unknown key " + key;
            return false;
        }
    }
    const char* missing = !have_format ? "suitefile.format"
                          : out.name.empty() ? "suite.name"
                          : out.version == 0 ? "suite.version"
                          : out.lineup.empty() ? "suite.lineup"
                          : out.session_ns == 0 ? "suite.session_ns"
                          : out.seat_reaction_ns == 0 ? "seat.reaction_ns"
                          : out.seeds == 0 ? "block.seeds"
                                           : nullptr;
    if (missing == nullptr && std::find(seen.begin(), seen.end(), "block.first_seed") == seen.end())
        missing = "block.first_seed";
    if (missing != nullptr) {
        why = std::string("the file sets no ") + missing;
        return false;
    }
    for (const std::string& r : split_names(kReservedSuiteNames)) {
        if (out.name == r) {
            why = "a suite file cannot be called '" + r + "', which names a built-in suite, block or grid";
            return false;
        }
    }
    const SeedBlock* hold = find_block("holdout");
    if (hold != nullptr && out.first_seed < hold->first + hold->count && hold->first < out.first_seed + out.seeds) {
        why = "the block " + std::to_string(out.first_seed) + ".." + std::to_string(out.first_seed + out.seeds - 1) +
              " overlaps the holdout block, which a suite file may not spend";
        return false;
    }
    std::sort(out.sets.begin(), out.sets.end());
    return true;
}

// The whole file, every field in a fixed order: what suitefile.digest hashes.
[[nodiscard]] inline std::string suite_file_text(const SuiteFile& f) {
    std::string s;
    append_kv(s, "suitefile.format", std::int64_t{kSuiteFileFormat});
    append_kv(s, "suite.name", f.name);
    append_kv(s, "suite.version", static_cast<std::int64_t>(f.version));
    append_kv(s, "suite.lineup", f.lineup);
    append_kv(s, "suite.session_ns", f.session_ns);
    append_kv(s, "seat.reaction_ns", f.seat_reaction_ns);
    append_kv(s, "block.first_seed", static_cast<std::int64_t>(f.first_seed));
    append_kv(s, "block.seeds", static_cast<std::int64_t>(f.seeds));
    for (const auto& [k, v] : f.sets) append_kv(s, "set." + k, v);
    return s;
}

// The suitefile.* condition lines: the format, every reference parameter, sorted,
// so the fingerprint covers what the placebo hashes may not, and a digest of the
// whole definition. A run's record rebuilds the suite from its own lines, so an
// edited line would otherwise agree with itself; against the digest it cannot.
[[nodiscard]] inline std::string suite_definition(const SuiteFile& f) {
    std::string s;
    append_kv(s, "suitefile.format", std::int64_t{kSuiteFileFormat});
    for (const auto& [k, v] : f.sets) append_kv(s, "suitefile.set." + k, v);
    std::string digest;
    append_hex(digest, fnv1a(0xcbf29ce484222325ULL, suite_file_text(f)));
    append_kv(s, "suitefile.digest", digest);
    return s;
}

// The suite and block a file defines. The seat, the placebo, the health gate and
// the step ceiling are the standard suite's, whatever the file.
[[nodiscard]] inline bool suite_from_file(const SuiteFile& f, SuiteDef& def, SeedBlock& block, std::string& why) {
    SuiteDef d = standard_suite();
    d.name = detail::keep(f.name);
    d.version = f.version;
    d.lineup = detail::keep(f.lineup);
    d.session_ns = f.session_ns;
    d.seat_reaction_ns = f.seat_reaction_ns;
    std::string change;
    const RosterConfig before = d.roster;
    for (const auto& [k, v] : f.sets) {
        if (!set_roster_param(d.roster, k, v)) {
            why = "this build has no parameter " + k;
            return false;
        }
        std::int64_t was = 0;
        if (get_roster_param(before, k, was) && was != v) change += (change.empty() ? "" : ", ") + k + "=" + std::to_string(v);
    }
    if (d.session_ns != standard_suite().session_ns)
        change += (change.empty() ? "" : ", ") + std::string("session ") + std::to_string(d.session_ns) + " ns";
    if (d.seat_reaction_ns != standard_suite().seat_reaction_ns)
        change += (change.empty() ? "" : ", ") + std::string("seat reaction ") + std::to_string(d.seat_reaction_ns) + " ns";
    d.change = detail::keep(change);
    d.definition = suite_definition(f);
    std::size_t seat = 0;
    if (!lineup_size(d, seat, why)) {
        why = "suite.lineup=" + f.lineup + ": " + why;
        return false;
    }
    if (seat + 1 > Session::kMaxParticipants) {
        why = "suite.lineup=" + f.lineup + " leaves no room for the seat under the participant ceiling";
        return false;
    }
    def = d;
    block = SeedBlock{"file", f.first_seed, f.seeds};
    return true;
}

// The file a run was made from, out of the run's own suite.cfg: every line a
// file can set is among its conditions, so no file is needed.
[[nodiscard]] inline bool suite_file_from_record(std::string_view cfg, SuiteFile& out, std::string& why) {
    std::string text = "suitefile.format=" + cfg_value(cfg, "suitefile.format") + "\n";
    for (const char* k : {"suite.name", "suite.version", "suite.lineup", "suite.session_ns", "seat.reaction_ns",
                          "block.first_seed", "block.seeds"})
        text += std::string(k) + "=" + cfg_value(cfg, k) + "\n";
    std::size_t at = 0;
    while (at < cfg.size()) {
        std::size_t end = cfg.find('\n', at);
        if (end == std::string_view::npos) end = cfg.size();
        const std::string_view line = cfg.substr(at, end - at);
        at = end + 1;
        if (line.starts_with("suitefile.set.")) text += "set." + std::string(line.substr(14)) + "\n";
    }
    if (!parse_suite_file(text, out, why)) {
        why = "the run's own record does not describe a suite file this build reads: " + why;
        return false;
    }
    return true;
}

// The placebo arm on every seed of the block, against the health gate: the seeds
// that fail, with why. Empty: admitted.
[[nodiscard]] inline bool admit_suite(const SuiteDef& def, const SeedBlock& block,
                                      std::vector<std::pair<std::uint64_t, Health>>& failing, std::string& why) {
    failing.clear();
    for (std::uint32_t k = 0; k < block.count; ++k) {
        const std::uint64_t seed = block.first + k;
        Roster r;
        Origin origin = Origin::Reference;
        ArmRecord rec;
        if (!build_arm_roster(def, "placebo", r, origin, why) ||
            !run_arm(def, block, seed, "placebo", origin, std::move(r), rec, why))
            return false;
        if (!rec.session.healthy()) failing.emplace_back(seed, rec.session.health);
    }
    return true;
}

[[nodiscard]] inline std::string admission_text(const SuiteDef& def, const SeedBlock& block,
                                                const std::vector<std::pair<std::uint64_t, Health>>& failing) {
    std::string seeds;
    for (std::size_t i = 0; i < failing.size(); ++i) {
        if (i == 8) {
            seeds += ", ...";
            break;
        }
        seeds += (i == 0 ? "" : ", ") + std::to_string(failing[i].first) + " (" + health_name(failing[i].second) + ")";
    }
    return "suite " + std::string(def.name) + " is not admitted: its placebo fails the health gate on " +
           std::to_string(failing.size()) + " of " + std::to_string(block.count) + " seeds: " + seeds +
           ". A market whose book comes loose from the fundamental is not a market";
}

} // namespace px::arena::suite
