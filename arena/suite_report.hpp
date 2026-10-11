// arena/suite_report.hpp — what a standard-suite run says, computed only from
// its RunRecord, after every session has finished.
//
// This is the only floating point in the experiment runner, and it never runs
// inside a session: arena/suite.hpp produces integer records, and everything
// here is arithmetic on those records. Because every function takes a
// RunRecord and nothing else, a later step can rebuild one from the files on
// disk and print the same report.
//
// THE STATISTIC. Every arm runs on the same seeds, and the seed fixes the path
// of the fundamental and its closing value, so the difference between two arms
// *at the same seed* cancels the largest noise term: inventory marked at a
// random-walk price. The paired difference is reported with its standard
// error, a 95% interval from Student's t, the smallest effect the block could
// resolve (2.8 standard errors, as bench/summarize_arena.py defines it), the
// full range, and the share of variance the pairing removed. That last figure
// is printed because it varies enormously: pairing removes most of the noise
// between two versions of one strategy and almost none between unrelated ones.
#pragma once

#include "arena/suite.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace px::arena::suite {

// Two-sided 95% critical values of Student's t, by degrees of freedom. A table
// rather than a function, so the same number comes out on every platform; it
// must cover every block's count - 1, and test_arena checks that it does.
[[nodiscard]] inline double t975(std::size_t df) noexcept {
    static constexpr double k[] = {
        0,     12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, // 0..10
        2.201, 2.179,  2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,        // 11..20
        2.080, 2.074,  2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042,        // 21..30
    };
    if (df < sizeof k / sizeof k[0]) return k[df];
    if (df == 63) return 1.998;
    if (df == 127) return 1.979;
    return 0; // not in the table: test_arena fails if a block needs it
}

struct Paired {
    std::size_t n = 0;
    double mean = 0, sd = 0, se = 0, t = 0, lo = 0, hi = 0, resolvable = 0;
    double median = 0, min = 0, max = 0;
    std::size_t ahead = 0;     // seeds where the arm beat the baseline
    double var_removed = 0;    // 1 - var(d) / (var(a) + var(b))
    bool var_removed_defined = false;
    bool identical = false;    // every per-seed difference exactly 0
    bool straddles = false;    // the full range includes 0
    [[nodiscard]] bool separated() const noexcept { return !identical && (lo > 0 || hi < 0); }
};

[[nodiscard]] inline double sample_var(const std::vector<double>& v, double mean) {
    if (v.size() < 2) return 0;
    double ss = 0;
    for (const double x : v) ss += (x - mean) * (x - mean);
    return ss / static_cast<double>(v.size() - 1);
}

[[nodiscard]] inline double mean_of(const std::vector<double>& v) {
    if (v.empty()) return 0;
    double s = 0;
    for (const double x : v) s += x;
    return s / static_cast<double>(v.size());
}

// The paired statistic of `arm - base`, seed by seed, in whatever units the
// inputs are in. Inputs are integers: the per-seed values are exact.
[[nodiscard]] inline Paired paired(const std::vector<std::int64_t>& base, const std::vector<std::int64_t>& arm) {
    Paired p;
    const std::size_t n = std::min(base.size(), arm.size());
    p.n = n;
    if (n == 0) return p;
    std::vector<double> a(n), b(n), d(n);
    bool all_zero = true;
    for (std::size_t i = 0; i < n; ++i) {
        b[i] = static_cast<double>(base[i]);
        a[i] = static_cast<double>(arm[i]);
        const std::int64_t di = arm[i] - base[i];
        d[i] = static_cast<double>(di);
        if (di != 0) all_zero = false;
        if (di > 0) ++p.ahead;
    }
    p.identical = all_zero;
    p.mean = mean_of(d);
    p.sd = std::sqrt(sample_var(d, p.mean));
    p.se = p.sd / std::sqrt(static_cast<double>(n));
    p.t = t975(n - 1);
    p.lo = p.mean - p.t * p.se;
    p.hi = p.mean + p.t * p.se;
    p.resolvable = 2.8 * p.se;
    std::vector<double> sorted = d;
    std::sort(sorted.begin(), sorted.end());
    p.min = sorted.front();
    p.max = sorted.back();
    p.median = n % 2 == 1 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
    p.straddles = p.min <= 0 && p.max >= 0;
    const double va = sample_var(a, mean_of(a));
    const double vb = sample_var(b, mean_of(b));
    if (va + vb > 0) {
        p.var_removed = 1.0 - sample_var(d, p.mean) / (va + vb);
        p.var_removed_defined = true;
    }
    return p;
}

