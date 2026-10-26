// tests/test_app.cpp — what each of the app's commands turns into, checked
// without starting anything.
//
// The app is a front door: a command is either answered inside it or turned
// into a make invocation and a program to run. commands.hpp holds that
// translation as pure functions, so it is tested here the way the registry is
// held to kStrategyNames in test_arena: the table /help prints and the handlers
// cannot drift apart, and every built-in strategy has a line in /strategies.
// make test-app drives the real binary.
#include "app/commands.hpp"
#include "arena/strategies/registry.hpp"
#include "test_harness.hpp"

#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace px::app;

namespace {

std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> out;
    std::string error;
    PX_CHECK(split_words(line, out, error));
    return out;
}

Plan plan(const std::string& line, const Env& env = Env{}) { return plan_for(words(line), env); }

bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const std::string& x : v)
        if (x == s) return true;
    return false;
}

void test_split_words() {
    std::vector<std::string> out;
    std::string error;
    PX_CHECK(split_words("  /compare   a,b  smoke ", out, error));
    PX_CHECK((out == std::vector<std::string>{"/compare", "a,b", "smoke"}));
    PX_CHECK(split_words("/add \"my dir/x.hpp\" 'it''s'", out, error));
    PX_CHECK((out == std::vector<std::string>{"/add", "my dir/x.hpp", "its"}));
    PX_CHECK(split_words("/add \"\"", out, error));
    PX_CHECK((out == std::vector<std::string>{"/add", ""})); // an empty quoted word is still a word
    PX_CHECK(split_words("", out, error) && out.empty());
    PX_CHECK(!split_words("/add \"unclosed", out, error)); // refused, never guessed
    PX_CHECK(error.find("quote") != std::string::npos);
}

// Every command /help lists has a handler, and every handler is listed: no
// listed command may fall through to "no handler".
void test_every_command_is_handled() {
    const char* example_args[] = {"", "x.hpp", "maker", "smoke"};
    Env env;
    env.latest_run = "runs/maker-smoke";
    for (const CommandInfo& c : kCommands) {
        bool handled = false;
        for (const char* a : example_args) {
            const Plan p = plan(std::string(c.name) + " " + a, env);
            PX_CHECK(p.error.find("no handler") == std::string::npos);
            handled = handled || p.error.empty();
        }
        PX_CHECK(handled); // some argument list is accepted
        PX_CHECK(*c.summary != '\0');
    }
    PX_CHECK(find_command("/exit") == find_command("/quit"));
    PX_CHECK(find_command("/nosuch") == nullptr);
    PX_CHECK(plan("/nosuch").error.find("no command /nosuch") != std::string::npos);
    PX_CHECK(plan("compare maker").error.find("start with /") != std::string::npos);
    PX_CHECK(plan("C:/msys64/compare maker smoke").error.find("MSYS2_ARG_CONV_EXCL") != std::string::npos);
    PX_CHECK(plan("c:/msys64/help").error.find("MSYS2_ARG_CONV_EXCL") != std::string::npos);
    PX_CHECK(plan("C:").error.find("start with /") != std::string::npos);
    PX_CHECK(plan("").internal && plan("").error.empty());
}

void test_internal_commands() {
    for (const char* line : {"/help", "/help compare", "/learn", "/learn 3", "/strategies", "/runs", "/report",
                             "/report maker-smoke", "/quit", "/exit"}) {
        const Plan p = plan(line);
        PX_CHECK(p.internal && p.error.empty() && p.build.empty() && p.run.empty());
    }
    PX_CHECK(!plan("/runs extra").error.empty());
    PX_CHECK(!plan("/strategies x").error.empty());
    PX_CHECK(!plan("/report a b").error.empty());
}

