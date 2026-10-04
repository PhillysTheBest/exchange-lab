// arena/sweep.cpp — the experiment: run the same market repeatedly, change one
// thing, and write down what happened.
//
// The headline question is "what is 10 microseconds worth?", but this is a sweep
// *runner* rather than one hard-coded sweep, because the honest answer needs
// more than one curve. Any field of the roster or the session can be the swept
// variable, several seeds run at every point, and both the per-session rows and
// the per-point summary go to CSV. That makes the three things a reader should
// demand cheap to produce:
//
//   the curve        --param=maker.reaction_ns --values=1000,10000,100000,1000000
//   the null run     --null, which switches off the only informed participant.
//                    With no information in the session there is nothing for
//                    speed to be worth, so the curve must go flat. If it does
//                    not, the effect is an artefact of the machinery.
//   the robustness   --param=informed.edge_ticks, maker.half_spread_ticks,
//                    noise.timer_ns, maker.impact_centiticks. A shape that only
//                    exists at one set of parameters is not a finding.
//
// WHAT THESE NUMBERS ARE. Simulated. Every nanosecond here is an input chosen by
// the experimenter, not a measurement of anything. The engine's measured latency
// is in README.md's tables and has nothing to do with this file: a session does
// not time itself and could not, having no clock but its own.
//
// PAIRED DIFFERENCES, because the levels are too noisy to read. A market maker's
// value is dominated by what its leftover inventory happened to be worth, which
// is a random walk, and across ten seeds the spread of *levels* swamps the effect
// being measured. But every point of a sweep is run on the same seeds, and the
// seed fixes the path of the fundamental value, so the difference between two
// points *at the same seed* cancels most of that noise. The table reports the
// level at each point and, beside it, the paired difference from the first point,
// which is the statistic the question is actually about. Common random numbers
// like this are ordinary practice in simulation experiments, and they cost
// nothing here because a session is reproducible by construction.
//
// Reported per participant:
//   value    cash + position * V - fees, marked at the fundamental, because the
//            only trustworthy mark in a closed session is the one no participant
//            can move. In currency units.
//   adverse  mean signed price move against the filler per fill over the markout
//            horizon, with the spread taken out. The definition and the reason
//            for splitting it from the spread are in arena/accounting.hpp. Read
//            it before value: it does not depend on how many fills a participant
//            happened to get, and value does.
//   spread   what each fill earned for providing liquidity. edge = spread +
//            adverse exactly; all three go to the CSV so the arithmetic can be
//            checked rather than taken on trust.
//   gap      mean |mid - fundamental| in ticks: the session's own sanity check,
//            printed above the result, because a book that has come loose from
//            the fundamental is not a market and nothing read off it counts.
#include "arena/session.hpp"
#include "px/types.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace px;
using namespace px::arena;

