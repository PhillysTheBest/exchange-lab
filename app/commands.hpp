// app/commands.hpp — what each slash command of the exchange-lab app means.
//
// The app is a front door, not a merger (docs/APP.md). Every command that does
// real work is turned into a child process: a make target that builds what it
// needs, then one of the programs the project already has. This header holds
// that translation as pure functions of a typed line, so tests/test_app.cpp can
// check it without starting a process, and main.cpp holds everything that
// touches the terminal, the file system or a child.
//
// Nothing here is in a session. The app never links the market, so no hash,
// fingerprint or published number can depend on it.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace px::app {

struct CommandInfo {
    const char* name;    // with its slash
    const char* usage;   // the arguments, as /help prints them
    const char* summary; // one line
    const char* detail;  // what it runs and an example or two
};

// The order is the order /help prints. test_app holds this table and
// plan_for() together, so a command cannot be listed and not handled, or the
// other way round.
inline constexpr CommandInfo kCommands[] = {
    {"/help", "[command]", "list the commands, or explain one", "/help compare"},
    {"/learn", "[N]", "the guided exercises in docs/LEARN.md, one at a time",
     "/learn lists them; /learn 1 shows the first."},
    {"/strategies", "", "the built-in strategies and the ones you installed", ""},
    {"/add", "PATH", "install a strategy of your own: one C++ header",
     "Runs make strategy STRATEGY=PATH: checks the name, copies the header into\n"
     "arena/strategies/user/ and rebuilds the lab programs, the only ones that can see it.\n"
     "  /add examples/strategies/imbalance.hpp"},
    {"/compare", "NAME[,NAME...] [smoke] [wide|busy|all|market=M]",
     "score strategies in the standard seat against an empty one, then show the report",
     "Runs suite_lab. Every candidate sits in the same seat of the same market on the same\n"
     "64 seeds (smoke: 4), beside a placebo arm whose seat never acts. wide and busy are\n"
     "scenario families; all runs every family, side by side, never pooled. market= runs\n"
     "on a market made by /calibrate instead: a family of its own.\n"
     "  /compare imbalance smoke\n"
     "  /compare imbalance,maker\n"
     "  /compare imbalance all\n"
     "  /compare imbalance market=demo"},
    {"/runs", "", "the run directories in runs/, newest first", ""},
    {"/report", "[RUN]", "show a run's report; the newest run by default", "/report imbalance-standard"},
    {"/reproduce", "[RUN]", "re-run a run from its own record and compare every file",
     "Runs suite_lab --reproduce. It refuses before running if the conditions or a strategy\n"
     "changed since the run. The newest run by default.\n"
     "  /reproduce imbalance-standard"},
    {"/watch", "[NAME,...] [seed=N] [still=NS] [script=PATH] [lineup=SPEC] [market=M]",
     "watch a market trade live, full screen; q comes back here",
     "Runs arena_lab. Names are added to the default line-up; lineup= replaces it.\n"
     "still= draws one frame at that simulated instant; script= plays a file of orders as you;\n"
     "market= watches a market made by /calibrate.\n"
     "  /watch\n"
     "  /watch imbalance seed=43\n"
     "  /watch still=60000000\n"
     "  /watch script=tools/arena_script_demo.txt"},
    {"/sweep", "[FLAGS...] [market=M]", "explore: run one market many times, changing one thing",
     "Runs sweep_lab with the flags as given; /sweep --help lists them. market= starts from\n"
     "a market made by /calibrate.\n"
     "  /sweep --seeds=8 --values=1000,1000000\n"
     "  /sweep --null --seeds=8"},
    {"/data", "[add FILE [SETTINGS] | show NAME | remove NAME]",
     "bring a market data file in as a tape, and see what it holds",
     "Runs ingest. A file you already have is read from disk, never fetched, and written to\n"
     "data/NAME.tape, which git ignores. /data alone lists the tapes. Settings, when the\n"
     "file does not say: format=csv|lobster|databento|tardis|binance name= columns= time=\n"
     "tick= symbol= instrument=\n"
     "  /data add examples/data/DEMO_2012-06-21_34200000_57600000_message_3.csv\n"
     "  /data add examples/data/trades.csv name=demo-trades\n"
     "  /data add quotes.csv columns=time,bid,bid_qty,ask,ask_qty\n"
     "  /data show demo-2012-06-21"},
    {"/calibrate", "NAME", "set the market to resemble a tape, and check that it is still a market",
     "Runs calibrate. It first runs the known-answer control, then moves the market's spread,\n"
     "order size and pace until its statistics match the tape's, then runs the health gate on\n"
     "64 seeds, and only then writes markets/NAME.cfg for /compare, /watch and /sweep market=NAME.\n"
     "A trades-only tape is refused: the control does not hold from trades alone.\n"
     "--known=NAME writes a tape whose answer is known, to try the whole path on.\n"
     "  /calibrate --known=known\n"
     "  /calibrate known\n"
     "  /calibrate --control"},
    {"/controls", "[smoke]", "check the evaluator itself against answers known in advance",
     "Runs controls: eight controls, each of which the tests show can fail."},
    {"/book", "", "an order book you drive by hand (type help inside)",
     "Runs the REPL: type orders and watch price-time priority work. quit comes back here."},
    {"/quit", "", "leave", ""},
};

