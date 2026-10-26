// marketdata/replay.cpp — play an order-by-order tape back through the engine,
// with one strategy trading in it.
//
//   replay NAME                    the tape alone: the control, which must hold
//   replay NAME strategy=S         S trades in the replayed market
//
// Compiled with the lab programs, so it can run an installed strategy, and
// never one of the programs that pin a hash. The design, and why history
// cannot react, is in marketdata/replay.hpp and docs/DATA.md.
//
// Exit status: 0 done (with no strategy: the control held); 1 refused, or the
// control did not hold; 2 the command line was wrong.
#include "marketdata/replay.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace px;
using namespace px::data;

namespace {

constexpr const char* kHelp =
    "replay: play an order-by-order tape back through the engine (docs/DATA.md)\n"
    "\n"
    "  replay NAME                   data/NAME.tape alone: the replayed book must equal the tape's\n"
    "                                after every event (the control)\n"
    "  replay NAME strategy=S        S trades in the replayed market: built-in or installed\n"
    "\n"
    "Settings, written key=value or --key=value:\n"
    "  strategy=   the strategy to run\n"
    "  reaction=   its reaction delay: 10us (the suite's) unless set; ns, us, ms or s\n"
    "  from=       how far into the tape it starts; the tape before builds the book\n"
    "  for=        how long it trades; to the tape's end unless set\n"
    "  data=DIR    where the tapes are (default data)\n"
    "\n"
    "History cannot react: every order on the tape arrives when it did, whatever the strategy\n"
    "does. Only tapes with every order (L3) can be replayed.\n";

int usage(const std::string& msg) {
    std::fprintf(stderr, "replay: %s\n(replay --help lists the settings)\n", msg.c_str());
    return 2;
}

int refuse(const std::string& msg) {
    std::fprintf(stderr, "replay: %s\n", msg.c_str());
    return 1;
}

int decimals_of(std::int64_t scale) {
    int d = 0;
    while (scale > 1) {
        scale /= 10;
        ++d;
    }
    return d;
}

// "10us", "250ms", "1h": a span of time, in nanoseconds.
bool parse_span(std::string s, std::int64_t& ns) {
    std::int64_t mul = 1;
    for (const auto& [suffix, m] : std::vector<std::pair<std::string, std::int64_t>>{
             {"ns", 1}, {"us", 1'000}, {"ms", 1'000'000}, {"s", 1'000'000'000}, {"m", 60'000'000'000},
             {"h", 3'600'000'000'000}}) {
        if (s.size() > suffix.size() && s.ends_with(suffix)) {
            mul = m;
            s.resize(s.size() - suffix.size());
            break;
        }
    }
    std::int64_t v = 0;
    if (!parse_int(s, v) || v < 0 || v > INT64_MAX / mul) return false;
    ns = v * mul;
    return true;
}

std::string pad(std::string s, std::size_t n) {
    if (s.size() < n) s.resize(n, ' ');
    return s;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
        std::fputs(kHelp, stdout);
        return 0;
    }
    std::string data_dir = "data", strategy;
    std::vector<std::string> names;
    ReplayConfig cfg;
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
        else if (k == "strategy") strategy = v;
        else if (k == "reaction" || k == "from" || k == "for") {
            std::int64_t ns = 0;
            if (!parse_span(v, ns)) return usage(k + "=" + v + " is not a span of time (10us, 250ms, 30s, 1h)");
            (k == "reaction" ? cfg.reaction_ns : k == "from" ? cfg.from_ns : cfg.for_ns) = ns;
        } else {
            return usage("unknown setting " + k + "=");
        }
    }
    if (data_dir.empty()) return usage("data= needs a directory");
    while (data_dir.size() > 1 && data_dir.back() == '/') data_dir.pop_back();
    if (names.size() != 1) return usage(names.empty() ? "say which tape: replay NAME" : "say one tape");
    const std::string& name = names[0];
    if (!valid_name(name)) return usage("'" + name + "' is not a tape name; /data lists them");

