// marketdata/tape.hpp — the one internal format every source is converted to.
//
// A tape is a text file of integer columns, one event per row, behind a header
// of key=value lines. Every importer writes one and nothing downstream reads
// anything else, so supporting a new source means writing one importer
// (docs/DATA.md). Integers only, so the same source file gives the same tape,
// byte for byte, on Windows and Linux.
//
//   # exchange-lab tape: integer columns, prices in ticks. ...
//   tape.version=1
//   name=aapl-2012-06-21
//   ...
//   t_ns,kind,side,id,price,qty
//   0,A,B,16113575,58533,18
//
// Columns:
//   t_ns   nanoseconds since the tape's first event (the header's origin says
//          what that first event's time was in the source)
//   kind   A add: an order rests        C cancel: qty leaves a resting order
//          T trade: qty executes        L level: the total at a price is now qty
//          R reset: forget the book (a new snapshot follows)
//   side   A and C: the resting order's side. T: the aggressor's side, the one
//          that crossed. L: the level's side. B buy, S sell, - not known.
//   id     the source's order id, 0 when it has none. For T, the resting order
//          that executed, 0 when that is not known or not visible.
//   price  in ticks (header: tick price units a tick, price_scale a currency unit)
//   qty    in the source's units times qty_scale
#pragma once

#include "marketdata/parse.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace px::data {

enum class Depth : std::uint8_t { Trades = 0, L2 = 1, L3 = 2 };

[[nodiscard]] inline const char* depth_name(Depth d) noexcept {
    switch (d) {
        case Depth::Trades: return "trades";
        case Depth::L2: return "L2";
        case Depth::L3: return "L3";
    }
    return "?";
}

[[nodiscard]] inline const char* depth_words(Depth d) noexcept {
    switch (d) {
        case Depth::Trades: return "trades only";
        case Depth::L2: return "price levels (L2)";
        case Depth::L3: return "order by order (L3)";
    }
    return "?";
}

[[nodiscard]] inline bool parse_depth(std::string_view s, Depth& out) noexcept {
    if (s == "trades") out = Depth::Trades;
    else if (s == "L2") out = Depth::L2;
    else if (s == "L3") out = Depth::L3;
    else return false;
    return true;
}

struct TapeEvent {
    std::int64_t t = 0;
    char kind = 'T';
    char side = '-';
    std::uint64_t id = 0;
    std::int64_t price = 0; // ticks
    std::int64_t qty = 0;
};

struct TapeHeader {
    std::string name;
    std::string instrument;
    std::string date;          // the trading date when the source says, else empty
    std::string format;        // the importer's name: csv, lobster, ...
    std::string importer;      // its name and version: "lobster 1"
    std::string source_file;   // the file's name, never its directory
    std::string source_cksum;  // what `cksum FILE` prints: "CRC LENGTH"
    Depth depth = Depth::Trades;
    std::int64_t price_scale = 1; // price units per currency unit, a power of ten
    std::int64_t tick = 1;        // price units per tick
    std::int64_t qty_scale = 1;   // qty units per unit of the source, a power of ten
    std::int64_t origin_ns = 0;   // the first event's time as the source gave it
    TimeKind origin_kind = TimeKind::Relative;
    std::uint64_t skipped = 0;    // source rows the importer could not use
    std::string skipped_why;      // what they were: "10 auction trades, 2 halts"
    std::string notes;            // anything else a reader should know, one line
};

struct Tape {
    TapeHeader h;
    std::vector<TapeEvent> ev;
};

inline constexpr int kTapeVersion = 1;
inline constexpr const char* kTapeColumns = "t_ns,kind,side,id,price,qty";

// A tape or market name: lower-case letters, digits and - _ . ; starting with a
// letter or digit; at most 64 characters. It becomes a file name, and a run
// directory's name, on every platform.
[[nodiscard]] inline bool valid_name(std::string_view n) noexcept {
    if (n.empty() || n.size() > 64) return false;
    for (std::size_t i = 0; i < n.size(); ++i) {
        const char c = n[i];
        const bool ok = (c >= 'a' && c <= 'z') || is_digit(c) || (i > 0 && (c == '-' || c == '_' || c == '.'));
        if (!ok) return false;
    }
    return n.back() != '.';
}

