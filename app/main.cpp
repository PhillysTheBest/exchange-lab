// app/main.cpp — exchange-lab: one prompt, slash commands, the existing programs underneath.
//
//   build/release/exchange-lab                        the prompt
//   build/release/exchange-lab -c '/compare maker smoke'   one command, then exit with its status
//   build/release/exchange-lab < commands.txt         one command per line, echoed
//
// docs/APP.md is the design. This file holds everything that touches the
// terminal, the file system or a child process; what each command means is in
// commands.hpp, where test_app can check it without starting anything.
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#endif

#include "app/commands.hpp"
#include "app/line.hpp"
#include "app/run.hpp"
#include "px_build_info.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace px::app;

namespace {

Env g_env;
bool g_colour = false;

std::string paint(const char* code, const std::string& s) {
    return g_colour ? "\x1b[" + std::string(code) + "m" + s + "\x1b[0m" : s;
}
std::string bold(const std::string& s) { return paint("1", s); }
std::string dim(const std::string& s) { return paint("2", s); }

void say(const std::string& s) { std::printf("%s\n", s.c_str()); }

int refuse(const std::string& why) {
    std::fflush(stdout);
    std::fprintf(stderr, "%s\n", paint("31", why).c_str());
    return 2;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// key=value files: suite.cfg and outcome.cfg.
std::string cfg_value(const fs::path& p, const std::string& key) {
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '=')
            return line.substr(key.size() + 1);
    }
    return "";
}

// ---- runs ------------------------------------------------------------------

struct RunInfo {
    std::string name;
    fs::file_time_type when;
    bool complete = false;
    std::string suite, block, candidates;
};

bool run_complete(const fs::path& dir) { return cfg_value(dir / "outcome.cfg", "status") == "complete"; }

// Newest first. A SUITE=all run is a directory of families with families.txt
// beside them; it is complete when every family is.
std::vector<RunInfo> list_runs() {
    std::vector<RunInfo> runs;
    std::error_code ec;
    if (!fs::is_directory("runs", ec)) return runs;
    for (const fs::directory_entry& e : fs::directory_iterator("runs", ec)) {
        if (!e.is_directory()) continue;
        RunInfo r;
        r.name = e.path().filename().string();
        fs::path cfg = e.path() / "suite.cfg";
        if (fs::exists(e.path() / "families.txt")) {
            r.when = fs::last_write_time(e.path() / "families.txt", ec);
            r.complete = true;
            r.suite = "all";
            for (const fs::directory_entry& f : fs::directory_iterator(e.path(), ec)) {
                if (!f.is_directory()) continue;
                r.complete = r.complete && run_complete(f.path());
                cfg = f.path() / "suite.cfg";
            }
        } else if (fs::exists(cfg)) {
            const fs::path outcome = e.path() / "outcome.cfg";
            r.when = fs::last_write_time(fs::exists(outcome) ? outcome : cfg, ec);
            r.complete = run_complete(e.path());
            r.suite = cfg_value(cfg, "suite.name");
        } else {
            continue;
        }
        r.block = cfg_value(cfg, "block.name");
        r.candidates = cfg_value(cfg, "run.candidates");
        runs.push_back(r);
    }
    std::sort(runs.begin(), runs.end(), [](const RunInfo& a, const RunInfo& b) { return a.when > b.when; });
    return runs;
}

std::string latest_run() {
    for (const RunInfo& r : list_runs())
        if (r.complete) return "runs/" + r.name;
    return "";
}

// A run named the way /runs lists it, or a path.
std::string resolve_run(std::string arg) {
    while (arg.size() > 1 && arg.back() == '/') arg.pop_back();
    if (arg.empty()) return g_env.latest_run;
    std::error_code ec;
    if (fs::is_directory("runs/" + arg, ec)) return "runs/" + arg;
    return arg;
}

// ---- strategies ------------------------------------------------------------

std::vector<std::string> installed_strategies() {
    std::vector<std::string> names;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator("arena/strategies/user", ec))
        if (e.path().extension() == ".hpp") names.push_back(e.path().stem().string());
    std::sort(names.begin(), names.end());
    return names;
}

