// marketdata/calibrate.hpp — setting the simulated market to match a tape, and
// the control that shows it can.
//
// Calibration runs the reference market (the standard suite's line-up, with no
// seat) on a block of seeds of its own, writes those sessions out as one tape
// (marketdata/session_tape.hpp) and measures it with the same function that
// measured the data (marketdata/stats.hpp), read at the data's depth. It then
// moves three parameters until the two sets of statistics agree:
//
//   spread  maker.half_spread_ticks and rival.half_spread_ticks, to the mean spread
//   size    noise.qty, to the mean print against the mean touch depth
//   pace    noise.timer_ns, to the prints it takes the mid to move one tick
//
// Means rather than medians: a median spread or print is a whole number of
// ticks or lots, and moves in jumps the search cannot steer by.
//
// Everything else keeps the reference market's value: the price grid, the
// fundamental's pace (calibration matches ratios, not the clock), and above all
// the informed trader and the parameters docs/ARENA.md calls load-bearing,
// which no public data can show.
//
// The search makes one number small: the sum, over the statistics the data's
// depth can show, of each one's distance from its target, as a ratio in
// thousandths so units never matter. A coarse scan of the three together comes
// first (calibrate(), below, says why); then descents, each of which scans each
// parameter's whole grid in turn and keeps a value only if it makes the sum
// strictly smaller, until a pass changes nothing; so it cannot cycle. The parameters are matched together
// rather than one statistic each, because each moves all three statistics: a
// wider spread also changes how often the mid moves. A full scan rather than
// bisection because the statistics are not all monotone in their parameter, and
// a scan is cheap: a 25 ms session takes a few milliseconds.
//
// THE KNOWN-ANSWER CONTROL. The project tests its evaluator against answers
// known in advance, and calibration gets the same: sessions run with known
// parameters on seeds of their own are written as a tape, and calibrating from
// that tape must recover every parameter within one step of its grid. Planted
// defects (a reader that drops every cancel, one that misreads the price scale)
// must make it fail; test_data holds both. /calibrate runs the control first
// and refuses to calibrate if it does not hold.
//
// Integer throughout, like the session it drives.
#pragma once

#include "arena/suite.hpp"
#include "marketdata/market_file.hpp"
#include "marketdata/session_tape.hpp"
#include "marketdata/stats.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace px::data {

struct MarketParams {
    std::int64_t half_spread_ticks = 2; // maker and rival alike
    std::int64_t noise_qty = 25;
    std::int64_t noise_timer_ns = 10'000;

    [[nodiscard]] auto key() const noexcept { return std::tuple{half_spread_ticks, noise_qty, noise_timer_ns}; }
    [[nodiscard]] bool operator==(const MarketParams& o) const noexcept { return key() == o.key(); }
};

// The reference market's values, which calibration starts from and keeps for
// any parameter the data cannot speak to.
[[nodiscard]] inline MarketParams reference_params() {
    const arena::RosterConfig r{};
    return MarketParams{r.maker_half_spread_ticks, r.noise_qty, r.noise_timer_ns};
}

enum class Knob : std::uint8_t { Spread = 0, Size, Pace };
inline constexpr Knob kKnobs[] = {Knob::Spread, Knob::Size, Knob::Pace};

[[nodiscard]] inline const char* knob_name(Knob k) noexcept {
    switch (k) {
        case Knob::Spread: return "spread";
        case Knob::Size: return "size";
        case Knob::Pace: return "pace";
    }
    return "?";
}

// What each knob moves, under the names --set= takes.
[[nodiscard]] inline const char* knob_sets(Knob k) noexcept {
    switch (k) {
        case Knob::Spread: return "maker.half_spread_ticks, rival.half_spread_ticks";
        case Knob::Size: return "noise.qty";
        case Knob::Pace: return "noise.timer_ns";
    }
    return "?";
}

// The statistic each knob is matched on.
[[nodiscard]] inline const char* knob_stat(Knob k) noexcept {
    switch (k) {
        case Knob::Spread: return "mean spread, hundredths of a tick";
        case Knob::Size: return "mean print / mean touch depth, thousandths";
        case Knob::Pace: return "prints per one-tick move, hundredths";
    }
    return "?";
}

// The least depth a tape needs for a knob to be calibrated from it.
[[nodiscard]] inline Depth knob_needs(Knob k) noexcept { return k == Knob::Pace ? Depth::Trades : Depth::L2; }

