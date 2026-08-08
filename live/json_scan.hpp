// live/json_scan.hpp — a specialized field scanner, not a JSON parser.
//
// No JSON library is installed in this toolchain (no package manager pulled
// one in — see the plan doc's environment notes), and Kalshi's feed messages
// are shallow, flat objects with a known, small set of fields. Hand-scanning
// for "key": value at the top level of a message is simpler and faster than
// writing (or vendoring) a general recursive-descent parser for a handful of
// fields — the standard trade-off for this kind of narrow, high-volume feed
// parsing. This does NOT handle nested objects, arrays of objects, or escaped
// quotes inside string values; it is deliberately scoped to what Kalshi's
// ticker/orderbook_delta/orderbook_snapshot envelopes actually look like.
#pragma once

#include <cctype>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace px::live {

namespace detail {
// Finds "key" as a JSON object key (i.e. preceded by '{' or ',' and followed by
// ':', ignoring whitespace) starting the search at `from`. Returns the offset
// just past the ':' that follows it, or npos if not found.
inline std::size_t find_value_start(std::string_view json, std::string_view key, std::size_t from = 0) {
    const std::string quoted_key = std::string("\"") + std::string(key) + "\"";
    std::size_t pos = from;
    for (;;) {
        pos = json.find(quoted_key, pos);
        if (pos == std::string_view::npos) return std::string_view::npos;
        std::size_t after = pos + quoted_key.size();
        while (after < json.size() && (json[after] == ' ' || json[after] == '\t')) ++after;
        if (after < json.size() && json[after] == ':') {
            ++after;
            while (after < json.size() && (json[after] == ' ' || json[after] == '\t')) ++after;
            return after;
        }
        pos += quoted_key.size(); // matched substring wasn't actually "key": — keep searching
    }
}
} // namespace detail

// Extracts a top-level string field's value (without surrounding quotes).
// Does not un-escape \" or \\ — none of the fields this project reads need it.
[[nodiscard]] inline bool json_extract_string(std::string_view json, std::string_view key, std::string_view& out) {
    const std::size_t start = detail::find_value_start(json, key);
    if (start == std::string_view::npos || start >= json.size() || json[start] != '"') return false;
    const std::size_t end = json.find('"', start + 1);
    if (end == std::string_view::npos) return false;
    out = json.substr(start + 1, end - start - 1);
    return true;
}

// Extracts a top-level numeric field as a double (handles ints and decimals).
[[nodiscard]] inline bool json_extract_number(std::string_view json, std::string_view key, double& out) {
    const std::size_t start = detail::find_value_start(json, key);
    if (start == std::string_view::npos) return false;
    std::size_t end = start;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) ||
                                  json[end] == '-' || json[end] == '+' || json[end] == '.' ||
                                  json[end] == 'e' || json[end] == 'E')) {
        ++end;
    }
    if (end == start) return false;
    const auto res = std::from_chars(json.data() + start, json.data() + end, out);
    return res.ec == std::errc{};
}

// Extracts a top-level integer field.
[[nodiscard]] inline bool json_extract_int(std::string_view json, std::string_view key, long long& out) {
    double d;
    if (!json_extract_number(json, key, d)) return false;
    out = static_cast<long long>(d);
    return true;
}

// Extracts an array of [price, qty] integer pairs from a field shaped like
// "yes": [[50,10],[51,5]] — bare (unquoted) numeric pairs. NOT the shape
// Kalshi's WS feed actually uses (see json_extract_dollar_pair_array below) —
// kept only as a general-purpose helper for a hypothetical future feed that
// does use bare numbers. Bounded, depth-tracked bracket matching; not a
// general JSON array parser.
[[nodiscard]] inline bool json_extract_pair_array(std::string_view json, std::string_view key,
                                                   std::vector<std::pair<long long, long long>>& out) {
    out.clear();
    const std::size_t start = detail::find_value_start(json, key);
    if (start == std::string_view::npos || start >= json.size() || json[start] != '[') return false;

    int depth = 0;
    std::size_t array_end = std::string_view::npos;
    for (std::size_t i = start; i < json.size(); ++i) {
        if (json[i] == '[') ++depth;
        else if (json[i] == ']') {
            --depth;
            if (depth == 0) { array_end = i; break; }
        }
    }
    if (array_end == std::string_view::npos) return false;

    const std::string_view body = json.substr(start + 1, array_end - start - 1);
    std::size_t pos = 0;
    for (;;) {
        const std::size_t open = body.find('[', pos);
        if (open == std::string_view::npos) break;
        const std::size_t close = body.find(']', open);
        if (close == std::string_view::npos) break;
        const std::string_view pair = body.substr(open + 1, close - open - 1);
        const std::size_t comma = pair.find(',');
        if (comma != std::string_view::npos) {
            long long a = 0, b = 0;
            std::from_chars(pair.data(), pair.data() + comma, a);
            std::from_chars(pair.data() + comma + 1, pair.data() + pair.size(), b);
            out.emplace_back(a, b);
        }
        pos = close + 1;
    }
    return true;
}