namespace {

struct Options {
    std::string param = "maker.reaction_ns";
    std::vector<std::int64_t> values{1'000, 10'000, 100'000, 1'000'000};
    int seeds = 20;
    std::size_t requests = 200'000;
    std::uint64_t base_seed = 42;
    std::string csv;
    std::string summary_csv;
    bool replay_check = false;
    bool quiet = false;
    std::string label = "sweep";
    std::vector<std::pair<std::string, std::int64_t>> overrides;
};

void usage() {
    std::printf(
        "arena sweep — run one market many times, changing one thing.\n\n"
        "  --param=NAME          the swept field (default maker.reaction_ns)\n"
        "  --values=A,B,C        the points to run (default 1000,10000,100000,1000000)\n"
        "  --seeds=N             sessions per point, all points sharing them (default 20)\n"
        "  --requests=N          engine requests per session (default 200000)\n"
        "  --base-seed=N         first seed (default 42)\n"
        "  --set=NAME=VALUE      fix any other field; may be repeated\n"
        "  --null                shorthand for --set=informed.enabled=0\n"
        "  --csv=PATH            one row per participant per session\n"
        "  --summary-csv=PATH    one row per swept point\n"
        "  --label=NAME          a name for the run, written into both CSVs\n"
        "  --replay-check        run every session twice and compare hashes\n"
        "  --quiet               tables only\n\n"
        "fields:\n  %s\n  %s\n\n"
        "Every number this prints is simulated. The engine's measured latency is in\n"
        "README.md and is unrelated to any reaction delay set here.\n",
        kRosterParamNames, Session::kSessionParamNames);
}

[[nodiscard]] bool parse_i64(std::string_view s, std::int64_t& out) {
    if (s.empty()) return false;
    std::int64_t v = 0;
    std::size_t i = s[0] == '-' ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) {
        if (s[i] == '_') continue;
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    out = s[0] == '-' ? -v : v;
    return true;
}

[[nodiscard]] bool apply_field(RosterConfig& rc, SessionConfig& sc, std::string_view key, std::int64_t value) {
    if (set_roster_param(rc, key, value)) return true;
    return Session::set_param(sc, key, value);
}

// Median and full range of a set of samples, computed once the sessions are over.
// This is the only floating point anywhere near the arena, and it is reporting
// rather than simulation.
struct Spread {
    double median = 0, lo = 0, hi = 0;
};

[[nodiscard]] Spread spread_of(std::vector<double> v) {
    Spread s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.lo = v.front();
    s.hi = v.back();
    s.median = v.size() % 2 == 1 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2.0;
    return s;
}

[[nodiscard]] double to_currency(std::int64_t price_units) {
    return static_cast<double>(price_units) / static_cast<double>(kPriceScale);
}

// One session's results: a single cell of the (point x seed) grid.
struct Cell {
    double maker_value = 0, maker_adverse = 0, maker_spread = 0, maker_adv_pct = 0;
    double maker_fills = 0, maker_wakes = 0;
    double rival_value = 0, informed_value = 0;
    double trades = 0, gap = 0, gap_max = 0, unpriced_pct = 0;
    std::uint64_t hash = 0;
    bool replayed = true;
};

[[nodiscard]] bool run_one(const Options& opt, std::int64_t swept_value, std::uint64_t seed, std::FILE* csv,
                           Cell& cell) {
    RosterConfig rc;
    SessionConfig sc;
    sc.max_requests = opt.requests;
    sc.seed = seed;
    for (const auto& o : opt.overrides) {
        if (!apply_field(rc, sc, o.first, o.second)) return false;
    }
    if (!apply_field(rc, sc, opt.param, swept_value)) return false;

    Session s;
    s.start(sc, rc);
    s.run();

    if (opt.replay_check) {
        Session again;
        again.start(sc, rc);
        again.run();
        cell.replayed = again.hash() == s.hash();
    }
    cell.hash = s.hash();

    const Price mark = s.fair_value().value();
    const int mi = s.maker_index();
    const int ri = s.rival_index();
    if (mi < 0) return false;

    for (std::size_t i = 0; i < s.participants(); ++i) {
        const Account& a = s.ledger().at(i);
        const double adverse_pct =
            a.markout_n == 0 ? 0.0 : 100.0 * static_cast<double>(a.adverse_fills) / static_cast<double>(a.markout_n);
        if (csv != nullptr) {
            std::fprintf(csv,
                         "%s,%s,%lld,%llu,%zu,%s,%lld,%d,%lld,%.4f,%llu,%llu,%llu,%lld,%lld,%lld,%llu,%.2f,%llu,"
                         "%llu,%llu,%llu,%.3f,%016llx\n",
                         opt.label.c_str(), opt.param.c_str(), static_cast<long long>(swept_value),
                         static_cast<unsigned long long>(seed), i, s.name_of(i),
                         static_cast<long long>(s.reaction_of(i)), s.informed(i) ? 1 : 0,
                         static_cast<long long>(a.value(mark)), to_currency(a.value(mark)),
                         static_cast<unsigned long long>(a.fills), static_cast<unsigned long long>(a.maker_fills),
                         static_cast<unsigned long long>(a.taker_fills), static_cast<long long>(a.adverse_milli()),
                         static_cast<long long>(a.spread_milli()), static_cast<long long>(a.edge_milli()),
                         static_cast<unsigned long long>(a.markout_n), adverse_pct,
                         static_cast<unsigned long long>(a.orders_sent), static_cast<unsigned long long>(a.rejects),
                         static_cast<unsigned long long>(s.wakes_of(i)),
                         static_cast<unsigned long long>(s.stats().trades), s.stats().mean_gap_ticks(),
                         static_cast<unsigned long long>(s.hash()));
        }
        if (static_cast<int>(i) == mi) {
            cell.maker_value = to_currency(a.value(mark));
            cell.maker_adverse = static_cast<double>(a.adverse_milli()) / 1000.0;
            cell.maker_spread = static_cast<double>(a.spread_milli()) / 1000.0;
            cell.maker_adv_pct = adverse_pct;
            cell.maker_fills = static_cast<double>(a.fills);
            cell.maker_wakes = static_cast<double>(s.wakes_of(i));
        } else if (static_cast<int>(i) == ri) {
            cell.rival_value = to_currency(a.value(mark));
        } else if (s.informed(i)) {
            cell.informed_value = to_currency(a.value(mark));
        }
    }
    const SessionStats& st = s.stats();
    cell.trades = static_cast<double>(st.trades);
    cell.gap = st.mean_gap_ticks();
    cell.gap_max = static_cast<double>(st.gap_max_ticks);
    const std::uint64_t gap_total = st.gap_samples + st.gap_unpriced;
    cell.unpriced_pct =
        gap_total == 0 ? 0.0 : 100.0 * static_cast<double>(st.gap_unpriced) / static_cast<double>(gap_total);
    return true;
}

// Everything the table and the CSV need about one swept point.
struct Point {
    std::int64_t value = 0;
    Spread maker_value, maker_adverse, maker_spread, maker_adv_pct;
    Spread rival_value, informed_value, trades, gap;
    Spread d_value, d_adverse; // paired differences from the first point
    double maker_fills = 0, maker_wakes = 0, gap_max = 0, unpriced_pct = 0;
    std::uint64_t first_hash = 0;
    bool replays = true;
};

template <class Get>
[[nodiscard]] Spread column(const std::vector<Cell>& row, Get get) {
    std::vector<double> v;
    v.reserve(row.size());
    for (const Cell& c : row) v.push_back(get(c));
    return spread_of(v);
}

// The same statistic taken seed by seed against the baseline point, which is
// what cancels the shared randomness.
template <class Get>
[[nodiscard]] Spread paired(const std::vector<Cell>& row, const std::vector<Cell>& base, Get get) {
    std::vector<double> v;
    v.reserve(row.size());
    for (std::size_t i = 0; i < row.size() && i < base.size(); ++i) v.push_back(get(row[i]) - get(base[i]));
    return spread_of(v);
}

template <class Get>
[[nodiscard]] double mean_of(const std::vector<Cell>& row, Get get) {
    if (row.empty()) return 0;
    double s = 0;
    for (const Cell& c : row) s += get(c);
    return s / static_cast<double>(row.size());
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        std::int64_t n = 0;
        if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else if (a == "--null") {
            opt.overrides.emplace_back("informed.enabled", 0);
            if (opt.label == "sweep") opt.label = "null";
        } else if (a == "--replay-check") {
            opt.replay_check = true;
        } else if (a == "--quiet") {
            opt.quiet = true;
        } else if (a.starts_with("--param=")) {
            opt.param = std::string(a.substr(8));
        } else if (a.starts_with("--label=")) {
            opt.label = std::string(a.substr(8));
        } else if (a.starts_with("--csv=")) {
            opt.csv = std::string(a.substr(6));
        } else if (a.starts_with("--summary-csv=")) {
            opt.summary_csv = std::string(a.substr(14));
        } else if (a.starts_with("--seeds=") && parse_i64(a.substr(8), n) && n > 0) {
            opt.seeds = static_cast<int>(n);
        } else if (a.starts_with("--requests=") && parse_i64(a.substr(11), n) && n > 0) {
            opt.requests = static_cast<std::size_t>(n);
        } else if (a.starts_with("--base-seed=") && parse_i64(a.substr(12), n)) {
            opt.base_seed = static_cast<std::uint64_t>(n);
        } else if (a.starts_with("--values=")) {
            opt.values.clear();
            std::string_view rest = a.substr(9);
            while (!rest.empty()) {
                const std::size_t comma = rest.find(',');
                const std::string_view tok = rest.substr(0, comma);
                if (!parse_i64(tok, n)) {
                    std::fprintf(stderr, "bad value in --values: %.*s\n", static_cast<int>(tok.size()), tok.data());
                    return 2;
                }
                opt.values.push_back(n);
                if (comma == std::string_view::npos) break;
                rest = rest.substr(comma + 1);
            }
        } else if (a.starts_with("--set=")) {
            const std::string_view kv = a.substr(6);
            const std::size_t eq = kv.find('=');
            if (eq == std::string_view::npos || !parse_i64(kv.substr(eq + 1), n)) {
                std::fprintf(stderr, "expected --set=NAME=VALUE, got %.*s\n", static_cast<int>(kv.size()), kv.data());
                return 2;
            }
            opt.overrides.emplace_back(std::string(kv.substr(0, eq)), n);
        } else {
            std::fprintf(stderr, "unknown argument: %.*s (try --help)\n", static_cast<int>(a.size()), a.data());
            return 2;
        }
    }
    if (opt.values.empty()) {
        std::fprintf(stderr, "nothing to sweep\n");
        return 2;
    }

