// arena/strategies/script.hpp — a participant that does what a file tells it.
//
// This is the replayable answer to trading by hand. Live keystrokes cannot go
// into a session: reading when a key arrived means reading a wall clock, and a
// wall clock inside a session breaks the replay contract that everything else
// here depends on (see arena/clock.hpp). A file of orders stamped in *simulated*
// time has none of that problem. The session plays it at exactly the same
// instants on every machine and every run, so a hand-written session hashes
// identically to a headless one.
//
// It is also the honest way to put yourself in the market. You decide what to do
// and when, with the whole book in front of you, and then the result is
// reproducible by anyone who has the file — which is more than can be said for
// a recording of somebody playing.
//
// THE FORMAT, one order per line:
//
//     # comments and blank lines are ignored
//     <at>  buy|sell  <qty>  <price>      a GTC limit
//     <at>  buy|sell  <qty>  market       takes whatever is there
//     <at>  cancel    <n>                 cancels its own n-th order, 1-based
//
// `at` is simulated nanoseconds from the start of the session, and may carry
// `us` or `ms` (`250us`, `4ms`). Prices are in currency units with up to four
// decimals, the same convention the REPL uses: `100.02`. Lines must be in
// non-decreasing time order, which the loader checks, because a file that
// silently reordered itself would be a file that does not mean what it says.
//
// Its reaction delay applies as it does to every other participant: an order
// stamped at `at` is submitted when the strategy wakes at `at`, and what it saw is
// the market as of `at - reaction_ns`. Typing a time does not buy you
// clairvoyance.
#pragma once

#include "arena/strategy.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace px::arena::strategies {

struct ScriptedOrder {
    SimTime at = 0;
    Action action{};
};

// Parses the format above. Returns false with `error` naming the line, rather
// than skipping what it did not understand: a typo in a script should stop the
// session, not quietly change it.
[[nodiscard]] inline bool parse_script(std::string_view text, Price tick, std::vector<ScriptedOrder>& out,
                                       std::string& error) {
    out.clear();
    std::size_t line_no = 0;
    SimTime last_at = 0;
    std::size_t pos = 0;

    const auto fail = [&](const char* why) {
        error = "line " + std::to_string(line_no) + ": " + why;
        return false;
    };

    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        ++line_no;

        // Strip a comment, then surrounding blanks.
        const std::size_t hash = line.find('#');
        if (hash != std::string_view::npos) line = line.substr(0, hash);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r')) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        if (line.empty()) continue;

        std::vector<std::string_view> word;
        std::size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            const std::size_t start = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
            if (i > start) word.push_back(line.substr(start, i - start));
        }
        if (word.size() < 3) return fail("expected: <at> buy|sell <qty> <price|market>, or <at> cancel <n>");

        // ---- the timestamp, with an optional unit ----
        SimTime at = 0;
        {
            std::string_view t = word[0];
            std::int64_t mul = 1;
            if (t.size() > 2 && t.substr(t.size() - 2) == "us") {
                mul = 1'000;
                t = t.substr(0, t.size() - 2);
            } else if (t.size() > 2 && t.substr(t.size() - 2) == "ms") {
                mul = 1'000'000;
                t = t.substr(0, t.size() - 2);
            } else if (t.size() > 2 && t.substr(t.size() - 2) == "ns") {
                t = t.substr(0, t.size() - 2);
            }
            if (t.empty()) return fail("empty timestamp");
            for (const char c : t) {
                if (c == '_') continue;
                if (c < '0' || c > '9') return fail("timestamp is not a number");
                at = at * 10 + (c - '0');
            }
            at *= mul;
        }
        if (at < last_at) return fail("timestamps must not go backwards");
        last_at = at;

        // ---- cancel ----
        if (word[1] == "cancel") {
            std::int64_t which = 0;
            for (const char c : word[2]) {
                if (c < '0' || c > '9') return fail("cancel takes an order number");
                which = which * 10 + (c - '0');
            }
            if (which < 1) return fail("order numbers start at 1");
            Action a;
            a.kind = Action::Kind::Cancel;
            // Resolved against this participant's own live orders when it acts,
            // because the engine's order ids are the session's to assign.
            a.cancel_id = static_cast<OrderId>(which);
            out.push_back(ScriptedOrder{at, a});
            continue;
        }

        // ---- buy / sell ----
        if (word[1] != "buy" && word[1] != "sell") return fail("expected buy, sell or cancel");
        if (word.size() < 4) return fail("expected a price, or the word market");
        const Side side = word[1] == "buy" ? Side::Buy : Side::Sell;

        std::int64_t qty = 0;
        for (const char c : word[2]) {
            if (c < '0' || c > '9') return fail("quantity is not a number");
            qty = qty * 10 + (c - '0');
        }
        if (qty < 1) return fail("quantity must be at least 1");

        if (word[3] == "market") {
            out.push_back(ScriptedOrder{at, Action::market(side, static_cast<Qty>(qty))});
            continue;
        }

        // A price in currency units: "100.02". Integer parse, four decimals, the
        // same convention as the REPL — no floating point anywhere near a session.
        Price whole = 0, frac = 0;
        int frac_digits = 0;
        bool seen_dot = false, any_digit = false;
        for (const char c : word[3]) {
            if (c == '.' && !seen_dot) {
                seen_dot = true;
            } else if (c >= '0' && c <= '9') {
                any_digit = true;
                if (seen_dot) {
                    if (++frac_digits > 4) return fail("a price carries at most four decimals");
                    frac = frac * 10 + (c - '0');
                } else {
                    whole = whole * 10 + (c - '0');
                    if (whole > kMaxPrice / kPriceScale) return fail("price is too large");
                }
            } else {
                return fail("price is not a number");
            }
        }
        if (!any_digit) return fail("price is not a number");
        while (frac_digits++ < 4) frac *= 10;
        const Price price = whole * kPriceScale + frac;
        if (price <= 0) return fail("price must be positive");
        if (tick > 0 && price % tick != 0) return fail("price is not on the tick grid");
        out.push_back(ScriptedOrder{at, Action::limit(side, price, static_cast<Qty>(qty))});
    }
    return true;
}

