// marketdata/importers.hpp — one importer per outside format, each producing a
// tape (marketdata/tape.hpp).
//
// An importer reads the source's rows into RawEvents, with prices and
// quantities still as decimals and times as nanoseconds in whatever origin the
// source used. finish() then does what every importer needs and none should do
// differently: put prices on one integer scale, find the tick, convert prices
// to ticks, put the rows in time order and count time from the first one.
//
// A malformed file is refused, naming the line. An importer never guesses a
// price or a side it was not given: a side it cannot read is '-', and a row it
// cannot use for a stated reason (an auction trade, a halt) is counted in the
// tape's header rather than dropped silently.
#pragma once

#include "marketdata/json.hpp"
#include "marketdata/parse.hpp"
#include "marketdata/tape.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::data {

struct ImportOptions {
    std::string format;     // empty: detected from the file
    std::string name;       // empty: from the file name
    std::string columns;    // generic CSV: the role of each column, in order
    TimeFormat time = TimeFormat::Auto;
    std::string tick;       // a decimal in currency units; empty: found from the prices
    std::string symbol;     // a file holding several instruments: which one
    std::string instrument; // the instrument's name when the file does not say
};

struct RawEvent {
    std::int64_t t = 0;
    char kind = 'T';
    char side = '-';
    std::uint64_t id = 0;
    Decimal price{};
    Decimal qty{};
    std::uint64_t line = 0; // in the source, for error messages
};

// What an importer hands to finish(): the rows, and what it learned about them.
struct RawTape {
    TapeHeader h; // name, instrument, date, format, importer, depth, notes, skipped
    std::vector<RawEvent> ev;
    TimeKind kind = TimeKind::Relative;
    std::map<std::string, std::uint64_t> skipped; // why -> how many
    bool book_seeded = false;
};

[[nodiscard]] inline std::string line_ref(std::uint64_t line) { return "line " + std::to_string(line) + ": "; }

[[nodiscard]] inline std::string skipped_text(const std::map<std::string, std::uint64_t>& s) {
    std::string out;
    for (const auto& [why, n] : s) {
        if (!out.empty()) out += ", ";
        out += grouped(static_cast<std::int64_t>(n)) + " " + why;
    }
    return out;
}

// The common second half of every importer. `opts.tick`, if given, must hold
// every price exactly; otherwise the tick is the largest step that does.
[[nodiscard]] inline bool finish(RawTape& raw, const ImportOptions& opts, Tape& out, std::string& why) {
    if (raw.ev.empty()) {
        why = "no usable rows";
        if (!raw.skipped.empty()) why += " (skipped " + skipped_text(raw.skipped) + ")";
        return false;
    }
    // Time order. A file written newest first is reversed; one with rows out of
    // order is sorted, stably, and the header says how many moved.
    bool ascending = true, descending = true;
    std::uint64_t backwards = 0;
    for (std::size_t i = 1; i < raw.ev.size(); ++i) {
        if (raw.ev[i].t < raw.ev[i - 1].t) {
            ascending = false;
            ++backwards;
        }
        if (raw.ev[i].t > raw.ev[i - 1].t) descending = false;
    }
    std::string notes = raw.h.notes;
    auto note = [&notes](const std::string& s) {
        if (!notes.empty()) notes += "; ";
        notes += s;
    };
    if (!ascending && descending) {
        std::reverse(raw.ev.begin(), raw.ev.end());
        note("rows were newest first and were reversed");
    } else if (!ascending) {
        std::stable_sort(raw.ev.begin(), raw.ev.end(), [](const RawEvent& a, const RawEvent& b) { return a.t < b.t; });
        note(std::to_string(backwards) + " rows were out of time order and were sorted");
    }

    // One decimal scale for prices and one for quantities.
    int pd = 0, qd = 0;
    for (const RawEvent& e : raw.ev) {
        if (e.kind != 'R') pd = std::max(pd, needed_decimals(e.price));
        qd = std::max(qd, needed_decimals(e.qty));
    }
    Decimal tick_dec{};
    if (!opts.tick.empty()) {
        if (!parse_decimal(opts.tick, tick_dec) || tick_dec.mantissa <= 0) {
            why = "tick=" + opts.tick + " is not a positive number";
            return false;
        }
        pd = std::max(pd, needed_decimals(tick_dec));
    }
    if (pd > 9 || qd > 9) {
        why = std::string(pd > 9 ? "prices" : "quantities") + " have more than 9 decimals";
        return false;
    }
    std::vector<std::int64_t> px(raw.ev.size(), 0);
    std::int64_t g = 0;
    for (std::size_t i = 0; i < raw.ev.size(); ++i) {
        const RawEvent& e = raw.ev[i];
        if (e.kind == 'R') continue;
        if (!scale_to(e.price, pd, px[i]) || px[i] <= 0) {
            why = line_ref(e.line) + "price " + decimal_text(e.price.mantissa, e.price.decimals) + " is not usable";
            return false;
        }
        g = gcd(g, px[i]);
    }
    std::int64_t tick = g == 0 ? 1 : g;
    if (!opts.tick.empty()) {
        if (!scale_to(tick_dec, pd, tick) || tick <= 0) {
            why = "tick=" + opts.tick + " does not fit the prices' scale";
            return false;
        }
        for (std::size_t i = 0; i < raw.ev.size(); ++i) {
            if (raw.ev[i].kind != 'R' && px[i] % tick != 0) {
                why = line_ref(raw.ev[i].line) + "price " + decimal_text(px[i], pd) + " is not on the " +
                      decimal_text(tick, pd) + " grid given by tick=";
                return false;
            }
        }
    }

    Tape& t = out;
    t = Tape{};
    t.h = raw.h;
    t.h.price_scale = pow10(pd);
    t.h.tick = tick;
    t.h.qty_scale = pow10(qd);
    t.h.origin_ns = raw.ev.front().t;
    t.h.origin_kind = raw.kind;
    std::uint64_t skipped = 0;
    for (const auto& [w, n] : raw.skipped) skipped += n;
    t.h.skipped = skipped;
    t.h.skipped_why = skipped_text(raw.skipped);
    t.h.notes = notes;
    t.ev.reserve(raw.ev.size());
    for (std::size_t i = 0; i < raw.ev.size(); ++i) {
        const RawEvent& e = raw.ev[i];
        TapeEvent x;
        x.t = e.t - t.h.origin_ns;
        x.kind = e.kind;
        x.side = e.side;
        x.id = e.id;
        x.price = e.kind == 'R' ? 0 : px[i] / tick;
        if (!scale_to(e.qty, qd, x.qty) || x.qty < 0) {
            why = line_ref(e.line) + "quantity " + decimal_text(e.qty.mantissa, e.qty.decimals) + " is not usable";
            return false;
        }
        if ((x.kind == 'A' || x.kind == 'T') && x.qty == 0) {
            why = line_ref(e.line) + "a quantity of zero";
            return false;
        }
        t.ev.push_back(x);
    }
    return true;
}

// Splits a whole file into lines without copying, dropping a final empty one.
[[nodiscard]] inline std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view l = text.substr(at, end - at);
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        out.push_back(l);
        at = end + 1;
    }
    return out;
}

[[nodiscard]] inline std::string base_name(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

// ---- generic CSV -----------------------------------------------------------------

// The roles a column of a generic CSV can play.
enum class Role : std::uint8_t { Skip, Time, Price, Qty, Side, BuyerMaker, Bid, BidQty, Ask, AskQty, Symbol };

[[nodiscard]] inline bool role_from_word(std::string_view w, Role& r) noexcept {
    if (w == "-" || w == "skip") r = Role::Skip;
    else if (w == "time") r = Role::Time;
    else if (w == "price") r = Role::Price;
    else if (w == "qty" || w == "size" || w == "amount" || w == "volume") r = Role::Qty;
    else if (w == "side") r = Role::Side;
    else if (w == "buyer_maker") r = Role::BuyerMaker;
    else if (w == "bid") r = Role::Bid;
    else if (w == "bid_qty" || w == "bid_size") r = Role::BidQty;
    else if (w == "ask") r = Role::Ask;
    else if (w == "ask_qty" || w == "ask_size") r = Role::AskQty;
    else if (w == "symbol") r = Role::Symbol;
    else return false;
    return true;
}

// A header name's role, from the names exports commonly use.
[[nodiscard]] inline Role role_from_header(std::string_view raw) {
    const std::string h = lower(trim(raw));
    auto any = [&h](std::initializer_list<const char*> names) {
        for (const char* n : names)
            if (h == n) return true;
        return false;
    };
    if (any({"time", "timestamp", "ts", "datetime", "date_time", "ts_event", "transact_time", "trade_time",
             "epoch", "time_exchange", "exchange_time"}))
        return Role::Time;
    if (any({"price", "px", "trade_price", "last", "last_price"})) return Role::Price;
    if (any({"size", "qty", "quantity", "amount", "volume", "vol", "shares", "trade_size", "last_size", "base_amount"}))
        return Role::Qty;
    if (any({"side", "direction", "aggressor", "taker_side", "aggressor_side", "taker"})) return Role::Side;
    if (any({"is_buyer_maker", "isbuyermaker", "buyer_maker"})) return Role::BuyerMaker;
    if (any({"bid", "bid_price", "best_bid", "bid_px", "bidprice", "best_bid_price"})) return Role::Bid;
    if (any({"bid_size", "bid_qty", "bidsize", "best_bid_qty", "bid_amount", "best_bid_size"})) return Role::BidQty;
    if (any({"ask", "ask_price", "best_ask", "ask_px", "offer", "askprice", "best_ask_price", "offer_price"}))
        return Role::Ask;
    if (any({"ask_size", "ask_qty", "asksize", "best_ask_qty", "ask_amount", "best_ask_size", "offer_size"}))
        return Role::AskQty;
    if (any({"symbol", "ticker", "instrument", "product_id", "pair", "market"})) return Role::Symbol;
    return Role::Skip;
}

// An aggressor side as exports write it; '-' when it cannot be read.
[[nodiscard]] inline char side_from_text(std::string_view raw) {
    const std::string s = lower(trim(raw));
    if (s == "b" || s == "buy" || s == "bid" || s == "1" || s == "+1" || s == "long" || s == "bought") return 'B';
    if (s == "s" || s == "sell" || s == "ask" || s == "a" || s == "-1" || s == "short" || s == "offer" || s == "sold")
        return 'S';
    return '-';
}

// "is the buyer the maker": true means a seller crossed the spread.
[[nodiscard]] inline char side_from_buyer_maker(std::string_view raw) {
    const std::string s = lower(trim(raw));
    if (s == "true" || s == "1" || s == "t" || s == "yes" || s == "y") return 'S';
    if (s == "false" || s == "0" || s == "f" || s == "no" || s == "n") return 'B';
    return '-';
}

// A field that reads as a number or a time: a header row has at least one that does not.
[[nodiscard]] inline bool looks_numeric(std::string_view s) {
    Decimal d{};
    std::int64_t ns = 0;
    TimeKind k{};
    s = trim(s);
    return s.empty() || parse_decimal(s, d) || parse_time(s, TimeFormat::Auto, ns, k);
}

// Any delimited text file of trades, quotes or both. A row with a price is a
// trade; a row with a bid or an ask moves the best quote on that side. Columns
// are recognised from the header row, or named with columns=.
[[nodiscard]] inline bool import_csv(const std::string& path, std::string_view text, const ImportOptions& opts,
                                     RawTape& raw, std::string& why) {
    const std::vector<std::string_view> lines = lines_of(text);
    std::size_t first = 0;
    while (first < lines.size() && trim(lines[first]).empty()) ++first;
    if (first == lines.size()) {
        why = "the file is empty";
        return false;
    }
    const char delim = guess_delimiter(lines[first]);
    std::vector<std::string> f;
    if (!split_csv(lines[first], delim, f)) {
        why = line_ref(first + 1) + "an unclosed quote";
        return false;
    }
    // A header row names a column the way exports do, or is mostly words; a data
    // row is mostly numbers and times, even with a side written as a word.
    std::size_t words = 0, numbers = 0;
    bool known_name = false;
    for (const std::string& x : f) {
        (looks_numeric(x) ? numbers : words) += 1;
        known_name = known_name || role_from_header(x) != Role::Skip;
    }
    const bool header = known_name || words > numbers;

    std::vector<Role> roles;
    if (!opts.columns.empty()) {
        std::vector<std::string> words;
        if (!split_csv(opts.columns, ',', words)) words.clear();
        for (const std::string& w : words) {
            Role r{};
            if (!role_from_word(trim(w), r)) {
                why = "columns=: '" + w +
                      "' is not a role (time, price, qty, side, buyer_maker, bid, bid_qty, ask, ask_qty, symbol, -)";
                return false;
            }
            roles.push_back(r);
        }
    } else if (header) {
        for (const std::string& x : f) roles.push_back(role_from_header(x));
    } else {
        why = "this file has no header row, so say which column is which, for example "
              "columns=time,price,qty,side (use - for a column to ignore)";
        return false;
    }
    auto col = [&roles](Role r) {
        for (std::size_t i = 0; i < roles.size(); ++i)
            if (roles[i] == r) return static_cast<int>(i);
        return -1;
    };
    const int c_time = col(Role::Time), c_price = col(Role::Price), c_qty = col(Role::Qty), c_side = col(Role::Side),
              c_bm = col(Role::BuyerMaker), c_bid = col(Role::Bid), c_bidq = col(Role::BidQty),
              c_ask = col(Role::Ask), c_askq = col(Role::AskQty), c_sym = col(Role::Symbol);
    if (c_time < 0 || (c_price < 0 && c_bid < 0 && c_ask < 0)) {
        std::string seen;
        if (header)
            for (const std::string& x : f) seen += (seen.empty() ? "" : ", ") + std::string(trim(x));
        why = std::string("cannot tell which column is the ") + (c_time < 0 ? "time" : "price") +
              (header ? " (the header has: " + seen + ")" : "") +
              "; name them with columns=, for example columns=time,price,qty,side";
        return false;
    }

    raw.h.format = "csv";
    raw.h.importer = "csv 1";
    raw.h.source_file = base_name(path);

    // First every row as written, then in time order, then into events: a quote
    // row's meaning depends on the quote before it in time, which in a file
    // written newest first is the row after it.
    struct Row {
        std::int64_t t = 0;
        std::uint64_t line = 0;
        bool trade = false, bid = false, ask = false;
        Decimal price{}, qty{1, 0}, bid_px{}, bid_qty{1, 0}, ask_px{}, ask_qty{1, 0};
        char side = '-';
    };
    std::vector<Row> rows;
    std::set<std::string> symbols;
    for (std::size_t li = first + (header ? 1 : 0); li < lines.size(); ++li) {
        if (trim(lines[li]).empty()) continue;
        const std::uint64_t line = li + 1;
        if (!split_csv(lines[li], delim, f)) {
            why = line_ref(line) + "an unclosed quote";
            return false;
        }
        if (f.size() < roles.size()) {
            why = line_ref(line) + std::to_string(f.size()) + " fields, expected " + std::to_string(roles.size());
            return false;
        }
        auto field = [&f](int c) { return c < 0 ? std::string_view{} : trim(f[static_cast<std::size_t>(c)]); };
        auto number = [&](int c, const char* what, Decimal& d) {
            if (!parse_decimal(field(c), d)) {
                why = line_ref(line) + what + " '" + std::string(field(c)) + "' is not a number";
                return false;
            }
            return true;
        };
        if (c_sym >= 0) {
            const std::string sym(field(c_sym));
            if (!opts.symbol.empty() && sym != opts.symbol) {
                ++raw.skipped["rows for other symbols"];
                continue;
            }
            symbols.insert(sym);
        }
        Row r;
        r.line = line;
        TimeKind kind{};
        if (!parse_time(field(c_time), opts.time, r.t, kind)) {
            why = line_ref(line) + "time '" + std::string(field(c_time)) +
                  "' is not a time this importer reads (try time=s, ms, us, ns, iso or clock)";
            return false;
        }
        raw.kind = kind;
        if (c_price >= 0 && !field(c_price).empty()) {
            r.trade = true;
            if (!number(c_price, "price", r.price)) return false;
            if (c_qty >= 0 && !number(c_qty, "quantity", r.qty)) return false;
            r.side = c_side >= 0 ? side_from_text(field(c_side)) : c_bm >= 0 ? side_from_buyer_maker(field(c_bm)) : '-';
        }
        if (c_bid >= 0 && !field(c_bid).empty()) {
            r.bid = true;
            if (!number(c_bid, "bid", r.bid_px)) return false;
            if (c_bidq >= 0 && !field(c_bidq).empty() && !number(c_bidq, "bid size", r.bid_qty)) return false;
        }
        if (c_ask >= 0 && !field(c_ask).empty()) {
            r.ask = true;
            if (!number(c_ask, "ask", r.ask_px)) return false;
            if (c_askq >= 0 && !field(c_askq).empty() && !number(c_askq, "ask size", r.ask_qty)) return false;
        }
        if (!r.trade && !r.bid && !r.ask) {
            ++raw.skipped["rows with no price, bid or ask"];
            continue;
        }
        rows.push_back(r);
    }

    bool ascending = true, descending = true;
    std::uint64_t backwards = 0;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].t < rows[i - 1].t) {
            ascending = false;
            ++backwards;
        }
        if (rows[i].t > rows[i - 1].t) descending = false;
    }
    if (!ascending && descending) {
        std::reverse(rows.begin(), rows.end());
        raw.h.notes = "rows were newest first and were reversed";
    } else if (!ascending) {
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.t < b.t; });
        raw.h.notes = std::to_string(backwards) + " rows were out of time order and were sorted";
    }

    // A quote moves the best price on a side: the old level empties and the new
    // one holds the quoted size.
    bool any_quote = false;
    Decimal last[2]{};
    bool have[2] = {false, false};
    auto quote = [&](const Row& r, int s, const Decimal& p, const Decimal& q) {
        any_quote = true;
        const char side = s == 0 ? 'B' : 'S';
        if (have[s] && (last[s].mantissa != p.mantissa || last[s].decimals != p.decimals)) {
            RawEvent gone;
            gone.t = r.t;
            gone.kind = 'L';
            gone.side = side;
            gone.price = last[s];
            gone.qty = Decimal{0, 0};
            gone.line = r.line;
            raw.ev.push_back(gone);
        }
        RawEvent e;
        e.t = r.t;
        e.kind = 'L';
        e.side = side;
        e.price = p;
        e.qty = q;
        e.line = r.line;
        raw.ev.push_back(e);
        last[s] = p;
        have[s] = true;
    };
    for (const Row& r : rows) {
        if (r.trade) {
            RawEvent e;
            e.t = r.t;
            e.kind = 'T';
            e.side = r.side;
            e.price = r.price;
            e.qty = r.qty;
            e.line = r.line;
            raw.ev.push_back(e);
        }
        if (r.bid) quote(r, 0, r.bid_px, r.bid_qty);
        if (r.ask) quote(r, 1, r.ask_px, r.ask_qty);
    }
    if (symbols.size() > 1) {
        std::string list;
        std::size_t n = 0;
        for (const std::string& s : symbols) {
            if (n++ == 4) {
                list += ", ...";
                break;
            }
            list += (list.empty() ? "" : ", ") + s;
        }
        why = "the file holds " + std::to_string(symbols.size()) + " instruments (" + list +
              "): pick one with symbol=";
        return false;
    }
    raw.h.depth = any_quote ? Depth::L2 : Depth::Trades;
    if (any_quote)
        raw.h.notes = std::string("best bid and ask only: one price level a side") +
                      (raw.h.notes.empty() ? "" : "; " + raw.h.notes);
    raw.h.instrument = !opts.instrument.empty() ? opts.instrument : symbols.empty() ? "" : *symbols.begin();
    return true;
}