// Whether the lab programs were built with it: the generated manifest lists every
// header that was installed when they were last built.
bool built_into_lab(const std::string& name) {
    const std::string manifest = read_file(g_env.build + "/generated/px_user_strategies.hpp");
    return manifest.find("(name == \"" + name + "\")") != std::string::npos;
}

std::vector<std::string> strategy_names() {
    std::vector<std::string> names;
    for (const StrategyNote& s : kStrategyNotes) names.push_back(s.name);
    for (const std::string& n : installed_strategies()) names.push_back(n);
    return names;
}

// ---- the internal commands -------------------------------------------------

int cmd_help(const std::vector<std::string>& args) {
    if (args.empty()) {
        for (const CommandInfo& c : kCommands) {
            std::string name = c.name;
            name.resize(14, ' ');
            say("  " + bold(name) + c.summary);
        }
        say(dim("/help NAME explains one. Tab completes; up and down recall earlier lines;"));
        say(dim("Ctrl-C stops a running command and keeps the prompt."));
        return 0;
    }
    const std::string name = args[0][0] == '/' ? args[0] : "/" + args[0];
    const CommandInfo* c = find_command(name);
    if (c == nullptr) return refuse("no command " + name + "; /help lists them");
    say(bold(std::string(c->name) + (*c->usage ? " " : "") + c->usage));
    say("  " + std::string(c->summary));
    if (*c->detail) {
        std::istringstream in(c->detail);
        std::string line;
        while (std::getline(in, line)) say("  " + line);
    }
    return 0;
}

int cmd_learn(const std::vector<std::string>& args) {
    std::ifstream in("docs/LEARN.md");
    if (!in) return refuse("docs/LEARN.md is missing");
    std::vector<std::pair<std::string, std::vector<std::string>>> sections;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("## ", 0) == 0) {
            sections.push_back({line.substr(3), {}});
        } else if (!sections.empty()) {
            sections.back().second.push_back(line);
        }
    }
    std::vector<std::size_t> exercises; // sections that are numbered
    for (std::size_t i = 0; i < sections.size(); ++i)
        if (!sections[i].first.empty() && std::isdigit(static_cast<unsigned char>(sections[i].first[0])))
            exercises.push_back(i);
    if (args.empty()) {
        say("The exercises, from one order book to a strategy of your own, scored and reproduced:");
        for (const std::size_t i : exercises) say("  " + sections[i].first);
        say(dim("/learn 1 shows the first. docs/LEARN.md has them all, and GLOSSARY.md every term."));
        return 0;
    }
    const std::string& want = args[0];
    for (const std::size_t i : exercises) {
        const std::string& title = sections[i].first;
        if (title.rfind(want + ".", 0) != 0) continue;
        say(bold(title));
        for (const std::string& l : sections[i].second) say(l);
        const bool last = i == exercises.back();
        say(dim(last ? "That was the last exercise." : "/learn " + std::to_string(std::stoi(want) + 1) + " is next."));
        return 0;
    }
    return refuse("no exercise " + want + "; /learn lists them");
}

int cmd_strategies() {
    say(bold("Built in") + ": the reference market, and two strategies the evaluator uses on itself");
    for (const StrategyNote& s : kStrategyNotes) {
        std::string name = s.name;
        name.resize(11, ' ');
        say("  " + name + s.note);
    }
    say(bold("Installed by you") + ", in arena/strategies/user/; only the lab programs can see them");
    const std::vector<std::string> mine = installed_strategies();
    if (mine.empty()) say("  none yet: /add path/to/name.hpp installs one; examples/strategies/ has one to copy");
    for (const std::string& n : mine) {
        std::string name = n;
        name.resize(11, ' ');
        say("  " + name + (built_into_lab(n) ? "arena/strategies/user/" + n + ".hpp"
                                             : "installed, but the lab is not built with it yet: /add it again"));
    }
    say(dim(std::string("The market every candidate joins: ") + kDefaultLineup));
    return 0;
}

