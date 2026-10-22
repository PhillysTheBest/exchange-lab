// tests/test_data.cpp — the tape format, the importers, the statistics
// calibration matches and the session writer it measures the simulation with
// (docs/DATA.md), checked against answers known in advance. The known-answer
// control itself runs many hundreds of sessions, so make test-data runs it,
// through the calibrate program, with its planted defects.
//
// The samples in examples/data/ are synthetic: made up for these tests, so they
// carry no licence. The LOBSTER sample comes with its orderbook file, written by
// the same made-up market, so the book a tape rebuilds can be held to the book
// LOBSTER's own format says it should be after every message.
#include "marketdata/calibrate.hpp"
#include "marketdata/importers.hpp"
#include "marketdata/stats.hpp"
#include "test_harness.hpp"

#include <string>
#include <vector>

using namespace px::data;

namespace {

const std::string kSamples = "examples/data/";
const std::string kLobster = kSamples + "DEMO_2012-06-21_34200000_57600000_message_3.csv";
const std::string kLobsterBook = kSamples + "DEMO_2012-06-21_34200000_57600000_orderbook_3.csv";

bool import_text(const std::string& path, const std::string& text, const ImportOptions& opts, Tape& out,
                 std::string& why) {
    return import_file(path, text, "", opts, out, why);
}

void test_numbers() {
    Decimal d{};
    PX_CHECK(parse_decimal("185.230", d) && d.mantissa == 185230 && d.decimals == 3 && needed_decimals(d) == 2);
    PX_CHECK(parse_decimal("1.5e-05", d) && d.mantissa == 15 && d.decimals == 6);
    PX_CHECK(parse_decimal("2E3", d) && d.mantissa == 2000 && d.decimals == 0);
    PX_CHECK(parse_decimal("-0.50", d) && d.mantissa == -50 && d.decimals == 2);
    PX_CHECK(parse_decimal("0.00000001", d) && d.mantissa == 1 && d.decimals == 8);
    PX_CHECK(parse_decimal(" \"42\" ", d) && d.mantissa == 42); // quoted and padded, as exports write them
    PX_CHECK(!parse_decimal("abc", d) && !parse_decimal("1.2.3", d) && !parse_decimal("", d) && !parse_decimal(".", d));
    PX_CHECK(!parse_decimal("1234567890123456789", d)); // 19 significant digits: refused, never rounded
    std::int64_t v = 0;
    PX_CHECK(scale_to(Decimal{18523, 2}, 4, v) && v == 1852300);
    PX_CHECK(scale_to(Decimal{185230, 3}, 2, v) && v == 18523);
    PX_CHECK(!scale_to(Decimal{18523, 3}, 2, v)); // would lose a digit
    PX_CHECK(!parse_int("99999999999999999999", v) && !parse_int("12a", v) && parse_int("-7", v) && v == -7);
    PX_CHECK(gcd(250, 100) == 50 && gcd(0, 7) == 7);
    PX_CHECK(grouped(1234567) == "1,234,567" && grouped(-1000) == "-1,000" && grouped(12) == "12");
    PX_CHECK(decimal_text(18523, 2) == "185.23" && decimal_text(5, 3) == "0.005" && decimal_text(-5, 1) == "-0.5");
}

void test_times() {
    std::int64_t ns = 0;
    TimeKind k{};
    PX_CHECK(parse_time("2024-01-02T09:30:00.123456789Z", TimeFormat::Auto, ns, k) && k == TimeKind::Epoch);
    PX_CHECK(ns == 1704187800LL * kNsPerSec + 123456789);
    PX_CHECK(parse_time("2024-01-02 10:30:00+01:00", TimeFormat::Auto, ns, k) && ns == 1704187800LL * kNsPerSec);
    PX_CHECK(parse_time("2024-01-02", TimeFormat::Auto, ns, k) && ns == 1704153600LL * kNsPerSec);
    PX_CHECK(parse_time("1704187800", TimeFormat::Auto, ns, k) && ns == 1704187800LL * kNsPerSec);
    PX_CHECK(parse_time("1704187800123", TimeFormat::Auto, ns, k) && ns == 1704187800123LL * 1'000'000);
    PX_CHECK(parse_time("1704187800123456", TimeFormat::Auto, ns, k) && ns == 1704187800123456LL * 1'000);
    PX_CHECK(parse_time("1704187800123456789", TimeFormat::Auto, ns, k) && ns == 1704187800123456789LL);
    PX_CHECK(parse_time("34200.004241176", TimeFormat::Auto, ns, k) && k == TimeKind::Midnight &&
             ns == 34200LL * kNsPerSec + 4241176);
    PX_CHECK(parse_time("09:30:00.5", TimeFormat::Auto, ns, k) && k == TimeKind::Midnight &&
             ns == 34200LL * kNsPerSec + 500'000'000);
    PX_CHECK(parse_time("1704187800.25", TimeFormat::Auto, ns, k) && k == TimeKind::Epoch);
    PX_CHECK(parse_time("12", TimeFormat::Millis, ns, k) && ns == 12'000'000); // named, not guessed
    PX_CHECK(!parse_time("yesterday", TimeFormat::Auto, ns, k));
    PX_CHECK(!parse_time("2024-13-02T00:00:00", TimeFormat::Auto, ns, k)); // month 13
    PX_CHECK(!parse_time("25:00:00", TimeFormat::Auto, ns, k));
    PX_CHECK(time_text(1704187800LL * kNsPerSec + 123456789, TimeKind::Epoch) == "2024-01-02 09:30:00.123");
    PX_CHECK(time_text(34200LL * kNsPerSec, TimeKind::Midnight) == "09:30:00.000");
    PX_CHECK(duration_text(25'000'000) == "25.0 ms" && duration_text(41 * kNsPerSec) == "41 s");
    TimeFormat f{};
    PX_CHECK(parse_time_format("us", f) && f == TimeFormat::Micros && !parse_time_format("minutes", f));
}

void test_csv_and_cksum() {
    std::vector<std::string> f;
    PX_CHECK(split_csv("a,\"b,c\",\"say \"\"hi\"\"\",", ',', f));
    PX_CHECK((f == std::vector<std::string>{"a", "b,c", "say \"hi\"", ""}));
    PX_CHECK(!split_csv("a,\"open", ',', f));
    PX_CHECK(guess_delimiter("a;b;c") == ';' && guess_delimiter("a\tb") == '\t' && guess_delimiter("abc") == ',');
    // The numbers `cksum` prints, so anyone can check a tape's source without this program.
    PX_CHECK(cksum_of("") == "4294967295 0");
    PX_CHECK(cksum_of("abc\n") == "1112837078 4");
    PX_CHECK(cksum_of("The quick brown fox jumps over the lazy dog") == "2074844392 43");
}

Tape small_tape() {
    Tape t;
    t.h.name = "small";
    t.h.instrument = "X";
    t.h.format = "csv";
    t.h.importer = "csv 1";
    t.h.source_file = "x.csv";
    t.h.source_cksum = "1 2";
    t.h.depth = Depth::L3;
    t.h.price_scale = 100;
    t.h.origin_ns = 34200LL * kNsPerSec;
    t.h.origin_kind = TimeKind::Midnight;
    t.ev = {{0, 'A', 'B', 7, 9999, 100}, {5, 'A', 'S', 8, 10001, 50}, {9, 'T', 'S', 7, 9999, 40},
            {9, 'C', 'B', 7, 9999, 60}, {12, 'R', '-', 0, 0, 0}, {12, 'L', 'B', 0, 9998, 10}};
    return t;
}

void test_tape_round_trip() {
    const Tape t = small_tape();
    const std::string text = tape_text(t);
    Tape back;
    std::string why;
    PX_CHECK(parse_tape(text, back, why));
    PX_CHECK(tape_text(back) == text); // what is read is what was written, byte for byte
    PX_CHECK(back.ev.size() == 6 && back.ev[2].kind == 'T' && back.ev[2].side == 'S' && back.h.price_scale == 100);
    PX_CHECK(back.h.origin_kind == TimeKind::Midnight && back.h.depth == Depth::L3);

    Tape head;
    TapeCounts c;
    PX_CHECK(parse_tape(text, head, why, true, &c) && head.ev.empty() && c.events == 6 && c.span_ns == 12);

    // A tape cut short, a changed row and a row out of order are each refused, naming the line.
    const std::string cut = text.substr(0, text.rfind('\n', text.size() - 2) + 1); // the last row lost
    PX_CHECK(!parse_tape(cut, back, why) && why.find("cut short") != std::string::npos);
    std::string bad = text;
    bad.replace(bad.find("5,A,S,8"), 7, "5,Q,S,8");
    PX_CHECK(!parse_tape(bad, back, why) && why.find("line ") == 0 && why.find("not a tape row") != std::string::npos);
    bad = text;
    bad.replace(bad.find("9,T,S"), 1, "4");
    PX_CHECK(!parse_tape(bad, back, why) && why.find("backwards") != std::string::npos);
    bad = text;
    bad.replace(bad.find("tape.version=1"), 14, "tape.version=9");
    PX_CHECK(!parse_tape(bad, back, why) && why.find("version") != std::string::npos);
    PX_CHECK(!parse_tape("name=x\n", back, why) && why.find("not a tape") != std::string::npos);

    PX_CHECK(valid_name("aapl-2012-06-21") && valid_name("btc_usd.v2") && !valid_name("AAPL") &&
             !valid_name("-x") && !valid_name("a/b") && !valid_name("") && !valid_name("x."));
    PX_CHECK(name_from_file("/data/AAPL Trades.csv") == "aapl-trades" && name_from_file("__.csv") == "tape");
}

void test_generic_csv() {
    std::string text;
    PX_CHECK(read_file(kSamples + "trades.csv", text));
    Tape t;
    std::string why;
    PX_CHECK(import_file(kSamples + "trades.csv", text, "", ImportOptions{}, t, why));
    PX_CHECK(t.h.name == "trades" && t.h.depth == Depth::Trades && t.h.format == "csv" && t.ev.size() == 500);
    PX_CHECK(t.h.source_cksum == "2433452726 16270" && t.h.source_file == "trades.csv");
    PX_CHECK(t.h.origin_kind == TimeKind::Epoch && t.ev.front().t == 0);
    PX_CHECK(t.h.price_scale == 100 && t.h.tick == 25 && t.h.qty_scale == 100); // 0.25 a tick, 0.01 of a unit
    bool sides = true;
    for (const TapeEvent& e : t.ev) sides = sides && e.kind == 'T' && (e.side == 'B' || e.side == 'S') && e.id == 0;
    PX_CHECK(sides);

    // The same rows newest first: reversed, and the tape comes out identical apart from the note and checksum.
    std::vector<std::string_view> lines = lines_of(text);
    std::string reversed(lines[0]);
    reversed += '\n';
    for (std::size_t i = lines.size(); i-- > 1;) reversed += std::string(lines[i]) + "\n";
    Tape r;
    PX_CHECK(import_file(kSamples + "trades.csv", reversed, "", ImportOptions{}, r, why));
    PX_CHECK(r.h.notes.find("reversed") != std::string::npos);
    bool same = r.ev.size() == t.ev.size();
    for (std::size_t i = 0; same && i < t.ev.size(); ++i)
        same = r.ev[i].t == t.ev[i].t && r.ev[i].price == t.ev[i].price && r.ev[i].qty == t.ev[i].qty;
    PX_CHECK(same);

    // Rows out of order are sorted, and the tape says how many moved.
    PX_CHECK(import_text("x.csv", "time,price,size\n3,10.5,1\n1,10.0,2\n2,10.25,1\n", ImportOptions{}, t, why));
    PX_CHECK(t.ev.size() == 3 && t.ev[0].price == 40 && t.ev[2].price == 42 && t.h.tick == 25);
    PX_CHECK(t.h.notes.find("1 rows were out of time order") != std::string::npos);

    // No header: refused until columns= says which column is which.
    const std::string bare = "1700000000000;101.5;3;S\n1700000000100;101.0;1;B\n";
    PX_CHECK(!import_text("bare.csv", bare, ImportOptions{}, t, why) && why.find("columns=") != std::string::npos);
    ImportOptions o;
    o.columns = "time,price,qty,side";
    PX_CHECK(import_text("bare.csv", bare, o, t, why) && t.ev.size() == 2 && t.ev[0].side == 'S');
    o.columns = "time,cost,qty";
    PX_CHECK(!import_text("bare.csv", bare, o, t, why) && why.find("'cost' is not a role") != std::string::npos);
    o.columns = "-,price,qty";
    PX_CHECK(!import_text("bare.csv", bare, o, t, why) && why.find("time") != std::string::npos);

    // A malformed row is refused, naming its line; nothing is guessed.
    PX_CHECK(!import_text("x.csv", "time,price,size\n1,10,1\n2,ten,1\n", ImportOptions{}, t, why) &&
             why.find("line 3: price 'ten' is not a number") == 0);
    PX_CHECK(!import_text("x.csv", "time,price,size\n1,10,1\nnoon,10,1\n", ImportOptions{}, t, why) &&
             why.find("line 3: time 'noon'") == 0);
    PX_CHECK(!import_text("x.csv", "time,price,size\n1,10,1\n2,10\n", ImportOptions{}, t, why) &&
             why.find("line 3: 2 fields, expected 3") == 0);
    PX_CHECK(!import_text("x.csv", "time,price,size\n1,10,0\n", ImportOptions{}, t, why) &&
             why.find("a quantity of zero") != std::string::npos);
    PX_CHECK(!import_text("x.csv", "time,price,size\n1,-10,1\n", ImportOptions{}, t, why) &&
             why.find("not usable") != std::string::npos);
    PX_CHECK(!import_text("x.csv", "", ImportOptions{}, t, why) && why.find("empty") != std::string::npos);
    PX_CHECK(!import_text("x.csv", "time,price\n", ImportOptions{}, t, why) && why.find("no usable rows") == 0);

    // Several instruments: refused without symbol=, read with it; is_buyer_maker gives the side.
    const std::string multi = "ts,symbol,price,qty,is_buyer_maker\n1,BTCUSDT,100,1,true\n2,ETHUSDT,5,2,false\n"
                              "3,BTCUSDT,101,1,false\n";
    PX_CHECK(!import_text("m.csv", multi, ImportOptions{}, t, why) && why.find("symbol=") != std::string::npos);
    o = ImportOptions{};
    o.symbol = "BTCUSDT";
    PX_CHECK(import_text("m.csv", multi, o, t, why) && t.ev.size() == 2 && t.h.instrument == "BTCUSDT");
    PX_CHECK(t.ev[0].side == 'S' && t.ev[1].side == 'B' && t.h.skipped == 1);

    // tick= must hold every price; it is never rounded to.
    o = ImportOptions{};
    o.tick = "0.05";
    PX_CHECK(import_text("x.csv", "time,price,size\n1,10.05,1\n2,10.10,1\n", o, t, why) && t.h.tick == 5 &&
             t.ev[0].price == 201);
    PX_CHECK(!import_text("x.csv", "time,price,size\n1,10.05,1\n2,10.12,1\n", o, t, why) &&
             why.find("line 3: price 10.12 is not on the 0.05 grid") == 0);

    // Quotes: each change of the best price empties the old level and fills the new one.
    PX_CHECK(import_text("q.csv", "time,bid,bid_size,ask,ask_size\n1,10.00,5,10.02,7\n2,10.01,3,10.02,9\n",
                         ImportOptions{}, t, why));
    PX_CHECK(t.h.depth == Depth::L2 && t.ev.size() == 5);
    PX_CHECK(t.ev[2].kind == 'L' && t.ev[2].side == 'B' && t.ev[2].price == 1000 && t.ev[2].qty == 0);
    PX_CHECK(t.ev[3].kind == 'L' && t.ev[3].price == 1001 && t.ev[3].qty == 3 && t.ev[4].qty == 9);

    // The sample with no header, read with columns=.
    PX_CHECK(read_file(kSamples + "quotes_and_trades.csv", text));
    o = ImportOptions{};
    o.columns = "time,price,qty,side,bid,bid_qty,ask,ask_qty";
    PX_CHECK(import_file(kSamples + "quotes_and_trades.csv", text, "", o, t, why));
    PX_CHECK(t.h.depth == Depth::L2 && t.h.origin_kind == TimeKind::Epoch && t.h.tick == 1 && t.h.price_scale == 100);
}

// The LOBSTER sample's book, rebuilt from the tape, is held to its orderbook file
// after every message: three levels a side, prices and sizes.
void test_lobster() {
    std::string msg, book;
    PX_CHECK(read_file(kLobster, msg) && read_file(kLobsterBook, book));
    Tape t;
    std::string why;
    PX_CHECK(import_file(kLobster, msg, book, ImportOptions{}, t, why));
    PX_CHECK(t.h.name == "demo-2012-06-21" && t.h.instrument == "DEMO" && t.h.date == "2012-06-21");
    PX_CHECK(t.h.depth == Depth::L3 && t.h.format == "lobster" && t.h.origin_kind == TimeKind::Midnight);
    PX_CHECK(t.h.skipped == 3 && t.h.skipped_why == "1 auction trades, 2 trading halt messages");
    PX_CHECK(t.h.notes.find("seeded") != std::string::npos);
    PX_CHECK(t.h.price_scale == 100 && t.h.tick == 1);

    const std::vector<std::string_view> ml = lines_of(msg), bl = lines_of(book);
    PX_CHECK(ml.size() == bl.size());
    std::size_t seeds = 0;
    while (seeds < t.ev.size() && t.ev[seeds].kind == 'L') ++seeds;
    PX_CHECK(seeds == 6); // three levels a side before the first message
    TapeBook b;
    for (std::size_t i = 0; i < seeds; ++i) b.apply(t.ev[i]);
    std::size_t next = seeds, compared = 0, mismatches = 0;
    std::vector<std::string> f, g;
    for (std::size_t j = 0; j < ml.size(); ++j) {
        PX_CHECK(split_csv(ml[j], ',', f) && split_csv(bl[j], ',', g) && g.size() == 12);
        if (f[1] == "6" || f[1] == "7") continue; // not continuous trading: not in the tape
        b.apply(t.ev[next++]);
        const auto asks = b.top(1, 3), bids = b.top(0, 3);
        for (std::size_t lv = 0; lv < 3; ++lv) {
            std::int64_t ap = 0, as = 0, bp = 0, bs = 0;
            PX_CHECK(parse_int(g[4 * lv], ap) && parse_int(g[4 * lv + 1], as) && parse_int(g[4 * lv + 2], bp) &&
                     parse_int(g[4 * lv + 3], bs));
            // The tape's prices are in cents; LOBSTER's are dollars x 10,000.
            const bool ask_ok = lv < asks.size() ? asks[lv].first * 100 == ap && asks[lv].second == as : as == 0;
            const bool bid_ok = lv < bids.size() ? bids[lv].first * 100 == bp && bids[lv].second == bs : bs == 0;
            mismatches += ask_ok && bid_ok ? 0 : 1;
        }
        ++compared;
    }
    PX_CHECK(next == t.ev.size());
    PX_CHECK(compared > 600 && mismatches == 0);

    // Without the orderbook file the book starts empty, and the tape says so.
    Tape bare;
    PX_CHECK(import_file(kLobster, msg, "", ImportOptions{}, bare, why));
    PX_CHECK(bare.h.notes.find("starts empty") != std::string::npos && bare.ev.size() == t.ev.size() - seeds);

    // A malformed row is refused naming its line; the name must be LOBSTER's.
    std::string broken = msg;
    const std::size_t third = broken.find('\n', broken.find('\n') + 1) + 1;
    broken.insert(third, "34200.1,9,1,100,1000000,1\n");
    PX_CHECK(!import_file(kLobster, broken, "", ImportOptions{}, bare, why) && why.find("line 3: ") == 0);
    ImportOptions o;
    o.format = "lobster";
    PX_CHECK(!import_file("x.csv", msg, "", o, bare, why) && why.find("not a LOBSTER") != std::string::npos);
    PX_CHECK(detect_format(kLobster, ml[0]) == "lobster" && detect_format("trades.csv", "a,b") == "csv");
}

// Statistics on tapes whose answers are worked out by hand.
void test_stats() {
    Tape t;
    t.h.depth = Depth::L2;
    // [0,100): bid 100 x10, ask 102 x30: spread 2, depth 20. [100,1000): ask 101 x10: spread 1, depth 10.
    t.ev = {{0, 'L', 'B', 0, 100, 10},   {0, 'L', 'S', 0, 102, 30},  {100, 'L', 'S', 0, 102, 0},
            {100, 'L', 'S', 0, 101, 10}, {400, 'T', 'B', 0, 101, 4}, {400, 'T', 'B', 0, 101, 6},
            {1000, 'L', 'B', 0, 100, 10}};
    TapeStats s = tape_stats(t);
    PX_CHECK(s.spread_ticks == 1 && s.touch_depth == 10 && s.two_sided_ns == 1000 && s.crossed_ns == 0);
    PX_CHECK(s.prints == 1 && s.trades == 2 && s.print_qty == 10 && s.buy_prints == 1); // one aggressor, one print
    // Means: depth (20 for 100 ns, 10 for 900) is 11; spread (2, then 1) is 1.1 ticks; one print of 10.
    PX_CHECK(s.touch_depth_mean_x100 == 1100 && s.spread_mean_x100 == 110 && s.print_mean_x100 == 1000);
    PX_CHECK(s.size_depth_pml == 909 && s.span_ns == 1000);
    // Read as trades only, the book is not measured at all.
    s = tape_stats(t, Depth::Trades);
    PX_CHECK(s.depth == Depth::Trades && s.spread_ticks == -1 && s.touch_depth == -1 && s.print_qty == 10);

    // Prints per move: the price rises a tick every print, so every run of 20
    // prints moves 20 ticks, a squared move of 400 ticks^2: 0.05 prints a tick.
    Tape p;
    p.h.depth = Depth::Trades;
    for (int i = 0; i <= 40; ++i) p.ev.push_back({i * 10, 'T', i % 2 == 0 ? 'B' : 'S', 0, 100 + i, 1});
    s = tape_stats(p);
    PX_CHECK(s.prints == 41 && s.move_windows == 21 && s.move_sumsq == 21 * 1600 && s.prints_per_move_x100 == 5);
    PX_CHECK(!s.enough_prints());
    // A reset closes the runs: none may span two sessions. 20 prints, then 21.
    p.ev.insert(p.ev.begin() + 20, TapeEvent{195, 'R', '-', 0, 0, 0});
    s = tape_stats(p);
    PX_CHECK(s.move_windows == 1 && s.resets == 1);

    // Orders the tape never added (they rested before it began) come out of the anonymous size.
    TapeBook b;
    b.apply({0, 'L', 'B', 0, 50, 300});
    b.apply({1, 'A', 'B', 9, 50, 100});
    b.apply({2, 'C', 'B', 1234, 50, 200}); // unknown id: anonymous size
    b.apply({3, 'T', 'S', 9, 50, 30});     // a sell crossed: order 9, a bid, executes
    std::int64_t px = 0, qty = 0;
    PX_CHECK(b.best(0, px, qty) && px == 50 && qty == 170 && b.resting_orders() == 1);
    b.apply({4, 'L', 'B', 0, 50, 60}); // a snapshot says less than the known order: anonymous goes to zero
    PX_CHECK(b.best(0, px, qty) && qty == 70);
    b.apply({5, 'R', '-', 0, 0, 0});
    PX_CHECK(!b.best(0, px, qty) && b.resting_orders() == 0);

    // The LOBSTER sample's statistics: a book that never crosses, and every statistic measurable.
    std::string msg, book;
    PX_CHECK(read_file(kLobster, msg) && read_file(kLobsterBook, book));
    std::string why;
    PX_CHECK(import_file(kLobster, msg, book, ImportOptions{}, t, why));
    s = tape_stats(t);
    PX_CHECK(s.spread_ticks > 0 && s.touch_depth > 0 && s.print_qty > 0 && s.prints_per_move_x100 > 0);
    PX_CHECK(s.crossed_ns == 0 && s.move_windows > 0);
    std::printf("  LOBSTER sample: spread %lld ticks, touch depth %lld, print %lld, %lld.%02lld prints a move\n",
                static_cast<long long>(s.spread_ticks), static_cast<long long>(s.touch_depth),
                static_cast<long long>(s.print_qty), static_cast<long long>(s.prints_per_move_x100 / 100),
                static_cast<long long>(s.prints_per_move_x100 % 100));
}

// A session written as a tape rebuilds the engine's own book, every level on
// both sides, after every step; and writing it changes nothing in the session.
void test_session_tape() {
    using namespace px::arena;
    using px::kPriceScale;
    using px::LevelView;
    using px::Side;
    const suite::SuiteDef def = suite::standard_suite();
    Roster roster, bare_roster;
    suite::Origin origin{};
    std::string why;
    PX_CHECK(suite::build_arm_roster(def, "", roster, origin, why) &&
             suite::build_arm_roster(def, "", bare_roster, origin, why));
    Session s, bare;
    PX_CHECK(s.start(suite::suite_session_config(def, 42), std::move(roster)) == Session::StartError::None);
    PX_CHECK(bare.start(suite::suite_session_config(def, 42), std::move(bare_roster)) == Session::StartError::None);
    SessionTapeWriter w("check", 100, kPriceScale);
    w.attach(s, 0);
    TapeBook b;
    std::size_t applied = 0, steps = 0, mismatches = 0;
    std::uint64_t levels = 0;
    while (s.step()) {
        ++steps;
        w.sync();
        const std::vector<TapeEvent>& ev = w.tape().ev;
        for (; applied < ev.size(); ++applied) b.apply(ev[applied]);
        for (const Side side : {Side::Buy, Side::Sell}) {
            const std::uint32_t n = s.engine().depth(s.instrument(), side);
            const auto mine = b.top(side == Side::Buy ? 0 : 1, n + 1);
            bool same = mine.size() == n;
            for (std::uint32_t i = 0; same && i < n; ++i) {
                const LevelView lv = s.engine().level(s.instrument(), side, i);
                same = mine[i].first * 100 == lv.price && static_cast<std::uint64_t>(mine[i].second) == lv.qty;
            }
            mismatches += same ? 0 : 1;
            levels += n;
        }
    }
    s.finish();
    w.detach(s);
    while (bare.step()) {
    }
    bare.finish();
    PX_CHECK(steps > 10'000 && levels > 100'000 && mismatches == 0);
    PX_CHECK(s.hash() == bare.hash()); // the tap reads; it never acts
    const Tape& t = w.tape();
    PX_CHECK(t.h.depth == Depth::L3 && t.h.tick == 100 && t.h.price_scale == kPriceScale && w.rested() > 0);
    std::uint64_t kinds[128] = {};
    for (const TapeEvent& e : t.ev) ++kinds[static_cast<unsigned char>(e.kind)];
    PX_CHECK(kinds['A'] > 0 && kinds['C'] > 0 && kinds['T'] > 0 && kinds['L'] == 0 && kinds['R'] == 0);
    std::printf("  session tape: %zu steps, %zu rows, book identical to the engine's after every step\n", steps,
                t.ev.size());

    // Two sessions on one tape: a reset between them, time running on, and the
    // same tape every time it is made.
    const std::uint64_t seeds[] = {42, 43};
    Tape one, two;
    PX_CHECK(simulate(def, seeds, 2, one, why) && simulate(def, seeds, 2, two, why));
    PX_CHECK(tape_text(one) == tape_text(two));
    std::size_t resets = 0;
    for (const TapeEvent& e : one.ev) resets += e.kind == 'R' ? 1 : 0;
    PX_CHECK(resets == 1 && one.ev.back().t > def.session_ns && one.ev.back().t <= 2 * def.session_ns);
    Tape back;
    PX_CHECK(parse_tape(tape_text(one), back, why) && tape_text(back) == tape_text(one));
}

void test_calibration_parts() {
    PX_CHECK(ratio_distance(100, 100) == 0 && ratio_distance(125, 100) == 250 && ratio_distance(100, 125) == 250);
    PX_CHECK(ratio_distance(0, 100) == kFar && ratio_distance(-1, -1) == 0);
    PX_CHECK(grid_index(Knob::Pace, 20'000) == 5 && grid_index(Knob::Size, 33) == -1);
    const MarketParams ref = reference_params();
    PX_CHECK(ref.half_spread_ticks == 2 && ref.noise_qty == 25 && ref.noise_timer_ns == 10'000);
    for (const Knob k : kKnobs) PX_CHECK(grid_index(k, knob_value(ref, k)) >= 0);       // the start is on every grid
    for (const Knob k : kKnobs) PX_CHECK(grid_index(k, knob_value(control_truth(), k)) >= 0);
    for (const Knob k : kKnobs) PX_CHECK(knob_value(control_truth(), k) != knob_value(ref, k)); // every knob must move
    const px::arena::suite::SuiteDef m = market_with(px::arena::suite::standard_suite(), MarketParams{4, 30, 15'000});
    PX_CHECK(m.roster.maker_half_spread_ticks == 4 && m.roster.rival_half_spread_ticks == 4 && m.roster.noise_qty == 30 &&
             m.roster.noise_timer_ns == 15'000 && std::string(m.name) == "standard");
    // Calibration's seeds are never a block a result is read on.
    for (const std::uint64_t s : kCalibrationSeeds) PX_CHECK(s < 42 || s > 105);
    for (const std::uint64_t s : kControlSeeds) PX_CHECK((s < 42 || s > 105) && (s < 9001 || s > 9008));

    // The planted defects do what their names say.
    Tape t = small_tape();
    Tape d = t;
    plant(d, Defect::DropCancels);
    PX_CHECK(d.ev.size() == t.ev.size() - 1);
    for (const TapeEvent& e : d.ev) PX_CHECK(e.kind != 'C');
    d = t;
    plant(d, Defect::PriceTimesTen);
    PX_CHECK(d.ev[0].price == 10 * t.ev[0].price && d.ev.size() == t.ev.size());
    d = t;
    plant(d, Defect::None);
    PX_CHECK(tape_text(d) == tape_text(t));
}

// DBN bytes for the tests: the prefix, a metadata block, then records.
struct DbnWriter {
    std::string b;
    explicit DbnWriter(unsigned version = 2) {
        b = "DBN";
        b += static_cast<char>(version);
        std::string meta(100, '\0');
        meta.replace(0, 9, "XNAS.ITCH");
        put(meta.size(), 4);
        b += meta;
    }
    void put(std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i) b += static_cast<char>((v >> (8 * i)) & 0xFF);
    }
    // The 16-byte header: length in words, rtype, publisher, instrument, ts_event.
    void header(std::size_t len, unsigned rtype, std::uint32_t inst, std::uint64_t ts) {
        put(len / 4, 1);
        put(rtype, 1);
        put(1, 2);
        put(inst, 4);
        put(ts, 8);
    }
    void mbo(std::uint64_t ts, char action, char side, std::int64_t price, std::uint32_t size, std::uint64_t id,
             std::uint32_t inst = 7) {
        header(56, 0xA0, inst, ts - 10);
        put(id, 8);
        put(static_cast<std::uint64_t>(price), 8);
        put(size, 4);
        put(0, 1); // flags
        put(0, 1); // channel
        b += action;
        b += side;
        put(ts, 8); // ts_recv
        put(0, 4);
        put(0, 4);
    }
    void mbp1(std::uint64_t ts, char action, char side, std::int64_t price, std::uint32_t size, std::int64_t bid,
              std::uint32_t bid_sz, std::int64_t ask, std::uint32_t ask_sz) {
        header(80, 0x01, 7, ts - 10);
        put(static_cast<std::uint64_t>(price), 8);
        put(size, 4);
        b += action;
        b += side;
        put(0, 1);
        put(0, 1);
        put(ts, 8);
        put(0, 4);
        put(0, 4);
        put(static_cast<std::uint64_t>(bid), 8);
        put(static_cast<std::uint64_t>(ask), 8);
        put(bid_sz, 4);
        put(ask_sz, 4);
        put(1, 4);
        put(1, 4);
    }
};

void test_databento() {
    std::string text, why;
    const std::string path = kSamples + "databento_mbo.csv";
    PX_CHECK(read_file(path, text));
    std::string_view first = std::string_view(text).substr(0, text.find('\n'));
    PX_CHECK(detect_format(path, first) == "databento");
    Tape t;
    PX_CHECK(import_file(path, text, "", ImportOptions{}, t, why));
    PX_CHECK(t.h.format == "databento" && t.h.depth == Depth::L3 && t.h.instrument == "DEMO" &&
             t.h.origin_kind == TimeKind::Epoch && t.h.name == "databento_mbo");
    PX_CHECK(t.h.price_scale == 100 && t.h.tick == 1);
    PX_CHECK(t.h.skipped == 4 && t.h.skipped_why.find("3 fills") == 0 && t.h.skipped_why.find("1 records with no action") != std::string::npos);
    // The clear, five adds, the modify as a cancel and an add, a partial cancel,
    // a trade that leaves the book alone and the cancels that follow it.
    std::string kinds;
    for (const TapeEvent& e : t.ev) kinds += e.kind;
    PX_CHECK(kinds == "RAAAAACACTCCATC");
    PX_CHECK(t.ev[6].id == 2 && t.ev[6].qty == 20 && t.ev[6].price == 9999 && t.ev[7].price == 9998 && t.ev[7].qty == 25);
    PX_CHECK(t.ev[9].side == 'S' && t.ev[9].id == 0 && t.ev[9].price == 10000 && t.ev[9].qty == 12);
    PX_CHECK(t.ev[13].side == 'B' && t.ev[13].price == 10001);
    TapeBook b;
    for (const TapeEvent& e : t.ev) b.apply(e);
    PX_CHECK((b.top(0, 3) == std::vector<std::pair<std::int64_t, std::int64_t>>{{10000, 5}, {9998, 25}}));
    PX_CHECK((b.top(1, 3) == std::vector<std::pair<std::int64_t, std::int64_t>>{{10002, 15}, {10003, 3}}));

    // MBP-1 as CSV, pretty: decimal prices and ISO times, levels from snapshots.
    const std::string mbp =
        "ts_recv,ts_event,rtype,publisher_id,instrument_id,action,side,depth,price,size,flags,ts_in_delta,sequence,"
        "bid_px_00,ask_px_00,bid_sz_00,ask_sz_00,bid_ct_00,ask_ct_00\n"
        "2024-01-02T14:30:00.000000000Z,2024-01-02T14:30:00.000000000Z,1,1,7,A,B,0,10.00,5,0,0,1,10.00,,5,0,1,0\n"
        "2024-01-02T14:30:00.001000000Z,2024-01-02T14:30:00.001000000Z,1,1,7,A,A,0,10.02,4,0,0,2,10.00,10.02,5,4,1,1\n"
        "2024-01-02T14:30:00.002000000Z,2024-01-02T14:30:00.002000000Z,1,1,7,T,A,0,10.00,2,0,0,3,10.00,10.02,5,4,1,1\n"
        "2024-01-02T14:30:00.002000000Z,2024-01-02T14:30:00.002000000Z,1,1,7,C,B,0,10.00,2,0,0,4,10.00,10.02,3,4,1,1\n"
        "2024-01-02T14:30:00.003000000Z,2024-01-02T14:30:00.003000000Z,1,1,7,A,B,0,10.01,1,0,0,5,10.01,10.02,1,4,1,1\n";
    PX_CHECK(import_file("glbx.mbp-1.csv", mbp, "", ImportOptions{}, t, why));
    PX_CHECK(t.h.depth == Depth::L2 && t.h.notes.find("1 price level a side") != std::string::npos);
    kinds.clear();
    for (const TapeEvent& e : t.ev) kinds += e.kind;
    PX_CHECK(kinds == "LLTLLL"); // bid, ask, the print, the bid's new size, then the bid moving up
    PX_CHECK(t.ev[2].side == 'S' && t.ev[2].qty == 2 && t.ev[4].qty == 0 && t.ev[4].price == 1000 && t.ev[5].price == 1001);

    // Trades, raw: prints only.
    const std::string trades =
        "ts_recv,ts_event,rtype,publisher_id,instrument_id,action,side,depth,price,size,flags,ts_in_delta,sequence\n"
        "1704205800000000000,1704205800000000000,0,1,7,T,B,0,10010000000,3,0,0,1\n"
        "1704205800000500000,1704205800000500000,0,1,7,T,N,0,10000000000,1,0,0,2\n";
    PX_CHECK(import_file("x.trades.csv", trades, "", ImportOptions{}, t, why) && t.h.depth == Depth::Trades &&
             t.ev.size() == 2 && t.ev[0].side == 'B' && t.ev[1].side == '-' && t.h.instrument == "instrument_id 7");

    // Several instruments need symbol=; bars and a bad field are refused.
    std::string two = trades + "1704205800000600000,1704205800000600000,0,1,8,T,B,0,10010000000,3,0,0,3\n";
    PX_CHECK(!import_file("x.csv", two, "", ImportOptions{}, t, why) && why.find("pick one with symbol=") != std::string::npos);
    ImportOptions eight;
    eight.symbol = "8";
    PX_CHECK(import_file("x.csv", two, "", eight, t, why) && t.ev.size() == 1 && t.h.skipped == 2);
    PX_CHECK(!import_file("x.csv", "ts_event,rtype,publisher_id,instrument_id,open,high,low,close,volume\n", "",
                          ImportOptions{}, t, why) &&
             why.find("OHLCV") != std::string::npos);
    std::string bad = trades;
    bad.replace(bad.find(",T,B,"), 5, ",T,Q,");
    PX_CHECK(!import_file("x.csv", bad, "", ImportOptions{}, t, why) && why.find("line 2: side") == 0);

    // DBN: the same records, binary. Prices in 10^-9, little-endian.
    const std::int64_t D = 1'000'000'000;
    DbnWriter w;
    w.mbo(1'704'205'800'000'000'000ULL, 'A', 'B', 100 * D, 10, 1);
    w.mbo(1'704'205'800'000'001'000ULL, 'A', 'A', 101 * D, 4, 2);
    w.mbo(1'704'205'800'000'002'000ULL, 'T', 'B', 101 * D, 4, 0);
    w.mbo(1'704'205'800'000'002'000ULL, 'F', 'A', 101 * D, 4, 2);
    w.mbo(1'704'205'800'000'002'000ULL, 'C', 'A', 101 * D, 4, 2);
    PX_CHECK(readable("x.dbn", w.b, why) && detect_format("x.dbn", w.b.substr(0, w.b.find('\n'))) == "databento");
    PX_CHECK(import_file("xnas-itch-20240102.mbo.dbn", w.b, "", ImportOptions{}, t, why));
    PX_CHECK(t.h.depth == Depth::L3 && t.h.importer == "databento 1 (DBN v2)" && t.h.notes.find("XNAS.ITCH") != std::string::npos);
    kinds.clear();
    for (const TapeEvent& e : t.ev) kinds += e.kind;
    PX_CHECK(kinds == "AATC" && t.ev[2].side == 'B' && t.ev[2].id == 0 && t.h.tick == 1 && t.h.price_scale == 1);
    PX_CHECK(t.h.name == "xnas-itch-20240102" && t.h.instrument == "instrument_id 7");
    // A record of a type not read is skipped by its length; one cut short is refused.
    DbnWriter m;
    m.header(16, 0x16, 7, 1); // a symbol mapping with no body
    m.mbp1(1'704'205'800'000'000'000ULL, 'A', 'B', 10 * D, 5, 10 * D, 5, kDbUndefPrice, 0);
    m.mbp1(1'704'205'800'000'001'000ULL, 'T', 'A', 10 * D, 2, 10 * D, 3, 11 * D, 1);
    PX_CHECK(import_file("x.dbn", m.b, "", ImportOptions{}, t, why) && t.h.depth == Depth::L2 && t.h.skipped == 1);
    kinds.clear();
    for (const TapeEvent& e : t.ev) kinds += e.kind;
    PX_CHECK(kinds == "LTLL");
    PX_CHECK(!import_file("x.dbn", m.b.substr(0, m.b.size() - 3), "", ImportOptions{}, t, why) &&
             why.find("cut short") != std::string::npos);
    m.mbo(1'704'205'800'000'002'000ULL, 'A', 'B', 10 * D, 1, 9);
    PX_CHECK(!import_file("x.dbn", m.b, "", ImportOptions{}, t, why) && why.find("mixes schemas") != std::string::npos);
    PX_CHECK(!import_file("x.dbn", DbnWriter(4).b, "", ImportOptions{}, t, why) && why.find("version 4") != std::string::npos);

    // What cannot be read is refused with what to do instead.
    PX_CHECK(!readable("x.dbn.zst", std::string("\x28\xb5\x2f\xfd rest"), why) && why.find("zstd -d") != std::string::npos);
    PX_CHECK(!readable("x.bin", std::string("ab\0cd", 5), why) && why.find("binary") != std::string::npos);
}

// Tardis: a book file and its trades file, merged by arrival time.
void test_tardis() {
    std::string book, trades, why;
    const std::string path = kSamples + "demo_incremental_book_L2_2024-01-02_DEMO.csv";
    PX_CHECK(read_file(path, book) && read_file(kSamples + "demo_trades_2024-01-02_DEMO.csv", trades));
    PX_CHECK(detect_format(path, std::string_view(book).substr(0, book.find('\n'))) == "tardis");
    const std::vector<Companion> c = companions_of(path, "");
    PX_CHECK(c.size() == 1 && c[0].path == kSamples + "demo_trades_2024-01-02_DEMO.csv");
    Tape t;
    PX_CHECK(import_file(path, book, trades, ImportOptions{}, t, why));
    PX_CHECK(t.h.format == "tardis" && t.h.depth == Depth::L2 && t.h.instrument == "DEMO" &&
             t.h.date == "2024-01-02" && t.h.name == "demo-demo-2024-01-02" && t.h.origin_kind == TimeKind::Epoch);
    PX_CHECK(t.h.notes == "demo incremental_book_L2, with its trades file");
    PX_CHECK(t.h.price_scale == 10 && t.h.tick == 5 && t.h.qty_scale == 100); // prices in ticks of 0.5
    // The first snapshot starts the book; the second, after updates, resets it.
    // Each trade sits where it arrived, between the book's rows.
    std::string kinds;
    for (const TapeEvent& e : t.ev) kinds += e.kind;
    PX_CHECK(kinds == "LLLLLTLTLLRLLLLTLLTL");
    PX_CHECK(t.ev[5].side == 'B' && t.ev[5].price == 84002 && t.ev[5].qty == 100 && t.ev[5].id == 0);
    PX_CHECK(t.ev[15].side == 'S' && t.ev[18].side == '-' && t.ev[8].qty == 0);
    TapeBook b;
    for (const TapeEvent& e : t.ev) b.apply(e);
    PX_CHECK((b.top(0, 3) == std::vector<std::pair<std::int64_t, std::int64_t>>{{84001, 75}, {84000, 200}}));
    PX_CHECK((b.top(1, 3) == std::vector<std::pair<std::int64_t, std::int64_t>>{{84003, 200}, {84004, 75}}));
    // Without its trades file: the book, and a note that there are no prints.
    PX_CHECK(import_file(path, book, "", ImportOptions{}, t, why) &&
             t.h.notes.find("no trades file beside it") != std::string::npos && t.ev.size() == 16);
    // The trades file alone is a trades-only tape.
    PX_CHECK(import_file("demo_trades_2024-01-02_DEMO.csv", trades, "", ImportOptions{}, t, why) &&
             t.h.depth == Depth::Trades && t.ev.size() == 4 && t.h.name == "demo-demo-2024-01-02-trades");

    // book_snapshot_N and quotes: one snapshot a row, made level updates.
    const std::string snap =
        "exchange,symbol,timestamp,local_timestamp,asks[0].price,asks[0].amount,bids[0].price,bids[0].amount,"
        "asks[1].price,asks[1].amount,bids[1].price,bids[1].amount\n"
        "deribit,BTC-PERPETUAL,1704186000000000,1704186000000500,42001,10,42000,20,42002,5,41999,7\n"
        "deribit,BTC-PERPETUAL,1704186000001000,1704186000001500,42001,4,42000,20,42002,5,41999,7\n"
        "deribit,BTC-PERPETUAL,1704186000002000,1704186000002500,42002,5,42000,20,42003,1,41999,7\n";
    PX_CHECK(import_file("deribit_book_snapshot_2_2024-01-02_BTC-PERPETUAL.csv", snap, "", ImportOptions{}, t, why));
    PX_CHECK(t.h.notes.find("book_snapshot, 2 levels a side; no trades file") == 8 && t.h.name == "deribit-btc-perpetual-2024-01-02");
    kinds.clear();
    for (const TapeEvent& e : t.ev) kinds += e.kind;
    PX_CHECK(kinds == "LLLLLLL"); // four levels, the ask's new size, then the touch gone and a new level behind
    PX_CHECK(t.ev[4].price == 42001 && t.ev[4].qty == 4 && t.ev[5].price == 42001 && t.ev[5].qty == 0 && t.ev[6].price == 42003);
    const std::string quotes = "exchange,symbol,timestamp,local_timestamp,ask_amount,ask_price,bid_price,bid_amount\n"
                               "kraken,XBT/USD,1704186000000000,1704186000000500,2,42001,42000,3\n"
                               "kraken,XBT/USD,1704186000001000,1704186000001500,2,42001,42000.5,1\n";
    PX_CHECK(import_file("kraken_quotes_2024-01-02_XBT-USD.csv", quotes, "", ImportOptions{}, t, why));
    PX_CHECK(t.h.name == "kraken-xbt-usd-2024-01-02" && t.h.notes.find("1 level a side") != std::string::npos);
    kinds.clear();
    for (const TapeEvent& e : t.ev) kinds += e.kind;
    PX_CHECK(kinds == "LLLL" && t.ev[2].qty == 0 && t.ev[3].price == 84001 && t.ev[3].qty == 1);

    // The companion each book file names; a LOBSTER file names its orderbook.
    PX_CHECK(companions_of("d/x_quotes_2024-01-02_A.csv", "")[0].path == "d/x_trades_2024-01-02_A.csv");
    PX_CHECK(companions_of("x_book_snapshot_25_2024-01-02_A.csv", "tardis")[0].path == "x_trades_2024-01-02_A.csv");
    PX_CHECK(companions_of("x_trades_2024-01-02_A.csv", "").empty());
    PX_CHECK(companions_of("A_1_2_3_message_5.csv", "tardis").empty() &&
             companions_of("A_1_2_3_message_5.csv", "")[0].path == "A_1_2_3_orderbook_5.csv");

    // Several symbols need symbol=; a bad side or a type with no book is refused.
    std::string two = trades + "demo,OTHER,1704186001400000,1704186001400000,1005,buy,1.0,1\n";
    PX_CHECK(!import_file("x.csv", two, "", ImportOptions{}, t, why) && why.find("2 symbols (DEMO, OTHER)") != std::string::npos);
    ImportOptions other;
    other.symbol = "OTHER";
    PX_CHECK(import_file("x.csv", two, "", other, t, why) && t.ev.size() == 1 && t.h.skipped == 4);
    std::string bad = book;
    bad.replace(bad.find(",true,ask,"), 10, ",true,mid,");
    PX_CHECK(!import_file(path, bad, "", ImportOptions{}, t, why) && why.find("line 2: side 'mid'") == 0);
    PX_CHECK(!import_file("x.csv", "exchange,symbol,timestamp,local_timestamp,funding_rate\n", "", ImportOptions{}, t, why) &&
             why.find("no book or trades") != std::string::npos);
    std::string bad_trades = trades;
    bad_trades.replace(bad_trades.find("42001.0"), 7, "42oo1.0");
    PX_CHECK(!import_file(path, book, bad_trades, ImportOptions{}, t, why) && why.find("the trades file beside it: line 2") == 0);
}

// Binance: data.binance.vision's files, with and without their headers.
void test_binance() {
    std::string book, trades, spot, why;
    const std::string path = kSamples + "BTCUSDT-bookTicker-2024-01-02.csv";
    PX_CHECK(read_file(path, book) && read_file(kSamples + "BTCUSDT-trades-2024-01-02.csv", trades) &&
             read_file(kSamples + "ETHUSDT-trades-2025-01-02.csv", spot));
    auto first = [](const std::string& x) { return std::string_view(x).substr(0, x.find('\n')); };
    PX_CHECK(detect_format(path, first(book)) == "binance" && detect_format("x.csv", first(trades)) == "binance");
    PX_CHECK(detect_format("ETHUSDT-trades-2025-01-02.csv", first(spot)) == "binance");
    PX_CHECK(detect_format("eth.csv", first(spot)) == "csv"); // no header and no Binance name: the generic reader's
    const std::vector<Companion> c = companions_of(path, "");
    PX_CHECK(c.size() == 2 && c[0].path == kSamples + "BTCUSDT-trades-2024-01-02.csv" &&
             c[1].path == kSamples + "BTCUSDT-aggTrades-2024-01-02.csv");
    Tape t;
    PX_CHECK(import_file(path, book, trades, ImportOptions{}, t, why));
    PX_CHECK(t.h.format == "binance" && t.h.depth == Depth::L2 && t.h.instrument == "BTCUSDT" && t.h.date == "2024-01-02" &&
             t.h.name == "btcusdt-bookticker-2024-01-02" && t.h.notes == "binance bookTicker, with its trades file");
    PX_CHECK(t.h.price_scale == 10 && t.h.tick == 1 && t.h.qty_scale == 10);
    std::string kinds;
    for (const TapeEvent& e : t.ev) kinds += e.kind;
    PX_CHECK(kinds == "LLTLTLLLLTL"); // each trade between the quote before it and the quote it left
    PX_CHECK(t.ev[2].side == 'S' && t.ev[2].price == 420001 && t.ev[2].qty == 3 && t.ev[4].side == 'B');
    TapeBook b;
    for (const TapeEvent& e : t.ev) b.apply(e);
    PX_CHECK((b.top(0, 3) == std::vector<std::pair<std::int64_t, std::int64_t>>{{420002, 1}}));
    PX_CHECK((b.top(1, 3) == std::vector<std::pair<std::int64_t, std::int64_t>>{{420003, 21}}));
    PX_CHECK(import_file(path, book, "", ImportOptions{}, t, why) && t.h.notes.find("no trades file beside it") != std::string::npos);

    // Spot: no header, microseconds, is_buyer_maker as True and False.
    PX_CHECK(import_file(kSamples + "ETHUSDT-trades-2025-01-02.csv", spot, "", ImportOptions{}, t, why));
    PX_CHECK(t.h.depth == Depth::Trades && t.h.notes == "binance trades (spot, no header)" && t.h.instrument == "ETHUSDT");
    PX_CHECK(t.ev.size() == 4 && t.ev[0].side == 'S' && t.ev[1].side == 'B' && t.ev[3].t - t.ev[0].t == 250'000'000);
    // aggTrades, spot and futures; the same rows in milliseconds read the same.
    const std::string agg = "26129,0.01633102,4.70443515,27781,27781,1498793709153,True,True\n"
                            "26130,0.01633103,1.00000000,27782,27783,1498793709200,False,True\n";
    PX_CHECK(import_file("LTCBTC-aggTrades-2017-06-30.csv", agg, "", ImportOptions{}, t, why) && t.ev.size() == 2 &&
             t.h.notes.find("one taker order's trades at one price") != std::string::npos && t.h.date == "2017-06-30");
    PX_CHECK(t.ev[1].t - t.ev[0].t == 47'000'000 && t.ev[0].side == 'S');
    const std::string fagg = "agg_trade_id,price,quantity,first_trade_id,last_trade_id,transact_time,is_buyer_maker\n"
                             "1,100.5,2,10,11,1704186000000,false\n";
    PX_CHECK(import_file("x.csv", fagg, "", ImportOptions{}, t, why) && t.h.format == "binance" && t.ev[0].side == 'B');
    // A spot file renamed is read by its width when the format is named.
    ImportOptions bin;
    bin.format = "binance";
    PX_CHECK(import_file("eth.csv", spot, "", bin, t, why) && t.ev.size() == 4);
    PX_CHECK(!import_file("eth.csv", "1,2,3\n", "", bin, t, why) && why.find("not a Binance file") == 0);

    // A bad flag or time is refused, naming the line; so is a bad trades file beside a book.
    std::string bad = spot;
    bad.replace(bad.find(",False,"), 7, ",Maybe,");
    PX_CHECK(!import_file("ETHUSDT-trades-2025-01-02.csv", bad, "", ImportOptions{}, t, why) &&
             why.find("line 2: is_buyer_maker 'Maybe'") == 0);
    std::string late = trades;
    late.replace(late.find("1704186000019"), 13, "2024-01-02x");
    PX_CHECK(!import_file(path, book, late, ImportOptions{}, t, why) && why.find("the trades file beside it: line 3: time") == 0);
    std::string narrow = book + "1,2,3\n";
    PX_CHECK(!import_file(path, narrow, "", ImportOptions{}, t, why) && why.find("line 7: 3 fields") == 0);
}

// JSON, read without floating point: numbers stay text.
void test_json() {
    Json j;
    std::string why;
    PX_CHECK(parse_json(R"( {"a": [1, -2.50, "x\"yé"], "b": {"c": null, "d": true}, "e": 1e3} )", j, why));
    PX_CHECK(j.is_object() && j.get("a")->items.size() == 3 && j.get("a")->items[1].text == "-2.50");
    PX_CHECK(j.get("a")->items[2].text == "x\"y\xc3\xa9" && j.get("b")->str("d") == "true" && j.get("b")->str("c").empty());
    PX_CHECK(j.str("e") == "1e3" && j.get("nope") == nullptr);
    PX_CHECK(!parse_json("{\"a\": 1,}", j, why) && why.find("JSON: ") == 0);
    PX_CHECK(!parse_json("[1] 2", j, why) && why.find("after the value") != std::string::npos);
    PX_CHECK(!parse_json("\"abc", j, why) && why.find("no closing quote") != std::string::npos);
    PX_CHECK(!parse_json(std::string(100, '[') + std::string(100, ']'), j, why) && why.find("too deep") != std::string::npos);
}

// Coinbase: a captured feed, the full channel, REST trades, Advanced Trade.
void test_coinbase() {
    std::string feed, why;
    const std::string path = kSamples + "coinbase_feed.jsonl";
    PX_CHECK(read_file(path, feed));
    PX_CHECK(detect_format(path, feed) == "coinbase");
    Tape t;
    PX_CHECK(import_file(path, feed, "", ImportOptions{}, t, why));
    PX_CHECK(t.h.format == "coinbase" && t.h.depth == Depth::L2 && t.h.instrument == "BTC-USD" && t.h.date == "2024-01-02");
    PX_CHECK(t.h.skipped == 3 && t.h.skipped_why.find("1 last_match messages") != std::string::npos);
    std::string kinds;
    for (const TapeEvent& e : t.ev) kinds += e.kind;
    PX_CHECK(kinds == "LLLLLTLTLL");
    // The side a match gives is the resting order's: a resting sell was lifted by a buyer.
    PX_CHECK(t.ev[5].side == 'B' && t.ev[5].price == 4200002 && t.ev[5].qty == 25 && t.ev[7].side == 'S');
    PX_CHECK(t.ev[0].t == t.ev[4].t); // the snapshot carries no time: it takes the next message's
    TapeBook b;
    for (const TapeEvent& e : t.ev) b.apply(e);
    PX_CHECK((b.top(0, 3) == std::vector<std::pair<std::int64_t, std::int64_t>>{{4200000, 150}}));
    PX_CHECK((b.top(1, 3) == std::vector<std::pair<std::int64_t, std::int64_t>>{{4200002, 50}, {4200005, 200}}));
    // The same match on two channels counts once.
    std::string twice = feed + feed.substr(feed.find("{\"type\":\"match\""), feed.find('\n', feed.find("{\"type\":\"match\"")) -
                                                                                  feed.find("{\"type\":\"match\"") + 1);
    PX_CHECK(import_file(path, twice, "", ImportOptions{}, t, why) && t.ev.size() == 10);

    // The full channel: order by order, its level2 rows left out.
    const std::string full =
        R"({"type":"received","time":"2024-01-02T09:00:00.000000Z","product_id":"ETH-USD","order_id":"o1","size":"1.0","price":"2300.00","side":"buy","order_type":"limit"})"
        "\n"
        R"({"type":"open","time":"2024-01-02T09:00:00.000100Z","product_id":"ETH-USD","order_id":"o1","price":"2300.00","remaining_size":"1.0","side":"buy"})"
        "\n"
        R"({"type":"open","time":"2024-01-02T09:00:00.000200Z","product_id":"ETH-USD","order_id":"o2","price":"2300.50","remaining_size":"2.0","side":"sell"})"
        "\n"
        R"({"type":"match","time":"2024-01-02T09:00:00.000300Z","product_id":"ETH-USD","trade_id":1,"maker_order_id":"o2","taker_order_id":"t9","side":"sell","size":"0.5","price":"2300.50"})"
        "\n"
        R"({"type":"done","time":"2024-01-02T09:00:00.000300Z","product_id":"ETH-USD","order_id":"t9","reason":"filled","side":"buy"})"
        "\n"
        R"({"type":"change","time":"2024-01-02T09:00:00.000400Z","product_id":"ETH-USD","order_id":"o2","new_size":"1.0","old_size":"1.5","price":"2300.50","side":"sell"})"
        "\n"
        R"({"type":"change","time":"2024-01-02T09:00:00.000500Z","product_id":"ETH-USD","order_id":"o1","new_price":"2300.25","old_price":"2300.00","new_size":"1.0","old_size":"1.0","side":"buy"})"
        "\n"
        R"({"type":"done","time":"2024-01-02T09:00:00.000600Z","product_id":"ETH-USD","order_id":"zz","reason":"canceled","remaining_size":"3","price":"2299","side":"buy"})"
        "\n"
        R"({"type":"done","time":"2024-01-02T09:00:00.000700Z","product_id":"ETH-USD","order_id":"o2","reason":"canceled","remaining_size":"1.0","price":"2300.50","side":"sell"})"
        "\n"
        R"({"type":"snapshot","time":"2024-01-02T09:00:00.000800Z","product_id":"ETH-USD","bids":[["2300.25","1.0"]],"asks":[["2301","4"]]})"
        "\n";
    PX_CHECK(import_file("eth.jsonl", full, "", ImportOptions{}, t, why));
    PX_CHECK(t.h.depth == Depth::L3 && t.h.notes.find("order by order") != std::string::npos &&
             t.h.notes.find("1 message about orders resting before the capture began") != std::string::npos);
    PX_CHECK(t.h.skipped_why.find("2 level2 rows") != std::string::npos);
    kinds.clear();
    for (const TapeEvent& e : t.ev) kinds += e.kind;
    PX_CHECK(kinds == "AATCCAC"); // a smaller size keeps its place; a new price leaves and comes back
    PX_CHECK(t.ev[2].id == t.ev[1].id && t.ev[2].side == 'B' && t.ev[3].qty == 5 && t.ev[4].id == t.ev[0].id);
    TapeBook l3;
    for (const TapeEvent& e : t.ev) l3.apply(e);
    PX_CHECK((l3.top(0, 3) == std::vector<std::pair<std::int64_t, std::int64_t>>{{9201, 10}}) && l3.top(1, 3).empty()); // 2300.25, in ticks of 0.25

    // REST trades from Exchange, newest first; the side is the maker's.
    const std::string rest = "[\n  {\"time\": \"2024-01-02T09:00:01.000Z\", \"trade_id\": 3, \"price\": \"100.02\", "
                             "\"size\": \"1\", \"side\": \"sell\"},\n  {\"time\": \"2024-01-02T09:00:00.500Z\", "
                             "\"trade_id\": 2, \"price\": \"100.00\", \"size\": \"2\", \"side\": \"buy\"}\n]\n";
    PX_CHECK(detect_format("trades.json", rest) == "coinbase");
    PX_CHECK(import_file("trades.json", rest, "", ImportOptions{}, t, why) && t.h.depth == Depth::Trades && t.ev.size() == 2);
    PX_CHECK(t.ev[0].side == 'S' && t.ev[1].side == 'B' && t.ev[1].price - t.ev[0].price == 1 && t.h.tick == 2 &&
             t.h.notes.find("the aggressor is the other") != std::string::npos);
    // Advanced Trade: REST and feed; its trade side is not read.
    const std::string at_rest = R"({"trades":[{"trade_id":"9","product_id":"BTC-USD","price":"42000","size":"0.1","time":"2024-01-02T09:00:00Z","side":"BUY","bid":"","ask":"","exchange":"coinbase"}],"best_bid":"41999","best_ask":"42001"})";
    PX_CHECK(import_file("t.json", at_rest, "", ImportOptions{}, t, why) && t.ev.size() == 1 && t.ev[0].side == '-' &&
             t.h.instrument == "BTC-USD" && t.h.notes.find("advanced trade") != std::string::npos);
    const std::string at_feed =
        R"({"channel":"l2_data","client_id":"","timestamp":"2024-01-02T09:00:00.000000001Z","sequence_num":0,"events":[{"type":"snapshot","product_id":"BTC-USD","updates":[{"side":"bid","event_time":"1970-01-01T00:00:00Z","price_level":"42000.00","new_quantity":"1.5"},{"side":"offer","event_time":"1970-01-01T00:00:00Z","price_level":"42000.10","new_quantity":"0.5"}]}]})"
        "\n"
        R"({"channel":"market_trades","client_id":"","timestamp":"2024-01-02T09:00:00.100Z","sequence_num":1,"events":[{"type":"snapshot","trades":[{"trade_id":"1","product_id":"BTC-USD","price":"41999","size":"1","side":"BUY","time":"2024-01-02T08:59:00Z"}]}]})"
        "\n"
        R"({"channel":"market_trades","client_id":"","timestamp":"2024-01-02T09:00:00.200Z","sequence_num":2,"events":[{"type":"update","trades":[{"trade_id":"2","product_id":"BTC-USD","price":"42000.10","size":"0.2","side":"BUY","time":"2024-01-02T09:00:00.199Z"}]}]})"
        "\n"
        R"({"channel":"l2_data","client_id":"","timestamp":"2024-01-02T09:00:00.201Z","sequence_num":3,"events":[{"type":"update","product_id":"BTC-USD","updates":[{"side":"offer","event_time":"2024-01-02T09:00:00.199Z","price_level":"42000.10","new_quantity":"0.3"}]}]})"
        "\n"
        R"({"channel":"heartbeats","client_id":"","timestamp":"2024-01-02T09:00:00.300Z","sequence_num":4,"events":[{"current_time":"x","heartbeat_counter":"1"}]})"
        "\n";
    PX_CHECK(import_file("at.jsonl", at_feed, "", ImportOptions{}, t, why) && t.h.depth == Depth::L2 && t.h.skipped == 2);
    kinds.clear();
    for (const TapeEvent& e : t.ev) kinds += e.kind;
    PX_CHECK(kinds == "LLTL" && t.ev[2].side == '-');

    // Two products need symbol=; a broken line is refused with its number.
    std::string two = feed + R"({"type":"match","trade_id":7,"maker_order_id":"x","taker_order_id":"y","side":"buy","size":"1","price":"10","product_id":"ETH-USD","time":"2024-01-02T09:00:01Z"})" "\n";
    PX_CHECK(!import_file(path, two, "", ImportOptions{}, t, why) && why.find("2 products (BTC-USD, ETH-USD)") != std::string::npos);
    ImportOptions eth;
    eth.symbol = "ETH-USD";
    PX_CHECK(import_file(path, two, "", eth, t, why) && t.ev.size() == 1 && t.h.instrument == "ETH-USD");
    std::string broken = feed;
    broken.insert(broken.find("{\"type\":\"heartbeat\""), "{\"type\":");
    PX_CHECK(!import_file(path, broken, "", ImportOptions{}, t, why) && why.find("line 7: JSON") == 0);
    PX_CHECK(!import_file(path, "{\"type\":\"snapshot\",\"product_id\":\"A\",\"bids\":[],\"asks\":[[\"1\",\"1\"]]}\n", "",
                          ImportOptions{}, t, why) &&
             why == "no message carries a time");
}

// Kraken: the trade history, REST trades and a WebSocket v2 feed.
void test_kraken() {
    std::string hist, why;
    const std::string path = kSamples + "XBTUSD.csv";
    PX_CHECK(read_file(path, hist));
    PX_CHECK(detect_format(path, hist) == "kraken" && detect_format("xbtusd.csv", hist) == "csv" &&
             detect_format("XBTUSD_60.csv", "1704186000,1,2,3,4,5,6") == "csv");
    Tape t;
    PX_CHECK(import_file(path, hist, "", ImportOptions{}, t, why));
    PX_CHECK(t.h.format == "kraken" && t.h.depth == Depth::Trades && t.h.instrument == "XBTUSD" && t.h.name == "xbtusd" &&
             t.h.date == "2024-01-02" && t.ev.size() == 5 && t.ev[0].side == '-' && t.ev[4].t - t.ev[0].t == 7'000'000'000);
    PX_CHECK(t.h.notes.find("prints have no side") != std::string::npos);
    ImportOptions kr;
    kr.format = "kraken";
    PX_CHECK(!import_file("XBTUSD_60.csv", "1704186000,1,2,3,4,5,6\n", "", kr, t, why) && why.find("OHLCVT bars") != std::string::npos);

    const std::string rest = R"({"error":[],"result":{"XXBTZUSD":[["42000.10000","0.01000000",1704186000.1234,"b","m","",101],)"
                             R"(["41999.90000","0.50000000",1704186000.5,"s","l","",102]],"last":"1704186000500000000"}})";
    PX_CHECK(detect_format("trades.json", rest) == "kraken");
    PX_CHECK(import_file("trades.json", rest, "", ImportOptions{}, t, why) && t.h.instrument == "XXBTZUSD" && t.ev.size() == 2);
    PX_CHECK(t.ev[0].side == 'B' && t.ev[1].side == 'S' && t.ev[1].t - t.ev[0].t == 376'600'000);
    PX_CHECK(!import_file("e.json", R"({"error":["EQuery:Unknown asset pair"],"result":{}})", "", ImportOptions{}, t, why) &&
             why == "Kraken returned an error: EQuery:Unknown asset pair");

    const std::string feed =
        R"({"method":"subscribe","result":{"channel":"book","symbol":"BTC/USD"},"success":true})" "\n"
        R"({"channel":"book","type":"snapshot","data":[{"symbol":"BTC/USD","bids":[{"price":42000.0,"qty":1.5}],"asks":[{"price":42000.1,"qty":0.5}],"checksum":1}]})" "\n"
        R"({"channel":"trade","type":"update","data":[{"symbol":"BTC/USD","side":"buy","price":42000.1,"qty":0.2,"ord_type":"market","trade_id":7,"timestamp":"2024-01-02T09:00:00.200000Z"}]})" "\n"
        R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD","bids":[],"asks":[{"price":42000.1,"qty":0.3}],"checksum":2,"timestamp":"2024-01-02T09:00:00.200100Z"}]})" "\n"
        R"({"channel":"heartbeat"})" "\n"
        R"({"channel":"book","type":"snapshot","data":[{"symbol":"BTC/USD","bids":[{"price":42000.0,"qty":1.0}],"asks":[{"price":42000.2,"qty":2}],"checksum":3}]})" "\n"
        R"({"channel":"book","type":"update","data":[{"symbol":"BTC/USD","bids":[{"price":42000.0,"qty":0}],"asks":[],"checksum":4,"timestamp":"2024-01-02T09:00:01Z"}]})" "\n";
    PX_CHECK(detect_format("k.jsonl", feed) == "kraken");
    PX_CHECK(import_file("k.jsonl", feed, "", ImportOptions{}, t, why));
    PX_CHECK(t.h.depth == Depth::L2 && t.h.instrument == "BTC/USD" && t.h.skipped == 2);
    std::string kinds;
    for (const TapeEvent& e : t.ev) kinds += e.kind;
    PX_CHECK(kinds == "LLTLRLLL" && t.ev[2].side == 'B');
    PX_CHECK(t.ev[0].t == t.ev[2].t && t.ev[4].t == t.ev[7].t); // untimed snapshots take the next time
    TapeBook b;
    for (const TapeEvent& e : t.ev) b.apply(e);
    PX_CHECK(b.top(0, 3).empty() && (b.top(1, 3) == std::vector<std::pair<std::int64_t, std::int64_t>>{{420002, 20}}));
    std::string two = feed + R"({"channel":"trade","type":"update","data":[{"symbol":"ETH/USD","side":"sell","price":1,"qty":1,"trade_id":1,"timestamp":"2024-01-02T09:00:02Z"}]})" "\n";
    PX_CHECK(!import_file("k.jsonl", two, "", ImportOptions{}, t, why) && why.find("2 symbols (BTC/USD, ETH/USD)") != std::string::npos);
}

// A market file round trip, its refusals, and the suite it becomes.
void test_market_file() {
    namespace su = px::arena::suite;
    std::string why;
    Calibration c;
    c.params = MarketParams{3, 40, 20'000};
    for (const Knob k : kKnobs) {
        Fit& f = c.fits[static_cast<std::size_t>(k)];
        f.knob = k;
        f.used = true;
        f.target = 100;
        f.achieved = 103;
        f.distance = 30;
    }
    ControlResult control;
    control.truth = control_truth();
    control.calibration.params = control_truth();
    for (bool& r : control.recovered) r = true;
    HealthResult health;
    health.block = "standard";
    health.seeds = health.passed = 64;
    const std::string text = market_file_text("demo", small_tape(), "1 2", c, control, health);

    MarketFile m;
    PX_CHECK(parse_market(text, m, why) && m.name == "demo");
    PX_CHECK(m.sets.size() == 4 && m.sets[0] == std::make_pair(std::string("maker.half_spread_ticks"), std::int64_t{3}) &&
             m.sets[1].second == 3 && m.sets[2].second == 40 && m.sets[3].second == 20'000);
    PX_CHECK(m.calibration.find("calibration.tape_cksum=1 2\n") != std::string::npos);
    PX_CHECK(m.calibration.find("set.") == std::string::npos && m.calibration.find("stat.") == std::string::npos);

    // The suite it becomes: the standard one with four values moved, named for the market.
    su::SuiteDef def = su::standard_suite();
    PX_CHECK(market_suite(m, def, why) && std::string(def.name) == "demo");
    PX_CHECK(def.roster.maker_half_spread_ticks == 3 && def.roster.rival_half_spread_ticks == 3 &&
             def.roster.noise_qty == 40 && def.roster.noise_timer_ns == 20'000);
    PX_CHECK(std::string(def.change) ==
             "maker.half_spread_ticks=3, rival.half_spread_ticks=3, noise.qty=40, noise.timer_ns=20000");
    const su::SeedBlock& block = su::kSeedBlocks[1];
    const std::string cond = su::conditions_text(def, block);
    PX_CHECK(cond.find("calibration.market=demo\n") != std::string::npos);
    // A built-in suite's conditions carry no calibration line, so its pinned fingerprint cannot move.
    for (const std::string& n : su::split_names(su::kSuiteNames)) {
        su::SuiteDef b = su::standard_suite();
        PX_CHECK(su::find_suite(n, b) && su::conditions_text(b, block).find("calibration.") == std::string::npos);
    }
    // A run's own record gives the same market back, without the file or the tape.
    su::RunInfo info;
    info.market_file = "markets/demo.cfg";
    const std::string cfg = su::suite_cfg_text(def, block, info);
    PX_CHECK(cfg.find("run.market_file=markets/demo.cfg\n") != std::string::npos);
    MarketFile r;
    su::SuiteDef again = su::standard_suite();
    PX_CHECK(market_from_record(cfg, r, why) && market_suite(r, again, why));
    PX_CHECK(su::condition_lines(su::suite_cfg_text(again, block, {})) == su::condition_lines(cfg));
    PX_CHECK(su::condition_lines(su::suite_cfg_text(su::standard_suite(), block, {})) != su::condition_lines(cfg));
    PX_CHECK(!market_from_record(su::suite_cfg_text(su::standard_suite(), block, {}), r, why));

    // The standard values move nothing, and say so.
    Calibration same = c;
    same.params = reference_params();
    PX_CHECK(parse_market(market_file_text("plain", small_tape(), "1 2", same, control, health), m, why) &&
             market_suite(m, def, why) && *def.change == '\0');

    // What the reader refuses.
    auto with = [&](std::string from, std::string to) {
        std::string t = text;
        const std::size_t at = t.find(from);
        PX_CHECK(at != std::string::npos);
        if (at != std::string::npos) t.replace(at, from.size(), to);
        return t;
    };
    const std::pair<std::string, std::string> refused[] = {
        {"status=complete\n", ""},                                                   // incomplete
        {"control.held=1", "control.held=0"},                                       // the control failed
        {"health.passed=64", "health.passed=63"},                                   // a seed failed the gate
        {"set.noise.qty=40", "set.informed.edge_ticks=9"},                          // a parameter calibration never moves
        {"set.noise.qty=40\n", ""},                                                 // one missing
        {"set.noise.qty=40\n", "set.noise.qty=40\nset.noise.qty=41\n"},            // one set twice
        {"set.noise.qty=40", "set.noise.qty=0"},                                    // not a value
        {"calibration.base=standard", "calibration.base=wide"},                     // another suite
        {"calibration.version=1", "calibration.version=2"},                         // another format
        {"calibration.market=demo", "calibration.market=standard"},                 // a suite's name
        {"calibration.tape_cksum=1 2\n", ""},                                       // which tape, unsaid
    };
    for (const auto& [from, to] : refused) PX_CHECK(!parse_market(with(from, to), m, why) && !why.empty());
    PX_CHECK(!market_name_ok("all", why) && !market_name_ok("Bad", why) && market_name_ok("aapl-2012", why));

    // Calibration moves exactly the parameters a market may set, and nothing else.
    const su::SuiteDef base = su::standard_suite();
    const su::SuiteDef moved = market_with(base, MarketParams{7, 11, 13});
    std::string listed = std::string(" ") + kMarketParamNames + " ";
    for (const std::string& k : su::split_names(px::arena::kRosterParamNames)) {
        std::int64_t a = 0, b = 0;
        PX_CHECK(px::arena::get_roster_param(base.roster, k, a) && px::arena::get_roster_param(moved.roster, k, b));
        PX_CHECK((a != b) == (listed.find(" " + k + " ") != std::string::npos));
    }
}

} // namespace

int main() {
    test_numbers();
    test_times();
    test_csv_and_cksum();
    test_tape_round_trip();
    test_generic_csv();
    test_lobster();
    test_databento();
    test_tardis();
    test_binance();
    test_json();
    test_coinbase();
    test_kraken();
    test_stats();
    test_session_tape();
    test_calibration_parts();
    test_market_file();
    PX_TEST_REPORT_AND_RETURN();
}