void test_argument_vectors() {
    Env env;
    const std::vector<std::string> make_suite = {"make", "-s", "--no-print-directory", "BUILD=build",
                                                 "ANNOUNCE=1", "build/release/suite_lab"};

    Plan p = plan("/compare imbalance,maker smoke wide", env);
    PX_CHECK(p.error.empty() && p.program == Program::Suite && p.build == make_suite);
    PX_CHECK((p.run == std::vector<std::string>{"build/release/suite_lab", "--candidates=imbalance,maker",
                                                "--block=smoke", "--suite=wide"}));
    p = plan("/compare imbalance maker");
    PX_CHECK((p.run == std::vector<std::string>{"build/release/suite_lab", "--candidates=imbalance,maker"}));
    PX_CHECK(!plan("/compare").error.empty());
    PX_CHECK(!plan("/compare smoke").error.empty());
    PX_CHECK(!plan("/compare maker wide busy").error.empty());
    p = plan("/compare imbalance holdout");
    PX_CHECK((p.run == std::vector<std::string>{"build/release/suite_lab", "--candidates=imbalance", "--block=holdout"}));
    PX_CHECK(!plan("/compare maker smoke holdout").error.empty());

    p = plan("/add examples/strategies/imbalance.hpp");
    PX_CHECK(p.program == Program::Make && p.run.empty());
    PX_CHECK((p.build == std::vector<std::string>{"make", "-s", "--no-print-directory", "BUILD=build", "ANNOUNCE=1", "strategy",
                                                  "STRATEGY=examples/strategies/imbalance.hpp"}));
    PX_CHECK(!plan("/add").error.empty() && !plan("/add a.hpp b.hpp").error.empty());

    PX_CHECK(!plan("/reproduce").error.empty()); // no run yet
    env.latest_run = "runs/newest";
    PX_CHECK((plan("/reproduce", env).run ==
              std::vector<std::string>{"build/release/suite_lab", "--reproduce=runs/newest"}));
    PX_CHECK(has(plan("/reproduce imbalance-smoke/", env).run, "--reproduce=runs/imbalance-smoke"));
    PX_CHECK(has(plan("/reproduce build/x", env).run, "--reproduce=build/x"));

    p = plan("/watch");
    PX_CHECK((p.run == std::vector<std::string>{"build/release/arena_lab"}) && p.program == Program::Arena);
    p = plan("/watch imbalance seed=43 still=60000000 script=tools/arena_script_demo.txt");
    PX_CHECK((p.run == std::vector<std::string>{"build/release/arena_lab",
                                                std::string("--strategies=") + kDefaultLineup + ",imbalance",
                                                "--seed=43", "--still=60000000",
                                                "--script=tools/arena_script_demo.txt"}));
    PX_CHECK(has(plan("/watch lineup=maker,noise").run, "--strategies=maker,noise"));
    PX_CHECK(!plan("/watch maker lineup=maker").error.empty());
    PX_CHECK(!plan("/watch speed=2").error.empty());

    p = plan("/sweep --null --seeds=8");
    PX_CHECK((p.run == std::vector<std::string>{"build/release/sweep_lab", "--null", "--seeds=8"}));
    PX_CHECK((plan("/controls smoke").run == std::vector<std::string>{"build/release/controls", "--block=smoke"}));
    PX_CHECK(!plan("/controls standard").error.empty());
    PX_CHECK((plan("/book").run == std::vector<std::string>{"build/release/repl"}));

    p = plan("/data");
    PX_CHECK(p.program == Program::Ingest && p.build.back() == "build/release/ingest" &&
             (p.run == std::vector<std::string>{"build/release/ingest", "list"}));
    p = plan("/data add \"my files/x.csv\" name=x columns=time,price");
    PX_CHECK((p.run == std::vector<std::string>{"build/release/ingest", "add", "my files/x.csv", "name=x",
                                                "columns=time,price"}));
    PX_CHECK((plan("/data show demo").run == std::vector<std::string>{"build/release/ingest", "show", "demo"}));
    PX_CHECK((plan("/data remove demo").run == std::vector<std::string>{"build/release/ingest", "remove", "demo"}));
    PX_CHECK(!plan("/data add").error.empty() && !plan("/data add a.csv b.csv").error.empty());
    PX_CHECK(!plan("/data show").error.empty() && !plan("/data show a name=b").error.empty());
    PX_CHECK(plan("/data fetch x").error.find("add, show or remove") != std::string::npos);
    PX_CHECK(!plan("/book x").error.empty());

    // A calibrated market: /calibrate makes one, market= uses it on the three lab programs.
    p = plan("/calibrate demo");
    PX_CHECK(p.program == Program::Calibrate && p.build.back() == "build/release/calibrate" &&
             (p.run == std::vector<std::string>{"build/release/calibrate", "demo"}));
    PX_CHECK((plan("/calibrate --known=known").run ==
              std::vector<std::string>{"build/release/calibrate", "--known=known"}));
    PX_CHECK((plan("/calibrate --control").run == std::vector<std::string>{"build/release/calibrate", "--control"}));
    PX_CHECK(!plan("/calibrate").error.empty() && !plan("/calibrate a b").error.empty());
    PX_CHECK((plan("/calibrate demo holdback=30").run ==
              std::vector<std::string>{"build/release/calibrate", "demo", "holdback=30"}));
    p = plan("/facts demo");
    PX_CHECK(p.program == Program::Calibrate && p.build.back() == "build/release/calibrate" &&
             (p.run == std::vector<std::string>{"build/release/calibrate", "--facts=demo"}));
    PX_CHECK(!plan("/facts").error.empty() && !plan("/facts a b").error.empty() && !plan("/facts x=1").error.empty());
    PX_CHECK((plan("/compare imbalance smoke market=demo").run ==
              std::vector<std::string>{"build/release/suite_lab", "--candidates=imbalance", "--block=smoke",
                                       "--market=demo"}));
    PX_CHECK(plan("/compare imbalance wide market=demo").error.find("not both") != std::string::npos);
    PX_CHECK(!plan("/compare imbalance market=").error.empty());
    PX_CHECK(plan("/compare imbalance seeds=8").error.find("unknown setting") != std::string::npos);
    PX_CHECK(has(plan("/watch imbalance market=demo").run, "--market=demo"));
    PX_CHECK((plan("/sweep market=demo --seeds=8").run ==
              std::vector<std::string>{"build/release/sweep_lab", "--market=demo", "--seeds=8"}));

    // An order-by-order tape played back: the control alone, or one strategy in it.
    p = plan("/replay demo");
    PX_CHECK(p.program == Program::Replay && p.build.back() == "build/release/replay" &&
             (p.run == std::vector<std::string>{"build/release/replay", "demo"}));
    PX_CHECK((plan("/replay demo maker reaction=1ms from=2s for=3s").run ==
              std::vector<std::string>{"build/release/replay", "demo", "strategy=maker", "reaction=1ms", "from=2s",
                                       "for=3s"}));
    PX_CHECK(!plan("/replay").error.empty() && !plan("/replay a b c").error.empty());
    PX_CHECK(plan("/replay demo data=x").error.find("unknown setting") != std::string::npos);

    // A Windows build names its programs and its make differently; nothing else changes.
    Env win;
    win.exe = ".exe";
    win.make = "mingw32-make";
    win.build = "build-x";
    win.rel = "build-x/release";
    p = plan("/compare maker", win);
    PX_CHECK(p.build[0] == "mingw32-make" && p.build[3] == "BUILD=build-x" &&
             p.build[5] == "build-x/release/suite_lab.exe" && p.run[0] == "build-x/release/suite_lab.exe");
}