int cmd_runs() {
    const std::vector<RunInfo> runs = list_runs();
    if (runs.empty()) {
        say("No runs yet. /compare NAME smoke makes one in runs/.");
        return 0;
    }
    std::size_t width = 3;
    for (const RunInfo& r : runs) width = std::max(width, r.name.size());
    auto pad = [](std::string s, std::size_t n) {
        s.resize(std::max(n, s.size()), ' ');
        return s;
    };
    say(dim(pad("run", width) + "  " + pad("status", 10) + "  " + pad("suite", 8) + "  " + pad("block", 8) +
            "  candidates"));
    for (const RunInfo& r : runs)
        say(pad(r.name, width) + "  " + pad(r.complete ? "complete" : "incomplete", 10) + "  " + pad(r.suite, 8) +
            "  " + pad(r.block, 8) + "  " + r.candidates);
    say(dim("Newest first. /report NAME shows one; /reproduce NAME re-runs it."));
    return 0;
}

int cmd_report(const std::vector<std::string>& args) {
    const std::string run = resolve_run(args.empty() ? "" : args[0]);
    if (run.empty()) return refuse("there is no run yet; /compare makes one");
    std::error_code ec;
    if (!fs::is_directory(run, ec)) return refuse("no run " + run + "; /runs lists them");
    if (fs::exists(run + "/families.txt")) {
        std::fputs(read_file(run + "/families.txt").c_str(), stdout);
        say(dim("Each family has a full report of its own: /report " + run + "/standard, /wide, /busy"));
        return 0;
    }
    if (!fs::exists(run + "/report.txt")) return refuse(run + " has no report: the run did not finish");
    std::fputs(read_file(run + "/report.txt").c_str(), stdout);
    if (!run_complete(run)) say(paint("31", run + " is incomplete: outcome.cfg does not say status=complete"));
    return 0;
}

// ---- running a command -----------------------------------------------------

int execute(const std::string& line, bool& quit) {
    std::vector<std::string> words;
    std::string error;
    if (!split_words(line, words, error)) return refuse(error);
    if (words.empty() || words[0][0] == '#') return 0;
    if (const char* home = std::getenv("HOME"); home != nullptr) // no shell, so ~ is expanded here
        for (std::string& w : words)
            if (w == "~" || w.rfind("~/", 0) == 0) w = home + w.substr(1);
    g_env.latest_run = latest_run();
    if ((words[0] == "/report" || words[0] == "/reproduce") && words.size() == 2) words[1] = resolve_run(words[1]);

    const Plan plan = plan_for(words, g_env);
    if (!plan.error.empty()) return refuse(plan.error);
    const std::string cmd = find_command(words[0])->name;
    const std::vector<std::string> args(words.begin() + 1, words.end());

    if (plan.internal) {
        if (cmd == "/help") return cmd_help(args);
        if (cmd == "/learn") return cmd_learn(args);
        if (cmd == "/strategies") return cmd_strategies();
        if (cmd == "/runs") return cmd_runs();
        if (cmd == "/report") return cmd_report(args);
        quit = true;
        return 0;
    }

    std::error_code ec;
    if (!plan.run.empty() && !fs::exists(plan.run[0], ec))
        say(dim("Building " + fs::path(plan.run[0]).filename().string() + " for the first time; this takes a minute."));
    if (!plan.build.empty()) {
        const int built = run_child(plan.build);
        if (built != 0) {
            if (built < 0) return refuse(plan.build[0] + " could not be started; is it installed?");
            refuse(exit_meaning(Program::Make, built));
            return built;
        }
    }
    if (plan.run.empty()) { // /add: the build was the whole command
        const std::string name = fs::path(args[0]).stem().string();
        say(dim("Next: /compare " + name + " smoke, then /compare " + name + " for the full 64 seeds."));
        return 0;
    }
    const int status = run_child(plan.run);
    if (status == 130) {
        say("\n" + dim("stopped"));
        return status;
    }
    const std::string meaning = exit_meaning(plan.program, status);
    if (!meaning.empty()) refuse(meaning);
    if (status == 0 && plan.program == Program::Suite) {
        g_env.latest_run = latest_run();
        say(dim("Saved in " + g_env.latest_run + ". /report shows it again; /reproduce re-runs it and compares."));
    }
    return status < 0 ? 127 : status;
}