inline constexpr const char* kDefaultLineup = "maker,rival,informed,noise:2,momentum";

// One line per built-in strategy, for /strategies. test_app checks that every
// name in the registry's kStrategyNames has one.
struct StrategyNote {
    const char* name;
    const char* note;
};
inline constexpr StrategyNote kStrategyNotes[] = {
    {"maker", "quotes both sides and learns the price from the order flow"},
    {"rival", "a second maker, at a fixed fast reaction delay"},
    {"informed", "sees the fundamental value nobody else can, and takes quotes on the wrong side of it"},
    {"noise", "random orders with no information: the uninformed flow"},
    {"momentum", "buys strength and sells weakness on the last few prints"},
    {"fader", "leans against the recent move; the worked example in arena/strategies/fader.hpp"},
    {"placebo", "never acts: the empty seat every candidate is compared against"},
    {"roundtrip", "buys then sells at market on a timer: the positive control"},
};

// Splits a typed line into words. Double or single quotes keep spaces inside a
// word; a quote that is never closed is an error rather than a guess.
[[nodiscard]] inline bool split_words(std::string_view line, std::vector<std::string>& out, std::string& error) {
    out.clear();
    std::string word;
    bool in_word = false;
    char quote = 0;
    for (const char c : line) {
        if (quote != 0) {
            if (c == quote) quote = 0;
            else word += c;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            in_word = true;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (in_word) out.push_back(word);
            word.clear();
            in_word = false;
        } else {
            word += c;
            in_word = true;
        }
    }
    if (quote != 0) {
        error = "a quote is never closed";
        return false;
    }
    if (in_word) out.push_back(word);
    return true;
}

[[nodiscard]] inline const CommandInfo* find_command(std::string_view name) {
    for (const CommandInfo& c : kCommands) {
        if (name == c.name) return &c;
    }
    if (name == "/exit") return find_command("/quit");
    return nullptr;
}

// Where things are and how to build them. main.cpp fills it in.
struct Env {
    std::string rel = "build/release"; // where the programs are
    std::string build = "build";       // passed to make as BUILD=
    std::string exe;                   // ".exe" on Windows
    std::string make = "make";         // mingw32-make on Windows
    std::string latest_run;            // the newest complete run directory, or empty
};

enum class Program { None, Make, Suite, Reproduce, Controls, Arena, Sweep, Repl, Ingest, Calibrate };

// What one command will do. Internal commands (help, learn, strategies, runs,
// report, quit) leave build and run empty and are answered by main.cpp.
struct Plan {
    bool internal = false;
    std::vector<std::string> build; // a make invocation to run first
    std::vector<std::string> run;   // then this program
    Program program = Program::None;
    std::string error;              // non-empty: refuse, and say this
};

[[nodiscard]] inline std::vector<std::string> make_target(const Env& env, const std::string& target) {
    return {env.make, "-s", "--no-print-directory", "BUILD=" + env.build, "ANNOUNCE=1", target};
}

[[nodiscard]] inline std::string program_path(const Env& env, const char* name) {
    return env.rel + "/" + name + env.exe;
}