// ---- LOBSTER -----------------------------------------------------------------------

// LOBSTER's message file: time (seconds after midnight), event type, order id,
// size, price (dollars x 10,000), direction (1 buy, -1 sell). No header. The
// name says the rest: TICKER_DATE_START_END_message_LEVELS.csv.
//
// Types: 1 a new limit order, 2 a partial cancel, 3 a deletion, 4 a visible
// execution, 5 a hidden execution, 6 a cross (auction) trade, 7 a halt. In an
// execution the direction is the resting order's side, so the aggressor is the
// other one. Auction trades and halts are not continuous trading: counted and
// left out.
//
// A file starts at the window's start, so orders resting from before it are
// not in it. When the matching _orderbook_ file sits beside it, its first row
// (the book after the first message) seeds the book as it was before that
// message; without it the tape says the book starts empty.
[[nodiscard]] inline bool lobster_name(std::string_view file, std::string& ticker, std::string& date) {
    const std::size_t m = file.find("_message_");
    if (m == std::string_view::npos) return false;
    const std::size_t u1 = file.find('_');
    if (u1 == std::string_view::npos || u1 + 11 > file.size() || file[u1 + 5] != '-' || file[u1 + 8] != '-')
        return false;
    ticker = std::string(file.substr(0, u1));
    date = std::string(file.substr(u1 + 1, 10));
    return true;
}

[[nodiscard]] inline bool looks_like_lobster(std::string_view path, std::string_view first_line) {
    std::string ticker, date;
    if (!lobster_name(base_name(path), ticker, date)) return false;
    std::vector<std::string> f;
    if (!split_csv(first_line, ',', f) || f.size() != 6) return false;
    std::int64_t type = 0;
    return parse_int(f[1], type) && type >= 1 && type <= 7;
}

[[nodiscard]] inline bool import_lobster(const std::string& path, std::string_view text, std::string_view orderbook,
                                         const ImportOptions& opts, RawTape& raw, std::string& why) {
    (void)opts;
    std::string ticker, date;
    if (!lobster_name(base_name(path), ticker, date)) {
        why = "not a LOBSTER message file name (TICKER_DATE_START_END_message_LEVELS.csv)";
        return false;
    }
    raw.h.format = "lobster";
    raw.h.importer = "lobster 1";
    raw.h.source_file = base_name(path);
    raw.h.instrument = ticker;
    raw.h.date = date;
    raw.h.depth = Depth::L3;
    raw.kind = TimeKind::Midnight;
    const std::vector<std::string_view> lines = lines_of(text);
    std::vector<std::string> f;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        if (trim(lines[li]).empty()) continue;
        const std::uint64_t line = li + 1;
        std::int64_t type = 0, id = 0, size = 0, price = 0, dir = 0, t = 0;
        if (!split_csv(lines[li], ',', f) || f.size() < 6 || !parse_seconds(f[0], t) || !parse_int(f[1], type) ||
            !parse_int(f[2], id) || !parse_int(f[3], size) || !parse_int(f[4], price) || !parse_int(f[5], dir) ||
            type < 1 || type > 7 || (dir != 1 && dir != -1) || id < 0 || size < 0) {
            why = line_ref(line) + "not a LOBSTER message row (time,type,id,size,price,direction): " +
                  std::string(lines[li]);
            return false;
        }
        if (type == 6) {
            ++raw.skipped["auction trades"];
            continue;
        }
        if (type == 7) {
            ++raw.skipped["trading halt messages"];
            continue;
        }
        RawEvent e;
        e.t = t;
        e.line = line;
        e.id = static_cast<std::uint64_t>(id);
        e.price = Decimal{price, 4};
        e.qty = Decimal{size, 0};
        const char resting = dir == 1 ? 'B' : 'S';
        switch (type) {
            case 1: e.kind = 'A'; e.side = resting; break;
            case 2:
            case 3: e.kind = 'C'; e.side = resting; break;
            case 4: e.kind = 'T'; e.side = resting == 'B' ? 'S' : 'B'; break;
            default: // 5: hidden, so no visible order is consumed
                e.kind = 'T';
                e.side = resting == 'B' ? 'S' : 'B';
                e.id = 0;
                break;
        }
        if (size == 0) {
            ++raw.skipped["rows of size zero"];
            continue;
        }
        raw.ev.push_back(e);
    }
    if (raw.ev.empty()) return true;

    // Seed the book from the orderbook file: its first row is the book after the
    // first message, so undo that message's effect on its own level.
    if (!orderbook.empty()) {
        const std::vector<std::string_view> ob = lines_of(orderbook);
        if (!ob.empty() && split_csv(ob.front(), ',', f) && f.size() >= 4 && f.size() % 4 == 0) {
            const RawEvent& m = raw.ev.front();
            // The side and size the first message moved on its level, if it moved one:
            // an add put size there, a cancel or a visible execution took it away.
            const bool moved = m.kind == 'A' || m.kind == 'C' || (m.kind == 'T' && m.id != 0);
            const char m_side = m.kind == 'T' ? (m.side == 'B' ? 'S' : 'B') : m.side;
            bool m_level_seen = false;
            std::vector<RawEvent> seed;
            bool ok = true;
            for (std::size_t lv = 0; lv + 3 < f.size(); lv += 4) {
                std::int64_t ap = 0, as = 0, bp = 0, bs = 0;
                ok = ok && parse_int(f[lv], ap) && parse_int(f[lv + 1], as) && parse_int(f[lv + 2], bp) &&
                     parse_int(f[lv + 3], bs);
                if (!ok) break;
                auto level = [&](std::int64_t p, std::int64_t s, char side) {
                    if (p <= 0 || p >= 9'999'999'999LL || s < 0) return; // LOBSTER's empty-level dummies
                    if (moved && m.price.mantissa == p && m_side == side) {
                        m_level_seen = true;
                        s += m.kind == 'A' ? -m.qty.mantissa : m.qty.mantissa;
                    }
                    if (s <= 0) return;
                    RawEvent e;
                    e.t = m.t;
                    e.kind = 'L';
                    e.side = side;
                    e.price = Decimal{p, 4};
                    e.qty = Decimal{s, 0};
                    e.line = 1;
                    seed.push_back(e);
                };
                level(ap, as, 'S');
                level(bp, bs, 'B');
            }
            // A cancel or execution that emptied its level leaves it out of the
            // snapshot; before the message it held exactly what was taken.
            if (ok && moved && m.kind != 'A' && !m_level_seen) {
                RawEvent e;
                e.t = m.t;
                e.kind = 'L';
                e.side = m_side;
                e.price = m.price;
                e.qty = m.qty;
                e.line = 1;
                seed.push_back(e);
            }
            if (ok && !seed.empty()) {
                raw.ev.insert(raw.ev.begin(), seed.begin(), seed.end());
                raw.book_seeded = true;
            }
        }
    }
    raw.h.notes = raw.book_seeded
                      ? "book seeded from the orderbook file"
                      : "book starts empty: orders resting before the file began are not in it, so the first "
                        "minutes understate depth (put the _orderbook_ file beside it to fix that)";
    return true;
}

// ---- levels from snapshots -------------------------------------------------------------

// The fewest-decimals form of a decimal, so two spellings of one price are one key.
[[nodiscard]] inline std::pair<std::int64_t, int> canonical(const Decimal& d) {
    std::int64_t m = d.mantissa;
    int n = d.decimals;
    while (n > 0 && m % 10 == 0) {
        m /= 10;
        --n;
    }
    return {m, n};
}

// Sources that give the top N levels of the book after every event, rather
// than what changed, become level updates here: a level that appeared or
// changed size gets its new size, one that left the view gets zero. So a tape
// made from them is a book of N levels a side.
class LevelDiff {
public:
    using Levels = std::vector<std::pair<Decimal, Decimal>>; // price, size; size 0 is no level

    void side(RawTape& raw, std::int64_t t, std::uint64_t line, char side, const Levels& now) {
        std::map<std::pair<std::int64_t, int>, std::pair<Decimal, Decimal>> next;
        for (const auto& [p, q] : now)
            if (q.mantissa > 0) next[canonical(p)] = {p, q};
        auto& was = side == 'B' ? bid_ : ask_;
        auto emit = [&](const Decimal& p, const Decimal& q) {
            RawEvent e;
            e.t = t;
            e.kind = 'L';
            e.side = side;
            e.price = p;
            e.qty = q;
            e.line = line;
            raw.ev.push_back(e);
        };
        for (const auto& [k, v] : was)
            if (next.find(k) == next.end()) emit(v.first, Decimal{0, 0});
        for (const auto& [k, v] : next) {
            const auto it = was.find(k);
            if (it == was.end() || canonical(it->second.second) != canonical(v.second)) emit(v.first, v.second);
        }
        was = std::move(next);
    }
    void clear() {
        bid_.clear();
        ask_.clear();
    }

private:
    std::map<std::pair<std::int64_t, int>, std::pair<Decimal, Decimal>> bid_, ask_;
};