// One row of comparison.csv, in currency units per session.
struct ComparisonRow {
    std::string baseline;
    std::string arm;
    Paired stats;
    bool admissible = true;
    std::string verdict; // not_admissible, identical, separated, not_separated
};

[[nodiscard]] inline double to_currency(double price_units) {
    return price_units / static_cast<double>(kPriceScale);
}

// The seat's value per seed for one arm, in block order.
[[nodiscard]] inline std::vector<std::int64_t> seat_values(const RunRecord& run, const std::string& arm) {
    std::vector<std::int64_t> v;
    for (const SessionRow& r : run.sessions) {
        if (r.arm == arm) v.push_back(r.seat_value);
    }
    return v;
}

// Every candidate against the placebo, then every later candidate against the
// first. Statistics always cover every seed of the block: a failing seed
// changes the verdict, it is never dropped, because dropping it would condition
// on the outcome.
[[nodiscard]] inline std::vector<ComparisonRow> compare(const RunRecord& run) {
    const std::vector<ArmSummary> arms = summarize_arms(run);
    auto admissible = [&arms](const std::string& name) {
        for (const ArmSummary& a : arms) {
            if (a.arm == name) return a.admissible();
        }
        return false;
    };
    std::vector<std::pair<std::string, std::string>> pairs;
    for (const CandidateInfo& c : run.info.candidates) pairs.emplace_back("placebo", c.name);
    for (std::size_t i = 1; i < run.info.candidates.size(); ++i) {
        pairs.emplace_back(run.info.candidates[0].name, run.info.candidates[i].name);
    }
    std::vector<ComparisonRow> out;
    for (const auto& [base, arm] : pairs) {
        ComparisonRow r;
        r.baseline = base;
        r.arm = arm;
        r.stats = paired(seat_values(run, base), seat_values(run, arm));
        r.admissible = admissible(base) && admissible(arm);
        if (!r.admissible) r.verdict = "not_admissible";
        else if (r.stats.identical) r.verdict = "identical";
        else if (r.stats.separated()) r.verdict = "separated";
        else r.verdict = "not_separated";
        out.push_back(std::move(r));
    }
    return out;
}

[[nodiscard]] inline std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

[[nodiscard]] inline std::string comparison_csv(const std::vector<ComparisonRow>& rows) {
    std::string s =
        "baseline,arm,seeds,mean_diff,se_diff,t_crit,ci95_lo,ci95_hi,resolvable,median_diff,"
        "min_diff,max_diff,range_straddles_zero,arm_ahead_seeds,var_removed_pct,verdict\n";
    for (const ComparisonRow& r : rows) {
        const Paired& p = r.stats;
        s += r.baseline + ',' + r.arm + ',';
        append_u(s, p.n);
        s += ',' + fmt("%.4f", to_currency(p.mean)) + ',' + fmt("%.4f", to_currency(p.se)) + ',' +
             fmt("%.3f", p.t) + ',' + fmt("%.4f", to_currency(p.lo)) + ',' + fmt("%.4f", to_currency(p.hi)) +
             ',' + fmt("%.4f", to_currency(p.resolvable)) + ',' + fmt("%.4f", to_currency(p.median)) + ',' +
             fmt("%.4f", to_currency(p.min)) + ',' + fmt("%.4f", to_currency(p.max)) + ',';
        append_u(s, p.straddles ? 1 : 0);
        s += ',';
        append_u(s, p.ahead);
        s += ',';
        // Empty against the placebo, whose value has no variance at all.
        if (p.var_removed_defined && r.baseline != "placebo") s += fmt("%.1f", 100.0 * p.var_removed);
        s += ',' + r.verdict + '\n';
    }
    return s;
}

