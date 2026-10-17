// arena/suite.cpp — the standard experiment runner's command line.
//
//   suite_lab --candidates=mine,other            every candidate on the standard block
//   suite_lab --candidates=mine --block=smoke    4 seeds: does it run, does it trade
//   make compare CANDIDATES=mine,other           the same, built first
//
// Every candidate is run in the same seat of the same market on the same named
// block of seeds, beside a placebo arm (the empty seat), and the run writes a
// directory holding the conditions, the raw per-session results and the paired
// comparison. There is deliberately no --set, --param, --seeds or --strategies:
// the conditions are whatever arena/suite.hpp defines, never whatever was
// typed, and ad-hoc exploration stays with sweep_lab. docs/RUNNER.md has the
// design and the measurements behind it.
//
// Compiled twice, like sweep: `suite` without PX_ARENA_USER_STRATEGIES, which
// can run built-in strategies only, and `suite_lab` with it, which can also run
// strategies installed with `make strategy`. Both share one reference market,
// and test-lab checks that their fingerprints agree.
//
// Exit codes: 0 the run is complete, whatever the verdicts; 1 a candidate
// session gave a different hash on replay; 2 usage or configuration error,
// nothing was run; 3 the reference market is not admissible on this build.
#include "arena/suite.hpp"
#include "arena/suite_report.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace px;
using namespace px::arena;
using namespace px::arena::suite;