// ---- Databento ----------------------------------------------------------------------
//
// Databento's normalised schemas, as its CSV export writes them (dbn --csv, or
// encoding=csv), or in its own binary encoding, DBN, uncompressed (zstd -d
// first). One importer reads both: the records are the same and only their
// encoding differs. Written from Databento's published documentation of the
// schemas and of DBN, and checked against hand-made samples, not a purchased file.
//
//   mbo      order by order (L3): adds, cancels, modifies and clears. In this
//            schema a trade (T) or a fill (F) does not change the book; the
//            cancel or modify that follows it does. So a trade becomes a print
//            that leaves the book alone, and a fill is counted and left out.
//   mbp-1, mbp-10, tbbo   price levels (L2): each record carries the top levels
//            after it, which become level updates; its trades become prints.
//   trades   prints only.
//
// Prices are integers in units of 10^-9 (decimals, with pretty_px); times are
// nanoseconds since the epoch (ISO 8601, with pretty_ts). ts_recv orders the
// records, because the files are sorted by it. A file holding several
// instruments needs symbol=, which matches the symbol column that map_symbols
// writes, or the instrument_id.

enum class DbSchema : std::uint8_t { Mbo, Mbp, Trades };

struct DbRecord {
    std::int64_t t = 0; // ts_recv
    std::string instrument; // the symbol when the file maps one, else the instrument_id
    char action = 'N';
    char side = 'N';
    bool priced = false;
    Decimal price{};
    Decimal size{};
    std::uint64_t order_id = 0;
    LevelDiff::Levels bids, asks; // mbp: the top levels after this record
    std::uint64_t line = 0;         // the CSV line, or the DBN record's position
};

inline constexpr std::int64_t kDbUndefPrice = 9'223'372'036'854'775'807LL; // INT64_MAX: no price

[[nodiscard]] inline bool db_price(std::string_view raw, bool& priced, Decimal& out) {
    raw = trim(raw);
    priced = false;
    if (raw.empty() || raw == "9223372036854775807" || raw == "9223372036.854775807") return true;
    if (raw.find('.') != std::string_view::npos || raw.find('e') != std::string_view::npos) {
        if (!parse_decimal(raw, out)) return false;
    } else {
        std::int64_t v = 0;
        if (!parse_int(raw, v)) return false;
        out = Decimal{v, 9};
    }
    priced = true;
    return true;
}

// The records, in file order, into events. Shared by the CSV and DBN readers.
[[nodiscard]] inline bool databento_events(std::vector<DbRecord>& recs, DbSchema schema, int levels,
                                           const ImportOptions& opts, RawTape& raw, std::string& why) {
    std::set<std::string> instruments;
    for (const DbRecord& r : recs) instruments.insert(r.instrument);
    if (opts.symbol.empty() && instruments.size() > 1) {
        std::string list;
        std::size_t n = 0;
        for (const std::string& s : instruments) {
            if (n++ == 4) {
                list += ", ...";
                break;
            }
            list += (list.empty() ? "" : ", ") + s;
        }
        why = "the file holds " + std::to_string(instruments.size()) + " instruments (" + list +
              "): pick one with symbol=";
        return false;
    }
    const std::string chosen = !opts.symbol.empty() ? opts.symbol : instruments.empty() ? "" : *instruments.begin();
    if (!opts.symbol.empty() && instruments.count(opts.symbol) == 0) {
        why = "no records for symbol=" + opts.symbol;
        return false;
    }
    raw.h.instrument = chosen;
    raw.kind = TimeKind::Epoch;
    for (const DbRecord& r : recs)
        if (r.instrument == chosen) {
            raw.h.date = time_text(r.t, TimeKind::Epoch).substr(0, 10);
            break;
        }
    raw.h.depth = schema == DbSchema::Mbo ? Depth::L3 : schema == DbSchema::Mbp ? Depth::L2 : Depth::Trades;

    struct Resting {
        char side;
        Decimal price, qty;
    };
    std::map<std::uint64_t, Resting> book; // mbo: what each order rests with, for a modify
    LevelDiff diff;
    std::uint64_t unknown_modifies = 0;
    auto push = [&raw](const DbRecord& r, char kind, char side, std::uint64_t id, const Decimal& p, const Decimal& q) {
        RawEvent e;
        e.t = r.t;
        e.kind = kind;
        e.side = side;
        e.id = id;
        e.price = p;
        e.qty = q;
        e.line = r.line;
        raw.ev.push_back(e);
    };
    for (DbRecord& r : recs) {
        if (r.instrument != chosen) {
            ++raw.skipped["records for other instruments"];
            continue;
        }
        const char side = r.side == 'B' ? 'B' : r.side == 'A' ? 'S' : '-';
        if (r.action == 'R') {
            push(r, 'R', '-', 0, Decimal{}, Decimal{});
            book.clear();
            diff.clear();
            if (schema != DbSchema::Mbp) continue;
        }
        if (schema == DbSchema::Mbp) {
            if (r.action == 'T' && r.priced && r.size.mantissa > 0) push(r, 'T', side, 0, r.price, r.size);
            diff.side(raw, r.t, r.line, 'B', r.bids);
            diff.side(raw, r.t, r.line, 'S', r.asks);
            continue;
        }
        if (schema == DbSchema::Trades) {
            if (r.action == 'T' && r.priced && r.size.mantissa > 0) push(r, 'T', side, 0, r.price, r.size);
            else ++raw.skipped["records that are not trades"];
            continue;
        }
        switch (r.action) {
            case 'A':
                if (!r.priced || side == '-' || r.size.mantissa <= 0) {
                    ++raw.skipped["adds with no price, side or size"];
                    break;
                }
                push(r, 'A', side, r.order_id, r.price, r.size);
                book[r.order_id] = Resting{side, r.price, r.size};
                break;
            case 'C': {
                if (r.size.mantissa <= 0) {
                    ++raw.skipped["cancels of size zero"];
                    break;
                }
                const auto it = book.find(r.order_id);
                const char s = it != book.end() ? it->second.side : side;
                push(r, 'C', s, r.order_id, it != book.end() ? it->second.price : r.price, r.size);
                if (it != book.end()) {
                    std::int64_t left = 0, take = 0;
                    const int d = std::max(it->second.qty.decimals, r.size.decimals);
                    if (scale_to(it->second.qty, d, left) && scale_to(r.size, d, take) && left > take)
                        it->second.qty = Decimal{left - take, d};
                    else
                        book.erase(it);
                }
                break;
            }
            case 'M': {
                // A new price or size: the order leaves its level and comes back
                // with the new values, as one cancel and one add on the tape.
                if (!r.priced || r.size.mantissa <= 0) {
                    ++raw.skipped["modifies with no price or size"];
                    break;
                }
                const auto it = book.find(r.order_id);
                const char s = it != book.end() ? it->second.side : side;
                if (it != book.end()) push(r, 'C', s, r.order_id, it->second.price, it->second.qty);
                else ++unknown_modifies;
                if (s == '-') break;
                push(r, 'A', s, r.order_id, r.price, r.size);
                book[r.order_id] = Resting{s, r.price, r.size};
                break;
            }
            case 'T':
                if (r.priced && r.size.mantissa > 0) push(r, 'T', side, 0, r.price, r.size);
                else ++raw.skipped["trades with no price or size"];
                break;
            case 'F': ++raw.skipped["fills (the book changes on the cancel or modify after each)"]; break;
            default: ++raw.skipped["records with no action"]; break;
        }
    }
    std::string notes;
    if (schema == DbSchema::Mbp)
        notes = std::to_string(levels) + " price level" + (levels == 1 ? "" : "s") + " a side";
    if (unknown_modifies > 0)
        notes += (notes.empty() ? "" : "; ") + grouped(static_cast<std::int64_t>(unknown_modifies)) +
                 " modifies of orders resting before the file began, whose old size is not known";
    raw.h.notes = notes;
    return true;
}

// "bid_px_03": a level's column, as the CSV names it.
[[nodiscard]] inline std::string db_level_key(const char* what, int lv) {
    return std::string(what) + (lv < 10 ? "_0" : "_") + std::to_string(lv);
}

[[nodiscard]] inline bool looks_like_databento_csv(std::string_view first_line) {
    return first_line.find("rtype") != std::string_view::npos &&
           first_line.find("publisher_id") != std::string_view::npos &&
           first_line.find("instrument_id") != std::string_view::npos;
}

[[nodiscard]] inline bool import_databento_csv(const std::string& path, std::string_view text,
                                               const ImportOptions& opts, RawTape& raw, std::string& why) {
    const std::vector<std::string_view> lines = lines_of(text);
    std::vector<std::string> f;
    if (lines.empty() || !split_csv(lines[0], ',', f) || !looks_like_databento_csv(lines[0])) {
        why = "not a Databento CSV file: the first line should name rtype, publisher_id and instrument_id";
        return false;
    }
    std::map<std::string, int> col;
    for (std::size_t i = 0; i < f.size(); ++i) col[std::string(trim(f[i]))] = static_cast<int>(i);
    auto has = [&col](const std::string& k) { return col.count(k) != 0; };
    if (has("open") && has("close")) {
        why = "bars (OHLCV) hold no orders or prints: export the mbo, mbp-1, mbp-10, tbbo or trades schema";
        return false;
    }
    int levels = 0;
    while (levels < 100 && has(db_level_key("bid_px", levels))) ++levels;
    const DbSchema schema = has("order_id") ? DbSchema::Mbo : levels > 0 ? DbSchema::Mbp : DbSchema::Trades;
    const char* ts = has("ts_recv") ? "ts_recv" : "ts_event";
    for (const char* k : {ts, "action", "side", "price", "size", "instrument_id"}) {
        if (!has(k)) {
            why = std::string("the header has no ") + k + " column";
            return false;
        }
    }
    raw.h.format = "databento";
    raw.h.importer = "databento 1";
    raw.h.source_file = base_name(path);
    const bool symbols = has("symbol");
    std::vector<DbRecord> recs;
    recs.reserve(lines.size());
    for (std::size_t li = 1; li < lines.size(); ++li) {
        if (trim(lines[li]).empty()) continue;
        const std::uint64_t line = li + 1;
        if (!split_csv(lines[li], ',', f) || f.size() < col.size()) {
            why = line_ref(line) + std::to_string(f.size()) + " fields, expected " + std::to_string(col.size());
            return false;
        }
        auto field = [&](const std::string& k) { return trim(f[static_cast<std::size_t>(col[k])]); };
        DbRecord r;
        r.line = line;
        TimeKind kind{};
        if (!parse_time(field(ts), TimeFormat::Auto, r.t, kind) || kind != TimeKind::Epoch) {
            why = line_ref(line) + std::string(ts) + " '" + std::string(field(ts)) + "' is not a time";
            return false;
        }
        const std::string_view a = field("action"), sd = field("side");
        r.action = a.size() == 1 ? a[0] : '?';
        r.side = sd.size() == 1 ? sd[0] : '?';
        if (r.side != 'A' && r.side != 'B' && r.side != 'N') {
            why = line_ref(line) + "side '" + std::string(sd) + "' is not A, B or N";
            return false;
        }
        if (!db_price(field("price"), r.priced, r.price)) {
            why = line_ref(line) + "price '" + std::string(field("price")) + "' is not a number";
            return false;
        }
        std::int64_t v = 0;
        if (!parse_int(field("size"), v) || v < 0) {
            why = line_ref(line) + "size '" + std::string(field("size")) + "' is not a whole number";
            return false;
        }
        r.size = Decimal{v, 0};
        if (schema == DbSchema::Mbo) {
            std::int64_t id = 0;
            if (!parse_int(field("order_id"), id) || id < 0) {
                why = line_ref(line) + "order_id '" + std::string(field("order_id")) + "' is not a whole number";
                return false;
            }
            r.order_id = static_cast<std::uint64_t>(id);
        }
        for (int lv = 0; lv < levels; ++lv) {
            for (const char s : {'b', 'a'}) {
                const std::string kp = db_level_key(s == 'b' ? "bid_px" : "ask_px", lv);
                const std::string kq = db_level_key(s == 'b' ? "bid_sz" : "ask_sz", lv);
                bool priced = false;
                Decimal p{};
                std::int64_t q = 0;
                if (!db_price(field(kp), priced, p) || (has(kq) && !field(kq).empty() && !parse_int(field(kq), q))) {
                    why = line_ref(line) + "level " + std::to_string(lv) + " is not a price and a size";
                    return false;
                }
                if (priced && q > 0) (s == 'b' ? r.bids : r.asks).emplace_back(p, Decimal{q, 0});
            }
        }
        r.instrument = symbols && !field("symbol").empty() ? std::string(field("symbol")) : std::string(field("instrument_id"));
        if (!opts.symbol.empty() && r.instrument != opts.symbol && std::string(field("instrument_id")) == opts.symbol)
            r.instrument = opts.symbol; // symbol= may name the id even when a symbol column exists
        recs.push_back(std::move(r));
    }
    return databento_events(recs, schema, levels, opts, raw, why);
}

// DBN, Databento's binary encoding: "DBN", a version byte, the metadata's
// length, the metadata, then records. Little-endian. Each record starts with a
// 16-byte header whose first byte is the record's length in 4-byte words, so a
// record of a type not read here, or a longer future version of one, is skipped
// by its length rather than misread.
[[nodiscard]] inline std::uint64_t le(std::string_view b, std::size_t at, int n) {
    std::uint64_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(b[at + static_cast<std::size_t>(i)]);
    return v;
}

[[nodiscard]] inline const char* dbn_rtype_name(unsigned rtype) {
    switch (rtype) {
        case 0x11: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: return "bars";
        case 0x12: return "status records";
        case 0x13: return "instrument definitions";
        case 0x14: return "imbalance records";
        case 0x15: return "error records";
        case 0x16: return "symbol mappings";
        case 0x17: return "system records";
        case 0x18: return "statistics";
        default: return "records of a type this importer does not read";
    }
}

