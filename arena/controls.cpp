// arena/controls.cpp — runs the evaluator's controls on a named block of seeds
// and fails if any does not hold (docs/CONTROLS.md).
//
//   controls                  the standard block, every control
//   controls --block=smoke    4 seeds: does it run (the noise-floor and positive
//                             controls are not expected to mean much there)
//
// Built-in strategies only, from the evidence-side build: no installed strategy
// can reach it. Exit 0 every control held, 1 one did not, 2 usage.
#include "arena/controls.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

using namespace px;
using namespace px::arena;
using namespace px::arena::suite;

int main(int argc, char** argv) {
    std::string block_name = "standard";
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--help" || a == "-h") {
            std::printf("controls - the evaluator's own controls, answers known in advance (docs/CONTROLS.md)\n\n"
                        "  --block=NAME   standard (64 seeds, the default) or smoke (4 seeds)\n"
                        "Every number is simulated. Exit 0 every control held, 1 one did not, 2 usage.\n");
            return 0;
        } else if (a.starts_with("--block=")) {
            block_name = std::string(a.substr(8));
        } else {
            std::fprintf(stderr, "unknown flag %s (try --help)\n", argv[i]);
            return 2;
        }
    }
    const SeedBlock* block = find_block(block_name);
    if (block == nullptr) {
        std::fprintf(stderr, "unknown block '%s': standard or smoke\n", block_name.c_str());
        return 2;
    }
    if (block->confirmatory) {
        std::fprintf(stderr, "the %s block is for confirming a finished strategy once; the controls run on standard or smoke\n",
                     block->name);
        return 2;
    }

    const SuiteDef def = standard_suite();
    SuiteDef slow = def;
    slow.seat_reaction_ns = def.seat_reaction_ns - 1; // 9,999 ns: a difference that should not matter

    // label, strategy, the suite it runs under
    struct Arm {
        const char* label;
        const char* strategy;
        const SuiteDef* def;
    };
    const Arm arms[] = {
        {"roundtrip", "roundtrip", &def},     // the positive control
        {"maker", "maker", &def},             // A/A, first copy
        {"maker-copy", "maker", &def},        // A/A, second copy
        {"maker-9999", "maker", &slow},       // the noise floor
    };

    RunRecord run;
    run.def = def;
    run.block = *block;
    run.info.binary = "controls";
    run.info.user_strategies = "none";
    for (const Arm& a : arms) {
        CandidateInfo c;
        c.name = a.label;
        run.info.candidates.push_back(c);
    }

    std::string why;
    auto run_one = [&](const SuiteDef& d, std::string_view strategy, std::string_view label, ArmRecord& rec) {
        Roster r;
        Origin origin = Origin::Reference;
        if (!build_arm_roster(d, strategy, r, origin, why) ||
            !run_arm(d, *block, rec.session.seed, label, origin, std::move(r), rec, why)) {
            std::fprintf(stderr, "cannot run %.*s: %s\n", static_cast<int>(label.size()), label.data(), why.c_str());
            std::exit(2);
        }
    };

    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint32_t k = 0; k < block->count; ++k) {
        const std::uint64_t seed = block->first + k;
        ArmRecord placebo, bare;
        placebo.session.seed = bare.session.seed = seed;
        run_one(def, "placebo", "placebo", placebo);
        run_one(def, "", "none", bare);
        placebo.session.check_kind = "no_seat";
        placebo.session.check_hash = bare.session.session_hash;
        placebo.session.check_ok = bare.session.session_hash == placebo.session.session_hash;
        run.sessions.push_back(placebo.session);
        for (ParticipantRow& p : placebo.parts) run.parts.push_back(std::move(p));
        for (const Arm& a : arms) {
            ArmRecord rec, again;
            rec.session.seed = again.session.seed = seed;
            run_one(*a.def, a.strategy, a.label, rec);
            run_one(*a.def, a.strategy, a.label, again);
            rec.session.check_kind = "replay";
            rec.session.check_hash = again.session.session_hash;
            rec.session.check_ok = again.session.session_hash == rec.session.session_hash;
            run.sessions.push_back(rec.session);
            for (ParticipantRow& p : rec.parts) run.parts.push_back(std::move(p));
        }
    }
    std::fprintf(stderr, "%zu sessions in %.1f s\n", run.sessions.size() * 2,
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());

    const std::vector<ControlResult> cs = {
        control_placebo_zero(run),
        control_placebo_inert(run),
        control_common_close(run),
        control_replay(run),
        control_identities(run),
        control_aa(run, "maker", "maker-copy"),
        control_noise_floor(run, "maker", "maker-9999", "maker with the seat at 9,999 ns against 10,000 ns"),
        control_positive(run, "roundtrip"),
    };
    std::printf("arena controls - simulated, not measured. Block %s: %u seeds (%llu..%llu).\n", block->name,
                block->count, static_cast<unsigned long long>(block->first),
                static_cast<unsigned long long>(block->first + block->count - 1));
    const std::string text = controls_text(cs);
    std::fwrite(text.data(), 1, text.size(), stdout);
    for (const ControlResult& c : cs) {
        if (!c.pass) return 1;
    }
    return 0;
}