// ---- tab completion ----------------------------------------------------------

std::vector<std::string> complete_path(const std::string& word) {
    std::vector<std::string> out;
    const std::size_t slash = word.rfind('/');
    const std::string dir = slash == std::string::npos ? "" : word.substr(0, slash + 1);
    const std::string prefix = word.substr(dir.size());
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(dir.empty() ? "." : dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind(prefix, 0) != 0 || (name[0] == '.' && prefix.empty())) continue;
        out.push_back(dir + name + (e.is_directory(ec) ? "/" : ""));
    }
    return out;
}

std::vector<std::string> keep_prefixed(const std::vector<std::string>& all, const std::string& prefix) {
    std::vector<std::string> out;
    for (const std::string& s : all)
        if (s.rfind(prefix, 0) == 0) out.push_back(s);
    return out;
}

// The names of the files in `dir` with extension `ext`, sorted: tapes or markets.
std::vector<std::string> names_in(const char* dir, const char* ext) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ext) names.push_back(e.path().stem().string());
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> complete(const std::string& line, std::size_t& start) {
    const std::size_t space = line.find_last_of(" \t");
    start = space == std::string::npos ? 0 : space + 1;
    const std::string word = line.substr(start);
    std::vector<std::string> first_words;
    std::string error;
    if (!split_words(line.substr(0, start), first_words, error)) return {};

    std::vector<std::string> commands;
    for (const CommandInfo& c : kCommands) commands.push_back(c.name);
    if (first_words.empty()) return keep_prefixed(commands, word);

    const std::string& cmd = first_words[0];
    if (cmd == "/help") {
        for (std::string& c : commands) c.erase(0, 1);
        return keep_prefixed(commands, word);
    }
    if (cmd == "/add") return complete_path(word);
    if (cmd == "/report" || cmd == "/reproduce") {
        if (word.find('/') != std::string::npos) return complete_path(word);
        std::vector<std::string> names;
        for (const RunInfo& r : list_runs()) names.push_back(r.name);
        return keep_prefixed(names, word);
    }
    if (cmd == "/watch" && word.rfind("script=", 0) == 0) {
        start += 7;
        return complete_path(word.substr(7));
    }
    if ((cmd == "/compare" || cmd == "/watch" || cmd == "/sweep") && word.rfind("market=", 0) == 0) {
        start += 7;
        return keep_prefixed(names_in("markets", ".cfg"), word.substr(7));
    }
    if (cmd == "/calibrate") {
        std::vector<std::string> options = names_in("data", ".tape");
        options.insert(options.end(), {"--known=", "--control", "--robustness", "holdback="});
        return keep_prefixed(options, word);
    }
    if (cmd == "/facts") return keep_prefixed(names_in("data", ".tape"), word);
    if (cmd == "/replay") {
        // The tape, then a strategy, then the settings.
        std::size_t names = 0;
        for (std::size_t i = 1; i < first_words.size(); ++i) names += first_words[i].find('=') == std::string::npos;
        if (names == 0) return keep_prefixed(names_in("data", ".tape"), word);
        std::vector<std::string> options = {"reaction=", "from=", "for="};
        if (names == 1) {
            const std::vector<std::string> strategies = strategy_names();
            options.insert(options.begin(), strategies.begin(), strategies.end());
        }
        return keep_prefixed(options, word);
    }
    if (cmd == "/sweep") return keep_prefixed({"market="}, word);
    if (cmd == "/compare" || cmd == "/watch") {
        const std::size_t comma = word.rfind(',');
        if (comma != std::string::npos) {
            start += comma + 1;
            return keep_prefixed(strategy_names(), word.substr(comma + 1));
        }
        std::vector<std::string> options = strategy_names();
        if (cmd == "/compare") options.insert(options.end(), {"smoke", "holdout", "wide", "busy", "all", "market="});
        else options.insert(options.end(), {"seed=", "still=", "script=", "lineup=", "market="});
        return keep_prefixed(options, word);
    }
    if (cmd == "/data") {
        if (first_words.size() == 1) return keep_prefixed({"add", "show", "remove"}, word);
        if (first_words[1] == "add") {
            if (first_words.size() == 2) return complete_path(word); // the file, then its settings
            return keep_prefixed({"format=", "name=", "columns=", "time=", "tick=", "symbol=", "instrument="}, word);
        }
        if (first_words.size() == 2 && (first_words[1] == "show" || first_words[1] == "remove"))
            return keep_prefixed(names_in("data", ".tape"), word);
        return {};
    }
    if (cmd == "/controls") return keep_prefixed({"smoke"}, word);
    if (cmd == "/learn") {
        std::vector<std::string> numbers;
        for (int i = 1; i <= 9; ++i) numbers.push_back(std::to_string(i));
        return keep_prefixed(numbers, word);
    }
    return {};
}