[[nodiscard]] inline bool import_dbn(const std::string& path, std::string_view b, const ImportOptions& opts,
                                     RawTape& raw, std::string& why) {
    if (b.substr(0, 3) != "DBN") {
        why = "not a DBN file";
        return false;
    }
    if (b.size() < 8) {
        why = "the DBN metadata is cut short";
        return false;
    }
    const unsigned version = static_cast<unsigned char>(b[3]);
    if (version < 1 || version > 3) {
        why = "DBN version " + std::to_string(version) + "; this build reads versions 1 to 3";
        return false;
    }
    const std::uint64_t meta = le(b, 4, 4);
    if (8 + meta > b.size() || meta < 18) {
        why = "the DBN metadata is cut short";
        return false;
    }
    std::string dataset(b.substr(8, 16));
    dataset.erase(std::find(dataset.begin(), dataset.end(), '\0'), dataset.end());
    raw.h.format = "databento";
    raw.h.importer = "databento 1 (DBN v" + std::to_string(version) + ")";
    raw.h.source_file = base_name(path);

    std::vector<DbRecord> recs;
    DbSchema schema = DbSchema::Trades;
    int levels = 0;
    bool seen = false;
    std::uint64_t n = 0;
    for (std::size_t at = 8 + meta; at < b.size(); ++n) {
        const std::size_t len = static_cast<std::size_t>(static_cast<unsigned char>(b[at])) * 4;
        if (len < 16 || at + len > b.size()) {
            why = "record " + std::to_string(n + 1) + " at byte " + std::to_string(at) + " is cut short";
            return false;
        }
        const std::string_view r = b.substr(at, len);
        at += len;
        const unsigned rtype = static_cast<unsigned char>(r[1]);
        DbSchema s{};
        int lv = 0;
        std::size_t need = 0;
        if (rtype == 0xA0) s = DbSchema::Mbo, need = 56;
        else if (rtype == 0x00) s = DbSchema::Trades, need = 48;
        else if (rtype == 0x01) s = DbSchema::Mbp, lv = 1, need = 80;
        else if (rtype == 0x0A) s = DbSchema::Mbp, lv = 10, need = 368;
        else {
            ++raw.skipped[dbn_rtype_name(rtype)];
            continue;
        }
        if (len < need) {
            why = "record " + std::to_string(n + 1) + " is shorter than its type";
            return false;
        }
        if (seen && (s != schema || lv != levels)) {
            why = "the file mixes schemas; export one schema a file";
            return false;
        }
        seen = true;
        schema = s;
        levels = lv;
        DbRecord d;
        d.line = n + 1;
        d.instrument = std::to_string(le(r, 4, 4));
        auto price = [](std::uint64_t v, bool& priced, Decimal& out) {
            const auto p = static_cast<std::int64_t>(v);
            priced = p != kDbUndefPrice;
            out = Decimal{priced ? p : 0, 9};
        };
        std::uint64_t ts_recv = 0;
        if (s == DbSchema::Mbo) {
            d.order_id = le(r, 16, 8);
            price(le(r, 24, 8), d.priced, d.price);
            d.size = Decimal{static_cast<std::int64_t>(le(r, 32, 4)), 0};
            d.action = r[38];
            d.side = r[39];
            ts_recv = le(r, 40, 8);
        } else {
            price(le(r, 16, 8), d.priced, d.price);
            d.size = Decimal{static_cast<std::int64_t>(le(r, 24, 4)), 0};
            d.action = r[28];
            d.side = r[29];
            ts_recv = le(r, 32, 8);
            for (int i = 0; i < lv; ++i) {
                const std::size_t o = 48 + static_cast<std::size_t>(i) * 32;
                bool bp = false, ap = false;
                Decimal bpx{}, apx{};
                price(le(r, o, 8), bp, bpx);
                price(le(r, o + 8, 8), ap, apx);
                const auto bq = static_cast<std::int64_t>(le(r, o + 16, 4)), aq = static_cast<std::int64_t>(le(r, o + 20, 4));
                if (bp && bq > 0) d.bids.emplace_back(bpx, Decimal{bq, 0});
                if (ap && aq > 0) d.asks.emplace_back(apx, Decimal{aq, 0});
            }
        }
        if (ts_recv > static_cast<std::uint64_t>(9'000'000'000'000'000'000LL)) ts_recv = le(r, 8, 8); // no ts_recv: ts_event
        d.t = static_cast<std::int64_t>(ts_recv);
        recs.push_back(std::move(d));
    }
    if (!seen) {
        why = "no order, trade or book records" + (raw.skipped.empty() ? std::string() : " (only " + skipped_text(raw.skipped) + ")");
        return false;
    }
    if (!databento_events(recs, schema, levels, opts, raw, why)) return false;
    if (!dataset.empty()) raw.h.notes = "dataset " + dataset + (raw.h.notes.empty() ? "" : "; " + raw.h.notes);
    if (raw.h.instrument.find_first_not_of("0123456789") == std::string::npos)
        raw.h.instrument = "instrument_id " + raw.h.instrument;
    return true;
}

[[nodiscard]] inline bool import_databento(const std::string& path, std::string_view bytes,
                                           const ImportOptions& opts, RawTape& raw, std::string& why) {
    if (bytes.substr(0, 3) == "DBN") return import_dbn(path, bytes, opts, raw, why);
    if (!import_databento_csv(path, bytes, opts, raw, why)) return false;
    if (raw.h.instrument.find_first_not_of("0123456789") == std::string::npos)
        raw.h.instrument = "instrument_id " + raw.h.instrument;
    return true;
}

// ---- Tardis ---------------------------------------------------------------------------
//
// Tardis.dev's downloadable CSV files, one data type a file, from its
// published documentation of them; checked against hand-made samples:
//
//   incremental_book_L2  exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount:
//                        the new total at a price, 0 when the level is gone. Snapshot
//                        rows after updates mean the book starts again.
//   book_snapshot_N      the top N levels a side after each change (asks[0].price, asks[0].amount, ...)
//   quotes               the best bid and ask after each change
//   trades               exchange,symbol,timestamp,local_timestamp,id,side,price,amount, where
//                        side is the taker's: buy, sell or unknown
//
// Times are microseconds since the epoch. local_timestamp, when the message
// arrived, orders the rows: every file is sorted by it, and it is what puts two
// files' rows in one order. A book file is read together with the trades file
// of the same exchange, day and symbol when that sits beside it.

[[nodiscard]] inline bool looks_like_tardis(std::string_view first_line) {
    return first_line.starts_with("exchange,symbol,timestamp,local_timestamp,");
}

struct TardisFile {
    std::string type; // incremental_book_L2, book_snapshot, quotes or trades
    int levels = 0;
    std::string exchange;
    std::set<std::string> symbols;
    std::vector<RawEvent> ev;
};

[[nodiscard]] inline bool read_tardis(std::string_view text, const ImportOptions& opts, TardisFile& out,
                                      std::map<std::string, std::uint64_t>& skipped, std::string& why) {
    const std::vector<std::string_view> lines = lines_of(text);
    std::vector<std::string> f;
    if (lines.empty() || !looks_like_tardis(lines[0]) || !split_csv(lines[0], ',', f)) {
        why = "not a Tardis CSV file: the first line should start exchange,symbol,timestamp,local_timestamp";
        return false;
    }
    std::map<std::string, int> col;
    for (std::size_t i = 0; i < f.size(); ++i) col[std::string(trim(f[i]))] = static_cast<int>(i);
    auto has = [&col](const std::string& k) { return col.count(k) != 0; };
    if (has("is_snapshot") && has("side") && has("price") && has("amount")) out.type = "incremental_book_L2";
    else if (has("asks[0].price")) out.type = "book_snapshot";
    else if (has("ask_price") && has("bid_price")) out.type = "quotes";
    else if (has("id") && has("side") && has("price") && has("amount")) out.type = "trades";
    else {
        why = "a Tardis file of a type with no book or trades in it (columns: " + std::string(lines[0]) +
              "); add its incremental_book_L2, book_snapshot, quotes or trades file";
        return false;
    }
    if (out.type == "book_snapshot")
        while (has("asks[" + std::to_string(out.levels) + "].price")) ++out.levels;
    if (out.type == "quotes") out.levels = 1;
    LevelDiff diff;
    bool in_snapshot = false, any_update = false;
    for (std::size_t li = 1; li < lines.size(); ++li) {
        if (trim(lines[li]).empty()) continue;
        const std::uint64_t line = li + 1;
        if (!split_csv(lines[li], ',', f) || f.size() < col.size()) {
            why = line_ref(line) + std::to_string(f.size()) + " fields, expected " + std::to_string(col.size());
            return false;
        }
        auto field = [&](const std::string& k) { return trim(f[static_cast<std::size_t>(col[k])]); };
        const std::string sym(field("symbol"));
        if (!opts.symbol.empty() && sym != opts.symbol) {
            ++skipped["rows for other symbols"];
            continue;
        }
        out.symbols.insert(sym);
        if (out.exchange.empty()) out.exchange = std::string(field("exchange"));
        RawEvent e;
        e.line = line;
        TimeKind kind{};
        if (!parse_time(field("local_timestamp"), TimeFormat::Micros, e.t, kind) &&
            !parse_time(field("local_timestamp"), TimeFormat::Auto, e.t, kind)) {
            why = line_ref(line) + "local_timestamp '" + std::string(field("local_timestamp")) + "' is not a time";
            return false;
        }
        auto number = [&](const std::string& k, Decimal& d) {
            if (parse_decimal(field(k), d)) return true;
            why = line_ref(line) + k + " '" + std::string(field(k)) + "' is not a number";
            return false;
        };
        if (out.type == "incremental_book_L2") {
            const bool snap = lower(field("is_snapshot")) == "true";
            if (snap && !in_snapshot && any_update) out.ev.push_back(RawEvent{e.t, 'R', '-', 0, {}, {}, line});
            in_snapshot = snap;
            any_update = any_update || !snap;
            const std::string side = lower(field("side"));
            if (side != "bid" && side != "ask") {
                why = line_ref(line) + "side '" + side + "' is not bid or ask";
                return false;
            }
            e.kind = 'L';
            e.side = side == "bid" ? 'B' : 'S';
            if (!number("price", e.price) || !number("amount", e.qty)) return false;
            out.ev.push_back(e);
        } else if (out.type == "trades") {
            e.kind = 'T';
            e.side = side_from_text(field("side"));
            if (!number("price", e.price) || !number("amount", e.qty)) return false;
            if (e.qty.mantissa <= 0) {
                ++skipped["trades of size zero"];
                continue;
            }
            out.ev.push_back(e);
        } else {
            // One snapshot a row: the levels it holds become level updates.
            RawTape scratch;
            for (const char side : {'B', 'S'}) {
                LevelDiff::Levels now;
                for (int i = 0; i < out.levels; ++i) {
                    const std::string p = out.type == "quotes" ? (side == 'B' ? "bid_price" : "ask_price")
                                                               : std::string(side == 'B' ? "bids[" : "asks[") +
                                                                     std::to_string(i) + "].price";
                    const std::string q = out.type == "quotes" ? (side == 'B' ? "bid_amount" : "ask_amount")
                                                               : std::string(side == 'B' ? "bids[" : "asks[") +
                                                                     std::to_string(i) + "].amount";
                    if (field(p).empty() || !has(q) || field(q).empty()) continue;
                    Decimal pd{}, qd{};
                    if (!number(p, pd) || !number(q, qd)) return false;
                    now.emplace_back(pd, qd);
                }
                diff.side(scratch, e.t, line, side, now);
            }
            for (RawEvent& x : scratch.ev) out.ev.push_back(x);
        }
    }
    return true;
}

[[nodiscard]] inline bool import_tardis(const std::string& path, std::string_view text, std::string_view trades,
                                        const ImportOptions& opts, RawTape& raw, std::string& why) {
    TardisFile main;
    if (!read_tardis(text, opts, main, raw.skipped, why)) return false;
    TardisFile other;
    if (!trades.empty() && main.type != "trades") {
        if (!read_tardis(trades, opts, other, raw.skipped, why)) {
            why = "the trades file beside it: " + why;
            return false;
        }
        if (other.type != "trades") other = TardisFile{};
    }
    std::set<std::string> symbols = main.symbols;
    symbols.insert(other.symbols.begin(), other.symbols.end());
    if (symbols.size() > 1) {
        std::string list;
        std::size_t n = 0;
        for (const std::string& x : symbols) {
            if (n++ == 4) {
                list += ", ...";
                break;
            }
            list += (list.empty() ? "" : ", ") + x;
        }
        why = "the files hold " + std::to_string(symbols.size()) + " symbols (" + list + "): pick one with symbol=";
        return false;
    }
    // Both files are in local_timestamp order: merge them, the book's row first on a tie.
    raw.ev.reserve(main.ev.size() + other.ev.size());
    std::merge(main.ev.begin(), main.ev.end(), other.ev.begin(), other.ev.end(), std::back_inserter(raw.ev),
               [](const RawEvent& a, const RawEvent& b) { return a.t < b.t; });
    raw.h.format = "tardis";
    raw.h.importer = "tardis 1";
    raw.h.source_file = base_name(path);
    raw.h.instrument = symbols.empty() ? "" : *symbols.begin();
    raw.kind = TimeKind::Epoch;
    raw.h.depth = main.type == "trades" ? Depth::Trades : Depth::L2;
    if (!raw.ev.empty()) raw.h.date = time_text(raw.ev.front().t, TimeKind::Epoch).substr(0, 10);
    std::string notes = main.exchange + " " + main.type;
    if (main.levels > 0) notes += ", " + std::to_string(main.levels) + " level" + (main.levels == 1 ? "" : "s") + " a side";
    if (!other.ev.empty()) notes += ", with its trades file";
    else if (main.type != "trades") notes += "; no trades file beside it, so no prints";
    raw.h.notes = notes;
    return true;
}