    std::FILE* csv = nullptr;
    if (!opt.csv.empty()) {
        csv = std::fopen(opt.csv.c_str(), "w");
        if (csv == nullptr) {
            std::fprintf(stderr, "cannot write %s\n", opt.csv.c_str());
            return 2;
        }
        std::fprintf(csv, "label,param,value,seed,participant,name,reaction_ns,informed,value_price_units,"
                          "value_currency,fills,maker_fills,taker_fills,adverse_milli,spread_milli,edge_milli,"
                          "markout_n,adverse_pct,orders_sent,rejects,wakes,session_trades,gap_ticks_mean,"
                          "session_hash\n");
    }

    if (!opt.quiet) {
        std::printf("arena sweep — simulated, not measured. The engine's latency is in README.md.\n");
        std::printf("  sweeping %s over %zu points, %d shared seeds each, %zu requests per session\n",
                    opt.param.c_str(), opt.values.size(), opt.seeds, opt.requests);
        for (const auto& o : opt.overrides) {
            std::printf("  fixed    %s = %lld\n", o.first.c_str(), static_cast<long long>(o.second));
        }
        std::printf("\n");
    }

    // The whole grid first, so that paired differences are available afterwards.
    std::vector<std::vector<Cell>> grid;
    for (const std::int64_t value : opt.values) {
        std::vector<Cell> row;
        row.reserve(static_cast<std::size_t>(opt.seeds));
        for (int k = 0; k < opt.seeds; ++k) {
            Cell c;
            if (!run_one(opt, value, opt.base_seed + static_cast<std::uint64_t>(k), csv, c)) {
                std::fprintf(stderr, "unknown field '%s' (try --help)\n", opt.param.c_str());
                if (csv != nullptr) std::fclose(csv);
                return 2;
            }
            row.push_back(c);
        }
        grid.push_back(std::move(row));
        if (!opt.quiet) std::printf("  %-14lld done\n", static_cast<long long>(value));
    }
    if (csv != nullptr) std::fclose(csv);