// ---- fixed-point decimal support -------------------------------------------
//
// Kalshi's WS feed (confirmed against the published AsyncAPI spec — see
// kalshi_feed.hpp) represents every price and contract-count field as a
// *quoted decimal string*, not a bare JSON number: `"price_dollars": "0.0800"`,
// `"delta_fp": "-5.00"`. The functions above (json_extract_number/_int) cannot
// read these — they stop at the opening `"`. Everything below exists to close
// that gap, which is the bug that made the original feed handler parse nothing.

// Parses an already-unquoted decimal string ("0.0800", "125.00", "-5.5", "5")
// into an integer scaled by 10^scale — e.g. scale=2 turns "0.62" into 62.
// Fractional digits beyond `scale` are rounded (half-up on the first dropped
// digit) rather than truncated, so a field with more precision than the target
// unit — Kalshi's own examples show price_dollars as 4 decimal digits, e.g.
// "0.0800", for a value that is always a whole number of cents — is handled
// exactly instead of silently losing a low-order digit. Returns false on any
// non-numeric content.
[[nodiscard]] inline bool parse_decimal_scaled(std::string_view s, int scale, long long& out) noexcept {
    if (s.empty()) return false;
    std::size_t i = 0;
    bool neg = false;
    if (s[i] == '+' || s[i] == '-') { neg = (s[i] == '-'); ++i; }
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;

    long long int_part = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        int_part = int_part * 10 + (s[i] - '0');
        ++i;
    }

    long long frac_kept = 0; // first `scale` fractional digits, left-aligned
    int kept_digits = 0;
    bool round_up = false;
    if (i < s.size() && s[i] == '.') {
        ++i;
        int digit_pos = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            const int d = s[i] - '0';
            if (digit_pos < scale) { frac_kept = frac_kept * 10 + d; ++kept_digits; }
            else if (digit_pos == scale) { round_up = d >= 5; } // first dropped digit only
            ++digit_pos;
            ++i;
        }
    }
    if (i != s.size()) return false; // trailing garbage — malformed

    while (kept_digits < scale) { frac_kept *= 10; ++kept_digits; } // pad implied trailing zeros

    long long scale_pow = 1;
    for (int k = 0; k < scale; ++k) scale_pow *= 10;

    const long long magnitude = int_part * scale_pow + frac_kept + (round_up ? 1 : 0);
    out = neg ? -magnitude : magnitude;
    return true;
}

// Extracts a quoted dollar-string field (e.g. "price_dollars": "0.0800") as
// integer cents (80 in that example — see parse_decimal_scaled for rounding).
[[nodiscard]] inline bool json_extract_dollars_as_cents(std::string_view json, std::string_view key,
                                                          long long& cents_out) {
    std::string_view s;
    if (!json_extract_string(json, key, s)) return false;
    return parse_decimal_scaled(s, 2, cents_out);
}

// Extracts a quoted fixed-point contract-count field (e.g. "delta_fp": "-5.00",
// 2 decimal places per the Kalshi spec) as a whole contract count, signed.
// Kalshi contracts are always whole units today; a genuinely fractional value
// (which the spec's 2-decimal-place format could in principle carry) truncates
// toward zero here rather than erroring, since the order book has no way to
// represent a fractional contract regardless.
[[nodiscard]] inline bool json_extract_fixed_point_qty(std::string_view json, std::string_view key,
                                                         long long& qty_out) {
    std::string_view s;
    if (!json_extract_string(json, key, s)) return false;
    long long hundredths = 0;
    if (!parse_decimal_scaled(s, 2, hundredths)) return false;
    qty_out = hundredths / 100; // truncates toward zero — see comment above
    return true;
}

