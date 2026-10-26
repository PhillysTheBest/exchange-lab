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
    if (df <= 30) return 0;
    // Past the table, the Cornish-Fisher expansion about z = 1.959964: within
    // 0.0005 of the exact value for every df above 30 (it gives 1.9984 at 63
    // and 1.9788 at 127, the table's entries). Needed when a ratio metric is
    // undefined on some seeds, which leaves a df no block has.
    const double z = 1.959964, n = static_cast<double>(df);
    const double z3 = z * z * z, z5 = z3 * z * z;
    return z + (z3 + z) / (4 * n) + (5 * z5 + 16 * z3 + 3 * z) / (96 * n * n);
}

// ---- the multiple-comparison correction ------------------------------------
//
// Holm's step-down correction needs a p-value per test. It is computed here
// from + - * / and sqrt only, which IEEE 754 rounds exactly on every platform,
// so the same digits come out everywhere: no std::atan, std::exp or std::log,
// which a library is free to round differently.

// atan for x >= 0: reduced to |u| <= tan(pi/8) by atan(x) = pi/2 - atan(1/x)
// and atan(x) = pi/4 + atan((x - 1) / (x + 1)), then a Taylor series that has
// converged to double precision by about 22 terms.
[[nodiscard]] inline double atan_portable(double x) noexcept {
    constexpr double kPi2 = 1.57079632679489661923, kPi4 = 0.78539816339744830962;
    constexpr double kTanPi8 = 0.41421356237309504880;
    if (x < 0) return -atan_portable(-x);
    double base = 0, sign = 1;
    if (x > 1) {
        base = kPi2;
        sign = -1;
        x = 1 / x;
    }
    if (x > kTanPi8) {
        base += sign * kPi4;
        x = (x - 1) / (x + 1);
    }
    const double x2 = x * x;
    double term = x, sum = 0;
    for (int k = 0; k < 40; ++k) {
        const double add = term / static_cast<double>(2 * k + 1);
        sum += (k % 2 == 0) ? add : -add;
        term *= x2;
        if ((term < 0 ? -term : term) < 1e-18) break;
    }
    return base + sign * sum;
}

// P(|T| >= |t|) for Student's t with an integer df, by the finite sums of
// Abramowitz and Stegun 26.7.3 (odd df) and 26.7.4 (even df). Exact up to
// rounding; it is not the t975 table, so at a boundary the two can disagree in
// the fourth digit, which the report says.
[[nodiscard]] inline double t_two_sided_p(double t, std::size_t df) noexcept {
    constexpr double kTwoOverPi = 0.63661977236758134308;
    if (df == 0) return 1;
    if (t < 0) t = -t;
    const double nu = static_cast<double>(df);
    const double r = std::sqrt(nu + t * t);
    const double s = t / r, c2 = nu / (r * r);
    double a = 0;
    if (df % 2 == 0) {
        double term = 1, sum = 1;
        for (std::size_t k = 1; k < df / 2; ++k) {
            term *= c2 * static_cast<double>(2 * k - 1) / static_cast<double>(2 * k);
            sum += term;
        }
        a = s * sum;
    } else {
        const double theta = atan_portable(t / std::sqrt(nu));
        double inner = 0;
        if (df > 1) {
            double term = 1, sum = 1;
            for (std::size_t k = 1; k <= (df - 3) / 2; ++k) {
                term *= c2 * static_cast<double>(2 * k) / static_cast<double>(2 * k + 1);
                sum += term;
            }
            inner = s * std::sqrt(c2) * sum;
        }
        a = kTwoOverPi * (theta + inner);
    }
    const double p = 1 - a;
    return p < 0 ? 0 : (p > 1 ? 1 : p);
}

// Holm's step-down adjustment, in the input's order: sort ascending, multiply
// the i-th smallest by (m - i), carry the running maximum, cap at 1. It
// controls the chance of any false separation among the m tests at the level
// used, for any dependence between them.
[[nodiscard]] inline std::vector<double> holm_adjusted(const std::vector<double>& p) {
    const std::size_t m = p.size();
    std::vector<std::size_t> order(m);
    for (std::size_t i = 0; i < m; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&p](std::size_t a, std::size_t b) { return p[a] < p[b]; });
    std::vector<double> out(m, 1.0);
    double running = 0;
    for (std::size_t i = 0; i < m; ++i) {
        double v = static_cast<double>(m - i) * p[order[i]];
        if (v > 1) v = 1;
        if (v > running) running = v;
        out[order[i]] = running;
    }
    return out;
}