    const std::string path = data_dir + "/" + name + ".tape";
    std::string text, why;
    if (!read_file(path, text)) return refuse("no tape " + name + " in " + data_dir + "/; /data lists the tapes");
    Tape tape;
    if (!parse_tape(text, tape, why)) return refuse(path + ": " + why);
    if (tape.h.depth != Depth::L3)
        return refuse(name + " is " + depth_words(tape.h.depth) +
                      ": replay plays back every order, so it needs an order-by-order (L3) tape. Calibrate a "
                      "market from this one instead: /calibrate " + name);
    const std::int64_t last = tape.ev.empty() ? 0 : tape.ev.back().t;
    if (cfg.from_ns > last) return usage("from= is past the tape's end, " + duration_text(last) + " in");

    std::unique_ptr<arena::StrategyHandle> handle;
    if (!strategy.empty()) {
        arena::RosterConfig rc;
        rc.tick = 1;
        for (const TapeEvent& e : tape.ev)
            if (e.price > 0) {
                rc.anchor = e.price;
                break;
            }
        arena::Roster r;
        if (!arena::add_named_strategy(strategy, rc, r)) {
            std::string known = arena::kStrategyNames;
#if defined(PX_ARENA_USER_STRATEGIES)
            if (*arena::kUserStrategyNames != '\0') known += std::string(", installed: ") + arena::kUserStrategyNames;
#endif
            return usage("no strategy '" + strategy + "' (built in: " + known + ")");
        }
        if (r.informed[0])
            return refuse("informed trades on the fundamental, and a replay has none: the tape is the only market");
        handle = std::move(r.strategies[0]);
    }
    cfg.check_book = handle == nullptr;

    const TapeHeader& h = tape.h;
    auto at = [&h](std::int64_t t) { return time_text(h.origin_ns + t, h.origin_kind); };
    auto qty = [&h](std::int64_t q) { return decimal_text(q, decimals_of(h.qty_scale)); };
    auto price = [&h](std::int64_t ticks) { return decimal_text(ticks * h.tick, decimals_of(h.price_scale)); };

    std::printf("replay %s%s: %s, %s, %zu events\n", name.c_str(),
                handle ? (" with " + strategy).c_str() : ", no strategy (the control)", path.c_str(),
                depth_words(h.depth), tape.ev.size());
    if (handle)
        std::printf("History cannot react. Every order on the tape arrives when it did, whatever the strategy\n"
                    "does: nobody cancels because it bid, and nobody follows its price. What it earns here is\n"
                    "what the recorded flow would have given a participant it never saw, so read it as the\n"
                    "most the flow could have given, not as what would have happened. Times are the tape's.\n");
    else
        std::printf("With no strategy in it, the replayed book must be the tape's own after every event. If it\n"
                    "is not, nothing a strategy earns in a replay of this tape means anything.\n");

    Replay rp;
    ReplayStats st;
    if (!rp.run(tape, std::move(handle), cfg, st, why)) return refuse(why);

    const std::int64_t span = st.end_t - st.start_t;
    if (!strategy.empty()) {
        std::printf("\n  window        %s to %s (%s)%s\n", at(st.start_t).c_str(), at(st.end_t).c_str(),
                    duration_text(span).c_str(), st.start_t > 0 ? "; the tape before it built the book" : "");
        std::printf("  reaction      %s%s\n", duration_text(cfg.reaction_ns).c_str(),
                    cfg.reaction_ns == 10'000 ? " (the suite's; reaction= sets it)" : "");
    }
    std::printf("\nWhat the tape did\n");
    std::printf("  events        %s: %s adds, %s cancels (%s partial, the queue put back in order), %s executions\n",
                grouped(static_cast<std::int64_t>(st.events)).c_str(), grouped(static_cast<std::int64_t>(st.adds)).c_str(),
                grouped(static_cast<std::int64_t>(st.cancels)).c_str(),
                grouped(static_cast<std::int64_t>(st.partial_cancels)).c_str(),
                grouped(static_cast<std::int64_t>(st.executions)).c_str());
    if (st.levels > 0)
        std::printf("  level sizes   %s (size the tape knew only by level: one order a level)\n",
                    grouped(static_cast<std::int64_t>(st.levels)).c_str());
    if (st.hidden_prints > 0)
        std::printf("  not replayed  %s prints that name no resting order (hidden executions): they change no book\n",
                    grouped(static_cast<std::int64_t>(st.hidden_prints)).c_str());
    if (st.resets > 0)
        std::printf("  resets        %s: history's book cleared, as the tape's was\n",
                    grouped(static_cast<std::int64_t>(st.resets)).c_str());
    if (st.unknown > 0)
        std::printf("  unknown       %s cancels and executions of orders resting before the tape began\n",
                    grouped(static_cast<std::int64_t>(st.unknown)).c_str());
    if (st.book_full > 0)
        std::printf("  book full     %s of history's orders found no room (the engine holds 2,048 levels a side)\n",
                    grouped(static_cast<std::int64_t>(st.book_full)).c_str());

