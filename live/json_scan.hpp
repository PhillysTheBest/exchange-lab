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
// "yes": [[50,10],[51,5]] — the documented shape of Kalshi's REST orderbook
// response. The WS orderbook_snapshot message is ASSUMED to mirror this (this
// project's source material confirmed the outer envelope's `type`/`market_ticker`
// fields but not the complete inner delta/snapshot field names — see
// kalshi_feed.hpp) — verify against a live capture before depending on this
// shape for real trading. Bounded, depth-tracked bracket matching; not a
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

} // namespace px::live