// ---- start-up ----------------------------------------------------------------

bool is_terminal(FILE* f) {
#ifdef _WIN32
    return _isatty(_fileno(f)) != 0;
#else
    return isatty(fileno(f)) != 0;
#endif
}

void find_programs(const char* argv0) {
    std::string self = argv0;
    std::replace(self.begin(), self.end(), '\\', '/');
    const std::size_t slash = self.rfind('/');
    if (slash != std::string::npos) {
        g_env.rel = self.substr(0, slash);
        if (g_env.rel.rfind("./", 0) == 0) g_env.rel.erase(0, 2);
        const std::string tail = "/release";
        if (g_env.rel.size() > tail.size() && g_env.rel.compare(g_env.rel.size() - tail.size(), tail.size(), tail) == 0)
            g_env.build = g_env.rel.substr(0, g_env.rel.size() - tail.size());
    }
#ifdef _WIN32
    g_env.exe = ".exe";
    g_env.make = "mingw32-make";
#endif
}

void banner() {
    std::string build = kBuildCommit;
    if (kBuildDirty) build += " with uncommitted changes";
    say(bold("exchange-lab") + dim(" · a simulated market on a real matching engine, re-run exactly · build " + build));
    say(dim(std::to_string(std::size(kStrategyNotes)) + " built-in strategies, " +
            std::to_string(installed_strategies().size()) + " installed. ") +
        "/help lists the commands; /learn starts the exercises.");
}

int usage() {
    std::fputs("usage: exchange-lab                 the prompt\n"
               "       exchange-lab -c 'COMMAND'    run one command and exit with its status\n"
               "       exchange-lab < FILE          run one command per line\n"
               "Run it from the repository root. Inside, /help lists the commands.\n",
               stderr);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    find_programs(argv[0]);
    std::string one;
    if (argc == 3 && std::strcmp(argv[1], "-c") == 0) one = argv[2];
    else if (argc != 1) return usage();

    if (!fs::exists("Makefile") || !fs::exists("arena/strategies/registry.hpp")) {
        std::fprintf(stderr, "exchange-lab: run it from the repository root, where the Makefile is "
                             "(for example: build/release/exchange-lab)\n");
        return 2;
    }

#ifdef _WIN32
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    const bool vt = GetConsoleMode(out, &mode) &&
                    SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
#else
    const bool vt = true;
#endif
    const char* term = std::getenv("TERM");
    g_colour = vt && is_terminal(stdout) && std::getenv("NO_COLOR") == nullptr &&
               !(term != nullptr && std::strcmp(term, "dumb") == 0);

    bool quit = false;
    if (!one.empty()) return execute(one, quit);

    if (!is_terminal(stdin)) {
        std::string line;
        int status = 0;
        while (!quit && std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            say("> " + line);
            status = execute(line, quit);
        }
        return status;
    }

    banner();
    LineEditor editor(complete);
    std::string line;
    const std::string prompt = paint("1;36", "> ");
    while (!quit && editor.read(prompt, line)) execute(line, quit);
    return 0;
}