// ---- the printed report --------------------------------------------------------

struct ArmHealth {
    double gap_median = 0, gap_worst = 0, one_sided_worst = 0;
};

[[nodiscard]] inline ArmHealth arm_health(const RunRecord& run, const std::string& arm) {
    std::vector<double> gaps;
    ArmHealth h;
    for (const SessionRow& r : run.sessions) {
        if (r.arm != arm) continue;
        const double g = r.gap_samples == 0 ? 0.0
                                            : static_cast<double>(r.gap_sum_ticks) / static_cast<double>(r.gap_samples);
        gaps.push_back(g);
        h.gap_worst = std::max(h.gap_worst, g);
        const std::uint64_t total = r.gap_samples + r.gap_unpriced;
        const double one = total == 0 ? 0.0 : 100.0 * static_cast<double>(r.gap_unpriced) / static_cast<double>(total);
        h.one_sided_worst = std::max(h.one_sided_worst, one);
    }
    if (!gaps.empty()) {
        std::sort(gaps.begin(), gaps.end());
        const std::size_t n = gaps.size();
        h.gap_median = n % 2 == 1 ? gaps[n / 2] : (gaps[n / 2 - 1] + gaps[n / 2]) / 2.0;
    }
    return h;
}

[[nodiscard]] inline double median_u(std::vector<std::uint64_t> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 == 1 ? static_cast<double>(v[n / 2])
                      : (static_cast<double>(v[n / 2 - 1]) + static_cast<double>(v[n / 2])) / 2.0;
}

[[nodiscard]] inline std::string with_commas(std::int64_t v) {
    std::string digits = std::to_string(v < 0 ? -v : v);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return v < 0 ? "-" + out : out;
}