    if (strategy.empty()) {
        std::printf("\nThe control\n");
        const bool held = st.mismatches == 0 && st.exec_out_of_queue == 0;
        std::printf("  book          %s\n",
                    st.mismatches == 0
                        ? ("the replayed book equals the tape's after every one of " +
                           grouped(static_cast<std::int64_t>(st.checked)) + " events")
                              .c_str()
                        : ("differs from the tape's after " + grouped(static_cast<std::int64_t>(st.mismatches)) +
                           " of " + grouped(static_cast<std::int64_t>(st.checked)) + " events; first: " +
                           st.first_mismatch)
                              .c_str());
        std::printf("  queue         %s\n",
                    st.exec_out_of_queue == 0
                        ? "every execution found its order first in the queue"
                        : (grouped(static_cast<std::int64_t>(st.exec_out_of_queue)) +
                           " executions found another order first: the source does not fill in time order")
                              .c_str());
        std::printf("  hash          %016llx (the engine's event stream)\n", static_cast<unsigned long long>(st.hash));
        std::printf("\n%s\n", held ? "The control holds: replay plays this tape back as it was."
                                   : "The control does not hold: a replay of this tape is not the tape.");
        return held ? 0 : 1;
    }

    std::printf("\nWhere history and the strategy collided\n");
    auto row = [](const char* what, const std::string& v) { std::printf("  %s%s\n", pad(what, 48).c_str(), v.c_str()); };
    row("history's flow that traded with the strategy", qty(static_cast<std::int64_t>(st.flow_with_strategy)));
    row("taken by the strategy from history's book", qty(static_cast<std::int64_t>(st.taken_from_book)));
    row("history's adds that crossed the strategy", qty(static_cast<std::int64_t>(st.adds_crossed)));
    row("cancels whose order the strategy had taken", grouped(static_cast<std::int64_t>(st.cancels_gone)));
    row("execution size that found its price taken", qty(static_cast<std::int64_t>(st.exec_unfilled)));
    row("partial cancels that moved the strategy up", grouped(static_cast<std::int64_t>(st.moved_ahead)));

    std::printf("\nThe strategy\n");
    std::printf("  orders        %s sent, %s fills, %s rejects, volume %s, %s looks\n",
                grouped(static_cast<std::int64_t>(st.orders)).c_str(), grouped(static_cast<std::int64_t>(st.fills)).c_str(),
                grouped(static_cast<std::int64_t>(st.rejects)).c_str(),
                qty(static_cast<std::int64_t>(st.volume)).c_str(), grouped(static_cast<std::int64_t>(st.wakes)).c_str());
    std::printf("  position      %s%s at the close, marked at %s (%s)\n", st.position > 0 ? "+" : "",
                qty(st.position).c_str(), price(st.mark).c_str(),
                st.mark == 0 ? "nothing to mark at" : "the last mid, or the last trade if a side was empty");
    const int vd = decimals_of(h.price_scale) + decimals_of(h.qty_scale);
    std::printf("  value         %s (its cash and its position at that mark, in the tape's currency)\n",
                decimal_text(st.value * h.tick, vd).c_str());
    std::printf("  hash          %016llx (the engine's event stream: the same tape and strategy give it again)\n",
                static_cast<unsigned long long>(st.hash));
    return 0;
}
