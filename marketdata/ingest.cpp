// marketdata/ingest.cpp — bring a market data file into exchange-lab as a tape.
//
//   ingest add FILE [format=] [name=] [columns=] [time=] [tick=] [symbol=] [instrument=]
//   ingest list
//   ingest show NAME
//   ingest remove NAME
//
// The app's /data commands run this program (docs/APP.md); so does make data.
// It reads a file the user already has on disk and never fetches anything. It
// links none of the market: an importer cannot reach a session, a hash or a
// published number (docs/DATA.md).
//
// Exit status: 0 done, 1 the file or tape was refused (the message says why),
// 2 the command line was wrong.
#include "marketdata/importers.hpp"
#include "marketdata/stats.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace px::data;

namespace {

constexpr const char* kHelp =
    "ingest: bring a market data file into exchange-lab as a tape (docs/DATA.md)\n"
    "\n"
    "  ingest add FILE [format=csv|lobster|databento|tardis] [name=NAME] [columns=ROLES] [time=FORMAT]\n"
    "                  [tick=DECIMAL] [symbol=SYMBOL] [instrument=NAME]\n"
    "  ingest list               the tapes in data/\n"
    "  ingest show NAME          what a tape holds, and the statistics calibration matches\n"
    "  ingest remove NAME        delete data/NAME.tape\n"
    "\n"
    "Settings are written name=value (--name=value works too); dir=DIR keeps tapes\n"
    "somewhere other than data/.\n"
    "  format=     csv, lobster, databento (CSV or uncompressed DBN) or tardis; found from\n"
    "              the file when left out. A LOBSTER message file is read with its orderbook\n"
    "              file, and a Tardis book file with its trades file, when it sits beside it\n"
    "  name=       the tape's name; from the file name when left out\n"
    "  columns=    a CSV's columns in order, when its header does not say or it has none:\n"
    "              time, price, qty, side, buyer_maker, bid, bid_qty, ask, ask_qty, symbol, -\n"
    "  time=       how the time column is written: s, ms, us, ns, iso or clock; found when left out\n"
    "  tick=       the price step, as a decimal (0.01); the largest step every price is on when left out\n"
    "  symbol=     a file holding several instruments: the one to read\n"
    "  instrument= what to call the instrument when the file does not say\n"
    "\n"
    "The file is read from disk, never fetched. A tape stays on this machine: data/ is\n"
    "git-ignored, and a tape made from data you paid for must never be committed.\n";

int usage(const std::string& msg) {
    std::fprintf(stderr, "ingest: %s\n(ingest --help lists the commands)\n", msg.c_str());
    return 2;
}

int refuse(const std::string& msg) {
    std::fprintf(stderr, "ingest: %s\n", msg.c_str());
    return 1;
}

void say(const std::string& s) { std::printf("%s\n", s.c_str()); }

std::string pad(std::string s, std::size_t n) {
    if (s.size() < n) s.resize(n, ' ');
    return s;
}

// A share of a whole, as "12.5%".
std::string pct(std::int64_t part, std::int64_t whole) {
    if (whole <= 0) return "-";
    return decimal_text(part * 1000 / whole, 1) + "%";
}

int decimals_of(std::int64_t scale) {
    int d = 0;
    while (scale > 1) {
        scale /= 10;
        ++d;
    }
    return d;
}

std::string price_text(const TapeHeader& h, std::int64_t ticks) {
    return decimal_text(ticks * h.tick, decimals_of(h.price_scale));
}

std::string tape_path(const std::string& dir, const std::string& name) { return dir + "/" + name + ".tape"; }

// ---- show ---------------------------------------------------------------------

void show(const Tape& tape) {
    const TapeHeader& h = tape.h;
    const TapeStats st = tape_stats(tape);
    std::int64_t lo = 0, hi = 0;
    for (const TapeEvent& e : tape.ev) {
        if (e.kind == 'R' || e.price <= 0 || (e.kind == 'L' && e.qty == 0)) continue;
        lo = lo == 0 ? e.price : std::min(lo, e.price);
        hi = std::max(hi, e.price);
    }
    const std::int64_t span = tape.ev.empty() ? 0 : tape.ev.back().t;

    say(h.name + ": " + (h.instrument.empty() ? "" : h.instrument + ", ") + (h.date.empty() ? "" : h.date + ", ") +
        depth_words(h.depth));
    say("  source        " + h.source_file + ", read as " + h.format + " (cksum " + h.source_cksum + "; importer " +
        h.importer + ")");
    say("  time          " + time_text(h.origin_ns, h.origin_kind) + " to " +
        time_text(h.origin_ns + span, h.origin_kind) + ", " + duration_text(span));
    std::string parts;
    auto part = [&parts](std::uint64_t n, const char* what) {
        if (n == 0) return;
        parts += (parts.empty() ? "" : ", ") + grouped(static_cast<std::int64_t>(n)) + " " + what + (n == 1 ? "" : "s");
    };
    part(st.adds, "add");
    part(st.cancels, "cancel");
    part(st.trades, "trade");
    part(st.levels, "level update");
    part(st.resets, "reset");
    say("  events        " + grouped(static_cast<std::int64_t>(st.events)) + (parts.empty() ? "" : ": " + parts));
    if (h.skipped > 0) say("  skipped       " + h.skipped_why);
    say("  prices        " + price_text(h, lo) + " to " + price_text(h, hi) + ", tick " + price_text(h, 1));
    if (!h.notes.empty()) say("  notes         " + h.notes);

    say("");
    say("What calibration matches (ratios, not the clock: docs/DATA.md)");
    if (st.depth == Depth::Trades) {
        say("  spread        not in a trades-only tape: calibration keeps the reference market's");
        say("  touch depth   not in a trades-only tape");
    } else {
        say("  spread        " + (st.spread_ticks < 0 ? std::string("never measurable: the book never had both sides")
                                                       : std::to_string(st.spread_ticks) + " ticks") +
            " (time-weighted median)");
        say("  touch depth   " + (st.touch_depth < 0 ? std::string("-") : grouped(st.touch_depth)) +
            " (time-weighted median of the best bid's and best ask's size, averaged)");
    }
    say("  print size    " + (st.print_qty < 0 ? std::string("no trades") : grouped(st.print_qty)) +
        " (median; a print is one aggressor's trades at one moment)" +
        (st.size_depth_pml >= 0 ? ", " + decimal_text(st.size_depth_pml, 1) + "% of touch depth" : ""));
    if (st.prints_per_move_x100 >= 0)
        say("  per move      " + decimal_text(st.prints_per_move_x100, 2) + " prints for the " +
            (st.depth == Depth::Trades ? "price" : "mid") + " to move one tick (from " +
            grouped(static_cast<std::int64_t>(st.move_windows)) + " windows of " + std::to_string(kMoveWindow) +
            " prints)");
    else
        say("  per move      not measurable: " +
            std::string(st.move_windows == 0 ? "fewer than " + std::to_string(kMoveWindow + 1) + " prints"
                                             : "the price never moved"));
    if (!st.enough_prints())
        say("  (fewer than " + std::to_string(10 * kMoveWindow) +
            " prints: too few for these numbers to mean much)");

    say("");
    say("For reference, not matched");
    const std::uint64_t known = st.buy_prints + st.sell_prints;
    say("  prints        " + grouped(static_cast<std::int64_t>(st.prints)) +
        (known > 0 ? ", " + pct(static_cast<std::int64_t>(st.buy_prints), static_cast<std::int64_t>(known)) +
                         " bought by the aggressor where the side is known"
                   : ", aggressor side not known"));
    if (span > 0)
        say("  prints a sec  " + decimal_text(static_cast<std::int64_t>(st.prints) * 100 * kNsPerSec / span, 2) +
            " (real time: calibration does not match the clock)");
    if (st.depth != Depth::Trades)
        say("  the book      both sides " + pct(st.two_sided_ns, span) + ", one side " + pct(st.one_sided_ns, span) +
            ", empty " + pct(st.empty_ns, span) + ", crossed or locked " + pct(st.crossed_ns, span) + " of the time");

    say("");
    switch (h.depth) {
        case Depth::L3:
            say("Order by order: calibration can match every statistic above, and replay (a later");
            say("stage) can play it back.");
            break;
        case Depth::L2:
            say("Price levels: calibration can match every statistic above. Replay needs order-by-order");
            say("data, which this is not.");
            break;
        case Depth::Trades:
            say("Trades only: calibration can match print size against itself and prints per move;");
            say("spread and depth need quotes, so the reference market's are kept.");
            break;
    }
}

// ---- the commands ---------------------------------------------------------------

int cmd_add(const std::string& file, const ImportOptions& opts, const std::string& dir) {
    std::string bytes;
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return refuse("no file " + file);
    if (!read_file(file, bytes)) return refuse("cannot read " + file);
    std::string why;
    if (!readable(file, bytes, why)) return refuse(file + ": " + why);

    // A second file read with this one, when it sits beside it: LOBSTER's
    // orderbook file, the trades of a Tardis book file.
    std::string companion, companion_what;
    for (const Companion& c : companions_of(file, opts.format)) {
        if (!fs::is_regular_file(c.path, ec)) continue;
        if (!read_file(c.path, companion)) return refuse("cannot read " + c.path);
        if (!readable(c.path, companion, why)) return refuse(c.path + ": " + why);
        companion_what = c.what;
        break;
    }

    Tape tape;
    if (!import_file(file, bytes, companion, opts, tape, why)) return refuse(file + ": " + why);

    const std::string path = tape_path(dir, tape.h.name);
    bool replaced = false;
    if (fs::exists(path, ec)) {
        std::string old_text;
        Tape old;
        std::string old_why;
        if (read_file(path, old_text) && parse_tape(old_text, old, old_why, true) &&
            old.h.source_cksum != tape.h.source_cksum) {
            return refuse("a tape called " + tape.h.name + " already exists, made from another file (" +
                          old.h.source_file + ", cksum " + old.h.source_cksum +
                          "); give this one another name with name=, or remove that one first");
        }
        replaced = true; // the same file again: a newer importer may read it differently
    }
    fs::create_directories(dir, ec);
    const std::string tmp = path + ".tmp";
    if (!write_file(tmp, tape_text(tape))) return refuse("cannot write " + tmp);
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
    if (ec) return refuse("cannot write " + path + ": " + ec.message());

    say(std::string(replaced ? "replaced " : "added ") + tape.h.name + " -> " + path +
        (companion.empty() ? "" : " (with " + companion_what + " beside it)"));
    say("");
    show(tape);
    return 0;
}

bool load(const std::string& dir, const std::string& name, Tape& tape, std::string& why) {
    if (!valid_name(name)) {
        why = "'" + name + "' is not a tape name";
        return false;
    }
    std::string text;
    const std::string path = tape_path(dir, name);
    if (!read_file(path, text)) {
        why = "no tape " + name + " in " + dir + "/; the list command shows them";
        return false;
    }
    if (!parse_tape(text, tape, why)) {
        why = path + ": " + why;
        return false;
    }
    return true;
}

int cmd_list(const std::string& dir) {
    std::vector<std::string> paths;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".tape") paths.push_back(e.path().string());
    std::sort(paths.begin(), paths.end());
    if (paths.empty()) {
        say("no tapes yet. Add a file you have: /data add FILE (examples/data/ has small samples)");
        return 0;
    }
    say(pad("name", 24) + "  " + pad("depth", 6) + "  " + pad("events", 12) + "  " + pad("span", 9) + "  " +
        pad("instrument", 12) + "  " + pad("date", 10) + "  source");
    for (const std::string& p : paths) {
        // Only the header: a day of order-by-order data is a large file.
        std::string head;
        std::FILE* f = std::fopen(p.c_str(), "rb");
        if (f != nullptr) {
            char buf[8192];
            const std::size_t n = std::fread(buf, 1, sizeof buf, f);
            head.assign(buf, n);
            std::fclose(f);
        }
        Tape t;
        std::string why;
        TapeCounts c;
        if (!parse_tape(head, t, why, true, &c)) {
            say(pad(fs::path(p).stem().string(), 24) + "  not a tape this build reads: " + why);
            continue;
        }
        say(pad(t.h.name, 24) + "  " + pad(depth_name(t.h.depth), 6) + "  " + pad(grouped(c.events), 12) + "  " +
            pad(c.span_ns >= 0 ? duration_text(c.span_ns) : "-", 9) + "  " + pad(t.h.instrument.empty() ? "-" : t.h.instrument, 12) + "  " +
            pad(t.h.date.empty() ? "-" : t.h.date, 10) + "  " + t.h.format + ": " + t.h.source_file);
    }
    return 0;
}

