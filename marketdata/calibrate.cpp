// marketdata/calibrate.cpp — set the simulated market to match a tape, after
// showing that calibration can recover a market whose answer is known.
//
//   calibrate NAME           data/NAME.tape -> markets/NAME.cfg, or a refusal saying why
//   calibrate --known=NAME   write the control's known-answer tape as data/NAME.tape
//   calibrate --control      the known-answer control and its two planted defects
//   calibrate --robustness   the control on five known markets and three sets of seeds
//
// The design and what it found are in docs/DATA.md. Everything here is a
// simulation: the sessions are invented, and no number is a real market's.
//
// Exit status: 0 done (for --control: the control held and both planted defects
// broke it); 1 refused, or the control did not hold; 2 the command line was wrong.
#include "marketdata/calibrate.hpp"

#include <cstdio>
#include <filesystem>
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
    "\n"
    "Settings: data=DIR (default data) and markets=DIR (default markets), written\n"
    "key=value or --key=value.\n"
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

int cmd_calibrate(const std::string& name, const std::string& data_dir, const std::string& markets_dir) {
    std::string why;
    if (!valid_name(name)) return usage("'" + name + "' is not a tape name; the list command of ingest shows them");
    if (!market_name_ok(name, why)) return usage(why);
    const std::string tape_path = data_dir + "/" + name + ".tape";
    std::string text;
    if (!read_file(tape_path, text)) return refuse("no tape " + name + " in " + data_dir + "/; /data lists the tapes");
    Tape tape;
    if (!parse_tape(text, tape, why)) return refuse(tape_path + ": " + why);
    const std::string tape_cksum = cksum_of(text);
    const TapeStats st = tape_stats(tape);

    std::printf("calibrate %s: %s, %s, %zu events\n", name.c_str(), tape_path.c_str(), depth_words(tape.h.depth),
                tape.ev.size());
    std::printf("Simulated, not measured. The market stays invented: it is set to resemble the tape in\n"
                "three statistics, and in nothing else.\n\n");

    // Refusals that need no simulation first.
    if (tape.h.depth == Depth::Trades)
        return refuse("a trades-only tape is not calibrated. From trades alone the known-answer control does not "
                      "hold (calibrate --control shows it): the bounce of prints between bid and ask cannot be told "
                      "apart from the pace of the flow. A tape with quotes or the book is needed (L2 or L3)");
    if (!st.enough_prints())
        return refuse("too few prints to calibrate from: " + std::to_string(st.prints) + " prints give " +
                      std::to_string(st.move_windows) + " windows of " + std::to_string(kMoveWindow) +
                      " in a row, and calibration needs " + std::to_string(10 * kMoveWindow) +
                      ". Bring a longer file");

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
    std::printf("   %d parameter sets simulated\n\n", cache.simulated());

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

    const std::string out = markets_dir + "/" + name + ".cfg";
    if (!write_whole(out, market_file_text(name, tape, tape_cksum, c, control, health), why)) return refuse(why);
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
    std::string data_dir = "data", markets_dir = "markets", known;
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
        else return usage("unknown setting " + k + "=");
    }
    for (std::string* d : {&data_dir, &markets_dir}) {
        if (d->empty()) return usage("data= and markets= need a directory");
        while (d->size() > 1 && d->back() == '/') d->pop_back();
    }
    if (!known.empty()) {
        if (!names.empty()) return usage("--known=NAME writes a tape; calibrate it afterwards");
        return cmd_known(known, data_dir);
    }
    if (names.size() != 1) return usage(names.empty() ? "say which tape: calibrate NAME" : "say one tape");
    return cmd_calibrate(names[0], data_dir, markets_dir);
}