// A file name made into a tape name: "AAPL_trades.csv" -> "aapl_trades".
[[nodiscard]] inline std::string name_from_file(std::string_view file) {
    const std::size_t slash = file.find_last_of("/\\");
    if (slash != std::string_view::npos) file = file.substr(slash + 1);
    const std::size_t dot = file.find('.');
    if (dot != std::string_view::npos && dot > 0) file = file.substr(0, dot);
    std::string out;
    for (const char c : lower(file)) {
        const bool ok = (c >= 'a' && c <= 'z') || is_digit(c) || c == '-' || c == '_';
        out += ok ? c : '-';
    }
    while (!out.empty() && (out.front() == '-' || out.front() == '_')) out.erase(out.begin());
    if (out.size() > 64) out.resize(64);
    if (out.empty()) out = "tape";
    return out;
}

[[nodiscard]] inline std::string tape_text(const Tape& tape) {
    const TapeHeader& h = tape.h;
    std::string s;
    s.reserve(64 * tape.ev.size() + 1024);
    s += "# exchange-lab tape: integer columns, prices in ticks (docs/DATA.md).\n";
    s += "# Made from a file you brought. Never commit a tape made from data you paid for.\n";
    auto kv = [&s](std::string_view k, std::string_view v) {
        s += k;
        s += '=';
        s += v;
        s += '\n';
    };
    kv("tape.version", std::to_string(kTapeVersion));
    kv("name", h.name);
    kv("instrument", h.instrument);
    kv("date", h.date);
    kv("source.format", h.format);
    kv("source.file", h.source_file);
    kv("source.cksum", h.source_cksum);
    kv("importer", h.importer);
    kv("depth", depth_name(h.depth));
    kv("price_scale", std::to_string(h.price_scale));
    kv("tick", std::to_string(h.tick));
    kv("qty_scale", std::to_string(h.qty_scale));
    kv("origin_ns", std::to_string(h.origin_ns));
    kv("origin.kind", time_kind_name(h.origin_kind));
    kv("skipped", std::to_string(h.skipped));
    kv("skipped.why", h.skipped_why);
    kv("notes", h.notes);
    kv("events", std::to_string(tape.ev.size()));
    kv("span_ns", std::to_string(tape.ev.empty() ? 0 : tape.ev.back().t));
    s += kTapeColumns;
    s += '\n';
    char buf[128];
    for (const TapeEvent& e : tape.ev) {
        const int n = std::snprintf(buf, sizeof buf, "%lld,%c,%c,%llu,%lld,%lld\n", static_cast<long long>(e.t),
                                    e.kind, e.side, static_cast<unsigned long long>(e.id),
                                    static_cast<long long>(e.price), static_cast<long long>(e.qty));
        s.append(buf, static_cast<std::size_t>(n));
    }
    return s;
}

// Reads a tape written by tape_text(). On failure `why` names the line. With
// header_only it stops at the column line, and `events` and `span_ns` are the
// header's word for them; listing a directory of large tapes needs no more.
struct TapeCounts {
    std::int64_t events = -1;
    std::int64_t span_ns = -1;
};