// Extracts an array of [price_dollars, count_fp] *quoted string* pairs — the
// actual shape of Kalshi's yes_dollars_fp/no_dollars_fp snapshot fields, e.g.
// [["0.0800","300.00"],["0.2200","333.00"]] — into (cents, whole-contract-qty)
// pairs. Same bounded bracket-matching approach as json_extract_pair_array,
// but reads two quoted decimal strings per element instead of two bare numbers.
[[nodiscard]] inline bool json_extract_dollar_pair_array(std::string_view json, std::string_view key,
                                                           std::vector<std::pair<long long, long long>>& out) {
    out.clear();
    const std::size_t start = detail::find_value_start(json, key);
    if (start == std::string_view::npos || start >= json.size() || json[start] != '[') return false;

    int depth = 0;
    std::size_t array_end = std::string_view::npos;
    for (std::size_t i = start; i < json.size(); ++i) {
        if (json[i] == '[') ++depth;
        else if (json[i] == ']') {
            --depth;
            if (depth == 0) { array_end = i; break; }
        }
    }
    if (array_end == std::string_view::npos) return false;

    const std::string_view body = json.substr(start + 1, array_end - start - 1);
    std::size_t pos = 0;
    for (;;) {
        const std::size_t open = body.find('[', pos);
        if (open == std::string_view::npos) break;
        const std::size_t close = body.find(']', open);
        if (close == std::string_view::npos) break;
        const std::string_view pair = body.substr(open + 1, close - open - 1);

        const std::size_t q1 = pair.find('"');
        const std::size_t q2 = (q1 == std::string_view::npos) ? std::string_view::npos : pair.find('"', q1 + 1);
        const std::size_t q3 = (q2 == std::string_view::npos) ? std::string_view::npos : pair.find('"', q2 + 1);
        const std::size_t q4 = (q3 == std::string_view::npos) ? std::string_view::npos : pair.find('"', q3 + 1);
        if (q4 != std::string_view::npos) {
            long long cents = 0, hundredths = 0;
            if (parse_decimal_scaled(pair.substr(q1 + 1, q2 - q1 - 1), 2, cents) &&
                parse_decimal_scaled(pair.substr(q3 + 1, q4 - q3 - 1), 2, hundredths)) {
                out.emplace_back(cents, hundredths / 100);
            }
        }
        pos = close + 1;
    }
    return true;
}

// ---- the reverse direction: formatting for outgoing REST bodies -----------
//
// Everything above reads Kalshi's WS feed, which is one-directional (we never
// send WS frames carrying prices/counts). Kalshi's REST order-write endpoints
// (live/kalshi_rest.cpp) need the opposite: our internal integer cents/qty
// formatted back into the same quoted fixed-point-decimal-string convention.
// Kept `long long`-typed rather than px::Price/px::Qty, same as every
// extraction function above, so this file stays free of a px/types.hpp
// dependency and either project-side integer type converts into it trivially.

// Formats an integer cent amount as a 2-decimal dollar string: 62 -> "0.62",
// -5 -> "-0.05". Symmetric to json_extract_dollars_as_cents / parse_decimal_scaled.
[[nodiscard]] inline std::string format_cents_as_dollars(long long cents) {
    const bool neg = cents < 0;
    const long long mag = neg ? -cents : cents;
    const long long whole = mag / 100;
    const long long frac = mag % 100;
    std::string out = neg ? "-" : "";
    out += std::to_string(whole);
    out += '.';
    if (frac < 10) out += '0';
    out += std::to_string(frac);
    return out;
}

// Formats a whole contract count as a 2-decimal fixed-point string: 10 ->
// "10.00" — the format Kalshi's `count` request field expects (see the
// verified V2 create-order schema in live/kalshi_rest.cpp). Kalshi contracts
// are always whole units today (same assumption json_extract_fixed_point_qty
// already documents for the read direction), so there is never a fractional
// part to encode here.
[[nodiscard]] inline std::string format_qty_fixed_point(unsigned long long qty) {
    return std::to_string(qty) + ".00";
}

} // namespace px::live