// The tape name a Tardis file gets: exchange, symbol and day, with anything a
// name cannot hold ("XBT/USD") made a hyphen. A trades file read on its own is
// named apart from its book file's tape.
[[nodiscard]] inline std::string tardis_name(const RawTape& raw) {
    const std::string ex = raw.h.notes.substr(0, raw.h.notes.find(' '));
    std::string n = lower(ex + "-" + raw.h.instrument + "-" + raw.h.date) + (raw.h.depth == Depth::Trades ? "-trades" : "");
    for (char& c : n)
        if (!((c >= 'a' && c <= 'z') || is_digit(c) || c == '-' || c == '_' || c == '.')) c = '-';
    return n;
}

// ---- Binance ---------------------------------------------------------------------------
//
// The CSV files data.binance.vision publishes, named SYMBOL-TYPE-DATE.csv, from
// its published descriptions of them; checked against hand-made samples:
//
//   trades      spot, no header: id,price,qty,quote_qty,time,is_buyer_maker,is_best_match
//               futures, a header: id,price,qty,quote_qty (base_qty on coin-margined),time,is_buyer_maker
//   aggTrades   spot, no header: agg_id,price,qty,first_id,last_id,time,is_buyer_maker,is_best_match
//               futures, a header: agg_trade_id,price,quantity,first_trade_id,last_trade_id,transact_time,
//               is_buyer_maker
//   bookTicker  futures, a header: update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,
//               transaction_time,event_time
//
// Times are epoch milliseconds, or microseconds in spot files from 2025: the
// number of digits says which. is_buyer_maker true means a seller crossed the
// spread. A bookTicker file is read with the trades file of the same symbol and
// day when it sits beside it, or failing that its aggTrades file.

// The type a Binance file's name gives, or "" when it gives none.
[[nodiscard]] inline std::string binance_type(std::string_view path) {
    const std::string base = base_name(path);
    for (const char* t : {"-aggTrades-", "-trades-", "-bookTicker-"}) {
        const std::string_view tv(t);
        if (base.find(tv) != std::string::npos) return std::string(tv.substr(1, tv.size() - 2));
    }
    return "";
}

// The type a Binance header names, or "" when the line is not one.
[[nodiscard]] inline std::string binance_header_type(std::string_view first_line) {
    const std::string h = lower(trim(first_line));
    if (h == "id,price,qty,quote_qty,time,is_buyer_maker" || h == "id,price,qty,base_qty,time,is_buyer_maker")
        return "trades";
    if (h == "agg_trade_id,price,quantity,first_trade_id,last_trade_id,transact_time,is_buyer_maker")
        return "aggTrades";
    if (h == "update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,transaction_time,event_time")
        return "bookTicker";
    return "";
}

[[nodiscard]] inline bool looks_like_binance(std::string_view path, std::string_view first_line) {
    if (!binance_header_type(first_line).empty()) return true;
    const std::string t = binance_type(path), l = lower(first_line);
    return (t == "trades" || t == "aggTrades") && !l.empty() && is_digit(l[0]) &&
           (l.find(",true") != std::string::npos || l.find(",false") != std::string::npos);
}

struct BinanceFile {
    std::string type;
    bool header = false;
    std::vector<RawEvent> ev;
};

[[nodiscard]] inline bool read_binance(const std::string& path, std::string_view text, BinanceFile& out,
                                       std::string& why) {
    const std::vector<std::string_view> lines = lines_of(text);
    if (lines.empty()) {
        why = "the file is empty";
        return false;
    }
    out.type = binance_header_type(lines[0]);
    out.header = !out.type.empty();
    if (!out.header) out.type = binance_type(path);
    std::vector<std::string> f;
    if (out.type.empty() && !out.header && split_csv(lines[0], ',', f)) // a spot file renamed: its width says
        out.type = f.size() == 7 ? "trades" : f.size() == 8 ? "aggTrades" : "";
    if (out.type.empty()) {
        why = "not a Binance file this build reads: its name should say trades, aggTrades or bookTicker, as "
              "data.binance.vision names them (BTCUSDT-trades-2024-01-02.csv), or it should start with their header";
        return false;
    }
    // Columns: price, qty, time, is_buyer_maker; bookTicker: bid, bid qty, ask, ask qty, time.
    const bool book = out.type == "bookTicker";
    const std::size_t c_time = book ? 5 : out.type == "trades" ? 4 : 5, c_bm = out.type == "trades" ? 5 : 6;
    const std::size_t need = book ? 6 : c_bm + 1;
    if (book && !out.header) {
        why = "a bookTicker file without its header: this build reads them as data.binance.vision writes them";
        return false;
    }
    LevelDiff diff;
    RawTape scratch;
    for (std::size_t li = out.header ? 1 : 0; li < lines.size(); ++li) {
        if (trim(lines[li]).empty()) continue;
        const std::uint64_t line = li + 1;
        if (!split_csv(lines[li], ',', f) || f.size() < need) {
            why = line_ref(line) + std::to_string(f.size()) + " fields, a Binance " + out.type + " file has " +
                  std::to_string(need) + " or more";
            return false;
        }
        RawEvent e;
        e.line = line;
        TimeKind kind{};
        if (!parse_time(trim(f[c_time]), TimeFormat::Auto, e.t, kind) || kind != TimeKind::Epoch) {
            why = line_ref(line) + "time '" + std::string(trim(f[c_time])) + "' is not epoch milliseconds or microseconds";
            return false;
        }
        auto number = [&](std::size_t c, const char* what, Decimal& d) {
            if (parse_decimal(trim(f[c]), d)) return true;
            why = line_ref(line) + what + " '" + std::string(trim(f[c])) + "' is not a number";
            return false;
        };
        if (book) {
            for (const char side : {'B', 'S'}) {
                const std::size_t c = side == 'B' ? 1 : 3;
                Decimal p{}, q{};
                if (!number(c, side == 'B' ? "best_bid_price" : "best_ask_price", p) ||
                    !number(c + 1, side == 'B' ? "best_bid_qty" : "best_ask_qty", q))
                    return false;
                LevelDiff::Levels now;
                if (q.mantissa > 0) now.emplace_back(p, q);
                diff.side(scratch, e.t, line, side, now);
            }
            continue;
        }
        e.kind = 'T';
        e.side = side_from_buyer_maker(f[c_bm]);
        if (e.side == '-') {
            why = line_ref(line) + "is_buyer_maker '" + std::string(trim(f[c_bm])) + "' is not true or false";
            return false;
        }
        if (!number(1, "price", e.price) || !number(2, "qty", e.qty)) return false;
        out.ev.push_back(e);
    }
    if (book) out.ev = std::move(scratch.ev);
    return true;
}

[[nodiscard]] inline bool import_binance(const std::string& path, std::string_view text, std::string_view trades,
                                         RawTape& raw, std::string& why) {
    BinanceFile main, other;
    if (!read_binance(path, text, main, why)) return false;
    if (main.type == "bookTicker" && !trades.empty()) {
        if (!read_binance("", trades, other, why)) {
            why = "the trades file beside it: " + why;
            return false;
        }
        if (other.type == "bookTicker") other = BinanceFile{};
    }
    raw.ev.reserve(main.ev.size() + other.ev.size());
    std::merge(main.ev.begin(), main.ev.end(), other.ev.begin(), other.ev.end(), std::back_inserter(raw.ev),
               [](const RawEvent& a, const RawEvent& b) { return a.t < b.t; });
    raw.h.format = "binance";
    raw.h.importer = "binance 1";
    raw.h.source_file = base_name(path);
    const std::string base = base_name(path);
    raw.h.instrument = base.find('-') == std::string::npos ? "" : base.substr(0, base.find('-'));
    raw.kind = TimeKind::Epoch;
    raw.h.depth = main.type == "bookTicker" ? Depth::L2 : Depth::Trades;
    if (!raw.ev.empty()) raw.h.date = time_text(raw.ev.front().t, TimeKind::Epoch).substr(0, 10);
    std::string notes = "binance " + main.type + (main.header ? "" : " (spot, no header)");
    if (main.type == "aggTrades") notes += ": a row is one taker order's trades at one price";
    if (main.type == "bookTicker")
        notes += other.ev.empty() ? "; no trades file beside it, so no prints"
                                  : ", with its " + other.type + " file";
    raw.h.notes = notes;
    return true;
}

// ---- Coinbase --------------------------------------------------------------------------
//
// Coinbase's trades and feeds as JSON, from its published API documentation;
// checked against hand-made samples:
//
//   REST trades (Exchange)       a JSON array of {time, trade_id, price, size, side}, newest first.
//                                side is the resting (maker) order's, so the aggressor is the other.
//   REST trades (Advanced Trade) {"trades": [{trade_id, product_id, price, size, time, side}, ...]}
//   a captured feed, one message a line (JSON Lines), from either API:
//     Exchange        level2: snapshot (bids, asks) then l2update (changes); matches: match;
//                     full: received, open, done, change and match, order by order (L3)
//     Advanced Trade  l2_data (snapshot, update) and market_trades (snapshot, update)
//
// Advanced Trade's trade side is not read: its documentation does not say whose
// side it is, and a guess would put the wrong sign on every print. A feed holding
// the full channel is read order by order and its level2 messages are left out,
// since the orders are the book. Every number is read as the text it was sent as.

[[nodiscard]] inline bool looks_like_coinbase(std::string_view head) {
    const std::size_t at = head.find_first_not_of(" \t\r\n");
    if (at == std::string_view::npos || (head[at] != '{' && head[at] != '[')) return false;
    if (head.find("\"product_id\"") != std::string_view::npos) return true;
    return head.find("\"trade_id\"") != std::string_view::npos && head.find("\"side\"") != std::string_view::npos &&
           head.find("\"size\"") != std::string_view::npos;
}

// a - b, as a decimal; false when it would be negative or cannot be scaled.
[[nodiscard]] inline bool decimal_minus(const Decimal& a, const Decimal& b, Decimal& out) {
    const int d = std::max(a.decimals, b.decimals);
    std::int64_t x = 0, y = 0;
    if (!scale_to(a, d, x) || !scale_to(b, d, y) || x < y) return false;
    out = Decimal{x - y, d};
    return true;
}

class CoinbaseReader {
public:
    std::map<std::string, std::uint64_t> skipped;
    std::string api;    // "exchange" or "advanced trade", from the first message that says
    std::string source; // "REST trades" or "feed"

    // One message of a feed, or one trade of a REST response; line numbers for errors.
    bool message(const Json& m, std::uint64_t line, std::string& why) {
        if (!m.is_object()) return fail(why, line, "a line that is not a JSON object");
        if (m.get("channel") != nullptr) return advanced(m, line, why);
        if (m.get("type") != nullptr) return exchange(m, line, why);
        return fail(why, line, "a message with neither type nor channel: not Coinbase's");
    }
    // A REST trade: Exchange's when `maker_side`, Advanced Trade's otherwise.
    bool rest_trade(const Json& tr, std::uint64_t line, bool maker_side, std::string& why) {
        if (!tr.is_object()) return fail(why, line, "a trade that is not a JSON object");
        std::int64_t t = 0;
        if (!time_of(tr.str("time"), line, t, why)) return false;
        Book& b = book(std::string(tr.str("product_id")));
        if (!seen_trade(b, tr.str("trade_id"))) return true;
        return print(b, tr, t, line, maker_side, why);
    }
    // Ends the read: fills in the times snapshots did not carry, and picks the product.
    bool finish(const std::string& symbol, RawTape& raw, std::string& why) {
        if (!pending_.empty()) {
            if (!any_time_) {
                why = "no message carries a time";
                return false;
            }
            for (auto& [b, i] : pending_) books_[b].ev[i].t = last_time_;
        }
        std::set<std::string> products;
        for (const auto& [p, b] : books_)
            if (!b.ev.empty()) products.insert(p);
        if (symbol.empty() && products.size() > 1) {
            std::string list;
            std::size_t n = 0;
            for (const std::string& x : products) {
                if (n++ == 4) {
                    list += ", ...";
                    break;
                }
                list += (list.empty() ? "" : ", ") + x;
            }
            why = "the file holds " + std::to_string(products.size()) + " products (" + list + "): pick one with symbol=";
            return false;
        }
        const std::string chosen = !symbol.empty() ? symbol : products.empty() ? "" : *products.begin();
        if (!symbol.empty() && products.count(symbol) == 0) {
            why = "nothing for symbol=" + symbol;
            return false;
        }
        for (const auto& [p, b] : books_)
            if (p != chosen && !b.ev.empty()) skipped["messages for other products"] += b.ev.size();
        Book& b = books_[chosen];
        raw.h.instrument = chosen;
        const bool l3 = b.l3;
        for (std::size_t i = 0; i < b.ev.size(); ++i) {
            if (l3 && b.from_level2[i]) {
                ++skipped["level2 rows (the full channel's orders are the book)"];
                continue;
            }
            raw.ev.push_back(b.ev[i]);
        }
        bool levels = false;
        for (const RawEvent& e : raw.ev) levels = levels || e.kind == 'L';
        raw.h.depth = l3 ? Depth::L3 : levels ? Depth::L2 : Depth::Trades;
        std::string notes = "coinbase " + (api.empty() ? std::string("exchange") : api) + " " + source;
        if (l3) notes += ", the full channel order by order; the book starts empty: orders resting before the "
                         "capture began are not in it";
        if (source == "REST trades" && api != "advanced trade")
            notes += "; the side given is the resting order's, so the aggressor is the other";
        if (api == "advanced trade") notes += "; the trade side is not read, since Coinbase does not say whose it is";
        if (b.unknown > 0)
            notes += "; " + grouped(static_cast<std::int64_t>(b.unknown)) + " message" + (b.unknown == 1 ? "" : "s") +
                     " about orders resting before the capture began";
        raw.h.notes = notes;
        return true;
    }

private:
    struct Resting {
        std::uint64_t id;
        char side;
        Decimal price, qty;
    };
    struct Book {
        std::vector<RawEvent> ev;
        std::vector<bool> from_level2; // beside ev: rows the full channel replaces
        std::map<std::string, Resting> resting;
        std::set<std::string> trades;
        std::uint64_t next_id = 1, unknown = 0;
        bool l3 = false, any_update = false, in_snapshot = false;
    };
    std::map<std::string, Book> books_;
    std::vector<std::pair<std::string, std::size_t>> pending_; // snapshot rows waiting for a time
    std::int64_t last_time_ = 0;
    bool any_time_ = false;
    std::string current_; // the product of the message being read

