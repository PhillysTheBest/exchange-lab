// marketdata/calibrate.cpp — set the simulated market to match a tape, after
// showing that calibration can recover a market whose answer is known.
//
//   calibrate --control      the known-answer control and its two planted defects
//   calibrate --robustness   the control on five known markets and three sets of seeds
//
// The design and what it found are in docs/DATA.md. Everything here is a
// simulation: the sessions are invented, and no number is a real market's.
//
// Exit status: 0 the control held and both planted defects broke it; 1 it did
// not; 2 the command line was wrong.
#include "marketdata/calibrate.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace px;
using namespace px::data;

namespace {

constexpr const char* kHelp =
    "calibrate: set the simulated market to match a tape (docs/DATA.md)\n"
    "\n"
    "  calibrate --control      run the known-answer control: sessions with known parameters\n"
    "                           are written as a tape, and calibrating from it must recover\n"
    "                           every parameter within one grid step; two planted defects must\n"
    "                           make it fail\n"
    "  calibrate --robustness   the same on five known markets and three sets of seeds\n"
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

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
        std::fputs(kHelp, stdout);
        return 0;
    }
    if (args.size() == 1 && args[0] == "--control") return cmd_control();
    if (args.size() == 1 && args[0] == "--robustness") return cmd_robustness();
    return usage(args.empty() ? "say what to do" : "unknown arguments");
}