[[nodiscard]] inline bool parse_tape(std::string_view text, Tape& out, std::string& why, bool header_only = false,
                                     TapeCounts* counts = nullptr) {
    out = Tape{};
    std::size_t at = 0;
    std::size_t line_no = 0;
    bool in_rows = false;
    std::int64_t declared = -1;
    std::int64_t span = -1;
    std::vector<std::string> f;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        if (!in_rows) {
            if (line == kTapeColumns) {
                in_rows = true;
                if (header_only) break;
                continue;
            }
            const std::size_t eq = line.find('=');
            if (eq == std::string_view::npos) {
                why = "line " + std::to_string(line_no) + ": expected key=value or the column line";
                return false;
            }
            const std::string_view k = line.substr(0, eq);
            const std::string v(line.substr(eq + 1));
            TapeHeader& h = out.h;
            std::int64_t n = 0;
            const bool num = parse_int(v, n);
            if (k == "tape.version") {
                if (!num || n != kTapeVersion) {
                    why = "tape version " + v + ", this build reads version " + std::to_string(kTapeVersion);
                    return false;
                }
            } else if (k == "name") h.name = v;
            else if (k == "instrument") h.instrument = v;
            else if (k == "date") h.date = v;
            else if (k == "source.format") h.format = v;
            else if (k == "source.file") h.source_file = v;
            else if (k == "source.cksum") h.source_cksum = v;
            else if (k == "importer") h.importer = v;
            else if (k == "depth") {
                if (!parse_depth(v, h.depth)) {
                    why = "line " + std::to_string(line_no) + ": unknown depth " + v;
                    return false;
                }
            } else if (k == "price_scale" && num && n > 0) h.price_scale = n;
            else if (k == "tick" && num && n > 0) h.tick = n;
            else if (k == "qty_scale" && num && n > 0) h.qty_scale = n;
            else if (k == "origin_ns" && num) h.origin_ns = n;
            else if (k == "origin.kind") {
                h.origin_kind = v == "epoch" ? TimeKind::Epoch : v == "midnight" ? TimeKind::Midnight : TimeKind::Relative;
            } else if (k == "skipped" && num && n >= 0) h.skipped = static_cast<std::uint64_t>(n);
            else if (k == "skipped.why") h.skipped_why = v;
            else if (k == "notes") h.notes = v;
            else if (k == "events" && num && n >= 0) {
                declared = n;
                if (!header_only) out.ev.reserve(static_cast<std::size_t>(n));
            } else if (k == "span_ns" && num && n >= 0) {
                span = n;
            } else {
                why = "line " + std::to_string(line_no) + ": unexpected " + std::string(line);
                return false;
            }
            continue;
        }
        // One row: t,kind,side,id,price,qty, all checked.
        if (!split_csv(line, ',', f) || f.size() != 6 || f[1].size() != 1 || f[2].size() != 1) {
            why = "line " + std::to_string(line_no) + ": expected " + kTapeColumns;
            return false;
        }
        TapeEvent e;
        std::int64_t id = 0;
        e.kind = f[1][0];
        e.side = f[2][0];
        const bool kind_ok = e.kind == 'A' || e.kind == 'C' || e.kind == 'T' || e.kind == 'L' || e.kind == 'R';
        const bool side_ok = e.side == 'B' || e.side == 'S' || e.side == '-';
        if (!parse_int(f[0], e.t) || !kind_ok || !side_ok || !parse_int(f[3], id) || id < 0 ||
            !parse_int(f[4], e.price) || !parse_int(f[5], e.qty)) {
            why = "line " + std::to_string(line_no) + ": not a tape row: " + std::string(line);
            return false;
        }
        e.id = static_cast<std::uint64_t>(id);
        if (!out.ev.empty() && e.t < out.ev.back().t) {
            why = "line " + std::to_string(line_no) + ": time goes backwards";
            return false;
        }
        out.ev.push_back(e);
    }
    if (!in_rows) {
        why = "no column line (" + std::string(kTapeColumns) + "): not a tape";
        return false;
    }
    if (counts != nullptr) *counts = TapeCounts{declared, span};
    if (header_only) return true;
    if (declared >= 0 && static_cast<std::size_t>(declared) != out.ev.size()) {
        why = "the header says " + std::to_string(declared) + " events and there are " +
              std::to_string(out.ev.size()) + ": the file is cut short";
        return false;
    }
    if (span >= 0 && span != (out.ev.empty() ? 0 : out.ev.back().t)) {
        why = "the header's span_ns does not match the last row: the file was changed";
        return false;
    }
    return true;
}

// ---- files ---------------------------------------------------------------------

[[nodiscard]] inline bool read_file(const std::string& path, std::string& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    out.clear();
    char buf[1 << 16];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    const bool err = std::ferror(f) != 0;
    return std::fclose(f) == 0 && !err;
}

// Binary mode: LF on every platform, so a tape compares byte for byte.
[[nodiscard]] inline bool write_file(const std::string& path, std::string_view text) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

[[nodiscard]] inline std::string cksum_of(std::string_view bytes) {
    Cksum c;
    c.add(bytes);
    return c.text();
}

} // namespace px::data