    static bool fail(std::string& why, std::uint64_t line, const std::string& what) {
        why = line_ref(line) + what;
        return false;
    }
    Book& book(const std::string& product) {
        current_ = product;
        return books_[product];
    }
    bool time_of(std::string_view text, std::uint64_t line, std::int64_t& t, std::string& why) {
        TimeKind kind{};
        if (!parse_time(text, TimeFormat::Auto, t, kind) || kind != TimeKind::Epoch)
            return fail(why, line, "time '" + std::string(text) + "' is not a date and time");
        any_time_ = true;
        last_time_ = t;
        for (auto& [b, i] : pending_) books_[b].ev[i].t = t;
        pending_.clear();
        return true;
    }
    bool number(const Json& o, const char* key, std::uint64_t line, Decimal& d, std::string& why) {
        if (parse_decimal(o.str(key), d)) return true;
        return fail(why, line, std::string(key) + " '" + std::string(o.str(key)) + "' is not a number");
    }
    void push(Book& b, std::int64_t t, std::uint64_t line, char kind, char side, std::uint64_t id, const Decimal& p,
              const Decimal& q, bool level2) {
        RawEvent e;
        e.t = t;
        e.kind = kind;
        e.side = side;
        e.id = id;
        e.price = p;
        e.qty = q;
        e.line = line;
        b.ev.push_back(e);
        b.from_level2.push_back(level2);
    }
    static bool seen_trade(Book& b, std::string_view trade_id) {
        if (trade_id.empty()) return true;
        return b.trades.insert(std::string(trade_id)).second; // the same trade on two channels counts once
    }
    // A trade. `maker_side`: the side given is the resting order's.
    bool print(Book& b, const Json& m, std::int64_t t, std::uint64_t line, bool maker_side, std::string& why) {
        Decimal p{}, q{};
        if (!number(m, "price", line, p, why) || !number(m, "size", line, q, why)) return false;
        if (q.mantissa <= 0) {
            ++skipped["trades of size zero"];
            return true;
        }
        char side = '-';
        if (maker_side) {
            const char given = side_from_text(m.str("side"));
            side = given == 'B' ? 'S' : given == 'S' ? 'B' : '-';
        }
        std::uint64_t id = 0;
        const auto it = b.resting.find(std::string(m.str("maker_order_id")));
        if (it != b.resting.end()) {
            id = it->second.id;
            Decimal left{};
            if (decimal_minus(it->second.qty, q, left) && left.mantissa > 0) it->second.qty = left;
            else b.resting.erase(it);
        } else if (b.l3 && !m.str("maker_order_id").empty()) {
            ++b.unknown;
        }
        push(b, t, line, 'T', side, id, p, q, false);
        return true;
    }
    bool level(Book& b, std::int64_t t, bool timed, std::uint64_t line, char side, const Json& p, const Json& q,
               std::string& why) {
        Decimal pd{}, qd{};
        if (!parse_decimal(p.text, pd) || !parse_decimal(q.text, qd))
            return fail(why, line, "a level '" + p.text + "', '" + q.text + "' that is not two numbers");
        push(b, t, line, 'L', side, 0, pd, qd, true);
        if (!timed) pending_.emplace_back(current_, b.ev.size() - 1);
        return true;
    }
    void snapshot_starts(Book& b, std::int64_t t, std::uint64_t line, bool timed) {
        if (b.any_update && !b.in_snapshot) {
            push(b, t, line, 'R', '-', 0, Decimal{}, Decimal{}, true);
            if (!timed) pending_.emplace_back(current_, b.ev.size() - 1);
        }
        b.in_snapshot = true;
    }

    bool exchange(const Json& m, std::uint64_t line, std::string& why) {
        if (api.empty()) api = "exchange";
        source = "feed";
        const std::string type(m.str("type"));
        if (type == "last_match") { // its time is before the capture began, so it times nothing
            ++skipped["last_match messages (a trade from before the capture began)"];
            return true;
        }
        Book& b = book(std::string(m.str("product_id")));
        std::int64_t t = last_time_;
        const bool timed = !m.str("time").empty();
        if (timed && !time_of(m.str("time"), line, t, why)) return false;
        if (type == "snapshot") {
            snapshot_starts(b, t, line, timed);
            for (const char side : {'B', 'S'}) {
                const Json* levels = m.get(side == 'B' ? "bids" : "asks");
                if (levels == nullptr || !levels->is_array()) return fail(why, line, "a snapshot with no bids or asks");
                for (const Json& l : levels->items) {
                    if (!l.is_array() || l.items.size() < 2) return fail(why, line, "a snapshot level that is not [price, size]");
                    if (!level(b, t, timed, line, side, l.items[0], l.items[1], why)) return false;
                }
            }
            return true;
        }
        if (type == "l2update") {
            if (!timed) return fail(why, line, "an l2update with no time");
            b.in_snapshot = false;
            b.any_update = true;
            const Json* ch = m.get("changes");
            if (ch == nullptr || !ch->is_array()) return fail(why, line, "an l2update with no changes");
            for (const Json& c : ch->items) {
                if (!c.is_array() || c.items.size() < 3) return fail(why, line, "a change that is not [side, price, size]");
                const char side = side_from_text(c.items[0].text);
                if (side == '-') return fail(why, line, "side '" + c.items[0].text + "' is not buy or sell");
                if (!level(b, t, true, line, side, c.items[1], c.items[2], why)) return false;
            }
            return true;
        }
        if (type == "match") {
            if (!timed) return fail(why, line, "a match with no time");
            if (!seen_trade(b, m.str("trade_id"))) return true;
            return print(b, m, t, line, true, why);
        }
        if (type == "open" || type == "done" || type == "change") {
            if (!timed) return fail(why, line, "an order message with no time");
            b.l3 = true;
            const std::string oid(m.str("order_id"));
            const auto it = b.resting.find(oid);
            if (type == "open") {
                const char side = side_from_text(m.str("side"));
                Decimal p{}, q{};
                if (side == '-') return fail(why, line, "side '" + std::string(m.str("side")) + "' is not buy or sell");
                if (!number(m, "price", line, p, why) || !number(m, "remaining_size", line, q, why)) return false;
                if (q.mantissa <= 0) {
                    ++skipped["opens of size zero"];
                    return true;
                }
                const std::uint64_t id = b.next_id++;
                b.resting[oid] = Resting{id, side, p, q};
                push(b, t, line, 'A', side, id, p, q, false);
                return true;
            }
            if (it == b.resting.end()) {
                // A taker that never rested, or an order from before the capture.
                if (type == "change" || m.str("reason") == "canceled") ++b.unknown;
                return true;
            }
            Resting& r = it->second;
            if (type == "done") {
                push(b, t, line, 'C', r.side, r.id, r.price, r.qty, false);
                b.resting.erase(it);
                return true;
            }
            // change: a smaller size keeps the order's place; a new price does not.
            Decimal np = r.price, nq = r.qty;
            if (!m.str("new_price").empty() && !number(m, "new_price", line, np, why)) return false;
            if (!m.str("new_size").empty() && !number(m, "new_size", line, nq, why)) return false;
            Decimal less{};
            if (canonical(np) == canonical(r.price) && decimal_minus(r.qty, nq, less)) {
                if (less.mantissa > 0) push(b, t, line, 'C', r.side, r.id, r.price, less, false);
                if (nq.mantissa > 0) r.qty = nq;
                else b.resting.erase(it);
                return true;
            }
            push(b, t, line, 'C', r.side, r.id, r.price, r.qty, false);
            if (nq.mantissa <= 0) {
                b.resting.erase(it);
                return true;
            }
            r.price = np;
            r.qty = nq;
            push(b, t, line, 'A', r.side, r.id, np, nq, false);
            return true;
        }
        if (type == "received" || type == "activate") {
            ++skipped["received and activate messages (an order not yet on the book)"];
            return true;
        }
        ++skipped[type + " messages"];
        return true;
    }

    bool advanced(const Json& m, std::uint64_t line, std::string& why) {
        api = "advanced trade";
        source = "feed";
        const std::string channel(m.str("channel"));
        if (channel != "l2_data" && channel != "market_trades") {
            ++skipped[channel + " messages"];
            return true;
        }
        std::int64_t t = 0;
        if (!time_of(m.str("timestamp"), line, t, why)) return false;
        const Json* events = m.get("events");
        if (events == nullptr || !events->is_array()) return fail(why, line, "a message with no events");
        for (const Json& ev : events->items) {
            const std::string type(ev.str("type"));
            if (channel == "market_trades") {
                const Json* trades = ev.get("trades");
                if (trades == nullptr || !trades->is_array()) return fail(why, line, "market_trades with no trades");
                for (const Json& tr : trades->items) {
                    Book& b = book(std::string(tr.str("product_id")));
                    if (type == "snapshot") {
                        ++skipped["market_trades snapshots (trades from before the capture began)"];
                        continue;
                    }
                    if (!seen_trade(b, tr.str("trade_id"))) continue;
                    if (!print(b, tr, t, line, false, why)) return false;
                }
                continue;
            }
            Book& b = book(std::string(ev.str("product_id")));
            if (type == "snapshot") snapshot_starts(b, t, line, true);
            else {
                b.in_snapshot = false;
                b.any_update = true;
            }
            const Json* ups = ev.get("updates");
            if (ups == nullptr || !ups->is_array()) return fail(why, line, "l2_data with no updates");
            for (const Json& u : ups->items) {
                const std::string sd = lower(u.str("side"));
                if (sd != "bid" && sd != "offer" && sd != "ask") return fail(why, line, "side '" + sd + "' is not bid or offer");
                const Json* p = u.get("price_level");
                const Json* q = u.get("new_quantity");
                if (p == nullptr || q == nullptr) return fail(why, line, "an update with no price_level or new_quantity");
                if (!level(b, t, true, line, sd == "bid" ? 'B' : 'S', *p, *q, why)) return false;
            }
        }
        return true;
    }
};

[[nodiscard]] inline bool import_coinbase(const std::string& path, std::string_view text, const ImportOptions& opts,
                                          RawTape& raw, std::string& why) {
    CoinbaseReader r;
    const std::size_t at = text.find_first_not_of(" \t\r\n");
    if (at == std::string_view::npos) {
        why = "the file is empty";
        return false;
    }
    Json doc;
    if (text[at] == '[' || (text[at] == '{' && JsonReader(text).parse(doc, why) && doc.get("trades") != nullptr)) {
        // One document: a REST response, or a feed saved as an array.
        if (text[at] == '[' && !parse_json(text, doc, why)) return false;
        const bool advanced = doc.is_object();
        const Json* list = advanced ? doc.get("trades") : &doc;
        if (list == nullptr || !list->is_array()) {
            why = "a JSON document with no list of trades or messages in it";
            return false;
        }
        r.source = "REST trades";
        if (advanced) r.api = "advanced trade";
        std::uint64_t n = 0;
        for (const Json& item : list->items) {
            ++n;
            const bool msg = item.get("type") != nullptr || item.get("channel") != nullptr;
            if (msg ? !r.message(item, n, why) : !r.rest_trade(item, n, !advanced, why)) {
                why = "item " + why.substr(5);
                return false;
            }
        }
    } else {
        why.clear();
        const std::vector<std::string_view> lines = lines_of(text);
        for (std::size_t li = 0; li < lines.size(); ++li) {
            if (trim(lines[li]).empty()) continue;
            Json m;
            if (!parse_json(lines[li], m, why)) {
                why = line_ref(li + 1) + why + " (a feed is read one JSON message a line)";
                return false;
            }
            if (!r.message(m, li + 1, why)) return false;
        }
    }
    if (!r.finish(opts.symbol, raw, why)) return false;
    raw.skipped = r.skipped;
    raw.h.format = "coinbase";
    raw.h.importer = "coinbase 1";
    raw.h.source_file = base_name(path);
    raw.kind = TimeKind::Epoch;
    if (!raw.ev.empty()) {
        std::int64_t first = raw.ev.front().t;
        for (const RawEvent& e : raw.ev) first = std::min(first, e.t);
        raw.h.date = time_text(first, TimeKind::Epoch).substr(0, 10);
    }
    return true;
}

// ---- Kraken ----------------------------------------------------------------------------
//
// Kraken's trades and feed, from its published documentation; checked against
// hand-made samples:
//
//   trade history     the CSV files Kraken offers for download, one a pair (XBTUSD.csv):
//                     timestamp,price,volume, no header, seconds since the epoch, no side
//   REST Trades       {"error": [], "result": {"XXBTZUSD": [[price, volume, time, "b"|"s",
//                     "m"|"l", misc, trade_id], ...], "last": "..."}}; b and s are the taker's side
//   a captured feed   WebSocket v2, one JSON message a line: book (snapshot, update) and
//                     trade (snapshot, update), where side is the taker's
//
// The trade history cannot say which side crossed the spread, so its prints have
// no side, and it is found by its shape: a file named for its pair, holding
// three numbers a line.

[[nodiscard]] inline bool looks_like_kraken_history(std::string_view path, std::string_view first_line) {
    const std::string base = base_name(path);
    if (base.size() < 5 || base.substr(base.size() - 4) != ".csv") return false;
    for (std::size_t i = 0; i + 4 < base.size(); ++i)
        if (!((base[i] >= 'A' && base[i] <= 'Z') || is_digit(base[i]))) return false;
    std::vector<std::string> f;
    if (!split_csv(first_line, ',', f) || f.size() != 3 || trim(f[0]).size() != 10) return false;
    for (const std::string& x : f) {
        Decimal d{};
        if (!parse_decimal(trim(x), d)) return false;
    }
    return true;
}

[[nodiscard]] inline bool looks_like_kraken_json(std::string_view head) {
    const std::size_t at = head.find_first_not_of(" \t\r\n");
    if (at == std::string_view::npos || head[at] != '{') return false;
    if (head.find("\"error\"") != std::string_view::npos && head.find("\"result\"") != std::string_view::npos) return true;
    return head.find("\"channel\"") != std::string_view::npos && head.find("\"symbol\"") != std::string_view::npos &&
           head.find("\"product_id\"") == std::string_view::npos;
}

