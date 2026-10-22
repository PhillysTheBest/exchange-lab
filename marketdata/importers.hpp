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

#include "marketdata/parse.hpp"
#include "marketdata/tape.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
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

// ---- detection and the front door ------------------------------------------------------

inline constexpr const char* kFormats = "csv lobster databento";

// Which importer reads this file. `first_line` is the file's first line.
[[nodiscard]] inline std::string detect_format(std::string_view path, std::string_view first_line) {
    if (first_line.substr(0, 3) == "DBN" || looks_like_databento_csv(first_line)) return "databento";
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
    else if (starts({'D', 'B', 'N'})) return true;
    else if (bytes.substr(0, std::min<std::size_t>(bytes.size(), 4096)).find('\0') != std::string_view::npos)
        why = "a binary file of no format this build reads (" + std::string(kFormats) + ")";
    else return true;
    return false;
}

// Imports one file into a tape. `companion` is a second file some formats
// read beside the first (LOBSTER's orderbook file); empty when there is none.
[[nodiscard]] inline bool import_file(const std::string& path, std::string_view bytes, std::string_view companion,
                                      const ImportOptions& opts, Tape& out, std::string& why) {
    std::string_view first_line = bytes.substr(0, bytes.find('\n'));
    if (!first_line.empty() && first_line.back() == '\r') first_line.remove_suffix(1);
    const std::string format = opts.format.empty() ? detect_format(path, first_line) : opts.format;
    RawTape raw;
    bool ok = false;
    if (format == "csv") ok = import_csv(path, bytes, opts, raw, why);
    else if (format == "lobster") ok = import_lobster(path, bytes, companion, opts, raw, why);
    else if (format == "databento") ok = import_databento(path, bytes, opts, raw, why);
    else {
        why = "unknown format " + format + " (formats: " + kFormats + ")";
        return false;
    }
    if (!ok) return false;
    raw.h.source_cksum = cksum_of(bytes);
    raw.h.name = !opts.name.empty() ? opts.name : name_from_file(path);
    if (format == "lobster" && opts.name.empty()) raw.h.name = lower(raw.h.instrument) + "-" + raw.h.date;
    if (!opts.instrument.empty()) raw.h.instrument = opts.instrument;
    if (!valid_name(raw.h.name)) {
        why = "'" + raw.h.name + "' cannot be a tape name (lower-case letters, digits, - _ .): give one with name=";
        return false;
    }
    return finish(raw, opts, out, why);
}

} // namespace px::data