void test_strategy_notes_match_the_registry() {
    std::istringstream names(px::arena::kStrategyNames);
    std::string name;
    std::size_t count = 0;
    while (names >> name) {
        ++count;
        bool found = false;
        for (const StrategyNote& s : kStrategyNotes) found = found || name == s.name;
        PX_CHECK(found);
    }
    PX_CHECK(count == std::size(kStrategyNotes));
}

void test_exit_meanings() {
    PX_CHECK(exit_meaning(Program::Suite, 0).empty());
    PX_CHECK(exit_meaning(Program::Suite, 3).find("not admissible") != std::string::npos);
    PX_CHECK(exit_meaning(Program::Reproduce, 1).find("NOT REPRODUCED") != std::string::npos);
    PX_CHECK(exit_meaning(Program::Controls, 1).find("control") != std::string::npos);
    PX_CHECK(exit_meaning(Program::Calibrate, 1).find("nothing was written") != std::string::npos);
    PX_CHECK(exit_meaning(Program::Sweep, 9).find("9") != std::string::npos);
    PX_CHECK(exit_meaning(Program::Ingest, 1).find("refused") != std::string::npos);
    PX_CHECK(exit_meaning(Program::Replay, 1).find("control did not hold") != std::string::npos);
    PX_CHECK(exit_meaning(Program::Arena, -1).find("could not be started") != std::string::npos);
}

} // namespace

int main() {
    test_split_words();
    test_every_command_is_handled();
    test_internal_commands();
    test_argument_vectors();
    test_strategy_notes_match_the_registry();
    test_exit_meanings();
    PX_TEST_REPORT_AND_RETURN();
}