[[nodiscard]] inline const std::vector<std::int64_t>& knob_grid(Knob k) {
    static const std::vector<std::int64_t> spread{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    static const std::vector<std::int64_t> size{5, 10, 15, 20, 25, 30, 40, 50, 60, 80, 100};
    static const std::vector<std::int64_t> pace{2'500, 5'000, 7'500, 10'000, 15'000, 20'000, 30'000, 40'000, 60'000, 80'000};
    return k == Knob::Spread ? spread : k == Knob::Size ? size : pace;
}

[[nodiscard]] inline std::int64_t& knob_value(MarketParams& p, Knob k) noexcept {
    return k == Knob::Spread ? p.half_spread_ticks : k == Knob::Size ? p.noise_qty : p.noise_timer_ns;
}
[[nodiscard]] inline std::int64_t knob_value(const MarketParams& p, Knob k) noexcept {
    return k == Knob::Spread ? p.half_spread_ticks : k == Knob::Size ? p.noise_qty : p.noise_timer_ns;
}

[[nodiscard]] inline std::int64_t knob_stat(const TapeStats& s, Knob k) noexcept {
    return k == Knob::Spread ? s.spread_mean_x100 : k == Knob::Size ? s.size_depth_pml : s.prints_per_move_x100;
}

// The position of a value on a knob's grid, or -1.
[[nodiscard]] inline int grid_index(Knob k, std::int64_t v) {
    const std::vector<std::int64_t>& g = knob_grid(k);
    for (std::size_t i = 0; i < g.size(); ++i)
        if (g[i] == v) return static_cast<int>(i);
    return -1;
}

// How far apart two positive statistics are, as a ratio in thousandths: 0 when
// equal, 250 when one is 1.25 times the other. Unmeasurable is infinitely far.
inline constexpr std::int64_t kFar = std::int64_t{1} << 50;
[[nodiscard]] inline std::int64_t ratio_distance(std::int64_t a, std::int64_t b) noexcept {
    if (a <= 0 || b <= 0) return a == b ? 0 : kFar;
    const std::int64_t hi = std::max(a, b), lo = std::min(a, b);
    if (hi > kFar / 1000) return kFar;
    return hi * 1000 / lo - 1000;
}

// The reference market with these parameters.
[[nodiscard]] inline arena::suite::SuiteDef market_with(const arena::suite::SuiteDef& base, const MarketParams& p) {
    arena::suite::SuiteDef d = base;
    d.roster.maker_half_spread_ticks = p.half_spread_ticks;
    d.roster.rival_half_spread_ticks = p.half_spread_ticks;
    d.roster.noise_qty = p.noise_qty;
    d.roster.noise_timer_ns = p.noise_timer_ns;
    return d;
}

// Seeds of calibration's own, apart from every named block a result is read on,
// so calibration never fits the seeds a strategy is later scored on. The control
// writes its known-answer tape on another set again.
inline constexpr std::uint64_t kCalibrationSeeds[] = {9001, 9002, 9003, 9004, 9005, 9006, 9007, 9008};
inline constexpr std::uint64_t kControlSeeds[] = {7001, 7002, 7003, 7004, 7005, 7006, 7007, 7008};

// Sessions of `def`'s reference market (no seat), one per seed, as one L3 tape.
[[nodiscard]] inline bool simulate(const arena::suite::SuiteDef& def, const std::uint64_t* seeds, std::size_t n,
                                   Tape& out, std::string& why) {
    SessionTapeWriter w(def.name, arena::SessionConfig{}.tick, kPriceScale);
    for (std::size_t k = 0; k < n; ++k) {
        arena::Roster roster;
        arena::suite::Origin origin{};
        if (!arena::suite::build_arm_roster(def, "", roster, origin, why)) return false;
        arena::Session s;
        const arena::Session::StartError bad = s.start(arena::suite::suite_session_config(def, seeds[k]), std::move(roster));
        if (bad != arena::Session::StartError::None) {
            why = arena::Session::explain(bad);
            return false;
        }
        w.attach(s, static_cast<std::int64_t>(k) * def.session_ns);
        std::uint64_t steps = 0;
        while (s.step() && ++steps < def.step_ceiling) {
        }
        s.finish();
        w.detach(s);
    }
    out = std::move(w.tape());
    return true;
}

struct Fit {
    Knob knob = Knob::Spread;
    bool used = false;            // false: the data's depth cannot show it, so the reference value is kept
    std::int64_t target = -1;     // the data's statistic
    std::int64_t achieved = -1;   // the calibrated market's
    std::int64_t distance = kFar; // ratio_distance(achieved, target)
    bool unreachable = false;     // the target lies beyond what any value on the grid gives
};

struct Calibration {
    MarketParams params;
    TapeStats achieved;
    Fit fits[3];
    int starts = 0;      // descents run from the best points of the coarse scan
    int evaluations = 0; // parameter sets simulated, on either set of seeds
};

// What the reference market's statistics are for one set of parameters, on
// calibration's seeds. They do not depend on the target, so one cache can serve
// several calibrations: the known-answer control runs three.
class MarketCache {
public:
    // `seeds` is how many of kCalibrationSeeds to run: the coarse scan uses two.
    [[nodiscard]] bool get(const arena::suite::SuiteDef& base, const MarketParams& p, std::size_t seeds, Depth depth,
                           TapeStats& out, std::string& why) {
        const auto key = std::tuple{p.half_spread_ticks, p.noise_qty, p.noise_timer_ns, seeds};
        auto it = tapes_.find(key);
        if (it == tapes_.end()) {
            Tape t;
            if (!simulate(market_with(base, p), kCalibrationSeeds, seeds, t, why)) return false;
            it = tapes_.emplace(key, std::map<int, TapeStats>{}).first;
            for (const Depth d : {Depth::Trades, Depth::L2, Depth::L3}) it->second[static_cast<int>(d)] = tape_stats(t, d);
            ++simulated_;
        }
        out = it->second[static_cast<int>(depth)];
        return true;
    }
    [[nodiscard]] int simulated() const noexcept { return simulated_; }

private:
    std::map<std::tuple<std::int64_t, std::int64_t, std::int64_t, std::size_t>, std::map<int, TapeStats>> tapes_;
    int simulated_ = 0;
};

// The coarse scan's grid: every spread, and a spread of sizes and paces wide
// enough to land in the right basin. The descent then refines on the full grids.
[[nodiscard]] inline const std::vector<std::int64_t>& coarse_grid(Knob k) {
    static const std::vector<std::int64_t> size{10, 25, 50, 100};
    static const std::vector<std::int64_t> pace{5'000, 10'000, 20'000, 40'000, 80'000};
    return k == Knob::Spread ? knob_grid(k) : k == Knob::Size ? size : pace;
}
inline constexpr std::size_t kCoarseSeeds = 2;
inline constexpr std::size_t kDescentStarts = 3;

// Moves the three parameters until the reference market's statistics, read at
// `depth`, are as close to `target` as the grids allow. In two stages, because
// a descent alone stalls in the wrong basin: one market with a wide spread and
// slow noise, and another with a wider spread and fast noise, have nearly the
// same pace. So first a coarse scan of the three together on two seeds, then a
// descent on all eight seeds from each of its three best points; the closest
// end wins.
[[nodiscard]] inline bool calibrate(const TapeStats& target, Depth depth, const arena::suite::SuiteDef& base,
                                    MarketCache& cache, Calibration& out, std::string& why) {
    out = Calibration{};
    const int before = cache.simulated();
    bool used[3] = {};
    for (const Knob k : kKnobs) {
        const auto i = static_cast<std::size_t>(k);
        used[i] = static_cast<int>(depth) >= static_cast<int>(knob_needs(k)) && knob_stat(target, k) > 0;
    }
    // One number to make small: every used statistic's distance from its target.
    auto total = [&](const TapeStats& st) {
        std::int64_t t = 0;
        for (const Knob k : kKnobs)
            if (used[static_cast<std::size_t>(k)]) t += ratio_distance(knob_stat(st, k), knob_stat(target, k));
        return t;
    };
    const std::size_t all = std::size(kCalibrationSeeds);
    auto cost = [&](const MarketParams& p, std::size_t seeds, std::int64_t& t) {
        TapeStats st;
        if (!cache.get(base, p, seeds, depth, st, why)) return false;
        t = total(st);
        return true;
    };

    // The coarse scan, over the knobs this depth can show; the rest stay at the reference.
    const MarketParams ref = reference_params();
    std::vector<std::pair<std::int64_t, MarketParams>> coarse;
    auto values = [&](Knob k) {
        return used[static_cast<std::size_t>(k)] ? coarse_grid(k) : std::vector<std::int64_t>{knob_value(ref, k)};
    };
    for (const std::int64_t a : values(Knob::Spread))
        for (const std::int64_t b : values(Knob::Size))
            for (const std::int64_t c : values(Knob::Pace)) {
                const MarketParams p{a, b, c};
                std::int64_t t = 0;
                if (!cost(p, kCoarseSeeds, t)) return false;
                coarse.emplace_back(t, p);
            }
    std::stable_sort(coarse.begin(), coarse.end(), [](const auto& x, const auto& y) { return x.first < y.first; });

    // A descent: each knob's whole grid in turn, keeping a value only if it makes
    // the total strictly smaller; when none does, every neighbour one grid step
    // away on each axis. Strictly smaller every time, so it cannot cycle.
    auto descend = [&](MarketParams p, std::int64_t& end_t) {
        if (!cost(p, all, end_t)) return std::optional<MarketParams>{};
        for (;;) {
            bool changed = false;
            for (const Knob k : kKnobs) {
                if (!used[static_cast<std::size_t>(k)]) continue;
                for (const std::int64_t v : knob_grid(k)) {
                    MarketParams q = p;
                    knob_value(q, k) = v;
                    std::int64_t t = 0;
                    if (!cost(q, all, t)) return std::optional<MarketParams>{};
                    if (t < end_t) {
                        p = q;
                        end_t = t;
                        changed = true;
                    }
                }
            }
            if (changed) continue;
            const int at[3] = {grid_index(Knob::Spread, p.half_spread_ticks), grid_index(Knob::Size, p.noise_qty),
                               grid_index(Knob::Pace, p.noise_timer_ns)};
            MarketParams best = p;
            for (int a = -1; a <= 1; ++a)
                for (int b = -1; b <= 1; ++b)
                    for (int c = -1; c <= 1; ++c) {
                        const int step[3] = {a, b, c};
                        MarketParams q = p;
                        bool ok = true;
                        for (const Knob k : kKnobs) {
                            const auto i = static_cast<std::size_t>(k);
                            if (step[i] == 0) continue;
                            const int j = at[i] + step[i];
                            if (!used[i] || at[i] < 0 || j < 0 || j >= static_cast<int>(knob_grid(k).size())) ok = false;
                            else knob_value(q, k) = knob_grid(k)[static_cast<std::size_t>(j)];
                        }
                        if (!ok || q == p) continue;
                        std::int64_t t = 0;
                        if (!cost(q, all, t)) return std::optional<MarketParams>{};
                        if (t < end_t) {
                            best = q;
                            end_t = t;
                        }
                    }
            if (best == p) return std::optional<MarketParams>{p};
            p = best;
        }
    };

    std::int64_t best_t = kFar * 4;
    MarketParams best = ref;
    for (std::size_t i = 0; i < coarse.size() && i < kDescentStarts; ++i) {
        std::int64_t t = 0;
        const std::optional<MarketParams> end = descend(coarse[i].second, t);
        if (!end) return false;
        ++out.starts;
        if (t < best_t) {
            best_t = t;
            best = *end;
        }
    }

    out.params = best;
    if (!cache.get(base, best, all, depth, out.achieved, why)) return false;
    for (const Knob k : kKnobs) {
        Fit& f = out.fits[static_cast<std::size_t>(k)];
        f.knob = k;
        f.used = used[static_cast<std::size_t>(k)];
        f.target = knob_stat(target, k);
        f.achieved = knob_stat(out.achieved, k);
        f.distance = f.used ? ratio_distance(f.achieved, f.target) : kFar;
        if (!f.used) continue;
        // Unreachable: no value on the grid gets past the achieved value
        // towards the target, so the grid's end is as close as this market gets.
        bool beyond = true;
        MarketParams q = best;
        for (const std::int64_t v : knob_grid(k)) {
            knob_value(q, k) = v;
            TapeStats st;
            if (!cache.get(base, q, all, depth, st, why)) return false;
            const std::int64_t s = knob_stat(st, k);
            if ((f.target > f.achieved && s > f.achieved) || (f.target < f.achieved && s < f.achieved)) beyond = false;
        }
        f.unreachable = f.distance > 0 && beyond;
    }
    out.evaluations = cache.simulated() - before;
    return true;
}

// ---- the known-answer control ------------------------------------------------

// The parameters the control's tape is made with: none of them the reference
// market's, so recovering them means calibration moved every knob.
[[nodiscard]] inline MarketParams control_truth() { return MarketParams{3, 40, 20'000}; }

// A fault planted in the path from tape to statistics, to show the control can fail.
enum class Defect : std::uint8_t { None = 0, DropCancels, PriceTimesTen };

[[nodiscard]] inline const char* defect_name(Defect d) noexcept {
    switch (d) {
        case Defect::None: return "none";
        case Defect::DropCancels: return "drops every cancel";
        case Defect::PriceTimesTen: return "misreads the price scale by ten";
    }
    return "?";
}

inline void plant(Tape& t, Defect d) {
    if (d == Defect::DropCancels) {
        std::erase_if(t.ev, [](const TapeEvent& e) { return e.kind == 'C'; });
    } else if (d == Defect::PriceTimesTen) {
        for (TapeEvent& e : t.ev) e.price *= 10;
    }
}

struct ControlResult {
    Depth depth = Depth::L3;
    Defect defect = Defect::None;
    MarketParams truth;
    Calibration calibration;
    bool recovered[3] = {}; // per knob: within one grid step of the truth, or not calibrated at this depth
    [[nodiscard]] bool held() const noexcept { return recovered[0] && recovered[1] && recovered[2]; }
};

// Writes sessions made with control_truth() as a tape, reads it at `depth`
// (through `defect`), calibrates from it, and checks what came back.
[[nodiscard]] inline bool known_answer(const arena::suite::SuiteDef& base, Depth depth, Defect defect,
                                       MarketCache& cache, ControlResult& out, std::string& why) {
    out = ControlResult{};
    out.depth = depth;
    out.defect = defect;
    out.truth = control_truth();
    Tape truth;
    if (!simulate(market_with(base, out.truth), kControlSeeds, std::size(kControlSeeds), truth, why)) return false;
    plant(truth, defect);
    if (!calibrate(tape_stats(truth, depth), depth, base, cache, out.calibration, why)) return false;
    for (const Knob k : kKnobs) {
        const auto i = static_cast<std::size_t>(k);
        if (!out.calibration.fits[i].used) {
            out.recovered[i] = true; // kept at the reference value, and said so
            continue;
        }
        const int want = grid_index(k, knob_value(out.truth, k));
        const int got = grid_index(k, knob_value(out.calibration.params, k));
        out.recovered[i] = want >= 0 && got >= 0 && (got > want ? got - want : want - got) <= 1;
    }
    return true;
}

// ---- a calibrated market, checked and written down ----------------------------

// The health gate on every seed of a block, for the market with no seat: the
// suite's own rule (arena/suite.hpp), applied before the market is written, so a
// market whose book comes loose from the fundamental is never offered.
struct HealthResult {
    std::string block;
    std::uint32_t seeds = 0;
    std::uint32_t passed = 0;
    std::vector<std::pair<std::uint64_t, arena::suite::Health>> failing; // seed, reason
    std::int64_t worst_gap_x100 = 0; // the largest mean |mid - V| of any seed, hundredths of a tick
    std::uint64_t worst_seed = 0;
    [[nodiscard]] bool held() const noexcept { return seeds > 0 && passed == seeds; }
};

[[nodiscard]] inline bool health_check(const arena::suite::SuiteDef& def, const arena::suite::SeedBlock& block,
                                       HealthResult& out, std::string& why) {
    out = HealthResult{};
    out.block = block.name;
    for (std::uint32_t k = 0; k < block.count; ++k) {
        const std::uint64_t seed = block.first + k;
        arena::Roster roster;
        arena::suite::Origin origin{};
        arena::suite::ArmRecord rec;
        if (!arena::suite::build_arm_roster(def, "", roster, origin, why) ||
            !arena::suite::run_arm(def, block, seed, "none", origin, std::move(roster), rec, why))
            return false;
        ++out.seeds;
        if (rec.session.healthy()) ++out.passed;
        else out.failing.emplace_back(seed, rec.session.health);
        const std::uint64_t n = rec.session.gap_samples;
        const auto gap = n == 0 ? std::int64_t{0} : static_cast<std::int64_t>(rec.session.gap_sum_ticks * 100 / n);
        if (k == 0 || gap > out.worst_gap_x100) {
            out.worst_gap_x100 = gap;
            out.worst_seed = seed;
        }
    }
    return true;
}

// "3 / 40 / 20000": spread, size and pace, the order every table uses.
[[nodiscard]] inline std::string knobs_text(const MarketParams& p) {
    return std::to_string(p.half_spread_ticks) + " / " + std::to_string(p.noise_qty) + " / " +
           std::to_string(p.noise_timer_ns);
}

[[nodiscard]] inline std::string matched_text(const Calibration& c) {
    std::string s;
    for (const Knob k : kKnobs) {
        if (!c.fits[static_cast<std::size_t>(k)].used) continue;
        if (!s.empty()) s += ", ";
        s += knob_name(k);
    }
    return s.empty() ? "nothing" : s;
}

// markets/NAME.cfg, as marketdata/market_file.hpp reads it. The calibration.*
// lines become conditions of every run on the market; set.* are its values;
// the rest is there to be read, and the control and health lines are what the
// reader insists on before the market may be used.
[[nodiscard]] inline std::string market_file_text(const std::string& name, const Tape& tape,
                                                  const std::string& tape_cksum, const Calibration& c,
                                                  const ControlResult& control, const HealthResult& health) {
    std::string s;
    auto kv = [&s](std::string_view k, std::string_view v) {
        s += k;
        s += '=';
        s += v;
        s += '\n';
    };
    s += "# exchange-lab market, calibrated from a tape (docs/DATA.md). Simulated, not measured:\n";
    s += "# it resembles the tape in the statistics below and in nothing else. The lab programs\n";
    s += "# read it: /compare STRATEGY market=" + name + ", /watch market=" + name + ", /sweep market=" + name + ".\n";
    s += "# Which tape, and how: conditions of every run on this market.\n";
    kv("calibration.version", std::to_string(kMarketVersion));
    kv("calibration.market", name);
    kv("calibration.base", "standard");
    kv("calibration.tape", tape.h.name);
    kv("calibration.tape_cksum", tape_cksum);
    kv("calibration.source_cksum", tape.h.source_cksum.empty() ? "-" : tape.h.source_cksum);
    kv("calibration.depth", depth_name(tape.h.depth));
    kv("calibration.matched", matched_text(c));
    s += "# What it sets. Every other parameter is the standard market's.\n";
    kv("set.maker.half_spread_ticks", std::to_string(c.params.half_spread_ticks));
    kv("set.rival.half_spread_ticks", std::to_string(c.params.half_spread_ticks));
    kv("set.noise.qty", std::to_string(c.params.noise_qty));
    kv("set.noise.timer_ns", std::to_string(c.params.noise_timer_ns));
    s += "# The tape it came from.\n";
    kv("tape.format", tape.h.format);
    kv("tape.file", tape.h.source_file);
    kv("tape.instrument", tape.h.instrument.empty() ? "-" : tape.h.instrument);
    kv("tape.date", tape.h.date.empty() ? "-" : tape.h.date);
    kv("tape.events", std::to_string(tape.ev.size()));
    kv("tape.span_ns", std::to_string(tape.ev.empty() ? 0 : tape.ev.back().t));
    s += "# Each statistic: the tape's, the market's, and how far apart as a ratio, in thousandths.\n";
    s += "# unreachable=1: no value on the grid comes closer, so the market is as near as it gets.\n";
    for (const Fit& f : c.fits) {
        const std::string k = std::string("stat.") + knob_name(f.knob);
        kv(k + ".measures", knob_stat(f.knob));
        kv(k + ".matched", f.used ? "1" : "0");
        kv(k + ".tape", std::to_string(f.target));
        kv(k + ".market", std::to_string(f.achieved));
        kv(k + ".apart", f.used ? std::to_string(f.distance) : "-");
        kv(k + ".unreachable", f.unreachable ? "1" : "0");
    }
    s += "# How it was searched for: the reference market, no seat, seeds of calibration's own.\n";
    kv("search.seeds", std::to_string(kCalibrationSeeds[0]) + "-" +
                           std::to_string(kCalibrationSeeds[std::size(kCalibrationSeeds) - 1]));
    kv("search.session_ns", std::to_string(arena::suite::standard_suite().session_ns));
    s += "# Why it may be used: the known-answer control held on the build that made it, and\n";
    s += "# the book stayed with the fundamental on every seed of the block, with no seat.\n";
    kv("control.depth", depth_name(control.depth));
    kv("control.truth", knobs_text(control.truth));
    kv("control.recovered", knobs_text(control.calibration.params));
    kv("control.held", control.held() ? "1" : "0");
    kv("health.block", health.block);
    kv("health.seeds", std::to_string(health.seeds));
    kv("health.passed", std::to_string(health.passed));
    std::string failing;
    for (const auto& [seed, h] : health.failing) {
        if (!failing.empty()) failing += ',';
        failing += std::to_string(seed) + ":" + arena::suite::health_name(h);
    }
    kv("health.failing", failing.empty() ? "-" : failing);
    kv("health.worst_mean_gap_x100", std::to_string(health.worst_gap_x100));
    kv("health.worst_seed", std::to_string(health.worst_seed));
    kv("status", "complete");
    return s;
}

} // namespace px::data