namespace {

namespace fs = std::filesystem;

constexpr const char* kFiles[] = {"suite.cfg",      "sessions.csv",   "participants.csv", "comparison.csv",
                                  "evaluation.csv", "attribution.csv", "report.txt",      "outcome.cfg"};
constexpr const char* kFamiliesFile = "families.txt";

#if defined(PX_ARENA_USER_STRATEGIES)
constexpr const char* kBinary = "suite_lab";
#else
constexpr const char* kBinary = "suite";
#endif

void usage() {
    std::printf(
        "arena suite - every candidate in the same seat of the same market, on the same seeds.\n\n"
        "  --candidates=A[,B,...]  1 to 8 strategies; the first is the baseline of the paired rows\n"
        "  --block=NAME            standard (64 seeds, the default) or smoke (4 seeds)\n"
        "  --suite=NAME            standard (the default), wide, busy, or all: every one, one directory each\n"
        "  --out=DIR               default runs/<a>[-vs-<b>...]-<block>/; a previous run there is replaced\n"
        "  --help\n\n"
        "%s\n\n"
        "Each suite is a fixed market (arena/suite.hpp); wide and busy each change one thing about\n"
        "standard and are scenario families, reported side by side and never pooled (docs/REPORT.md).\n"
        "The market, the seat, the session length and the seeds are fixed by the suite\n"
        "(arena/suite.hpp, docs/RUNNER.md) and cannot be changed here. Every number is\n"
        "simulated. The engine's measured latency is in README.md and is unrelated.\n",
        known_candidates_text().c_str());
}

[[nodiscard]] std::vector<std::string> split_commas(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

[[nodiscard]] bool write_file(const fs::path& p, const std::string& text) {
    // Binary mode: LF on every platform, so the raw files compare byte for byte.
    std::FILE* f = std::fopen(p.string().c_str(), "wb");
    if (f == nullptr) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

// A previous run directory (one holding `marker`: suite.cfg for one suite,
// families.txt for --suite=all) is replaced: exactly the files a run writes are
// deleted, nothing else. Any other directory that exists and is not empty is
// refused, so a typo cannot delete somebody's files.
[[nodiscard]] bool prepare_out(const fs::path& dir, const char* marker, std::string& why) {
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        if (!fs::is_directory(dir, ec)) {
            why = dir.string() + " exists and is not a directory";
            return false;
        }
        if (fs::exists(dir / marker, ec)) {
            if (std::string_view(marker) == kFamiliesFile) {
                fs::remove(dir / kFamiliesFile, ec);
            } else {
                for (const char* f : kFiles) fs::remove(dir / f, ec);
            }
        } else if (!fs::is_empty(dir, ec)) {
            why = dir.string() + " is not empty and holds no previous run (no " + marker +
                  "): refusing to write there";
            return false;
        }
    }
    fs::create_directories(dir, ec);
    if (ec) {
        why = "cannot create " + dir.string() + ": " + ec.message();
        return false;
    }
    return true;
}

// One suite, start to finish: every arm on every seed of the block, then the
// files, then the printed report. Returns the exit code of this suite alone;
// `family` receives what the --suite=all table needs.
int run_suite(const SuiteDef& def, const SeedBlock& block, const std::vector<std::string>& names,
              const fs::path& dir, FamilyResult& family) {
    RunRecord run;
    run.def = def;
    run.block = block;
    run.info.binary = kBinary;
#if defined(PX_ARENA_USER_STRATEGIES)
    run.info.user_strategies = kUserStrategyNames[0] != '\0' ? kUserStrategyNames : "none";
#else
    run.info.user_strategies = "none";
#endif
    std::string why;
    // Build every arm once before anything runs, so a problem costs no time.
    for (const std::string& n : names) {
        Roster r;
        Origin origin = Origin::Builtin;
        if (!build_arm_roster(def, n, r, origin, why)) {
            std::fprintf(stderr, "%s: %s\n", n.c_str(), why.c_str());
            return 2;
        }
        const auto* seat = dynamic_cast<const SeatOverride*>(r.strategies.back().get());
        CandidateInfo c;
        c.name = n;
        c.origin = origin;
        c.declared_reaction_ns = seat != nullptr ? seat->declared_reaction_ns() : 0;
        c.timer_ns = r.strategies.back()->timer_ns();
        run.info.candidates.push_back(c);
    }
    if (!prepare_out(dir, "suite.cfg", why)) {
        std::fprintf(stderr, "%s\n", why.c_str());
        return 2;
    }
    // The conditions first, before any session runs.
    if (!write_file(dir / "suite.cfg", suite_cfg_text(def, block, run.info))) {
        std::fprintf(stderr, "cannot write %s\n", (dir / "suite.cfg").string().c_str());
        return 2;
    }
    std::string sessions_csv = kSessionsHeader;
    std::string participants_csv = kParticipantsHeader;

    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t per_seed = 2 + 2 * names.size();
    std::fprintf(stderr, "%s: running %llu sessions: %u seeds x (placebo, no-seat check, %zu candidate%s x 2)\n",
                 def.name, static_cast<unsigned long long>(per_seed * block.count), block.count, names.size(),
                 names.size() == 1 ? "" : "s");
    std::uint64_t sessions_run = 0;

    // `candidate` empty: the market with no seat at all.
    auto run_one = [&](std::uint64_t seed, std::string_view candidate, std::string_view label, ArmRecord& rec) {
        Roster r;
        Origin origin = Origin::Reference;
        if (!build_arm_roster(def, candidate, r, origin, why) ||
            !run_arm(def, block, seed, label, origin, std::move(r), rec, why)) {
            std::fprintf(stderr, "cannot run %.*s at seed %llu: %s\n", static_cast<int>(label.size()), label.data(),
                         static_cast<unsigned long long>(seed), why.c_str());
            std::exit(2);
        }
        ++sessions_run;
    };

    for (std::uint32_t k = 0; k < block.count; ++k) {
        const std::uint64_t seed = block.first + k;

        // The placebo arm, checked against the same market with no seat at all:
        // that one comparison proves the reference replays and the seat is inert.
        ArmRecord placebo, bare;
        run_one(seed, "placebo", "placebo", placebo);
        run_one(seed, "", "none", bare);
        placebo.session.check_kind = "no_seat";
        placebo.session.check_hash = bare.session.session_hash;
        placebo.session.check_ok = bare.session.session_hash == placebo.session.session_hash;
        if (!placebo.session.check_ok) placebo.session.health = Health::Check;
        append_session_row(sessions_csv, placebo.session);
        append_participant_rows(participants_csv, placebo.parts);
        const std::uint64_t placebo_hash = placebo.session.session_hash;
        run.sessions.push_back(placebo.session);
        for (ParticipantRow& p : placebo.parts) run.parts.push_back(std::move(p));

        for (const std::string& n : names) {
            ArmRecord arm, again;
            run_one(seed, n, n, arm);
            run_one(seed, n, n, again); // fresh instances: catches state carried between sessions
            arm.session.check_kind = "replay";
            arm.session.check_hash = again.session.session_hash;
            arm.session.check_ok = again.session.session_hash == arm.session.session_hash;
            // A seat that sent nothing must leave the placebo's market exactly as
            // it was; if it does not, the premise the comparison rests on is broken.
            if (arm.session.inert && arm.session.session_hash != placebo_hash) arm.session.check_ok = false;
            if (!arm.session.check_ok) arm.session.health = Health::Check;
            append_session_row(sessions_csv, arm.session);
            append_participant_rows(participants_csv, arm.parts);
            run.sessions.push_back(arm.session);
            for (ParticipantRow& p : arm.parts) run.parts.push_back(std::move(p));
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "%s: %llu sessions in %.1f s\n", def.name, static_cast<unsigned long long>(sessions_run),
                 secs);

    const Outcome outcome = outcome_of(run);
    const std::uint64_t fingerprint = suite_fingerprint(def, block, placebo_hashes(run));
    const std::vector<ComparisonRow> rows = compare(run);
    bool identity_ok = true;
    const std::vector<AttributionRow> attribution = attribute(run, identity_ok);
    const std::string report = report_text(run, rows, fingerprint);

    bool ok = write_file(dir / "sessions.csv", sessions_csv);
    ok = write_file(dir / "participants.csv", participants_csv) && ok;
    ok = write_file(dir / "comparison.csv", comparison_csv(rows)) && ok;
    ok = write_file(dir / "evaluation.csv", evaluation_csv(evaluate(run))) && ok;
    ok = write_file(dir / "attribution.csv", attribution_csv(attribution)) && ok;
    ok = write_file(dir / "report.txt", report) && ok;
    std::fwrite(report.data(), 1, report.size(), stdout);
    // Last, and only if everything else was written, so a directory without
    // status=complete is known to be incomplete.
    ok = ok && write_file(dir / "outcome.cfg", outcome_cfg_text(run, fingerprint, sessions_run, outcome));
    if (!ok) {
        std::fprintf(stderr, "cannot write the run into %s\n", dir.string().c_str());
        return 2;
    }
    std::printf("wrote %s/  suite.cfg sessions.csv participants.csv comparison.csv evaluation.csv attribution.csv "
                "report.txt outcome.cfg\n",
                dir.generic_string().c_str());
    family.suite = def.name;
    family.change = def.change;
    family.reference_admissible = outcome.reference_admissible;
    family.rows = rows;
    return outcome.exit_code();
}

} // namespace

int main(int argc, char** argv) {
    std::string candidates_arg;
    bool have_candidates = false;
    std::string block_name = "standard";
    std::string suite_name = "standard";
    std::string out_arg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else if (a.starts_with("--candidates=")) {
            candidates_arg = std::string(a.substr(13));
            have_candidates = true;
        } else if (a.starts_with("--block=")) {
            block_name = std::string(a.substr(8));
        } else if (a.starts_with("--suite=")) {
            suite_name = std::string(a.substr(8));
        } else if (a.starts_with("--out=")) {
            out_arg = std::string(a.substr(6));
        } else {
            std::fprintf(stderr, "unknown flag %s (the suite's conditions cannot be changed here; try --help)\n",
                         argv[i]);
            return 2;
        }
    }
    if (!have_candidates) {
        std::fprintf(stderr, "say which strategies to run: --candidates=A[,B,...]\n");
        return 2;
    }
    const SeedBlock* block = find_block(block_name);
    if (block == nullptr) {
        std::fprintf(stderr, "unknown block '%s': standard or smoke\n", block_name.c_str());
        return 2;
    }
    std::vector<SuiteDef> suites;
    if (suite_name == "all") {
        for (const std::string& n : split_names(kSuiteNames)) {
            SuiteDef d = standard_suite();
            if (find_suite(n, d)) suites.push_back(d);
        }
    } else {
        SuiteDef d = standard_suite();
        if (!find_suite(suite_name, d)) {
            std::fprintf(stderr, "unknown suite '%s': %s, or all\n", suite_name.c_str(), kSuiteNames);
            return 2;
        }
        suites.push_back(d);
    }

    std::string why;
    const std::vector<std::string> names = split_commas(candidates_arg);
    if (!check_candidates(names, why)) {
        std::fprintf(stderr, "--candidates=%s: %s\n", candidates_arg.c_str(), why.c_str());
        return 2;
    }
    // Every suite's arms are built before anything runs, so a problem costs no time.
    for (const SuiteDef& d : suites) {
        for (const std::string& n : names) {
            Roster r;
            Origin origin = Origin::Builtin;
            if (!build_arm_roster(d, n, r, origin, why)) {
                std::fprintf(stderr, "--candidates=%s: %s\n", candidates_arg.c_str(), why.c_str());
                return 2;
            }
        }
    }

    fs::path dir;
    if (!out_arg.empty()) {
        dir = out_arg;
    } else {
        std::string leaf;
        for (std::size_t i = 0; i < names.size(); ++i) leaf += (i == 0 ? "" : "-vs-") + names[i];
        if (suite_name != "standard") leaf += "-" + suite_name;
        dir = fs::path("runs") / (leaf + "-" + block->name);
    }

    if (suites.size() == 1) {
        FamilyResult f;
        return run_suite(suites.front(), *block, names, dir, f);
    }

    // --suite=all: one directory per suite under `dir`, then one table across them.
    if (!prepare_out(dir, kFamiliesFile, why)) {
        std::fprintf(stderr, "%s\n", why.c_str());
        return 2;
    }
    std::vector<FamilyResult> fams;
    int worst = 0;
    for (const SuiteDef& d : suites) {
        FamilyResult f;
        const int code = run_suite(d, *block, names, dir / d.name, f);
        if (code == 2) return 2;
        // 3 (a reference not admissible) outranks 1 (a replay differed).
        if (code == 3 || (code == 1 && worst == 0)) worst = code;
        fams.push_back(std::move(f));
        std::printf("\n");
    }
    const std::string table = families_text(names, fams);
    std::fwrite(table.data(), 1, table.size(), stdout);
    if (!write_file(dir / kFamiliesFile, table)) {
        std::fprintf(stderr, "cannot write %s\n", (dir / kFamiliesFile).string().c_str());
        return 2;
    }
    std::printf("wrote %s/%s\n", dir.generic_string().c_str(), kFamiliesFile);
    return worst;
}