[[nodiscard]] inline std::string p_text(double p) {
    if (p < 0.0001) return "<0.0001";
    char b[16];
    std::snprintf(b, sizeof b, "%.4f", p);
    return b;
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
// inputs are in. Integer inputs (the raw records) are exact in a double up to
// 2^53, so "identical" is an exact test for them.
[[nodiscard]] inline Paired paired_values(const std::vector<double>& base, const std::vector<double>& arm) {
    Paired p;
    const std::size_t n = std::min(base.size(), arm.size());
    p.n = n;
    if (n == 0) return p;
    std::vector<double> a(n), b(n), d(n);
    bool all_zero = true;
    for (std::size_t i = 0; i < n; ++i) {
        b[i] = base[i];
        a[i] = arm[i];
        d[i] = arm[i] - base[i];
        if (d[i] != 0) all_zero = false;
        if (d[i] > 0) ++p.ahead;
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

[[nodiscard]] inline Paired paired(const std::vector<std::int64_t>& base, const std::vector<std::int64_t>& arm) {
    return paired_values(std::vector<double>(base.begin(), base.end()), std::vector<double>(arm.begin(), arm.end()));
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
        // Nothing is compared on a market that is not one, candidate against
        // candidate included: they share the placebo's market.
        r.admissible = admissible(base) && admissible(arm) && admissible("placebo");
        if (!r.admissible) r.verdict = "not_admissible";
        else if (r.stats.identical) r.verdict = "identical";
        else if (r.stats.separated()) r.verdict = "separated";
        else r.verdict = "not_separated";
        out.push_back(std::move(r));
    }
    return out;
}

// The two-sided p-value of a paired difference, against t with n - 1 degrees
// of freedom. A constant non-zero difference has no spread at all: p is 0.
[[nodiscard]] inline double paired_p(const Paired& p) noexcept {
    if (p.n < 2) return 1;
    if (p.se == 0) return p.mean == 0 ? 1 : 0;
    return t_two_sided_p(p.mean / p.se, p.n - 1);
}

// The value comparisons that are tests at all (admissible, not identical),
// with their raw and Holm-adjusted p-values. These are the run's hypotheses;
// the metric-by-metric rows describe how the money was made and are counted,
// not corrected (docs/REPORT.md, "Many comparisons").
struct HolmRow {
    std::string label;
    double p = 1, p_holm = 1;
    bool raw_separated = false;
    [[nodiscard]] bool separated() const noexcept { return p_holm < 0.05; }
};

[[nodiscard]] inline std::vector<HolmRow> holm_rows(const std::vector<std::pair<std::string, const ComparisonRow*>>& tests) {
    std::vector<HolmRow> out;
    std::vector<double> ps;
    for (const auto& [label, r] : tests) {
        if (r->verdict != "separated" && r->verdict != "not_separated") continue;
        HolmRow h;
        h.label = label;
        h.p = paired_p(r->stats);
        h.raw_separated = r->verdict == "separated";
        ps.push_back(h.p);
        out.push_back(std::move(h));
    }
    const std::vector<double> adj = holm_adjusted(ps);
    for (std::size_t i = 0; i < out.size(); ++i) out[i].p_holm = adj[i];
    return out;
}

[[nodiscard]] inline std::string holm_text(const std::vector<HolmRow>& h, const char* across) {
    std::string s;
    char b[256];
    std::snprintf(b, sizeof b, "\n  Holm across %s: %zu tests, family-wise 5%%\n", across, h.size());
    s += b;
    std::snprintf(b, sizeof b, "  %-34s | %-8s | %-8s | %-13s | %s\n", "test", "p", "p Holm", "raw verdict",
                  "after Holm");
    s += b;
    s += "  " + std::string(88, '-') + "\n";
    for (const HolmRow& r : h) {
        std::snprintf(b, sizeof b, "  %-34s | %-8s | %-8s | %-13s | %s\n", r.label.c_str(), p_text(r.p).c_str(),
                      p_text(r.p_holm).c_str(), r.raw_separated ? "separated" : "not separated",
                      r.separated() ? "separated" : "not separated");
        s += b;
    }
    s += "  p is two-sided, from t with n - 1 degrees of freedom; derived, like every number here. At a\n"
         "  boundary it can disagree with the interval, whose t is a three-digit table, in the fourth digit.\n";
    return s;
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

// ---- the seat, metric by metric (step 3, docs/REPORT.md) -------------------------

// What the seat did, per seed, read off the raw records. Every metric is a
// value of the seat in one arm; against the placebo (the empty seat, zero on
// all of them) a metric is its own level, so pairs are candidate against
// candidate. A ratio is undefined on a seed with nothing to divide by, and that
// seed is left out of that metric only; the seed count printed beside it says so.
struct MetricDef {
    const char* key;
    const char* label;
    const char* unit;
};

inline constexpr MetricDef kMetrics[] = {
    {"value", "value", "currency"},
    {"final_inventory", "final inventory", "qty"},
    {"mean_abs_inventory", "mean |inventory|", "qty"},
    {"max_abs_inventory", "max |inventory|", "qty"},
    {"max_drawdown", "max drawdown", "currency"},
    {"fills", "fills", "count"},
    {"turnover", "turnover", "qty"},
    {"fills_per_order", "fills per order", "ratio"},
    {"maker_share", "maker share", "% of fills"},
    {"spread_earned", "spread earned", "currency"},
    {"adverse", "adverse markout", "currency"},
    {"carry", "carry to close", "currency"},
};
inline constexpr std::size_t kMetricCount = sizeof kMetrics / sizeof kMetrics[0];

struct SeatSeed {
    std::uint64_t seed = 0;
    double v[kMetricCount] = {};
    bool defined[kMetricCount] = {};
    std::uint64_t markout_n = 0, unresolved = 0;
};

[[nodiscard]] inline const ParticipantRow* find_part(const RunRecord& run, const std::string& arm, std::uint64_t seed,
                                                     std::uint32_t index) {
    for (const ParticipantRow& p : run.parts) {
        if (p.seed == seed && p.index == index && p.arm == arm) return &p;
    }
    return nullptr;
}

// The seat's metrics for one arm, one entry per session row, in block order.
[[nodiscard]] inline std::vector<SeatSeed> seat_series(const RunRecord& run, const std::string& arm) {
    std::vector<SeatSeed> out;
    for (const SessionRow& r : run.sessions) {
        if (r.arm != arm) continue;
        SeatSeed x;
        x.seed = r.seed;
        const ParticipantRow* p = find_part(run, arm, r.seed, r.seat);
        const Account a = p != nullptr && p->seat ? p->account : Account{};
        auto set = [&x](std::size_t i, double v) {
            x.v[i] = v;
            x.defined[i] = true;
        };
        set(0, to_currency(static_cast<double>(r.seat_value)));
        set(1, static_cast<double>(a.position));
        set(2, r.seat_path_samples == 0 ? 0.0
                                        : static_cast<double>(r.seat_abs_position_sum) /
                                              static_cast<double>(r.seat_path_samples));
        set(3, static_cast<double>(r.seat_max_abs_position));
        set(4, to_currency(static_cast<double>(r.seat_max_drawdown)));
        set(5, static_cast<double>(a.fills));
        set(6, static_cast<double>(a.filled_qty));
        // Fills, not orders filled: one order can fill against several resting ones.
        if (a.orders_sent != 0) set(7, static_cast<double>(a.fills) / static_cast<double>(a.orders_sent));
        if (a.fills != 0) set(8, 100.0 * static_cast<double>(a.maker_fills) / static_cast<double>(a.fills));
        set(9, to_currency(static_cast<double>(a.spread_sum)));
        set(10, to_currency(static_cast<double>(a.adverse_sum)));
        // What the inventory earned between each fill's markout and the close:
        // value = spread + adverse + carry, exactly (any fees land in carry;
        // the suite charges none).
        set(11, to_currency(static_cast<double>(r.seat_value - a.spread_sum - a.adverse_sum)));
        x.markout_n = a.markout_n;
        x.unresolved = a.unresolved_fills;
        out.push_back(x);
    }
    return out;
}

// One metric's values, and the seeds they came from, where defined.
[[nodiscard]] inline std::vector<double> metric_values(const std::vector<SeatSeed>& s, std::size_t m) {
    std::vector<double> v;
    for (const SeatSeed& x : s) {
        if (x.defined[m]) v.push_back(x.v[m]);
    }
    return v;
}

struct MetricRow {
    std::string baseline; // empty for a level
    std::string arm;
    std::size_t metric = 0;
    Paired stats; // a level is paired against zero
    bool admissible = true;
    std::string verdict; // paired rows only
};

// Every candidate's level on every metric, then every later candidate against
// the first, on every metric, over the seeds where both are defined.
[[nodiscard]] inline std::vector<MetricRow> evaluate(const RunRecord& run) {
    const std::vector<ArmSummary> arms = summarize_arms(run);
    auto admissible = [&arms](const std::string& name) {
        for (const ArmSummary& a : arms) {
            if (a.arm == name) return a.admissible();
        }
        return false;
    };
    std::vector<MetricRow> out;
    std::vector<std::vector<SeatSeed>> series;
    for (const CandidateInfo& c : run.info.candidates) series.push_back(seat_series(run, c.name));
    for (std::size_t c = 0; c < series.size(); ++c) {
        for (std::size_t m = 0; m < kMetricCount; ++m) {
            MetricRow r;
            r.arm = run.info.candidates[c].name;
            r.metric = m;
            const std::vector<double> v = metric_values(series[c], m);
            r.stats = paired_values(std::vector<double>(v.size(), 0.0), v);
            r.admissible = admissible(r.arm);
            out.push_back(std::move(r));
        }
    }
    for (std::size_t c = 1; c < series.size(); ++c) {
        for (std::size_t m = 0; m < kMetricCount; ++m) {
            std::vector<double> b, a;
            for (const SeatSeed& x : series[0]) {
                for (const SeatSeed& y : series[c]) {
                    if (y.seed == x.seed && x.defined[m] && y.defined[m]) {
                        b.push_back(x.v[m]);
                        a.push_back(y.v[m]);
                    }
                }
            }
            MetricRow r;
            r.baseline = run.info.candidates[0].name;
            r.arm = run.info.candidates[c].name;
            r.metric = m;
            r.stats = paired_values(b, a);
            r.admissible = admissible(r.baseline) && admissible(r.arm) && admissible("placebo");
            if (!r.admissible) r.verdict = "not_admissible";
            else if (r.stats.n == 0) r.verdict = "undefined";
            else if (r.stats.identical) r.verdict = "identical";
            else if (r.stats.separated()) r.verdict = "separated";
            else r.verdict = "not_separated";
            out.push_back(std::move(r));
        }
    }
    return out;
}

[[nodiscard]] inline std::string evaluation_csv(const std::vector<MetricRow>& rows) {
    std::string s = "kind,baseline,arm,metric,unit,seeds,mean,se,t_crit,ci95_lo,ci95_hi,min,max,verdict\n";
    for (const MetricRow& r : rows) {
        const Paired& p = r.stats;
        const MetricDef& m = kMetrics[r.metric];
        s += r.baseline.empty() ? "level," : "paired,";
        s += r.baseline + ',' + r.arm + ',' + m.key + ',' + m.unit + ',';
        append_u(s, p.n);
        s += ',' + fmt("%.4f", p.mean) + ',' + fmt("%.4f", p.se) + ',' + fmt("%.3f", p.t) + ',' +
             fmt("%.4f", p.lo) + ',' + fmt("%.4f", p.hi) + ',' + fmt("%.4f", p.min) + ',' + fmt("%.4f", p.max) + ',';
        s += r.verdict + '\n';
    }
    return s;
}

// ---- where the money came from ---------------------------------------------------

// For each candidate and each reference participant: its value in the
// candidate's arm minus its value in the placebo arm, seed by seed. Every arm
// closes at the same fundamental and the market is closed, so on every seed
// these sum to minus the seat's value exactly; `identity_ok` records that.
struct AttributionRow {
    std::string arm;
    std::uint32_t index = 0;
    std::string name;
    Paired stats; // currency units: arm - placebo
};

[[nodiscard]] inline std::vector<AttributionRow> attribute(const RunRecord& run, bool& identity_ok) {
    identity_ok = true;
    std::vector<AttributionRow> out;
    for (const CandidateInfo& c : run.info.candidates) {
        std::vector<std::vector<double>> base, arm; // [reference index][seed]
        std::vector<std::string> names;
        for (const SessionRow& r : run.sessions) {
            if (r.arm != c.name) continue;
            std::int64_t sum = r.seat_value;
            for (std::uint32_t i = 0; i < r.seat; ++i) {
                const ParticipantRow* a = find_part(run, c.name, r.seed, i);
                const ParticipantRow* b = find_part(run, "placebo", r.seed, i);
                if (a == nullptr || b == nullptr) {
                    identity_ok = false;
                    continue;
                }
                if (base.size() <= i) {
                    base.resize(i + 1);
                    arm.resize(i + 1);
                    names.resize(i + 1);
                }
                names[i] = a->name;
                base[i].push_back(to_currency(static_cast<double>(b->value)));
                arm[i].push_back(to_currency(static_cast<double>(a->value)));
                sum += a->value - b->value;
            }
            if (sum != 0) identity_ok = false;
        }
        for (std::size_t i = 0; i < base.size(); ++i) {
            AttributionRow r;
            r.arm = c.name;
            r.index = static_cast<std::uint32_t>(i);
            r.name = names[i];
            r.stats = paired_values(base[i], arm[i]);
            out.push_back(std::move(r));
        }
    }
    return out;
}

[[nodiscard]] inline std::string attribution_csv(const std::vector<AttributionRow>& rows) {
    std::string s = "arm,index,name,seeds,mean_diff,se_diff,t_crit,ci95_lo,ci95_hi,min_diff,max_diff\n";
    for (const AttributionRow& r : rows) {
        const Paired& p = r.stats;
        s += r.arm + ',';
        append_u(s, r.index);
        s += ',' + r.name + ',';
        append_u(s, p.n);
        s += ',' + fmt("%.4f", p.mean) + ',' + fmt("%.4f", p.se) + ',' + fmt("%.3f", p.t) + ',' + fmt("%.4f", p.lo) +
             ',' + fmt("%.4f", p.hi) + ',' + fmt("%.4f", p.min) + ',' + fmt("%.4f", p.max) + '\n';
    }
    return s;
}

// ---- scenario families: one table across suites -------------------------------

struct FamilyResult {
    std::string suite;
    std::string change;
    bool reference_admissible = true;
    std::vector<ComparisonRow> rows;
};

// Each candidate's paired value against the placebo, one column per suite.
// Never pooled: a strategy that wins in one market and loses in another has
// two results, not an average.
[[nodiscard]] inline std::string families_text(const std::vector<std::string>& candidates,
                                               const std::vector<FamilyResult>& fams) {
    std::string s;
    char b[256];
    s += "\nscenario families: each candidate's value against the placebo, currency per session, mean +- s.e.\n";
    s += "  Each column is its own market, placebo and health gate. The columns are not pooled.\n";
    std::snprintf(b, sizeof b, "  %-14s", "candidate");
    s += b;
    for (const FamilyResult& f : fams) {
        std::snprintf(b, sizeof b, " | %-22s", f.suite.c_str());
        s += b;
    }
    s += "\n  " + std::string(14 + 25 * fams.size(), '-') + "\n";
    for (const std::string& c : candidates) {
        std::snprintf(b, sizeof b, "  %-14s", c.c_str());
        s += b;
        for (const FamilyResult& f : fams) {
            std::string cell = "-";
            for (const ComparisonRow& r : f.rows) {
                if (r.baseline != "placebo" || r.arm != c) continue;
                const char* tag = r.verdict == "separated"    ? "sep"
                                  : r.verdict == "identical"  ? "same"
                                  : r.verdict == "not_separated" ? "n.s."
                                                                 : "N/A";
                cell = fmt("%+8.2f", to_currency(r.stats.mean)) + " +- " + fmt("%5.2f", to_currency(r.stats.se)) + " " +
                       tag;
            }
            std::snprintf(b, sizeof b, " | %-22s", cell.c_str());
            s += b;
        }
        s += "\n";
    }
    for (const FamilyResult& f : fams) {
        std::snprintf(b, sizeof b, "  %-8s %s%s\n", f.suite.c_str(), f.change.empty() ? "the default market" : f.change.c_str(),
                      f.reference_admissible ? "" : "; REFERENCE NOT ADMISSIBLE, nothing in this column counts");
        s += b;
    }
    s += "  sep: the 95% interval excludes 0; n.s.: it does not; same: identical on every seed;\n"
         "  N/A: not admissible. Full tables are in each suite's report.txt.\n";
    // One family of tests across every cell: a strategy tried in three markets
    // has three chances to separate. Cells stay separate; only the threshold moves.
    std::vector<std::pair<std::string, const ComparisonRow*>> cells;
    for (const std::string& c : candidates) {
        for (const FamilyResult& f : fams) {
            if (!f.reference_admissible) continue;
            for (const ComparisonRow& r : f.rows) {
                if (r.baseline == "placebo" && r.arm == c) cells.emplace_back(c + " vs placebo, " + f.suite, &r);
            }
        }
    }
    const std::vector<HolmRow> holm = holm_rows(cells);
    if (holm.size() > 1) s += holm_text(holm, "every cell of the table");
    return s;
}

// ---- sensitivity: does the standard result survive a change? -----------------
//
// Every comparison of the run, in every condition of the protocol, beside the
// standard one. A result holds if, in every admissible condition, it separates
// with the standard result's sign. Each condition is its own test at 5%: for
// the claim "it is there in every condition" (an intersection-union test) no
// correction is needed, because the claim fails if any one test fails. The
// opposite claim, "it is there in some condition", would need one, and is not
// made here. Size is reported as each condition's mean as a share of the
// standard mean, as a range, never averaged.
[[nodiscard]] inline std::string sensitivity_text(const std::vector<FamilyResult>& conds) {
    std::string s;
    char b[256];
    s += "\nsensitivity: does the standard result survive a change of conditions? currency per session, mean +- s.e.\n";
    s += "  Each condition is its own market, placebo and health gate, and they are never pooled.\n";
    if (conds.empty() || conds.front().suite != "standard") return s;
    const FamilyResult& base = conds.front();
    for (const ComparisonRow& r0 : base.rows) {
        const std::string label = r0.arm + " vs " + r0.baseline;
        std::snprintf(b, sizeof b, "\n  %s\n", label.c_str());
        s += b;
        std::snprintf(b, sizeof b, "    %-12s | %-46s | %-19s | %-14s | %s\n", "condition", "change", "mean +- s.e.",
                      "verdict", "size");
        s += b;
        s += "    " + std::string(118, '-') + "\n";
        const bool base_sep = r0.verdict == "separated";
        std::size_t held = 0, tested = 0;
        std::string broke;
        double lo = 0, hi = 0;
        bool have_size = false;
        for (const FamilyResult& f : conds) {
            const ComparisonRow* r = nullptr;
            for (const ComparisonRow& x : f.rows)
                if (x.baseline == r0.baseline && x.arm == r0.arm) r = &x;
            std::string cell = "-", verdict = "-", size = "-";
            if (r != nullptr) {
                cell = fmt("%+8.2f", to_currency(r->stats.mean)) + " +- " + fmt("%6.2f", to_currency(r->stats.se));
                verdict = !f.reference_admissible || r->verdict == "not_admissible" ? "NOT ADMISSIBLE"
                          : r->verdict == "not_separated"                        ? "not separated"
                                                                                  : r->verdict;
                if (r0.stats.mean != 0) {
                    const double share = 100.0 * r->stats.mean / r0.stats.mean;
                    size = fmt("%.0f%%", share);
                    if (&f != &base && f.reference_admissible && r->verdict != "not_admissible") {
                        lo = have_size ? std::min(lo, share) : share;
                        hi = have_size ? std::max(hi, share) : share;
                        have_size = true;
                    }
                }
                if (&f != &base && f.reference_admissible && r->verdict != "not_admissible") {
                    ++tested;
                    const bool same_sign = (r->stats.mean > 0) == (r0.stats.mean > 0);
                    if (base_sep && r->verdict == "separated" && same_sign) ++held;
                    else if (base_sep) broke += (broke.empty() ? "" : ", ") + f.suite + (r->verdict == "separated" ? " (flips)" : " (vanishes)");
                }
            }
            std::snprintf(b, sizeof b, "    %-12s | %-46s | %-19s | %-14s | %s\n", f.suite.c_str(),
                          f.change.empty() ? "none" : f.change.c_str(), cell.c_str(), verdict.c_str(), size.c_str());
            s += b;
        }
        if (!base_sep) {
            s += "    standard does not separate, so there is no result to hold.\n";
        } else {
            std::snprintf(b, sizeof b, "    sign: %s in %zu of %zu conditions%s%s\n", held == tested ? "HOLDS" : "holds",
                          held, tested, broke.empty() ? "" : "; not in ", broke.c_str());
            s += b;
            if (have_size) {
                std::snprintf(b, sizeof b, "    size: %.0f%% to %.0f%% of standard's\n", lo, hi);
                s += b;
            }
        }
    }
    s += "\n  A result holds if it separates with standard's sign in every admissible condition. Each condition\n"
         "  is tested at 5% on its own, which is right for a claim that an effect is there in every one; a\n"
         "  claim that it is there in some condition would need a correction, and is not made.\n";
    for (const FamilyResult& f : conds) {
        if (f.reference_admissible) continue;
        std::snprintf(b, sizeof b, "  %s: REFERENCE NOT ADMISSIBLE, nothing in that row counts\n", f.suite.c_str());
        s += b;
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
    if (d.change[0] == '\0') std::snprintf(b, sizeof b, "  market    %s, every parameter at its default\n", d.lineup);
    else std::snprintf(b, sizeof b, "  market    %s, every parameter at its default except: %s\n", d.lineup, d.change);
    s += b;
    if (!d.calibration.empty()) {
        // A calibrated market: say which tape, and how little that means.
        std::snprintf(b, sizeof b, "            calibrated from the tape %s (%s): like it in %s, and in nothing else\n",
                      cfg_value(d.calibration, "calibration.tape").c_str(),
                      cfg_value(d.calibration, "calibration.depth").c_str(),
                      cfg_value(d.calibration, "calibration.matched").c_str());
        s += b;
    }
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
                // "Sent no orders" only when the records say so on every seed: an
                // identical value alone does not prove it.
                bool inert = false;
                for (const ArmSummary& a : arms) {
                    if (a.arm == r.arm) inert = a.seeds > 0 && a.inert == a.seeds;
                }
                std::snprintf(b, sizeof b, "  %s vs %s: identical on every seed; %s\n", r.arm.c_str(),
                              r.baseline.c_str(),
                              r.baseline != "placebo" ? "the two arms are the same market"
                              : inert                 ? "it sent no orders, so its sessions are the placebo's"
                                                      : "its value equals the placebo's, though it sent orders");
                s += b;
            }
        }
    }
    // With more than one value test, the correction is printed beside them.
    // One test needs none, and its report is unchanged.
    std::vector<std::pair<std::string, const ComparisonRow*>> value_tests;
    for (const ComparisonRow& r : rows) value_tests.emplace_back(r.arm + " vs " + r.baseline, &r);
    const std::vector<HolmRow> holm = holm_rows(value_tests);
    if (holm.size() > 1) s += holm_text(holm, "the value comparisons above");

    // Step 3: how the seat made its money, metric by metric.
    const std::vector<MetricRow> metrics = evaluate(run);
    std::size_t tests = 0, separated = 0;
    for (const ComparisonRow& r : rows) {
        if (r.verdict == "separated" || r.verdict == "not_separated") ++tests;
        if (r.verdict == "separated") ++separated;
    }
    if (!run.info.candidates.empty()) {
        std::snprintf(b, sizeof b, "\nthe seat, metric by metric: mean +- s.e. over the seeds where it is defined (%u seeds)\n",
                      blk.count);
        s += b;
        std::snprintf(b, sizeof b, "  %-17s | %-12s", "metric", "unit");
        s += b;
        for (const CandidateInfo& c : run.info.candidates) {
            std::snprintf(b, sizeof b, " | %-20s", c.name.c_str());
            s += b;
        }
        s += "\n  " + std::string(32 + 23 * run.info.candidates.size(), '-') + "\n";
        for (std::size_t m = 0; m < kMetricCount; ++m) {
            std::snprintf(b, sizeof b, "  %-17s | %-12s", kMetrics[m].label, kMetrics[m].unit);
            s += b;
            for (const MetricRow& r : metrics) {
                if (!r.baseline.empty() || r.metric != m) continue;
                std::string cell = r.stats.n == 0 ? std::string("undefined")
                                                  : fmt("%+10.2f", r.stats.mean) + " +- " + fmt("%6.2f", r.stats.se);
                if (r.stats.n != 0 && r.stats.n != blk.count) cell += " (" + std::to_string(r.stats.n) + ")";
                std::snprintf(b, sizeof b, " | %-20s", cell.c_str());
                s += b;
            }
            s += "\n";
        }
        std::uint64_t markouts = 0, unresolved = 0;
        for (const CandidateInfo& c : run.info.candidates) {
            for (const SeatSeed& x : seat_series(run, c.name)) {
                markouts += x.markout_n;
                unresolved += x.unresolved;
            }
        }
        std::snprintf(b, sizeof b,
                      "  value = spread earned + adverse markout + carry to close, exactly, at zero fees. The adverse column\n"
                      "  reads near zero for everyone by construction (the fundamental is a martingale; docs/ARENA.md),\n"
                      "  so a stale quote's loss shows as spread. %.1f%% of the seat's fills fell inside the last markout\n"
                      "  horizon and were marked at the close instead.\n",
                      markouts == 0 ? 0.0 : 100.0 * static_cast<double>(unresolved) / static_cast<double>(markouts));
        s += b;
    }
    if (run.info.candidates.size() > 1) {
        std::snprintf(b, sizeof b, "\npaired against %s, metric by metric: arm - %s, seed by seed\n",
                      run.info.candidates[0].name.c_str(), run.info.candidates[0].name.c_str());
        s += b;
        std::snprintf(b, sizeof b, "  %-14s | %-17s | %-20s | %-24s | %-6s | %s\n", "arm", "metric", "mean +- s.e.",
                      "95% interval", "seeds", "verdict");
        s += b;
        s += "  " + std::string(108, '-') + "\n";
        for (const MetricRow& r : metrics) {
            if (r.baseline.empty()) continue;
            const Paired& p = r.stats;
            std::string verdict = r.verdict == "not_admissible" ? "NOT ADMISSIBLE"
                                  : r.verdict == "not_separated" ? "not separated"
                                                                 : r.verdict;
            std::snprintf(b, sizeof b, "  %-14s | %-17s | %+9.2f +- %7.2f | %+10.2f .. %+10.2f | %6zu | %s\n",
                          r.arm.c_str(), kMetrics[r.metric].label, p.mean, p.se, p.lo, p.hi, p.n, verdict.c_str());
            s += b;
            if (r.verdict == "separated" || r.verdict == "not_separated") ++tests;
            if (r.verdict == "separated") ++separated;
        }
    }

    bool identity_ok = true;
    const std::vector<AttributionRow> attribution = attribute(run, identity_ok);
    if (!attribution.empty()) {
        s += "\nwhere the seat's money came from: each reference participant's value in the candidate's arm\n"
             "minus its value in the placebo arm, currency per session, mean +- s.e.\n";
        std::snprintf(b, sizeof b, "  %-3s %-10s", "#", "reference");
        s += b;
        for (const CandidateInfo& c : run.info.candidates) {
            std::snprintf(b, sizeof b, " | %-18s", c.name.c_str());
            s += b;
        }
        s += "\n  " + std::string(14 + 21 * run.info.candidates.size(), '-') + "\n";
        for (std::uint32_t i = 0; i < seat; ++i) {
            std::string name;
            for (const AttributionRow& r : attribution) {
                if (r.index == i) name = r.name;
            }
            std::snprintf(b, sizeof b, "  %-3u %-10s", i, name.c_str());
            s += b;
            for (const CandidateInfo& c : run.info.candidates) {
                std::string cell = "-";
                for (const AttributionRow& r : attribution) {
                    if (r.arm == c.name && r.index == i) {
                        cell = fmt("%+8.2f", r.stats.mean) + " +- " + fmt("%5.2f", r.stats.se);
                    }
                }
                std::snprintf(b, sizeof b, " | %-18s", cell.c_str());
                s += b;
            }
            s += "\n";
        }
        s += identity_ok ? "  On every seed these sum to minus the seat's value exactly: the market is closed.\n"
                         : "  THE CLOSED IDENTITY FAILED: these do not sum to minus the seat's value on every seed.\n";
    }
    if (tests > 0 && holm.size() > 1) {
        std::size_t holm_separated = 0;
        for (const HolmRow& h : holm) holm_separated += h.separated() ? 1 : 0;
        std::snprintf(b, sizeof b,
                      "\n  %zu interval tests above, %zu separated. At 95%% each, about %.1f would separate by chance if\n"
                      "  nothing were there. The %zu value comparisons carry a Holm correction (%zu separate after it);\n"
                      "  the metric rows are description and are not corrected, so read a lone separation there with that\n"
                      "  in mind.\n",
                      tests, separated, 0.05 * static_cast<double>(tests), holm.size(), holm_separated);
        s += b;
    } else if (tests > 0) {
        std::snprintf(b, sizeof b,
                      "\n  %zu interval tests above, %zu separated. At 95%% each, about %.1f would separate by chance if\n"
                      "  nothing were there; no correction is applied, so read a lone separation with that in mind.\n",
                      tests, separated, 0.05 * static_cast<double>(tests));
        s += b;
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
    if (blk.confirmatory) {
        std::snprintf(b, sizeof b,
                      "  %s block: confirmatory. These seeds are not standard's; report this run once, after tuning on\n"
                      "            standard, beside the standard run and never pooled with it.\n",
                      blk.name);
        s += b;
    }
    return s;
}

} // namespace px::arena::suite