    std::vector<Point> points;
    for (std::size_t p = 0; p < grid.size(); ++p) {
        const std::vector<Cell>& row = grid[p];
        Point pt;
        pt.value = opt.values[p];
        pt.maker_value = column(row, [](const Cell& c) { return c.maker_value; });
        pt.maker_adverse = column(row, [](const Cell& c) { return c.maker_adverse; });
        pt.maker_spread = column(row, [](const Cell& c) { return c.maker_spread; });
        pt.maker_adv_pct = column(row, [](const Cell& c) { return c.maker_adv_pct; });
        pt.rival_value = column(row, [](const Cell& c) { return c.rival_value; });
        pt.informed_value = column(row, [](const Cell& c) { return c.informed_value; });
        pt.trades = column(row, [](const Cell& c) { return c.trades; });
        pt.gap = column(row, [](const Cell& c) { return c.gap; });
        pt.d_value = paired(row, grid[0], [](const Cell& c) { return c.maker_value; });
        pt.d_adverse = paired(row, grid[0], [](const Cell& c) { return c.maker_adverse; });
        pt.maker_fills = mean_of(row, [](const Cell& c) { return c.maker_fills; });
        pt.maker_wakes = mean_of(row, [](const Cell& c) { return c.maker_wakes; });
        pt.gap_max = mean_of(row, [](const Cell& c) { return c.gap_max; });
        pt.unpriced_pct = mean_of(row, [](const Cell& c) { return c.unpriced_pct; });
        pt.first_hash = row.empty() ? 0 : row.front().hash;
        for (const Cell& c : row) pt.replays = pt.replays && c.replayed;
        points.push_back(pt);
    }