// Reads a script from a file. Returns false with `error` saying what went wrong,
// which for a missing file is the path and for a bad line is the line number.
[[nodiscard]] inline bool load_script(const char* path, Price tick, std::vector<ScriptedOrder>& out,
                                      std::string& error) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        error = std::string("cannot read ") + path;
        return false;
    }
    std::string text;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    if (!parse_script(text, tick, out, error)) {
        error = std::string(path) + ": " + error;
        return false;
    }
    if (out.empty()) {
        error = std::string(path) + ": no orders in it";
        return false;
    }
    return true;
}

struct Scripted {
    const char* name = "you";

    SimTime reaction_ns = 10'000;
    // How often it looks at the clock. The strategy contract offers a periodic timer
    // and not a "wake me at T", so a scripted time is honoured at the first wake
    // at or after it: granularity is this value, plus the reaction delay on top
    // as for any participant. 10 us is finer than anyone writes a script by hand,
    // and making it coarser is the way to make a long script cheap.
    SimTime timer_ns = 10'000;

    std::vector<ScriptedOrder>* orders = nullptr; // owned by the caller

    void reset(std::uint64_t) { next_ = 0; }

    ActionBatch on_event(const MarketView& v) {
        ActionBatch out;
        if (orders == nullptr) return out;
        // Everything whose time has come, in one reaction. A script that asks
        // for more than a batch holds is split across the next wakes rather
        // than dropped.
        while (next_ < orders->size() && (*orders)[next_].at <= v.now_ns && out.size() < ActionBatch::kMax) {
            Action a = (*orders)[next_].action;
            if (a.kind == Action::Kind::Cancel) {
                // The file says "my third order"; turn that into whichever of
                // its live orders that is, or drop it if it is already gone.
                const std::size_t which = static_cast<std::size_t>(a.cancel_id);
                if (which == 0 || which > v.my_order_count) {
                    ++next_;
                    continue;
                }
                a.cancel_id = v.my_orders[which - 1].id;
            }
            out.push(a);
            ++next_;
        }
        return out;
    }

    [[nodiscard]] std::size_t submitted() const noexcept { return next_; }

private:
    std::size_t next_ = 0;
};

} // namespace px::arena::strategies
