// marketdata/calibrate.cpp — set the simulated market to match a tape, after
// showing that calibration can recover a market whose answer is known.
//
//   calibrate NAME           data/NAME.tape -> markets/NAME.cfg, or a refusal saying why
//   calibrate --known=NAME   write the control's known-answer tape as data/NAME.tape
//   calibrate --control      the known-answer control and its two planted defects
//   calibrate --robustness   the control on five known markets and three sets of seeds
//   calibrate --facts=NAME   the tape's stylised facts beside the simulated market's
//   calibrate NAME holdback=30   calibrate on the first 70% of the tape, check on the rest
//
// The design and what it found are in docs/DATA.md. Everything here is a
// simulation: the sessions are invented, and no number is a real market's.
//
// Exit status: 0 done (for --control: the control held and both planted defects
// broke it); 1 refused, or the control did not hold; 2 the command line was wrong.
#include "marketdata/calibrate.hpp"

#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

using namespace px;
using namespace px::data;

namespace {

constexpr const char* kHelp =
    "calibrate: set the simulated market to match a tape (docs/DATA.md)\n"
    "\n"
    "  calibrate NAME           match the standard market to data/NAME.tape (made by ingest) on\n"
    "                           spread, size against depth and pace; run the known-answer control\n"
    "                           and the health gate; write markets/NAME.cfg. Then\n"
    "                           suite_lab, sweep_lab and arena_lab take --market=NAME\n"
    "  calibrate --known=NAME   write the known-answer control's tape as data/NAME.tape: sessions\n"
    "                           of a market whose parameters are known, to try the whole path on\n"
    "  calibrate --control      run the known-answer control: sessions with known parameters\n"
    "                           are written as a tape, and calibrating from it must recover\n"
    "                           every parameter within one grid step; two planted defects must\n"
    "                           make it fail\n"
    "  calibrate --robustness   the same on five known markets and three sets of seeds\n"
    "  calibrate --facts=NAME   the stylised facts of data/NAME.tape beside the simulated market's\n"
    "                           (and NAME's calibrated market, if markets/NAME.cfg exists): a\n"
    "                           diagnostic, never a pass or a fail\n"
    "\n"
    "Settings: data=DIR (default data) and markets=DIR (default markets), written\n"
    "key=value or --key=value. holdback=PCT (10 to 50) calibrates NAME on the first part of\n"
    "its tape and reports the fit on the last PCT% of its events, which calibration never saw.\n"
    "\n"
    "Simulated, not measured: nothing here is a real market's number.\n";

int usage(const std::string& msg) {
    std::fprintf(stderr, "calibrate: %s\n(calibrate --help lists the commands)\n", msg.c_str());
    return 2;
}

std::string params_text(const MarketParams& p, const bool* used = nullptr) {
    auto one = [&](Knob k) {
        if (used != nullptr && !used[static_cast<std::size_t>(k)]) return std::string("-");
        return std::to_string(knob_value(p, k));
    };
    return one(Knob::Spread) + " / " + one(Knob::Size) + " / " + one(Knob::Pace);
}

std::string pad(std::string s, std::size_t n) {
    if (s.size() < n) s.resize(n, ' ');
    return s;
}

std::string seeds_text(const std::uint64_t* seeds, std::size_t n) {
    return std::to_string(seeds[0]) + "-" + std::to_string(seeds[n - 1]);
}

// The control's verdict for one depth: did it hold, and did each defect break it.
struct ControlRun {
    Depth depth;
    Defect defect;
    ControlResult result;
};

int cmd_control() {
    const arena::suite::SuiteDef base = arena::suite::standard_suite();
    const MarketParams truth = control_truth();
    std::printf("The known-answer control. Sessions run with known parameters are written as a tape,\n"
                "and calibrating from it must recover each parameter within one step of its grid.\n"
                "Simulated, not measured: nothing here is a real market.\n\n");
    std::printf("  truth: half spread %lld ticks, noise.qty %lld, noise.timer_ns %lld, on seeds %s;\n"
                "  calibration runs on seeds %s, %zu sessions of %lld ms each\n\n",
                static_cast<long long>(truth.half_spread_ticks), static_cast<long long>(truth.noise_qty),
                static_cast<long long>(truth.noise_timer_ns), seeds_text(kControlSeeds, std::size(kControlSeeds)).c_str(),
                seeds_text(kCalibrationSeeds, std::size(kCalibrationSeeds)).c_str(), std::size(kCalibrationSeeds),
                static_cast<long long>(base.session_ns / 1'000'000));
    std::printf("  %s  %s  %s  %s\n", pad("depth", 6).c_str(), pad("reader", 42).c_str(),
                pad("recovered: spread / size / pace", 32).c_str(), "held");
    std::fflush(stdout);

    MarketCache cache;
    const ControlRun runs_init[] = {{Depth::L3, Defect::None, {}},
                                    {Depth::L3, Defect::DropCancels, {}},
                                    {Depth::L3, Defect::PriceTimesTen, {}},
                                    {Depth::Trades, Defect::None, {}}};
    std::vector<ControlRun> runs(std::begin(runs_init), std::end(runs_init));
    for (ControlRun& r : runs) {
        std::string why;
        if (!known_answer(base, r.depth, r.defect, cache, r.result, why)) {
            std::fprintf(stderr, "calibrate: %s\n", why.c_str());
            return 1;
        }
        bool used[3];
        for (const Knob k : kKnobs) used[static_cast<std::size_t>(k)] = r.result.calibration.fits[static_cast<std::size_t>(k)].used;
        const bool planted = r.defect != Defect::None;
        std::printf("  %s  %s  %s  %s\n", pad(depth_name(r.depth), 6).c_str(),
                    pad(planted ? std::string("planted: ") + defect_name(r.defect) : "as written", 42).c_str(),
                    pad(params_text(r.result.calibration.params, used), 32).c_str(),
                    r.result.held() ? "yes" : (planted ? "no, as it must not" : "no"));
        std::fflush(stdout);
    }
    const bool l3 = runs[0].result.held();
    const bool defects_fail = !runs[1].result.held() && !runs[2].result.held();
    const bool trades = runs[3].result.held();
    std::printf("\n");
    std::printf("  %s\n", l3 ? "Order by order, and at price levels, which read the same statistics, the control holds."
                             : "THE CONTROL DID NOT HOLD: calibration cannot be trusted on this build.");
    std::printf("  %s\n", defects_fail ? "Both planted defects break it, so it can fail."
                                       : "A PLANTED DEFECT DID NOT BREAK IT: the control cannot fail, so it shows nothing.");
    std::printf("  %s\n", trades ? "From trades alone it holds too."
                                 : "From trades alone it does not hold, so a trades-only tape is not calibrated.");
    std::printf("  (%d parameter sets simulated)\n", cache.simulated());
    return l3 && defects_fail ? 0 : 1;
}

int cmd_robustness() {
    const arena::suite::SuiteDef base = arena::suite::standard_suite();
    const MarketParams truths[] = {{3, 40, 20'000}, {2, 25, 10'000}, {5, 15, 40'000}, {1, 60, 5'000}, {6, 20, 7'500}};
    const std::uint64_t offsets[] = {0, 100, 200};
    std::printf("The known-answer control on five known markets, each written on three sets of seeds.\n"
                "Simulated, not measured. Recovered within one grid step on every parameter, or not.\n\n");
    std::printf("  %s  %s  %s  %s  %s\n", pad("truth: spread / size / pace", 28).c_str(), pad("seeds", 10).c_str(),
                pad("recovered", 22).c_str(), pad("steps off", 10).c_str(), "within one step");
    std::fflush(stdout);
    MarketCache cache;
    int held = 0, total = 0;
    for (const MarketParams& truth : truths) {
        for (const std::uint64_t off : offsets) {
            std::uint64_t seeds[std::size(kControlSeeds)];
            for (std::size_t i = 0; i < std::size(kControlSeeds); ++i) seeds[i] = kControlSeeds[i] + off;
            Tape t;
            std::string why;
            Calibration c;
            if (!simulate(market_with(base, truth), seeds, std::size(seeds), t, why) ||
                !calibrate(tape_stats(t, Depth::L3), Depth::L3, base, cache, c, why)) {
                std::fprintf(stderr, "calibrate: %s\n", why.c_str());
                return 1;
            }
            std::string off_text;
            bool ok = true;
            for (const Knob k : kKnobs) {
                const int d = grid_index(k, knob_value(c.params, k)) - grid_index(k, knob_value(truth, k));
                ok = ok && d >= -1 && d <= 1;
                off_text += (off_text.empty() ? "" : " ") + std::string(d > 0 ? "+" : "") + std::to_string(d);
            }
            held += ok ? 1 : 0;
            ++total;
            std::printf("  %s  %s  %s  %s  %s\n", pad(params_text(truth), 28).c_str(),
                        pad(seeds_text(seeds, std::size(seeds)), 10).c_str(), pad(params_text(c.params), 22).c_str(),
                        pad(off_text, 10).c_str(), ok ? "yes" : "no");
            std::fflush(stdout);
        }
    }
    std::printf("\n  %d/%d within one grid step on every parameter (%d parameter sets simulated)\n", held, total,
                cache.simulated());
    return 0;
}

int refuse(const std::string& msg) {
    std::fprintf(stderr, "calibrate: %s\n", msg.c_str());
    return 1;
}

// "2.3%" from thousandths of a ratio.
std::string apart_text(std::int64_t d) {
    if (d >= kFar) return "-";
    return std::to_string(d / 10) + "." + std::to_string(d % 10) + "%";
}

std::string hundredths(std::int64_t v) {
    if (v < 0) return "-";
    const std::string frac = std::to_string(v % 100);
    return std::to_string(v / 100) + "." + (frac.size() < 2 ? "0" : "") + frac;
}

// Writes `text` to `path` through a temporary file, so a reader never sees half of it.
bool write_whole(const std::string& path, const std::string& text, std::string& why) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    if (!write_file(tmp, text)) {
        why = "cannot write " + tmp;
        return false;
    }
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
    if (ec) {
        why = "cannot write " + path + ": " + ec.message();
        return false;
    }
    return true;
}

constexpr const char* kKnownFile = "known-answer-control";

// The control's tape, as a file anyone can calibrate: the whole path, tried on
// an answer known in advance.
int cmd_known(const std::string& name, const std::string& data_dir) {
    if (!valid_name(name)) return usage("--known=" + name + ": lower-case letters, digits, - _ and ., starting with a "
                                        "letter or digit");
    const std::string path = data_dir + "/" + name + ".tape";
    std::string old;
    Tape existing;
    std::string why;
    if (read_file(path, old) && (!parse_tape(old, existing, why, true) || existing.h.source_file != kKnownFile))
        return refuse(path + " already exists, made from another file; choose another name");
    const arena::suite::SuiteDef base = arena::suite::standard_suite();
    const MarketParams truth = control_truth();
    Tape t;
    if (!simulate(market_with(base, truth), kControlSeeds, std::size(kControlSeeds), t, why)) return refuse(why);
    t.h.name = name;
    t.h.source_file = kKnownFile;
    t.h.notes = "simulated, not measured: the standard market with half spread " +
                std::to_string(truth.half_spread_ticks) + " ticks, noise.qty " + std::to_string(truth.noise_qty) +
                " and noise.timer_ns " + std::to_string(truth.noise_timer_ns) + ", seeds " +
                seeds_text(kControlSeeds, std::size(kControlSeeds)) + ": calibration should recover " +
                knobs_text(truth);
    if (!write_whole(path, tape_text(t), why)) return refuse(why);
    std::printf("wrote %s: %zu events from %zu simulated sessions whose parameters are known\n", path.c_str(),
                t.ev.size(), std::size(kControlSeeds));
    std::printf("  spread / size / pace: %s. Calibrating it should give the same back:\n", knobs_text(truth).c_str());
    std::printf("  calibrate %s%s\n", name.c_str(), data_dir == "data" ? "" : (" data=" + data_dir).c_str());
    return 0;
}

// ---- stylised facts: printed, never judged ------------------------------------

std::string thousandths_signed(std::int64_t v) {
    if (v == kNotMeasured) return "-";
    const std::int64_t a = v < 0 ? -v : v;
    std::string frac = std::to_string(a % 1000);
    while (frac.size() < 3) frac = "0" + frac;
    return std::string(v < 0 ? "-" : "+") + std::to_string(a / 1000) + "." + frac;
}
std::string whole_or_dash(std::int64_t v) { return v == kNotMeasured ? "-" : std::to_string(v); }
std::string hundredths_or_dash(std::int64_t v) { return v == kNotMeasured ? "-" : hundredths(v); }
std::string three(std::int64_t a, std::int64_t b, std::int64_t c) {
    if (a == kNotMeasured || b == kNotMeasured || c == kNotMeasured) return "-";
    return std::to_string(a) + " / " + std::to_string(b) + " / " + std::to_string(c);
}

struct FactsColumn {
    std::string title;
    StylisedFacts f;
};

void print_facts(const std::vector<FactsColumn>& cols) {
    using Row = std::pair<const char*, std::function<std::string(const StylisedFacts&)>>;
    const std::vector<Row> rows = {
        {"interval, ns (10 mean times between prints)", [](const StylisedFacts& f) { return std::to_string(f.interval_ns); }},
        {"intervals with a return", [](const StylisedFacts& f) { return std::to_string(f.returns); }},
        {"return autocorrelation, lag 1", [](const StylisedFacts& f) { return thousandths_signed(f.ret_ac1_pml); }},
        {"|return| autocorrelation, lag 1", [](const StylisedFacts& f) { return thousandths_signed(f.abs_ac1_pml); }},
        {"|return| autocorrelation, lag 10", [](const StylisedFacts& f) { return thousandths_signed(f.abs_ac10_pml); }},
        {"  not told apart from 0 inside +-",
         [](const StylisedFacts& f) { return f.null_band_pml == kNotMeasured ? std::string("-") : thousandths_signed(f.null_band_pml).substr(1); }},
        {"returns beyond 3 s.d., per 10,000 (normal: 27)", [](const StylisedFacts& f) { return whole_or_dash(f.tail3_x10k); }},
        {"intervals with no move, per 1,000", [](const StylisedFacts& f) { return whole_or_dash(f.zero_returns_pml); }},
        {"volume against |return|, correlation", [](const StylisedFacts& f) { return thousandths_signed(f.volvol_pml); }},
        {"spread, ticks: 10th / 50th / 90th percentile", [](const StylisedFacts& f) { return three(f.spread_p10, f.spread_p50, f.spread_p90); }},
        {"time at a one-tick spread, per 1,000", [](const StylisedFacts& f) { return whole_or_dash(f.one_tick_pml); }},
        {"touch depth: 10th / 50th / 90th percentile", [](const StylisedFacts& f) { return three(f.depth_p10, f.depth_p50, f.depth_p90); }},
        {"orders added per print", [](const StylisedFacts& f) { return hundredths_or_dash(f.adds_per_print_x100); }},
        {"cancels per print", [](const StylisedFacts& f) { return hundredths_or_dash(f.cancels_per_print_x100); }},
        {"cancels per order added, per 1,000", [](const StylisedFacts& f) { return whole_or_dash(f.cancels_per_add_pml); }},
    };
    std::string head = "   " + pad("statistic", 46);
    for (const FactsColumn& c : cols) head += "  " + pad(c.title, 16);
    std::printf("%s\n", head.c_str());
    for (const Row& r : rows) {
        std::string line = "   " + pad(r.first, 46);
        for (const FactsColumn& c : cols) line += "  " + pad(r.second(c.f), 16);
        std::printf("%s\n", line.c_str());
    }
}

void print_facts_notes() {
    std::printf(
        "   What they say, and what they do not (docs/DATA.md, \"Stylised facts\"):\n"
        "   - return autocorrelation: outside +- the band, one interval's move predicts the next's. Bid-ask\n"
        "     bounce makes it negative on a tape read from prints; slow quotes make it positive. Not why.\n"
        "   - |return| autocorrelation: volatility clustering, positive and slow to decay in real markets.\n"
        "     The fundamental here jumps at a constant rate, so the simulation has little to cluster.\n"
        "   - beyond 3 s.d.: a normal gives 27 per 10,000; more is a fatter tail at this interval. Moves are\n"
        "     whole ticks, so it is coarse, and it estimates no tail exponent.\n"
        "   - volume against |return|: at a fixed number of prints an interval it mostly reads how print\n"
        "     sizes vary; it says nothing about which drives which.\n"
        "   - spread, depth and order flow: the shape behind the means calibration fits. An exchange's\n"
        "     messages depend on order types the arena does not have.\n");
}

[[nodiscard]] bool load_tape(const std::string& name, const std::string& data_dir, Tape& tape, std::string& text,
                             std::string& why) {
    const std::string tape_path = data_dir + "/" + name + ".tape";
    if (!read_file(tape_path, text)) {
        why = "no tape " + name + " in " + data_dir + "/; /data lists the tapes";
        return false;
    }
    if (!parse_tape(text, tape, why)) {
        why = tape_path + ": " + why;
        return false;
    }
    return true;
}

// A simulated market's facts on calibration's seeds, read at `depth`.
[[nodiscard]] bool market_facts(const arena::suite::SuiteDef& def, Depth depth, StylisedFacts& out, std::string& why) {
    Tape t;
    if (!simulate(def, kCalibrationSeeds, std::size(kCalibrationSeeds), t, why)) return false;
    out = stylised_facts(t, depth);
    return true;
}

int cmd_facts(const std::string& name, const std::string& data_dir, const std::string& markets_dir) {
    std::string why, text;
    if (!valid_name(name)) return usage("'" + name + "' is not a tape name; the list command of ingest shows them");
    Tape tape;
    if (!load_tape(name, data_dir, tape, text, why)) return refuse(why);
    std::vector<FactsColumn> cols;
    cols.push_back({"tape " + name, stylised_facts(tape, tape.h.depth)});
    StylisedFacts f;
    if (!market_facts(arena::suite::standard_suite(), tape.h.depth, f, why)) return refuse(why);
    cols.push_back({"standard market", f});
    std::string cal_note = "no market " + name + " in " + markets_dir + "/ (calibrate " + name + " makes one)";
    MarketFile m;
    arena::suite::SuiteDef def;
    if (std::filesystem::exists(markets_dir + "/" + name + ".cfg")) {
        if (!read_market(name, markets_dir, m, why) || !market_suite(m, def, why)) return refuse(why);
        if (!market_facts(def, tape.h.depth, f, why)) return refuse(why);
        cols.push_back({"market " + name, f});
        cal_note = "market " + name + ": calibrated to this tape on three statistics, none of these";
    }
    std::printf("stylised facts of %s (%s, %zu events) beside the simulated market\n", name.c_str(),
                depth_words(tape.h.depth), tape.ev.size());
    std::printf("A diagnostic: none of these was fitted, and none is a pass or a fail. Each tape is read in\n"
                "intervals of %lld of its own mean times between prints, so a real day and a 25 ms session are\n"
                "compared at the same number of prints an interval, not on the same clock. The simulated\n"
                "columns are %zu sessions of %lld ms on seeds %s, read %s like the tape.\n\n",
                static_cast<long long>(kFactPrints), std::size(kCalibrationSeeds),
                static_cast<long long>(arena::suite::standard_suite().session_ns / 1'000'000),
                seeds_text(kCalibrationSeeds, std::size(kCalibrationSeeds)).c_str(), depth_words(tape.h.depth));
    print_facts(cols);
    std::printf("   %s\n\n", cal_note.c_str());
    print_facts_notes();
    return 0;
}

int cmd_calibrate(const std::string& name, const std::string& data_dir, const std::string& markets_dir,
                  std::int64_t holdback_pct) {
    std::string why;
    if (!valid_name(name)) return usage("'" + name + "' is not a tape name; the list command of ingest shows them");
    if (!market_name_ok(name, why)) return usage(why);
    const std::string tape_path = data_dir + "/" + name + ".tape";
    std::string text;
    if (!read_file(tape_path, text)) return refuse("no tape " + name + " in " + data_dir + "/; /data lists the tapes");
    Tape tape;
    if (!parse_tape(text, tape, why)) return refuse(tape_path + ": " + why);
    const std::string tape_cksum = cksum_of(text);
    // With part of the tape held back, calibration sees only the moments before
    // the cut; the book is still built from the whole tape, so the held-back part
    // starts from the book as it really stood.
    std::int64_t cut_ns = kTapeEnd;
    if (holdback_pct > 0 && !tape.ev.empty()) {
        const std::size_t at = tape.ev.size() * static_cast<std::size_t>(100 - holdback_pct) / 100;
        cut_ns = tape.ev[at < tape.ev.size() ? at : tape.ev.size() - 1].t;
    }
    const TapeStats st = tape_stats(tape, tape.h.depth, kTapeStart, cut_ns);
    const TapeStats held = holdback_pct > 0 ? tape_stats(tape, tape.h.depth, cut_ns, kTapeEnd) : TapeStats{};

    std::printf("calibrate %s: %s, %s, %zu events\n", name.c_str(), tape_path.c_str(), depth_words(tape.h.depth),
                tape.ev.size());
    std::printf("Simulated, not measured. The market stays invented: it is set to resemble the tape in\n"
                "three statistics, and in nothing else.\n");
    if (holdback_pct > 0)
        std::printf("Holding back the last %lld%% of its events (%llu, from t = %lld ns): calibration sees only the\n"
                    "first %llu, and the fit is then checked on the part it never saw.\n",
                    static_cast<long long>(holdback_pct), static_cast<unsigned long long>(held.events),
                    static_cast<long long>(cut_ns), static_cast<unsigned long long>(st.events));
    std::printf("\n");

    // Refusals that need no simulation first.
    if (tape.h.depth == Depth::Trades)
        return refuse("a trades-only tape is not calibrated. From trades alone the known-answer control does not "
                      "hold (calibrate --control shows it): the bounce of prints between bid and ask cannot be told "
                      "apart from the pace of the flow. A tape with quotes or the book is needed (L2 or L3)");
    if (!st.enough_prints())
        return refuse("too few prints to calibrate from: " + std::to_string(st.prints) + " prints give " +
                      std::to_string(st.move_windows) + " windows of " + std::to_string(kMoveWindow) +
                      " in a row, and calibration needs " + std::to_string(10 * kMoveWindow) +
                      (holdback_pct > 0 ? ". Hold back less, or bring a longer file" : ". Bring a longer file"));
    if (holdback_pct > 0 && !held.enough_prints())
        return refuse("too few prints in the held-back part to check against: " + std::to_string(held.prints) +
                      " prints give " + std::to_string(held.move_windows) + " windows, and a check needs " +
                      std::to_string(10 * kMoveWindow) + ". Hold back more, or bring a longer file");

    const arena::suite::SuiteDef base = arena::suite::standard_suite();
    MarketCache cache;

    // 1. The control, at this tape's depth, on this build.
    std::printf("1. The known-answer control, read %s: sessions made with known parameters are written\n"
                "   as a tape, and calibrating from it must give them back within one grid step.\n",
                depth_words(tape.h.depth));
    std::fflush(stdout);
    ControlResult control;
    if (!known_answer(base, tape.h.depth, Defect::None, cache, control, why)) return refuse(why);
    std::printf("   truth %s, recovered %s: %s\n\n", knobs_text(control.truth).c_str(),
                knobs_text(control.calibration.params).c_str(),
                control.held() ? "it holds" : "IT DOES NOT HOLD");
    if (!control.held())
        return refuse("the known-answer control does not hold on this build, so nothing it calibrates can be "
                      "trusted. Nothing was written");

    // 2. The tape.
    std::printf("2. Matching the tape: the standard market, no seat, %zu sessions of %lld ms on seeds %s\n",
                std::size(kCalibrationSeeds), static_cast<long long>(base.session_ns / 1'000'000),
                seeds_text(kCalibrationSeeds, std::size(kCalibrationSeeds)).c_str());
    std::fflush(stdout);
    Calibration c;
    if (!calibrate(st, tape.h.depth, base, cache, c, why)) return refuse(why);
    std::printf("   %s  %s  %s  %s  %s\n", pad("statistic", 52).c_str(), pad("tape", 8).c_str(),
                pad("market", 8).c_str(), pad("apart", 7).c_str(), "sets");
    bool unreachable = false;
    for (const Fit& f : c.fits) {
        const std::string label = std::string(knob_name(f.knob)) + ": " + knob_stat(f.knob);
        const std::string sets = std::string(knob_sets(f.knob)) + " = " + std::to_string(knob_value(c.params, f.knob));
        if (!f.used) {
            std::printf("   %s  %s  %s  %s  %s\n", pad(label, 52).c_str(), pad("-", 8).c_str(), pad("-", 8).c_str(),
                        pad("-", 7).c_str(), (sets + ", the standard value: not measurable on this tape").c_str());
            continue;
        }
        unreachable = unreachable || f.unreachable;
        std::printf("   %s  %s  %s  %s  %s%s\n", pad(label, 52).c_str(), pad(std::to_string(f.target), 8).c_str(),
                    pad(std::to_string(f.achieved), 8).c_str(), pad(apart_text(f.distance), 7).c_str(), sets.c_str(),
                    f.unreachable ? "  (unreachable)" : "");
    }
    if (unreachable)
        std::printf("   unreachable: no value on the grid comes closer; this market cannot look more like the tape there.\n");
    std::printf("   %d parameter sets simulated\n", cache.simulated());

    // Which other parameter sets the tape cannot tell from this one.
    Identification id;
    if (!identification(base, c, tape.h.depth, cache, id, why)) return refuse(why);
    std::string se_text;
    for (const Fit& f : c.fits) {
        if (!f.used) continue;
        se_text += (se_text.empty() ? "" : ", ") + std::string(knob_name(f.knob)) + " " +
                   std::to_string(id.se[static_cast<std::size_t>(f.knob)]);
    }
    std::string alike_text;
    for (const MarketParams& p : id.alike) alike_text += (alike_text.empty() ? "" : ", ") + params_text(p);
    if (id.alike.empty())
        std::printf("   identified: none of the %zu other parameter sets the search ran on all %zu seeds comes within\n"
                    "   2 s.e. of this one's statistics (s.e. %s). That is not proof that none would.\n\n",
                    id.tried, std::size(kCalibrationSeeds), se_text.c_str());
    else
        std::printf("   NOT UNIQUELY IDENTIFIED: %zu of the %zu other parameter sets the search ran on all %zu seeds\n"
                    "   give statistics within 2 s.e. of this one's (s.e. %s): %s.\n"
                    "   The tape cannot tell them apart. The one written is the closest, not the only one that fits.\n\n",
                    id.alike.size(), id.tried, std::size(kCalibrationSeeds), se_text.c_str(), alike_text.c_str());

    // The part calibration never saw.
    std::string more;
    if (holdback_pct > 0) {
        std::printf("2b. The held-back part, which calibration never saw: the market's statistics against it, and\n"
                    "    the tape's own two parts against each other, as a yardstick for how much a tape moves.\n");
        std::printf("   %s  %s  %s  %s  %s  %s\n", pad("statistic", 52).c_str(), pad("first", 8).c_str(),
                    pad("held", 8).c_str(), pad("market", 8).c_str(), pad("market-held", 12).c_str(), "first-held");
        more += "# The held-back part: the tape's last events, never seen by the search.\n";
        more += "holdback.pct=" + std::to_string(holdback_pct) + "\n";
        more += "holdback.from_ns=" + std::to_string(cut_ns) + "\n";
        more += "holdback.events=" + std::to_string(held.events) + "\n";
        for (const Fit& f : c.fits) {
            const std::string label = std::string(knob_name(f.knob)) + ": " + knob_stat(f.knob);
            const std::int64_t h = knob_stat(held, f.knob);
            const std::string k = std::string("holdback.") + knob_name(f.knob);
            if (!f.used) {
                std::printf("   %s  %s\n", pad(label, 52).c_str(), "-: not measurable on this tape");
                continue;
            }
            const std::int64_t mh = ratio_distance(f.achieved, h), fh = ratio_distance(f.target, h);
            std::printf("   %s  %s  %s  %s  %s  %s\n", pad(label, 52).c_str(), pad(std::to_string(f.target), 8).c_str(),
                        pad(std::to_string(h), 8).c_str(), pad(std::to_string(f.achieved), 8).c_str(),
                        pad(apart_text(mh), 12).c_str(), apart_text(fh).c_str());
            more += k + ".tape=" + std::to_string(h) + "\n";
            more += k + ".apart=" + std::to_string(mh) + "\n";
            more += k + ".first_apart=" + std::to_string(fh) + "\n";
        }
        std::printf("   A market as far from the held-back part as the tape's first part is fits as well as the\n"
                    "   tape agrees with itself; nothing here says it fits better.\n\n");
    }
    more += "# Parameter sets the tape cannot tell from this one: within 2 s.e. on every matched statistic.\n";
    more += "identified.se=" + (se_text.empty() ? std::string("-") : se_text) + "\n";
    more += "identified.tried=" + std::to_string(id.tried) + "\n";
    more += "identified.alike=" + (alike_text.empty() ? std::string("-") : alike_text) + "\n";

    // 3. The health gate, on the block strategies are scored on.
    const arena::suite::SuiteDef market = market_with(base, c.params);
    const arena::suite::SeedBlock& block = arena::suite::kSeedBlocks[0];
    std::printf("3. The health gate on the %s block (%u seeds), with no seat: the book must stay with the\n"
                "   fundamental (mean gap at most %llu ticks, one-sided at most %llu%% of the time) on every seed.\n",
                block.name, block.count, static_cast<unsigned long long>(base.health.max_mean_gap_ticks),
                static_cast<unsigned long long>(base.health.max_one_sided_pct));
    std::fflush(stdout);
    HealthResult health;
    if (!health_check(market, block, health, why)) return refuse(why);
    std::printf("   %u/%u seeds pass; the widest mean gap is %s ticks, on seed %llu\n\n", health.passed, health.seeds,
                hundredths(health.worst_gap_x100).c_str(), static_cast<unsigned long long>(health.worst_seed));
    if (!health.held()) {
        std::string failing;
        for (const auto& [seed, h] : health.failing) {
            if (!failing.empty()) failing += ", ";
            failing += std::to_string(seed) + " (" + arena::suite::health_name(h) + ")";
        }
        return refuse("the calibrated market is not a market: on " + std::to_string(health.failing.size()) + " of " +
                      std::to_string(health.seeds) + " seeds its book comes loose from the fundamental (" + failing +
                      "). Nothing was written");
    }

    // 4. What was not fitted.
    std::printf("4. Statistics that were not fitted: the stylised facts of the tape beside the calibrated market's,\n"
                "   read the same way. A diagnostic; nothing below decides whether the market is written.\n");
    std::fflush(stdout);
    StylisedFacts mf;
    if (!market_facts(market, tape.h.depth, mf, why)) return refuse(why);
    std::vector<FactsColumn> cols;
    if (holdback_pct > 0) {
        cols.push_back({"tape, first part", stylised_facts(tape, tape.h.depth, kTapeStart, cut_ns)});
        cols.push_back({"tape, held back", stylised_facts(tape, tape.h.depth, cut_ns, kTapeEnd)});
    } else {
        cols.push_back({"tape", stylised_facts(tape, tape.h.depth)});
    }
    cols.push_back({"market " + name, mf});
    print_facts(cols);
    std::printf("\n");

    const std::string holdback_condition =
        holdback_pct > 0 ? "last " + std::to_string(holdback_pct) + "% of events, from t=" + std::to_string(cut_ns)
                         : std::string();
    const std::string out = markets_dir + "/" + name + ".cfg";
    if (!write_whole(out, market_file_text(name, tape, tape_cksum, c, control, health, holdback_condition, more), why))
        return refuse(why);
    const std::string what = c.params == reference_params()
                                 ? std::string("every parameter unchanged")
                                 : "spread / size / pace " + knobs_text(c.params) + " (standard: " +
                                       knobs_text(reference_params()) + ")";
    std::printf("wrote %s: the standard market with %s\n", out.c_str(), what.c_str());
    std::printf("  It keeps the standard market's informed trader, fundamental and every parameter\n"
                "  docs/ARENA.md calls load-bearing: no public data shows them.\n");
    std::printf("  Use it: /compare STRATEGY market=%s   /watch market=%s   /sweep market=%s\n", name.c_str(),
                name.c_str(), name.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
        std::fputs(kHelp, stdout);
        return 0;
    }
    if (args.size() == 1 && args[0] == "--control") return cmd_control();
    if (args.size() == 1 && args[0] == "--robustness") return cmd_robustness();
    std::string data_dir = "data", markets_dir = "markets", known, facts, holdback;
    std::vector<std::string> names;
    for (const std::string& raw : args) {
        std::string a = raw;
        if (a.rfind("--", 0) == 0) a.erase(0, 2);
        const std::size_t eq = a.find('=');
        if (eq == std::string::npos) {
            if (raw.rfind("--", 0) == 0) return usage("unknown flag " + raw);
            names.push_back(raw);
            continue;
        }
        const std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if (k == "data") data_dir = v;
        else if (k == "markets") markets_dir = v;
        else if (k == "known") known = v;
        else if (k == "facts") facts = v;
        else if (k == "holdback") holdback = v;
        else return usage("unknown setting " + k + "=");
    }
    for (std::string* d : {&data_dir, &markets_dir}) {
        if (d->empty()) return usage("data= and markets= need a directory");
        while (d->size() > 1 && d->back() == '/') d->pop_back();
    }
    if (!known.empty()) {
        if (!names.empty() || !facts.empty() || !holdback.empty())
            return usage("--known=NAME writes a tape; calibrate it afterwards");
        return cmd_known(known, data_dir);
    }
    if (!facts.empty()) {
        if (!names.empty() || !holdback.empty()) return usage("--facts=NAME takes one tape and no other setting but data= and markets=");
        return cmd_facts(facts, data_dir, markets_dir);
    }
    std::int64_t pct = 0;
    if (!holdback.empty()) {
        if (!parse_int(holdback, pct) || pct < 10 || pct > 50)
            return usage("holdback=" + holdback + ": a whole percentage from 10 to 50");
    }
    if (names.size() != 1) return usage(names.empty() ? "say which tape: calibrate NAME" : "say one tape");
    return cmd_calibrate(names[0], data_dir, markets_dir, pct);
}