int cmd_show(const std::string& dir, const std::string& name) {
    Tape tape;
    std::string why;
    if (!load(dir, name, tape, why)) return refuse(why);
    show(tape);
    return 0;
}

int cmd_remove(const std::string& dir, const std::string& name) {
    if (!valid_name(name)) return refuse("'" + name + "' is not a tape name");
    const std::string path = tape_path(dir, name);
    std::error_code ec;
    if (!fs::remove(path, ec)) return refuse("no tape " + name + " in " + dir + "/");
    say("removed " + path + " (the file it came from is untouched)");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage("say what to do: add, list, show or remove");
    const std::string cmd = args[0];
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        std::fputs(kHelp, stdout);
        return 0;
    }
    ImportOptions opts;
    std::string dir = "data";
    std::vector<std::string> positional;
    for (std::size_t i = 1; i < args.size(); ++i) {
        std::string a = args[i];
        if (a.rfind("--", 0) == 0) a.erase(0, 2);
        const std::size_t eq = a.find('=');
        if (eq == std::string::npos || eq == 0) {
            positional.push_back(args[i]);
            continue;
        }
        const std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if (k == "dir") dir = v;
        else if (cmd != "add") return usage(k + "= is only for add");
        else if (k == "format") opts.format = v;
        else if (k == "name") opts.name = v;
        else if (k == "columns") opts.columns = v;
        else if (k == "tick") opts.tick = v;
        else if (k == "symbol") opts.symbol = v;
        else if (k == "instrument") opts.instrument = v;
        else if (k == "time") {
            if (!parse_time_format(v, opts.time)) return usage("time=" + v + ": say s, ms, us, ns, iso or clock");
        } else {
            return usage("unknown setting " + k + "=");
        }
    }
    if (dir.empty()) return usage("dir= needs a directory");
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    if (!opts.format.empty() && (std::string(" ") + kFormats + " ").find(" " + opts.format + " ") == std::string::npos)
        return usage("format=" + opts.format + ": formats are " + kFormats);
    if (!opts.name.empty() && !valid_name(opts.name))
        return usage("name=" + opts.name + ": lower-case letters, digits, - _ and ., starting with a letter or digit");

    if (cmd == "add") {
        if (positional.size() != 1) return usage("say one file: ingest add FILE");
        return cmd_add(positional[0], opts, dir);
    }
    if (cmd == "list") {
        if (!positional.empty()) return usage("list takes no names");
        return cmd_list(dir);
    }
    if (cmd == "show" || cmd == "remove") {
        if (positional.size() != 1) return usage("say one tape: ingest " + cmd + " NAME");
        return cmd == "show" ? cmd_show(dir, positional[0]) : cmd_remove(dir, positional[0]);
    }
    return usage("no command " + cmd + "; the commands are add, list, show and remove");
}