    // Sanity before result: was this a market at all?
    std::printf("\nmarket health — every point must look like a market, or nothing below counts\n");
    std::printf("  %-14s | %-24s | %-9s | %-10s | %s\n", opt.param.c_str(), "|mid - fundamental|", "worst",
                "one-sided", "trades");
    std::printf("  %s\n", std::string(82, '-').c_str());
    for (const Point& p : points) {
        std::printf("  %-14lld | %6.2f ticks (%5.2f..%5.2f) | %6.0f tk | %8.1f%% | %8.0f\n",
                    static_cast<long long>(p.value), p.gap.median, p.gap.lo, p.gap.hi, p.gap_max, p.unpriced_pct,
                    p.trades.median);
    }

    std::printf("\nlevels - medians across %d seeds, range in brackets, currency units\n", opt.seeds);
    std::printf("  %-14s | %-32s | %-10s | %-10s | %-9s | %-9s | %s\n", opt.param.c_str(), "maker value", "adverse",
                "spread", "rival", "informed", "fills");
    std::printf("  %s\n", std::string(112, '-').c_str());
    for (const Point& p : points) {
        std::printf("  %-14lld | %+10.2f (%+9.2f..%+9.2f) | %+10.3f | %+10.3f | %+9.2f | %+9.2f | %7.0f\n",
                    static_cast<long long>(p.value), p.maker_value.median, p.maker_value.lo, p.maker_value.hi,
                    p.maker_adverse.median, p.maker_spread.median, p.rival_value.median, p.informed_value.median,
                    p.maker_fills);
    }

    // The statistic the question is actually about. Every point runs on the same
    // seeds, so a difference taken seed by seed cancels the shared path of the
    // fundamental; a range that straddles zero means this many seeds do not
    // separate the two points, and the table says so in the last column rather
    // than leaving a reader to compare brackets.
    std::printf("\npaired against %s = %lld, seed by seed - the statistic to read\n", opt.param.c_str(),
                static_cast<long long>(points.front().value));
    std::printf("  %-14s | %-34s | %-34s | %s\n", opt.param.c_str(), "maker value, paired",
                "adverse per fill, paired", "value separated?");
    std::printf("  %s\n", std::string(106, '-').c_str());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Point& p = points[i];
        const bool straddles = p.d_value.lo <= 0 && p.d_value.hi >= 0;
        std::printf("  %-14lld | %+11.2f (%+10.2f..%+10.2f) | %+11.3f (%+10.3f..%+10.3f) | %s\n",
                    static_cast<long long>(p.value), p.d_value.median, p.d_value.lo, p.d_value.hi,
                    p.d_adverse.median, p.d_adverse.lo, p.d_adverse.hi,
                    i == 0 ? "baseline" : (straddles ? "no" : "yes"));
    }

