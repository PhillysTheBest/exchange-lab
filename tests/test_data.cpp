// tests/test_data.cpp — the tape format, the importers and the statistics
// calibration matches (docs/DATA.md), checked against answers known in advance.
//
// The samples in examples/data/ are synthetic: made up for these tests, so they
// carry no licence. The LOBSTER sample comes with its orderbook file, written by
// the same made-up market, so the book a tape rebuilds can be held to the book
// LOBSTER's own format says it should be after every message.
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
    PX_CHECK(s.size_depth_pml == 1000 && s.span_ns == 1000);
    // Read as trades only, the book is not measured at all.
    s = tape_stats(t, Depth::Trades);
    PX_CHECK(s.depth == Depth::Trades && s.spread_ticks == -1 && s.touch_depth == -1 && s.print_qty == 10);

    // Prints per move: the price is 100 for 20 prints, 101 for 20, then 103. Two windows
    // of 20 prints move 1 and 2 ticks: a variance of 2.5 ticks^2, so 8 prints a tick.
    Tape p;
    p.h.depth = Depth::Trades;
    for (int i = 0; i <= 40; ++i)
        p.ev.push_back({i * 10, 'T', i % 2 == 0 ? 'B' : 'S', 0, i < 20 ? 100 : i < 40 ? 101 : 103, 1});
    s = tape_stats(p);
    PX_CHECK(s.prints == 41 && s.move_windows == 2 && s.move_sumsq == 20 && s.prints_per_move_x100 == 800);
    PX_CHECK(!s.enough_prints());
    // A reset closes the windows: none may span two sessions.
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

} // namespace

int main() {
    test_numbers();
    test_times();
    test_csv_and_cksum();
    test_tape_round_trip();
    test_generic_csv();
    test_lobster();
    test_stats();
    PX_TEST_REPORT_AND_RETURN();
}