[[nodiscard]] inline bool import_kraken(const std::string& path, std::string_view text, const ImportOptions& opts,
                                        RawTape& raw, std::string& why) {
    raw.h.format = "kraken";
    raw.h.importer = "kraken 1";
    raw.h.source_file = base_name(path);
    raw.kind = TimeKind::Epoch;
    auto date = [&raw] {
        if (!raw.ev.empty()) {
            std::int64_t first = raw.ev.front().t;
            for (const RawEvent& e : raw.ev) first = std::min(first, e.t);
            raw.h.date = time_text(first, TimeKind::Epoch).substr(0, 10);
        }
    };
    auto when = [&why](std::string_view v, std::uint64_t line, std::int64_t& t) {
        TimeKind kind{};
        if (parse_time(v, TimeFormat::Auto, t, kind) && kind == TimeKind::Epoch) return true;
        why = line_ref(line) + "time '" + std::string(v) + "' is not a time since the epoch";
        return false;
    };
    const std::size_t at = text.find_first_not_of(" \t\r\n");
    if (at == std::string_view::npos) {
        why = "the file is empty";
        return false;
    }
    const std::vector<std::string_view> lines = lines_of(text);
    if (text[at] != '{') {
        // The trade history: timestamp, price, volume.
        std::vector<std::string> f;
        for (std::size_t li = 0; li < lines.size(); ++li) {
            if (trim(lines[li]).empty()) continue;
            const std::uint64_t line = li + 1;
            if (!split_csv(lines[li], ',', f) || f.size() != 3) {
                why = line_ref(line) + std::to_string(f.size()) + " fields: Kraken's trade history has three " +
                      "(timestamp, price, volume)" + (f.size() == 7 ? "; seven is its OHLCVT bars, which are not trades" : "");
                return false;
            }
            RawEvent e;
            e.line = line;
            e.kind = 'T';
            if (!when(trim(f[0]), line, e.t)) return false;
            if (!parse_decimal(trim(f[1]), e.price) || !parse_decimal(trim(f[2]), e.qty)) {
                why = line_ref(line) + "price '" + std::string(trim(f[1])) + "' or volume '" + std::string(trim(f[2])) +
                      "' is not a number";
                return false;
            }
            raw.ev.push_back(e);
        }
        const std::string base = base_name(path);
        raw.h.instrument = base.substr(0, base.find('.'));
        raw.h.depth = Depth::Trades;
        raw.h.notes = "kraken trade history; it does not say which side crossed the spread, so prints have no side";
        date();
        return true;
    }

    Json doc;
    std::string first_why;
    if (JsonReader(text).parse(doc, first_why) && doc.get("result") != nullptr) {
        // REST Trades: one pair, under its own key.
        const Json* res = doc.get("result");
        const Json* errs = doc.get("error");
        if (errs != nullptr && errs->is_array() && !errs->items.empty()) {
            why = "Kraken returned an error: " + errs->items[0].text;
            return false;
        }
        const Json* list = nullptr;
        if (res->is_object())
            for (std::size_t i = 0; i < res->keys.size(); ++i)
                if (res->keys[i] != "last" && res->items[i].is_array()) {
                    if (list != nullptr) {
                        why = "the response holds more than one pair";
                        return false;
                    }
                    list = &res->items[i];
                    raw.h.instrument = res->keys[i];
                }
        if (list == nullptr) {
            why = "a Kraken response with no trades in it";
            return false;
        }
        std::uint64_t n = 0;
        for (const Json& tr : list->items) {
            ++n;
            if (!tr.is_array() || tr.items.size() < 4) {
                why = "trade " + std::to_string(n) + ": not [price, volume, time, side, ...]";
                return false;
            }
            RawEvent e;
            e.line = n;
            e.kind = 'T';
            if (!when(tr.items[2].text, n, e.t)) return false;
            if (!parse_decimal(tr.items[0].text, e.price) || !parse_decimal(tr.items[1].text, e.qty)) {
                why = "trade " + std::to_string(n) + ": price or volume is not a number";
                return false;
            }
            e.side = side_from_text(tr.items[3].text);
            raw.ev.push_back(e);
        }
        raw.h.depth = Depth::Trades;
        raw.h.notes = "kraken REST trades; the side given is the taker's";
        date();
        return true;
    }

    // A captured WebSocket v2 feed.
    struct Book {
        std::vector<RawEvent> ev;
        std::vector<std::size_t> pending; // rows of an untimed snapshot: they take the next time seen
        std::int64_t last_t = 0;
        bool timed = false, any_update = false, in_snapshot = false;
        void time(std::int64_t t) {
            for (const std::size_t i : pending) ev[i].t = t;
            pending.clear();
            last_t = t;
            timed = true;
        }
    };
    std::map<std::string, Book> books;
    std::set<std::string> trade_ids;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        if (trim(lines[li]).empty()) continue;
        const std::uint64_t line = li + 1;
        Json m;
        if (!parse_json(lines[li], m, why)) {
            why = line_ref(line) + why + " (a feed is read one JSON message a line)";
            return false;
        }
        const std::string channel(m.str("channel")), type(m.str("type"));
        if (channel != "book" && channel != "trade") {
            ++raw.skipped[(channel.empty() ? std::string(m.str("method")).empty() ? "other" : "method" : channel) +
                          " messages"];
            continue;
        }
        const Json* data = m.get("data");
        if (data == nullptr || !data->is_array()) {
            why = line_ref(line) + "a " + channel + " message with no data";
            return false;
        }
        for (const Json& d : data->items) {
            const std::string sym(d.str("symbol"));
            Book& b = books[sym];
            if (channel == "trade") {
                if (type == "snapshot") {
                    ++raw.skipped["trade snapshots (trades from before the capture began)"];
                    continue;
                }
                if (!d.str("trade_id").empty() && !trade_ids.insert(sym + "/" + std::string(d.str("trade_id"))).second)
                    continue;
                RawEvent e;
                e.line = line;
                e.kind = 'T';
                if (!when(d.str("timestamp"), line, e.t)) return false;
                if (!parse_decimal(d.str("price"), e.price) || !parse_decimal(d.str("qty"), e.qty)) {
                    why = line_ref(line) + "price '" + std::string(d.str("price")) + "' or qty '" +
                          std::string(d.str("qty")) + "' is not a number";
                    return false;
                }
                e.side = side_from_text(d.str("side"));
                b.time(e.t);
                b.ev.push_back(e);
                continue;
            }
            // book: a snapshot carrying no time takes the next one this book sees.
            std::int64_t t = b.last_t;
            const bool timed = !d.str("timestamp").empty();
            if (timed) {
                if (!when(d.str("timestamp"), line, t)) return false;
                b.time(t);
            }
            const std::size_t from = b.ev.size();
            if (type == "snapshot") {
                if (b.any_update && !b.in_snapshot) b.ev.push_back(RawEvent{t, 'R', '-', 0, {}, {}, line});
                b.in_snapshot = true;
            } else {
                b.in_snapshot = false;
                b.any_update = true;
            }
            for (const char side : {'B', 'S'}) {
                const Json* levels = d.get(side == 'B' ? "bids" : "asks");
                if (levels == nullptr) continue;
                for (const Json& l : levels->items) {
                    RawEvent e;
                    e.t = t;
                    e.line = line;
                    e.kind = 'L';
                    e.side = side;
                    if (!parse_decimal(l.str("price"), e.price) || !parse_decimal(l.str("qty"), e.qty)) {
                        why = line_ref(line) + "a level whose price '" + std::string(l.str("price")) + "' or qty '" +
                              std::string(l.str("qty")) + "' is not a number";
                        return false;
                    }
                    b.ev.push_back(e);
                }
            }
            if (!timed)
                for (std::size_t i = from; i < b.ev.size(); ++i) b.pending.push_back(i);
        }
    }
    std::set<std::string> symbols;
    for (const auto& [sym, b] : books)
        if (!b.ev.empty()) symbols.insert(sym);
    if (opts.symbol.empty() && symbols.size() > 1) {
        std::string list;
        for (const std::string& x : symbols) list += (list.empty() ? "" : ", ") + x;
        why = "the file holds " + std::to_string(symbols.size()) + " symbols (" + list + "): pick one with symbol=";
        return false;
    }
    const std::string chosen = !opts.symbol.empty() ? opts.symbol : symbols.empty() ? "" : *symbols.begin();
    if (!opts.symbol.empty() && symbols.count(chosen) == 0) {
        why = "nothing for symbol=" + chosen;
        return false;
    }
    for (const auto& [sym, b] : books)
        if (sym != chosen && !b.ev.empty()) raw.skipped["messages for other symbols"] += b.ev.size();
    Book& b = books[chosen];
    if (!b.pending.empty()) {
        if (!b.timed) {
            why = "no message carries a time";
            return false;
        }
        b.time(b.last_t);
    }
    raw.ev = std::move(b.ev);
    raw.h.instrument = chosen;
    bool levels = false;
    for (const RawEvent& e : raw.ev) levels = levels || e.kind == 'L';
    raw.h.depth = levels ? Depth::L2 : Depth::Trades;
    raw.h.notes = "kraken feed (WebSocket v2); a trade's side is the taker's";
    date();
    return true;
}

// ---- Nasdaq TotalView-ITCH 5.0 ---------------------------------------------------------
//
// Nasdaq's binary feed for a whole day, as its sample files hold it
// (MMDDYYYY.NASDAQ_ITCH50, uncompressed), from the published specification:
// each message is a two-byte big-endian length and then the message, which
// starts with its type, stock locate, tracking number and six bytes of
// nanoseconds since midnight. Numbers are big-endian; prices have four decimals.
//
//   A, F  add order: an A on the tape          E  executed at the order's price: a T naming it
//   C     executed at another price: a T, or, when Nasdaq marks it not printable
//         (part of a cross), a C, so it leaves the book without making a print
//   X     partial cancel: a C                   D  delete: a C of what is left
//   U     replace: a C of what is left and an A under the new reference
//   P     a hidden order's execution: a print, with no side (Nasdaq writes B whatever it was)
//
// Cross trades (Q) are left out: the orders behind them leave the book as the
// non-printable executions above. A day's file holds every stock, so it is read
// for one, named with symbol=, and streamed rather than read whole: a file is
// gigabytes.

// A message's length by its type in ITCH 5.0; 0 for a type 5.0 does not have.
[[nodiscard]] inline std::size_t itch_length(unsigned char type) noexcept {
    switch (type) {
        case 'S': case 'W': return 12;
        case 'R': return 39;
        case 'H': return 25;
        case 'Y': case 'N': return 20;
        case 'L': return 26;
        case 'V': case 'J': case 'U': return 35;
        case 'K': return 28;
        case 'h': return 21;
        case 'A': case 'C': return 36;
        case 'F': case 'Q': return 40;
        case 'E': return 31;
        case 'X': return 23;
        case 'D': case 'B': return 19;
        case 'P': return 44;
        case 'I': return 50;
        case 'O': return 48;
        default: return 0;
    }
}

// True when the start of a file walks as ITCH 5.0 messages: every length one
// of the specification's, for the type it carries. A text file never starts
// with the zero byte a length does.
[[nodiscard]] inline bool looks_like_itch(std::string_view head) noexcept {
    std::size_t at = 0, n = 0;
    while (at + 3 <= head.size() && n < 64) {
        const std::size_t len = (static_cast<std::size_t>(static_cast<unsigned char>(head[at])) << 8) |
                                static_cast<unsigned char>(head[at + 1]);
        if (len == 0 || itch_length(static_cast<unsigned char>(head[at + 2])) != len) return false;
        at += 2 + len;
        ++n;
    }
    return n >= 2 || (n == 1 && at == head.size());
}

// The day an ITCH file's name gives: Nasdaq's MMDDYYYY, or YYYYMMDD.
[[nodiscard]] inline bool itch_date(std::string_view base, std::string& date) {
    std::size_t at = 0;
    while (at < base.size() && !is_digit(base[at])) ++at;
    if (at + 8 > base.size()) return false;
    for (std::size_t i = at; i < at + 8; ++i)
        if (!is_digit(base[i])) return false;
    const std::string d(base.substr(at, 8));
    auto ok = [](const std::string& y, const std::string& m, const std::string& dd) {
        return (y.starts_with("19") || y.starts_with("20")) && m >= "01" && m <= "12" && dd >= "01" && dd <= "31";
    };
    if (ok(d.substr(4, 4), d.substr(0, 2), d.substr(2, 2))) date = d.substr(4, 4) + "-" + d.substr(0, 2) + "-" + d.substr(2, 2);
    else if (ok(d.substr(0, 4), d.substr(4, 2), d.substr(6, 2))) date = d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2);
    else return false;
    return true;
}

class ItchReader {
public:
    ItchReader(std::string symbol, RawTape& raw) : symbol_(std::move(symbol)), raw_(raw) {}

    // Reads whatever whole messages `chunk` completes; keeps a partial one for the next.
    bool feed(std::string_view chunk, std::string& why) {
        if (buf_.empty()) {
            const std::size_t used = walk(chunk, why);
            if (used == npos) return false;
            buf_.assign(chunk.substr(used));
        } else {
            buf_.append(chunk);
            const std::size_t used = walk(buf_, why);
            if (used == npos) return false;
            buf_.erase(0, used);
        }
        return true;
    }
    bool end(std::string& why) {
        if (!buf_.empty()) {
            why = "the file is cut short: its last message ends after the file does";
            return false;
        }
        return true;
    }
    std::uint64_t messages = 0, not_printable = 0, crosses = 0;

private:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    struct Resting {
        char side;
        std::uint32_t price, shares;
    };
    std::string symbol_;
    RawTape& raw_;
    std::string buf_;
    std::uint64_t offset_ = 0; // of buf_'s first byte in the file
    std::unordered_map<std::uint64_t, Resting> book_;