    if (points.size() >= 2) {
        const Point& a = points.front();
        const Point& b = points.back();
        std::printf("\n  %s %lld -> %lld\n", opt.param.c_str(), static_cast<long long>(a.value),
                    static_cast<long long>(b.value));
        std::printf("    maker value, paired   %+.2f   (range %+.2f..%+.2f over %d seeds)\n", b.d_value.median,
                    b.d_value.lo, b.d_value.hi, opt.seeds);
        // A paired difference whose range straddles zero is not a result, and
        // saying so here is cheaper than letting a reader assume otherwise.
        const bool straddles = b.d_value.lo <= 0 && b.d_value.hi >= 0;
        std::printf("    the paired range %s zero, so the difference is %s at this seed count.\n",
                    straddles ? "STRADDLES" : "does not cross", straddles ? "NOT established" : "established");
        std::printf("    share of flow         %.0f fills -> %.0f fills, %.0f looks at the market -> %.0f\n",
                    a.maker_fills, b.maker_fills, a.maker_wakes, b.maker_wakes);
        std::printf("  Value moves for two reasons at once: a slower maker is picked off more per fill, and it also\n"
                    "  gets fewer fills, because it cannot keep a quote standing. Separate them by reading\n"
                    "  `adverse` — which is per fill, so flow-independent — beside the fill counts.\n");
    }

    bool all_replay = true;
    for (const Point& p : points) all_replay = all_replay && p.replays;
    if (opt.replay_check) {
        std::printf("\n  replay: every session run twice, hashes %s\n", all_replay ? "identical" : "DIFFERENT");
    }
    std::printf("  session hash at seed %llu, per point:", static_cast<unsigned long long>(opt.base_seed));
    for (const Point& p : points) std::printf(" %016llx", static_cast<unsigned long long>(p.first_hash));
    std::printf("\n");

    if (!opt.summary_csv.empty()) {
        std::FILE* f = std::fopen(opt.summary_csv.c_str(), "w");
        if (f == nullptr) {
            std::fprintf(stderr, "cannot write %s\n", opt.summary_csv.c_str());
            return 2;
        }
        std::fprintf(f, "label,param,value,seeds,maker_value_median,maker_value_lo,maker_value_hi,"
                        "maker_value_paired_median,maker_value_paired_lo,maker_value_paired_hi,"
                        "maker_adverse_median,maker_adverse_lo,maker_adverse_hi,maker_adverse_paired_median,"
                        "maker_spread_median,maker_adverse_pct_median,maker_fills_mean,maker_wakes_mean,"
                        "rival_value_median,informed_value_median,trades_median,gap_ticks_median,gap_ticks_worst,"
                        "one_sided_pct,session_hash_first_seed\n");
        for (const Point& p : points) {
            std::fprintf(f,
                         "%s,%s,%lld,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.1f,%.1f,"
                         "%.4f,%.4f,%.1f,%.3f,%.0f,%.2f,%016llx\n",
                         opt.label.c_str(), opt.param.c_str(), static_cast<long long>(p.value), opt.seeds,
                         p.maker_value.median, p.maker_value.lo, p.maker_value.hi, p.d_value.median, p.d_value.lo,
                         p.d_value.hi, p.maker_adverse.median, p.maker_adverse.lo, p.maker_adverse.hi,
                         p.d_adverse.median, p.maker_spread.median, p.maker_adv_pct.median, p.maker_fills,
                         p.maker_wakes, p.rival_value.median, p.informed_value.median, p.trades.median, p.gap.median,
                         p.gap_max, p.unpriced_pct, static_cast<unsigned long long>(p.first_hash));
        }
        std::fclose(f);
    }
    return all_replay ? 0 : 1;
}