[[nodiscard]] inline bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] inline Plan plan_for(const std::vector<std::string>& words, const Env& env) {
    Plan p;
    if (words.empty()) {
        p.internal = true;
        return p;
    }
    const std::string& name = words[0];
    const CommandInfo* info = find_command(name);
    if (info == nullptr) {
        // An MSYS2 shell rewrites an argument starting with / into a Windows path before a
        // Windows program sees it, so `exchange-lab -c /help` arrives as C:/msys64/help.
        const bool drive = name.size() >= 3 && name[1] == ':' && name[2] == '/' &&
                           ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z'));
        if (drive)
            p.error = name + " looks like a / command that the MSYS2 shell turned into a Windows path; "
                             "run export MSYS2_ARG_CONV_EXCL='*' first, or type the command at the prompt";
        else
            p.error = name.empty() || name[0] != '/' ? "commands start with /; type /help for the list"
                                                     : "no command " + name + "; type /help for the list";
        return p;
    }
    const std::string cmd = info->name;
    const std::vector<std::string> args(words.begin() + 1, words.end());

    if (cmd == "/help" || cmd == "/learn" || cmd == "/strategies" || cmd == "/runs" || cmd == "/report" ||
        cmd == "/quit") {
        p.internal = true;
        if ((cmd == "/strategies" || cmd == "/runs" || cmd == "/quit") && !args.empty())
            p.error = cmd + " takes no arguments";
        else if (args.size() > 1)
            p.error = cmd + " takes at most one argument";
        return p;
    }

    if (cmd == "/add") {
        if (args.size() != 1) {
            p.error = "say which header: /add path/to/name.hpp";
            return p;
        }
        p.build = make_target(env, "strategy");
        p.build.push_back("STRATEGY=" + args[0]);
        p.program = Program::Make;
        return p;
    }

    if (cmd == "/compare") {
        std::string candidates, block, suite, market;
        for (const std::string& a : args) {
            if (a == "smoke") {
                block = "smoke";
            } else if (a == "wide" || a == "busy" || a == "all") {
                if (!suite.empty()) {
                    p.error = "say one family: wide, busy or all";
                    return p;
                }
                suite = a;
            } else if (starts_with(a, "market=")) {
                market = a.substr(7);
                if (market.empty()) {
                    p.error = "say which market: market=NAME; /calibrate makes one";
                    return p;
                }
            } else if (a.find('=') != std::string::npos) {
                p.error = "unknown setting " + a + "; /help compare";
                return p;
            } else {
                if (!candidates.empty()) candidates += ',';
                candidates += a;
            }
        }
        if (candidates.empty()) {
            p.error = "say which strategies: /compare NAME[,NAME...]; /strategies lists them";
            return p;
        }
        if (!market.empty() && !suite.empty()) {
            p.error = "a calibrated market is a family of its own: give market= or " + suite + ", not both";
            return p;
        }
        p.build = make_target(env, program_path(env, "suite_lab"));
        p.run = {program_path(env, "suite_lab"), "--candidates=" + candidates};
        if (!block.empty()) p.run.push_back("--block=" + block);
        if (!suite.empty()) p.run.push_back("--suite=" + suite);
        if (!market.empty()) p.run.push_back("--market=" + market);
        p.program = Program::Suite;
        return p;
    }

    if (cmd == "/reproduce") {
        if (args.size() > 1) {
            p.error = "say one run: /reproduce [RUN]; /runs lists them";
            return p;
        }
        std::string run = args.empty() ? env.latest_run : args[0];
        if (run.empty()) {
            p.error = "there is no run yet; /compare makes one";
            return p;
        }
        while (run.size() > 1 && run.back() == '/') run.pop_back(); // a completed directory name ends in one
        if (run.find('/') == std::string::npos) run = "runs/" + run;
        p.build = make_target(env, program_path(env, "suite_lab"));
        p.run = {program_path(env, "suite_lab"), "--reproduce=" + run};
        p.program = Program::Reproduce;
        return p;
    }

    if (cmd == "/watch") {
        std::string extra, lineup, seed, still, script, market;
        for (const std::string& a : args) {
            if (starts_with(a, "seed=")) seed = a.substr(5);
            else if (starts_with(a, "still=")) still = a.substr(6);
            else if (starts_with(a, "script=")) script = a.substr(7);
            else if (starts_with(a, "lineup=")) lineup = a.substr(7);
            else if (starts_with(a, "market=")) market = a.substr(7);
            else if (a.find('=') != std::string::npos) {
                p.error = "unknown setting " + a + "; /help watch";
                return p;
            } else {
                if (!extra.empty()) extra += ',';
                extra += a;
            }
        }
        if (!lineup.empty() && !extra.empty()) {
            p.error = "give names or lineup=, not both";
            return p;
        }
        p.build = make_target(env, program_path(env, "arena_lab"));
        p.run = {program_path(env, "arena_lab")};
        if (!lineup.empty()) p.run.push_back("--strategies=" + lineup);
        else if (!extra.empty()) p.run.push_back(std::string("--strategies=") + kDefaultLineup + "," + extra);
        if (!seed.empty()) p.run.push_back("--seed=" + seed);
        if (!still.empty()) p.run.push_back("--still=" + still);
        if (!script.empty()) p.run.push_back("--script=" + script);
        if (!market.empty()) p.run.push_back("--market=" + market);
        p.program = Program::Arena;
        return p;
    }

    if (cmd == "/sweep") {
        p.build = make_target(env, program_path(env, "sweep_lab"));
        p.run = {program_path(env, "sweep_lab")};
        for (const std::string& a : args) p.run.push_back(starts_with(a, "market=") ? "--" + a : a);
        p.program = Program::Sweep;
        return p;
    }

    if (cmd == "/data") {
        p.build = make_target(env, program_path(env, "ingest"));
        p.run = {program_path(env, "ingest")};
        p.program = Program::Ingest;
        if (args.empty()) {
            p.run.push_back("list");
            return p;
        }
        const std::string& sub = args[0];
        std::vector<std::string> names, settings;
        for (std::size_t i = 1; i < args.size(); ++i)
            (args[i].find('=') != std::string::npos ? settings : names).push_back(args[i]);
        if (sub == "add") {
            if (names.size() != 1) {
                p.error = "say one file: /data add FILE [name=NAME ...]";
                return p;
            }
        } else if (sub == "show" || sub == "remove") {
            if (names.size() != 1 || !settings.empty()) {
                p.error = "say one tape: /data " + sub + " NAME; /data lists them";
                return p;
            }
        } else {
            p.error = "say add, show or remove, or nothing to list the tapes; /help data";
            return p;
        }
        p.run.push_back(sub);
        p.run.push_back(names[0]);
        p.run.insert(p.run.end(), settings.begin(), settings.end());
        return p;
    }

    if (cmd == "/calibrate") {
        // A tape's name, or the program's own flags as given: --known=, --control, --robustness.
        std::size_t names = 0;
        for (const std::string& a : args) names += starts_with(a, "-") ? 0 : 1;
        if (args.empty() || names > 1) {
            p.error = "say which tape: /calibrate NAME; /data lists them";
            return p;
        }
        p.build = make_target(env, program_path(env, "calibrate"));
        p.run = {program_path(env, "calibrate")};
        p.run.insert(p.run.end(), args.begin(), args.end());
        p.program = Program::Calibrate;
        return p;
    }

    if (cmd == "/controls") {
        if (args.size() > 1 || (args.size() == 1 && args[0] != "smoke")) {
            p.error = "/controls takes nothing, or smoke";
            return p;
        }
        p.build = make_target(env, program_path(env, "controls"));
        p.run = {program_path(env, "controls")};
        if (!args.empty()) p.run.push_back("--block=smoke");
        p.program = Program::Controls;
        return p;
    }

    if (cmd == "/book") {
        if (!args.empty()) {
            p.error = "/book takes no arguments";
            return p;
        }
        p.build = make_target(env, program_path(env, "repl"));
        p.run = {program_path(env, "repl")};
        p.program = Program::Repl;
        return p;
    }

    p.error = "no handler for " + cmd; // test_app makes this unreachable
    return p;
}