    static std::uint64_t be(const unsigned char* p, int n) noexcept {
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
        return v;
    }
    bool ours(const unsigned char* stock) const noexcept {
        std::size_t n = 8;
        while (n > 0 && stock[n - 1] == ' ') --n;
        return symbol_.size() == n && std::equal(symbol_.begin(), symbol_.end(), stock);
    }
    void push(std::int64_t t, char kind, char side, std::uint64_t id, std::uint32_t price, std::uint32_t qty) {
        RawEvent e;
        e.t = t;
        e.kind = kind;
        e.side = side;
        e.id = id;
        e.price = Decimal{static_cast<std::int64_t>(price), 4};
        e.qty = Decimal{static_cast<std::int64_t>(qty), 0};
        e.line = messages;
        raw_.ev.push_back(e);
    }
    // Takes `n` shares off a resting order, removing it when none are left.
    void reduce(std::unordered_map<std::uint64_t, Resting>::iterator it, std::uint32_t n) {
        if (n >= it->second.shares) book_.erase(it);
        else it->second.shares -= n;
    }
    std::size_t walk(std::string_view b, std::string& why) {
        std::size_t at = 0;
        while (at + 2 <= b.size()) {
            const auto* p = reinterpret_cast<const unsigned char*>(b.data() + at);
            const std::size_t len = (static_cast<std::size_t>(p[0]) << 8) | p[1];
            if (at + 2 + len > b.size()) break;
            ++messages;
            if (len == 0) {
                why = "byte " + std::to_string(offset_ + at + 1) + ": a message of length zero: not an ITCH 5.0 file";
                return npos;
            }
            const std::size_t want = itch_length(p[2]);
            if (want != 0 && want != len) {
                why = "byte " + std::to_string(offset_ + at + 1) + ": a '" + std::string(1, static_cast<char>(p[2])) +
                      "' message of " + std::to_string(len) + " bytes, where ITCH 5.0's is " + std::to_string(want) +
                      ": not ITCH 5.0, or the file is damaged";
                return npos;
            }
            if (want == 0) ++raw_.skipped["messages of a type ITCH 5.0 does not have"];
            else message(p + 2);
            at += 2 + len;
        }
        offset_ += at;
        return at;
    }
    void message(const unsigned char* m) {
        const auto t = static_cast<std::int64_t>(be(m + 5, 6));
        switch (m[0]) {
            case 'A':
            case 'F': {
                if (!ours(m + 24)) return;
                const std::uint64_t ref = be(m + 11, 8);
                const char side = m[19] == 'B' ? 'B' : 'S';
                const auto shares = static_cast<std::uint32_t>(be(m + 20, 4)), price = static_cast<std::uint32_t>(be(m + 32, 4));
                if (shares == 0) return;
                push(t, 'A', side, ref, price, shares);
                book_[ref] = Resting{side, price, shares};
                return;
            }
            case 'E':
            case 'C': {
                const auto it = book_.find(be(m + 11, 8));
                if (it == book_.end()) return;
                const auto n = static_cast<std::uint32_t>(be(m + 19, 4));
                if (m[0] == 'C' && m[31] == 'N') {
                    ++not_printable;
                    push(t, 'C', it->second.side, it->first, it->second.price, n);
                } else {
                    const auto price = m[0] == 'C' ? static_cast<std::uint32_t>(be(m + 32, 4)) : it->second.price;
                    push(t, 'T', it->second.side == 'B' ? 'S' : 'B', it->first, price, n);
                }
                reduce(it, n);
                return;
            }
            case 'X': {
                const auto it = book_.find(be(m + 11, 8));
                if (it == book_.end()) return;
                const auto n = std::min(static_cast<std::uint32_t>(be(m + 19, 4)), it->second.shares);
                push(t, 'C', it->second.side, it->first, it->second.price, n);
                reduce(it, n);
                return;
            }
            case 'D': {
                const auto it = book_.find(be(m + 11, 8));
                if (it == book_.end()) return;
                push(t, 'C', it->second.side, it->first, it->second.price, it->second.shares);
                book_.erase(it);
                return;
            }
            case 'U': {
                const auto it = book_.find(be(m + 11, 8));
                if (it == book_.end()) return;
                const Resting r = it->second;
                push(t, 'C', r.side, it->first, r.price, r.shares);
                book_.erase(it);
                const std::uint64_t ref = be(m + 19, 8);
                const auto shares = static_cast<std::uint32_t>(be(m + 27, 4)), price = static_cast<std::uint32_t>(be(m + 31, 4));
                if (shares == 0) return;
                push(t, 'A', r.side, ref, price, shares);
                book_[ref] = Resting{r.side, price, shares};
                return;
            }
            case 'P': {
                if (!ours(m + 24)) return;
                const auto shares = static_cast<std::uint32_t>(be(m + 20, 4));
                if (shares > 0) push(t, 'T', '-', 0, static_cast<std::uint32_t>(be(m + 32, 4)), shares);
                return;
            }
            case 'Q':
                if (ours(m + 19)) ++crosses;
                return;
            default: return;
        }
    }
};

[[nodiscard]] inline bool itch_start(const std::string& path, const ImportOptions& opts, RawTape& raw, std::string& why) {
    if (opts.symbol.empty()) {
        why = "an ITCH file holds every stock the exchange trades: pick one with symbol= (for example symbol=AAPL)";
        return false;
    }
    if (!itch_date(base_name(path), raw.h.date)) {
        why = "the file's name does not give its day: ITCH times are from midnight, so name the file as Nasdaq does "
              "(MMDDYYYY.NASDAQ_ITCH50)";
        return false;
    }
    raw.h.format = "itch";
    raw.h.importer = "itch 1 (TotalView-ITCH 5.0)";
    raw.h.source_file = base_name(path);
    raw.h.instrument = opts.symbol;
    raw.h.depth = Depth::L3;
    raw.kind = TimeKind::Midnight;
    return true;
}

[[nodiscard]] inline bool itch_finish(const ItchReader& r, const ImportOptions& opts, RawTape& raw, std::string& why) {
    if (raw.ev.empty()) {
        why = "no orders or trades for symbol=" + opts.symbol + " in " + grouped(static_cast<std::int64_t>(r.messages)) +
              " messages";
        return false;
    }
    if (r.crosses > 0)
        raw.skipped["cross trades (the orders behind them leave the book as executions marked not printable)"] = r.crosses;
    std::string notes = "nasdaq totalview-itch 5.0: " + grouped(static_cast<std::int64_t>(r.messages)) +
                        " messages read, " + opts.symbol + "'s kept; a day's file starts from an empty book";
    if (r.not_printable > 0)
        notes += "; " + grouped(static_cast<std::int64_t>(r.not_printable)) + " execution" +
                 (r.not_printable == 1 ? "" : "s") + " marked not printable left the book as cancels, with no print";
    raw.h.notes = notes;
    return true;
}

// From bytes in memory (the tests'); ingest streams the file with import_itch_file.
[[nodiscard]] inline bool import_itch(const std::string& path, std::string_view bytes, const ImportOptions& opts,
                                      RawTape& raw, std::string& why) {
    if (!itch_start(path, opts, raw, why)) return false;
    ItchReader r(opts.symbol, raw);
    return r.feed(bytes, why) && r.end(why) && itch_finish(r, opts, raw, why);
}

// ---- companions -------------------------------------------------------------------------

// A second file some formats read beside the first: LOBSTER's orderbook file,
// a Tardis book file's or a Binance bookTicker file's trades. Candidates, best first; ingest reads the first
// that exists.
struct Companion {
    std::string path;
    std::string what; // "the orderbook file"
};

[[nodiscard]] inline std::vector<Companion> companions_of(const std::string& path, const std::string& format) {
    std::vector<Companion> out;
    const std::string base = base_name(path);
    const std::string dir = path.substr(0, path.size() - base.size());
    const std::size_t m = base.find("_message_");
    if (m != std::string::npos && (format.empty() || format == "lobster"))
        out.push_back({dir + base.substr(0, m) + "_orderbook_" + base.substr(m + 9), "the orderbook file"});
    if (format.empty() || format == "tardis") {
        for (const char* type : {"_incremental_book_L2_", "_quotes_", "_book_snapshot_"}) {
            const std::size_t at = base.find(type);
            if (at == std::string::npos) continue;
            std::size_t end = at + std::string_view(type).size();
            if (std::string_view(type) == "_book_snapshot_") { // _book_snapshot_25_
                while (end < base.size() && is_digit(base[end])) ++end;
                if (end >= base.size() || base[end] != '_') continue;
                ++end;
            }
            out.push_back({dir + base.substr(0, at) + "_trades_" + base.substr(end), "its trades file"});
        }
    }
    const std::size_t b = base.find("-bookTicker-");
    if (b != std::string::npos && (format.empty() || format == "binance")) {
        out.push_back({dir + base.substr(0, b) + "-trades-" + base.substr(b + 12), "its trades file"});
        out.push_back({dir + base.substr(0, b) + "-aggTrades-" + base.substr(b + 12), "its aggTrades file"});
    }
    return out;
}

// ---- detection and the front door ------------------------------------------------------

inline constexpr const char* kFormats = "csv lobster databento tardis binance coinbase kraken itch";

// Which importer reads this file. `head` is the start of it: its first line
// decides, except for JSON, which may spread one record over several lines.
[[nodiscard]] inline std::string detect_format(std::string_view path, std::string_view head) {
    if (looks_like_itch(head.substr(0, 4096))) return "itch";
    std::string_view first_line = head.substr(0, head.find('\n'));
    if (!first_line.empty() && first_line.back() == '\r') first_line.remove_suffix(1);
    if (first_line.substr(0, 3) == "DBN" || looks_like_databento_csv(first_line)) return "databento";
    if (looks_like_coinbase(head.substr(0, 4096))) return "coinbase";
    if (looks_like_kraken_json(head.substr(0, 4096)) || looks_like_kraken_history(path, first_line)) return "kraken";
    if (looks_like_tardis(first_line)) return "tardis";
    if (looks_like_binance(path, first_line)) return "binance";
    if (looks_like_lobster(path, first_line)) return "lobster";
    return "csv";
}

// Refuses a file no importer can read, saying what to do instead.
[[nodiscard]] inline bool readable(const std::string& file, std::string_view bytes, std::string& why) {
    auto starts = [&bytes](std::initializer_list<unsigned char> magic) {
        std::size_t i = 0;
        for (const unsigned char c : magic) {
            if (i >= bytes.size() || static_cast<unsigned char>(bytes[i]) != c) return false;
            ++i;
        }
        return true;
    };
    if (starts({0x1f, 0x8b})) why = "compressed with gzip: run gzip -dk " + file + " and add the result";
    else if (starts({0x28, 0xb5, 0x2f, 0xfd})) why = "compressed with zstd: run zstd -d " + file + " and add the result";
    else if (starts({'P', 'K', 0x03, 0x04})) why = "a zip archive: unzip " + file + " and add the file inside";
    else if (bytes.empty()) why = "the file is empty";
    else if (starts({'D', 'B', 'N'}) || looks_like_itch(bytes.substr(0, 4096))) return true;
    else if (bytes.substr(0, std::min<std::size_t>(bytes.size(), 4096)).find('\0') != std::string_view::npos)
        why = "a binary file of no format this build reads (" + std::string(kFormats) + ")";
    else return true;
    return false;
}

// What every importer's tape gets last: its name, then finish().
[[nodiscard]] inline bool name_and_finish(const std::string& path, const std::string& format, const ImportOptions& opts,
                                          RawTape& raw, Tape& out, std::string& why) {
    raw.h.name = !opts.name.empty() ? opts.name : name_from_file(path);
    if ((format == "lobster" || format == "itch") && opts.name.empty())
        raw.h.name = lower(raw.h.instrument) + "-" + raw.h.date;
    if (format == "tardis" && opts.name.empty()) raw.h.name = tardis_name(raw);
    if (!opts.instrument.empty()) raw.h.instrument = opts.instrument;
    if (!valid_name(raw.h.name)) {
        why = "'" + raw.h.name + "' cannot be a tape name (lower-case letters, digits, - _ .): give one with name=";
        return false;
    }
    return finish(raw, opts, out, why);
}

// Imports one file into a tape. `companion` is a second file some formats
// read beside the first (LOBSTER's orderbook file); empty when there is none.
[[nodiscard]] inline bool import_file(const std::string& path, std::string_view bytes, std::string_view companion,
                                      const ImportOptions& opts, Tape& out, std::string& why) {
    const std::string format = opts.format.empty() ? detect_format(path, bytes) : opts.format;
    RawTape raw;
    bool ok = false;
    if (format == "csv") ok = import_csv(path, bytes, opts, raw, why);
    else if (format == "lobster") ok = import_lobster(path, bytes, companion, opts, raw, why);
    else if (format == "databento") ok = import_databento(path, bytes, opts, raw, why);
    else if (format == "tardis") ok = import_tardis(path, bytes, companion, opts, raw, why);
    else if (format == "binance") ok = import_binance(path, bytes, companion, raw, why);
    else if (format == "coinbase") ok = import_coinbase(path, bytes, opts, raw, why);
    else if (format == "kraken") ok = import_kraken(path, bytes, opts, raw, why);
    else if (format == "itch") ok = import_itch(path, bytes, opts, raw, why);
    else {
        why = "unknown format " + format + " (formats: " + kFormats + ")";
        return false;
    }
    if (!ok) return false;
    raw.h.source_cksum = cksum_of(bytes);
    return name_and_finish(path, format, opts, raw, out, why);
}

// An ITCH file, streamed from disk a megabyte at a time: a day's file is
// gigabytes, and only one stock's messages are kept.
[[nodiscard]] inline bool import_itch_file(const std::string& path, const ImportOptions& opts, Tape& out,
                                           std::string& why) {
    RawTape raw;
    if (!itch_start(path, opts, raw, why)) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        why = "cannot read " + path;
        return false;
    }
    ItchReader r(opts.symbol, raw);
    Cksum ck;
    std::string chunk(1 << 20, '\0');
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto n = static_cast<std::size_t>(in.gcount());
        if (n == 0) break;
        const std::string_view got(chunk.data(), n);
        ck.add(got);
        if (!r.feed(got, why)) return false;
    }
    if (in.bad()) {
        why = "cannot read " + path;
        return false;
    }
    if (!r.end(why) || !itch_finish(r, opts, raw, why)) return false;
    raw.h.source_cksum = ck.text();
    return name_and_finish(path, "itch", opts, raw, out, why);
}

} // namespace px::data