[[nodiscard]] inline std::string report_text(const RunRecord& run, const std::vector<ComparisonRow>& rows,
                                             std::uint64_t fingerprint) {
    std::string s;
    char b[512];
    const SuiteDef& d = run.def;
    const SeedBlock& blk = run.block;
    const std::vector<ArmSummary> arms = summarize_arms(run);
    std::size_t seat = 0;
    std::string why;
    if (!lineup_size(d, seat, why)) seat = 0;

    s += "arena suite - simulated, not measured. The engine's latency is in README.md.\n";
    std::snprintf(b, sizeof b, "  suite     %s v%u, block %s: %u seeds (%llu..%llu), %s ns of simulated time each\n",
                  d.name, d.version, blk.name, blk.count, static_cast<unsigned long long>(blk.first),
                  static_cast<unsigned long long>(blk.first + blk.count - 1), with_commas(d.session_ns).c_str());
    s += b;
    std::snprintf(b, sizeof b, "  market    %s, every parameter at its default\n", d.lineup);
    s += b;
    std::snprintf(b, sizeof b, "  seat      #%zu (owner %zu), appended last, never informed; reaction %s ns, set by the suite\n",
                  seat, seat + 1, with_commas(d.seat_reaction_ns).c_str());
    s += b;
    if (!run.info.candidates.empty()) {
        s += "            declared:";
        for (std::size_t i = 0; i < run.info.candidates.size(); ++i) {
            const CandidateInfo& c = run.info.candidates[i];
            std::snprintf(b, sizeof b, "%s %s %s ns", i == 0 ? "" : ",", c.name.c_str(),
                          with_commas(c.declared_reaction_ns).c_str());
            s += b;
        }
        std::snprintf(b, sizeof b, "; all ran at %s\n", with_commas(d.seat_reaction_ns).c_str());
        s += b;
    }
    s += "  arms      placebo (the empty seat)";
    for (const CandidateInfo& c : run.info.candidates) {
        s += ", " + c.name + " (" + (c.origin == Origin::Installed ? "installed" : "built in") + ")";
    }
    s += "\n  checks    every candidate session run twice; every placebo session against the market with no seat\n";
    std::snprintf(b, sizeof b, "  fingerprint %016llx\n", static_cast<unsigned long long>(fingerprint));
    s += b;

    s += "\nmarket health: read this first. A session that fails it is not a market, and an arm with one is not compared\n";
    std::snprintf(b, sizeof b, "  %-14s | %-31s | %-16s | %-7s | %s\n", "arm", "|mid - V| ticks: median (worst)",
                  "one-sided, worst", "healthy", "inert");
    s += b;
    s += "  " + std::string(92, '-') + "\n";
    for (const ArmSummary& a : arms) {
        const ArmHealth h = arm_health(run, a.arm);
        const std::string inert =
            a.arm == "placebo" ? std::string("-") : std::to_string(a.inert) + "/" + std::to_string(a.seeds);
        const std::string gap = fmt("%6.2f", h.gap_median) + " (" + fmt("%.2f", h.gap_worst) + ")";
        const std::string one = fmt("%6.2f%%", h.one_sided_worst);
        const std::string healthy = std::to_string(a.healthy) + "/" + std::to_string(a.seeds);
        std::snprintf(b, sizeof b, "  %-14s | %-31s | %-16s | %-7s | %s\n", a.arm.c_str(), gap.c_str(), one.c_str(),
                      healthy.c_str(), inert.c_str());
        s += b;
    }

    std::snprintf(b, sizeof b,
                  "\nthe seat: currency units per session, %u seeds; fills, orders and rejects are medians\n",
                  blk.count);
    s += b;
    std::snprintf(b, sizeof b, "  %-14s | %-17s | %-9s | %-21s | %6s | %6s | %s\n", "arm", "mean +- s.e.", "median",
                  "range", "fills", "orders", "rejects");
    s += b;
    s += "  " + std::string(96, '-') + "\n";
    for (const ArmSummary& a : arms) {
        std::vector<std::int64_t> zero;
        std::vector<std::int64_t> vals = seat_values(run, a.arm);
        zero.assign(vals.size(), 0);
        const Paired p = paired(zero, vals);
        std::vector<std::uint64_t> fills, orders, rejects;
        for (const SessionRow& r : run.sessions) {
            if (r.arm != a.arm) continue;
            fills.push_back(r.seat_fills);
            orders.push_back(r.seat_orders);
        }
        for (const ParticipantRow& pr : run.parts) {
            if (pr.arm == a.arm && pr.seat) rejects.push_back(pr.account.rejects);
        }
        std::snprintf(b, sizeof b, "  %-14s | %+8.2f +- %5.2f | %+9.2f | %+9.2f .. %+8.2f | %6.0f | %6.0f | %7.0f\n",
                      a.arm.c_str(), to_currency(p.mean), to_currency(p.se), to_currency(p.median),
                      to_currency(p.min), to_currency(p.max), median_u(fills), median_u(orders), median_u(rejects));
        s += b;
    }

    if (!rows.empty()) {
        s += "\npaired, seed by seed on a shared fundamental: the statistic to read\n";
        std::snprintf(b, sizeof b, "  %-24s | %-18s | %-20s | %-10s | %-21s | %-5s | %-7s | %s\n", "arm vs baseline",
                      "mean +- s.e.", "95% interval", "resolvable", "range", "ahead", "pairing", "verdict");
        s += b;
        s += "  " + std::string(140, '-') + "\n";
        for (const ComparisonRow& r : rows) {
            const Paired& p = r.stats;
            const std::string label = r.arm + " vs " + r.baseline;
            const std::string removed = r.baseline != "placebo" && p.var_removed_defined
                                            ? fmt("%5.0f%%", 100.0 * p.var_removed)
                                            : std::string("    -");
            std::string verdict = r.verdict;
            if (r.verdict == "not_admissible") {
                verdict = "NOT ADMISSIBLE";
            } else if (r.verdict == "not_separated") {
                verdict = "not separated";
            }
            std::snprintf(b, sizeof b,
                          "  %-24s | %+8.2f +- %6.2f | %+8.2f .. %+8.2f | +- %7.2f | %+9.2f .. %+8.2f | %2zu/%-2zu | %-7s | %s\n",
                          label.c_str(), to_currency(p.mean), to_currency(p.se), to_currency(p.lo), to_currency(p.hi),
                          to_currency(p.resolvable), to_currency(p.min), to_currency(p.max), p.ahead, p.n,
                          removed.c_str(), verdict.c_str());
            s += b;
        }
        std::snprintf(b, sizeof b,
                      "  The 95%% interval uses t with %zu degrees of freedom. \"resolvable\" is the smallest difference\n"
                      "  %u seeds could resolve (2.8 x s.e., as bench/summarize_arena.py). \"pairing\" is the share of\n"
                      "  variance that pairing by seed removed: high for related strategies, low for unrelated ones.\n",
                      static_cast<std::size_t>(blk.count) - 1, blk.count);
        s += b;
        for (const ComparisonRow& r : rows) {
            if (r.verdict == "identical") {
                std::snprintf(b, sizeof b, "  %s vs %s: identical on every seed; %s\n", r.arm.c_str(),
                              r.baseline.c_str(),
                              r.baseline == "placebo" ? "it sent no orders, so its sessions are the placebo's"
                                                      : "the two arms are the same market");
                s += b;
            }
        }
    }

    // What failed, by name, so no verdict has to be taken on trust.
    for (const ArmSummary& a : arms) {
        if (a.failing.empty()) continue;
        std::string list;
        for (std::size_t i = 0; i < a.failing.size() && i < 8; ++i) {
            if (i != 0) list += ", ";
            list += std::to_string(a.failing[i].first) + ": " + health_name(a.failing[i].second);
        }
        if (a.failing.size() > 8) list += ", ...";
        if (a.arm == "placebo") {
            std::snprintf(b, sizeof b,
                          "\n  THE REFERENCE MARKET IS NOT ADMISSIBLE on this build (%s): nothing is compared\n",
                          list.c_str());
        } else {
            std::snprintf(b, sizeof b,
                          "\n  NOT ADMISSIBLE: %s failed on %zu/%u seeds (%s); its numbers are in comparison.csv and do not count\n",
                          a.arm.c_str(), a.failing.size(), a.seeds, list.c_str());
        }
        s += b;
    }

    std::uint32_t placebo_checks = 0, placebo_seeds = 0;
    bool replays = true;
    for (const SessionRow& r : run.sessions) {
        if (r.arm == "placebo") {
            ++placebo_seeds;
            if (r.check_ok) ++placebo_checks;
        } else if (!r.check_ok) {
            replays = false;
        }
    }
    std::snprintf(b, sizeof b, "\n  checks: %s; placebo identical to the no-seat market on %u/%u seeds\n",
                  replays ? "every candidate session replayed identically" : "a candidate session was DIFFERENT on replay",
                  placebo_checks, placebo_seeds);
    s += b;
    for (const ArmSummary& a : arms) {
        if (a.check_failures == 0 || a.arm == "placebo") continue;
        std::string seeds;
        for (const SessionRow& r : run.sessions) {
            if (r.arm == a.arm && !r.check_ok) seeds += (seeds.empty() ? "" : ", ") + std::to_string(r.seed);
        }
        std::snprintf(b, sizeof b, "  %s: DIFFERENT on replay at seeds %s: state carried between sessions? not compared\n",
                      a.arm.c_str(), seeds.c_str());
        s += b;
    }
    for (const CandidateInfo& c : run.info.candidates) {
        if (c.origin != Origin::Installed) continue;
        std::snprintf(b, sizeof b,
                      "  %s is installed: replay is verified on this build; cross-platform replay is not claimed for it\n",
                      c.name.c_str());
        s += b;
    }
    if (blk.count < 16) {
        std::snprintf(b, sizeof b,
                      "  %s block: %u seeds show that a strategy runs; they cannot separate anything. Report the standard block.\n",
                      blk.name, blk.count);
        s += b;
    }
    return s;
}

} // namespace px::arena::suite