// What a program's exit status means, in words. Empty when 0 needs no comment.
[[nodiscard]] inline std::string exit_meaning(Program program, int status) {
    if (status == 0) return "";
    if (status < 0) return "it could not be started";
    switch (program) {
    case Program::Make:
        return "the build or the checks before it failed; the message above says why";
    case Program::Suite:
        if (status == 1) return "a replayed session differed: this run is not reproducible";
        if (status == 2) return "the command was refused; the message above says why";
        if (status == 3) return "the reference market is not admissible on this build, so nothing was compared";
        break;
    case Program::Reproduce:
        if (status == 1) return "NOT REPRODUCED: a file came out different";
        if (status == 2) return "refused before running; the message above says why";
        break;
    case Program::Controls:
        if (status == 1) return "a control did not hold: do not trust the evaluator on this build";
        if (status == 2) return "the command was refused; the message above says why";
        break;
    case Program::Ingest:
        if (status == 1) return "refused; the message above says why";
        if (status == 2) return "the command was refused; the message above says why";
        break;
    case Program::Calibrate:
        if (status == 1) return "refused, and nothing was written; the message above says why";
        if (status == 2) return "the command was refused; the message above says why";
        break;
    case Program::Arena:
    case Program::Sweep:
    case Program::Repl:
        if (status == 2) return "the command was refused; the message above says why";
        break;
    case Program::None:
        break;
    }
    return "it stopped with status " + std::to_string(status);
}

} // namespace px::app
