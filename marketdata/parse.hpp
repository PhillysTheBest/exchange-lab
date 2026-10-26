// marketdata/parse.hpp — reading numbers, times and CSV fields out of other
// people's files, in integers only.
//
// Everything an importer reads goes through here. Prices and quantities arrive
// as decimal text ("185.23", "0.00150000", "1e-05"), times as whatever the
// source chose (an ISO timestamp, epoch milliseconds, seconds after midnight).
// None of it goes through a double: a tape made on Windows must be byte for
// byte the tape made on Linux, and a strtod is free to round differently. So a
// decimal becomes a mantissa and a count of decimals, and a time becomes
// nanoseconds by integer arithmetic.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace px::data {

// ---- small text helpers -------------------------------------------------------

[[nodiscard]] inline std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '"'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '"'))
        s.remove_suffix(1);
    return s;
}

[[nodiscard]] inline std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

[[nodiscard]] inline bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Splits one CSV line. Fields may be quoted, and a quoted field may hold the
// delimiter or a doubled quote. Returns false for an unterminated quote.
[[nodiscard]] inline bool split_csv(std::string_view line, char delim, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cur += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    if (quoted) return false;
    out.push_back(cur);
    return true;
}

// The delimiter of a delimited text file, guessed from its first line: the
// most frequent of comma, semicolon, tab and bar.
[[nodiscard]] inline char guess_delimiter(std::string_view first_line) noexcept {
    const char cands[] = {',', ';', '\t', '|'};
    char best = ',';
    std::size_t best_n = 0;
    for (const char c : cands) {
        std::size_t n = 0;
        for (const char x : first_line) n += x == c ? 1 : 0;
        if (n > best_n) {
            best = c;
            best_n = n;
        }
    }
    return best;
}

// ---- integers and decimals -------------------------------------------------------

[[nodiscard]] inline bool parse_int(std::string_view s, std::int64_t& out) noexcept {
    s = trim(s);
    if (s.empty()) return false;
    bool neg = false;
    if (s.front() == '-' || s.front() == '+') {
        neg = s.front() == '-';
        s.remove_prefix(1);
    }
    if (s.empty() || s.size() > 19) return false;
    std::int64_t v = 0;
    for (const char c : s) {
        if (!is_digit(c)) return false;
        v = v * 10 + (c - '0');
        if (v < 0) return false; // overflow
    }
    out = neg ? -v : v;
    return true;
}

// A decimal number as mantissa * 10^-decimals: "185.230" is {185230, 3}.
// Accepts a sign and an exponent ("1.5e-05"), because crypto exports write
// small quantities that way. At most 18 significant digits.
struct Decimal {
    std::int64_t mantissa = 0;
    int decimals = 0;
};

[[nodiscard]] inline bool parse_decimal(std::string_view s, Decimal& out) noexcept {
    s = trim(s);
    if (s.empty()) return false;
    bool neg = false;
    if (s.front() == '-' || s.front() == '+') {
        neg = s.front() == '-';
        s.remove_prefix(1);
    }
    std::int64_t m = 0;
    int decimals = 0;
    int digits = 0;
    bool seen_dot = false;
    bool any = false;
    std::size_t i = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '.') {
            if (seen_dot) return false;
            seen_dot = true;
            continue;
        }
        if (c == 'e' || c == 'E') break;
        if (!is_digit(c)) return false;
        any = true;
        // Leading zeros cost nothing; trailing ones after the dot count as decimals.
        if (m == 0 && c == '0') {
            if (seen_dot) ++decimals;
            continue;
        }
        if (++digits > 18) return false;
        m = m * 10 + (c - '0');
        if (seen_dot) ++decimals;
    }
    if (!any) return false;
    if (i < s.size()) { // an exponent
        std::int64_t e = 0;
        if (!parse_int(s.substr(i + 1), e) || e > 18 || e < -18) return false;
        decimals -= static_cast<int>(e);
        while (decimals < 0) {
            if (m > 922337203685477580LL) return false;
            m *= 10;
            ++decimals;
        }
    }
    if (decimals > 18) return false;
    out.mantissa = neg ? -m : m;
    out.decimals = decimals;
    return true;
}

[[nodiscard]] inline std::int64_t pow10(int n) noexcept {
    std::int64_t p = 1;
    for (int i = 0; i < n; ++i) p *= 10;
    return p;
}

// The decimal in units of 10^-decimals. False if it does not fit, or if it
// has more decimals than asked for and the extra digits are not zero.
[[nodiscard]] inline bool scale_to(const Decimal& d, int decimals, std::int64_t& out) noexcept {
    std::int64_t m = d.mantissa;
    int have = d.decimals;
    while (have > decimals) {
        if (m % 10 != 0) return false;
        m /= 10;
        --have;
    }
    while (have < decimals) {
        if (m > 922337203685477580LL || m < -922337203685477580LL) return false;
        m *= 10;
        ++have;
    }
    out = m;
    return true;
}

// The fewest decimals that hold the value exactly: "185.230" needs 2.
[[nodiscard]] inline int needed_decimals(const Decimal& d) noexcept {
    std::int64_t m = d.mantissa;
    int n = d.decimals;
    while (n > 0 && m % 10 == 0) {
        m /= 10;
        --n;
    }
    return n;
}

[[nodiscard]] inline std::int64_t gcd(std::int64_t a, std::int64_t b) noexcept {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b != 0) {
        const std::int64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// "1234567" -> "1,234,567", for summaries people read.
[[nodiscard]] inline std::string grouped(std::int64_t v) {
    std::string digits = std::to_string(v < 0 ? -v : v);
    std::string out;
    const std::size_t n = digits.size();
    for (std::size_t i = 0; i < n; ++i) {
        out += digits[i];
        if ((n - i - 1) % 3 == 0 && i + 1 < n) out += ',';
    }
    return v < 0 ? "-" + out : out;
}

// A value in units of 10^-decimals, written as a decimal: (18523, 2) -> "185.23".
[[nodiscard]] inline std::string decimal_text(std::int64_t v, int decimals) {
    if (decimals <= 0) return std::to_string(v);
    const bool neg = v < 0;
    std::string digits = std::to_string(neg ? -v : v);
    while (static_cast<int>(digits.size()) <= decimals) digits.insert(digits.begin(), '0');
    std::string out = digits.substr(0, digits.size() - static_cast<std::size_t>(decimals)) + "." +
                      digits.substr(digits.size() - static_cast<std::size_t>(decimals));
    return neg ? "-" + out : out;
}

// ---- times ---------------------------------------------------------------------

// Days from 1970-01-01 to a civil date (Howard Hinnant's algorithm), so an
// ISO timestamp becomes epoch nanoseconds without a time library.
[[nodiscard]] inline std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? static_cast<unsigned>(-3) : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

inline void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2 ? 1 : 0;
}

inline constexpr std::int64_t kNsPerSec = 1'000'000'000;
inline constexpr std::int64_t kNsPerDay = 86'400 * kNsPerSec;

// How a time column is written. Auto looks at the value.
enum class TimeFormat : std::uint8_t { Auto, Seconds, Millis, Micros, Nanos, Iso, Clock };

// What a parsed time counts from.
enum class TimeKind : std::uint8_t { Epoch, Midnight, Relative };

[[nodiscard]] inline const char* time_kind_name(TimeKind k) noexcept {
    switch (k) {
        case TimeKind::Epoch: return "epoch";
        case TimeKind::Midnight: return "midnight";
        case TimeKind::Relative: return "relative";
    }
    return "?";
}

[[nodiscard]] inline bool parse_time_format(std::string_view s, TimeFormat& out) noexcept {
    if (s == "auto") out = TimeFormat::Auto;
    else if (s == "s") out = TimeFormat::Seconds;
    else if (s == "ms") out = TimeFormat::Millis;
    else if (s == "us") out = TimeFormat::Micros;
    else if (s == "ns") out = TimeFormat::Nanos;
    else if (s == "iso") out = TimeFormat::Iso;
    else if (s == "clock") out = TimeFormat::Clock;
    else return false;
    return true;
}

// Seconds with an optional fraction, exactly: "34200.004241176" -> ns.
[[nodiscard]] inline bool parse_seconds(std::string_view s, std::int64_t& ns) noexcept {
    s = trim(s);
    const std::size_t dot = s.find('.');
    std::int64_t whole = 0;
    if (!parse_int(dot == std::string_view::npos ? s : s.substr(0, dot), whole) || whole < 0) return false;
    std::int64_t frac = 0;
    if (dot != std::string_view::npos) {
        std::string_view f = s.substr(dot + 1);
        if (f.size() > 9) {
            for (const char c : f.substr(9))
                if (!is_digit(c)) return false;
            f = f.substr(0, 9); // below a nanosecond: dropped, not rounded
        }
        for (std::size_t i = 0; i < 9; ++i) {
            const char c = i < f.size() ? f[i] : '0';
            if (!is_digit(c)) return false;
            frac = frac * 10 + (c - '0');
        }
    }
    if (whole > 9'000'000'000LL) return false;
    ns = whole * kNsPerSec + frac;
    return true;
}

// "HH:MM:SS[.fffffffff]" -> ns after midnight.
[[nodiscard]] inline bool parse_clock(std::string_view s, std::int64_t& ns) noexcept {
    s = trim(s);
    if (s.size() < 8 || s[2] != ':' || s[5] != ':') return false;
    std::int64_t h = 0, m = 0, sec = 0;
    if (!parse_int(s.substr(0, 2), h) || !parse_int(s.substr(3, 2), m)) return false;
    if (!parse_seconds(s.substr(6), sec)) return false;
    if (h > 23 || m > 59 || sec >= 61 * kNsPerSec) return false;
    ns = (h * 3600 + m * 60) * kNsPerSec + sec;
    return true;
}

// "2024-01-02T09:30:00.123456789Z", "2024-01-02 09:30:00", "...+01:00":
// -> ns since the epoch, in UTC.
[[nodiscard]] inline bool parse_iso(std::string_view s, std::int64_t& ns) noexcept {
    s = trim(s);
    if (s.size() < 10 || s[4] != '-' || s[7] != '-') return false;
    std::int64_t y = 0, mo = 0, d = 0;
    if (!parse_int(s.substr(0, 4), y) || !parse_int(s.substr(5, 2), mo) || !parse_int(s.substr(8, 2), d)) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    std::int64_t day_ns = 0;
    std::int64_t offset_ns = 0;
    if (s.size() > 10) {
        if (s[10] != 'T' && s[10] != ' ') return false;
        std::string_view rest = s.substr(11);
        // A zone: Z, or +hh:mm / -hh:mm after the seconds.
        if (!rest.empty() && (rest.back() == 'Z' || rest.back() == 'z')) rest.remove_suffix(1);
        const std::size_t sign = rest.find_last_of("+-");
        if (sign != std::string_view::npos && sign >= 8) {
            const std::string_view zone = rest.substr(sign + 1);
            std::int64_t zh = 0, zm = 0;
            if (zone.size() == 5 && zone[2] == ':') {
                if (!parse_int(zone.substr(0, 2), zh) || !parse_int(zone.substr(3, 2), zm)) return false;
            } else if (zone.size() == 4) {
                if (!parse_int(zone.substr(0, 2), zh) || !parse_int(zone.substr(2, 2), zm)) return false;
            } else {
                return false;
            }
            offset_ns = (zh * 3600 + zm * 60) * kNsPerSec * (rest[sign] == '-' ? -1 : 1);
            rest = rest.substr(0, sign);
        }
        if (!parse_clock(rest, day_ns)) return false;
    }
    ns = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * kNsPerDay + day_ns - offset_ns;
    return true;
}

// One time value. With Auto the shape decides: a date is ISO, hh:mm:ss is a
// clock time after midnight, an integer is epoch seconds, milliseconds,
// microseconds or nanoseconds by its number of digits, and a decimal is
// seconds (epoch seconds when it is that large, after midnight otherwise).
[[nodiscard]] inline bool parse_time(std::string_view s, TimeFormat f, std::int64_t& ns, TimeKind& kind) noexcept {
    s = trim(s);
    if (s.empty()) return false;
    if (f == TimeFormat::Auto) {
        if (s.size() >= 10 && s[4] == '-' && s[7] == '-') f = TimeFormat::Iso;
        else if (s.find(':') != std::string_view::npos) f = TimeFormat::Clock;
        else if (s.find('.') != std::string_view::npos) f = TimeFormat::Seconds;
        else {
            std::size_t digits = 0;
            for (const char c : s) digits += is_digit(c) ? 1 : 0;
            if (digits >= 17) f = TimeFormat::Nanos;
            else if (digits >= 14) f = TimeFormat::Micros;
            else if (digits >= 11) f = TimeFormat::Millis;
            else f = TimeFormat::Seconds;
        }
    }
    std::int64_t v = 0;
    switch (f) {
        case TimeFormat::Iso:
            kind = TimeKind::Epoch;
            return parse_iso(s, ns);
        case TimeFormat::Clock:
            kind = TimeKind::Midnight;
            return parse_clock(s, ns);
        case TimeFormat::Seconds:
            if (!parse_seconds(s, ns)) return false;
            kind = ns >= 100'000'000LL * kNsPerSec ? TimeKind::Epoch
                   : ns <= kNsPerDay                ? TimeKind::Midnight
                                                    : TimeKind::Relative;
            return true;
        case TimeFormat::Millis:
            if (!parse_int(s, v) || v < 0 || v > 9'000'000'000'000'000'000LL / 1'000'000) return false;
            ns = v * 1'000'000;
            kind = TimeKind::Epoch;
            return true;
        case TimeFormat::Micros:
            if (!parse_int(s, v) || v < 0 || v > 9'000'000'000'000'000'000LL / 1'000) return false;
            ns = v * 1'000;
            kind = TimeKind::Epoch;
            return true;
        case TimeFormat::Nanos:
            if (!parse_int(s, v) || v < 0) return false;
            ns = v;
            kind = TimeKind::Epoch;
            return true;
        case TimeFormat::Auto: break;
    }
    return false;
}

// "2024-01-02 09:30:00.000" from epoch ns, or "09:30:00.000" from ns after
// midnight, for summaries. Milliseconds are enough for a person.
[[nodiscard]] inline std::string time_text(std::int64_t ns, TimeKind kind) {
    auto two = [](std::int64_t v) { return std::string(v < 10 ? "0" : "") + std::to_string(v); };
    std::string out;
    std::int64_t day_ns = ns;
    if (kind == TimeKind::Epoch) {
        const std::int64_t days = ns >= 0 ? ns / kNsPerDay : (ns - kNsPerDay + 1) / kNsPerDay;
        day_ns = ns - days * kNsPerDay;
        std::int64_t y = 0;
        unsigned m = 0, d = 0;
        civil_from_days(days, y, m, d);
        out = std::to_string(y) + "-" + two(m) + "-" + two(d) + " ";
    } else if (kind == TimeKind::Relative) {
        return decimal_text(ns / 1'000'000, 3) + " s";
    }
    const std::int64_t ms = (day_ns / 1'000'000) % 1000;
    const std::int64_t s = day_ns / kNsPerSec;
    out += two(s / 3600) + ":" + two((s / 60) % 60) + ":" + two(s % 60) + "." +
           (ms < 100 ? (ms < 10 ? "00" : "0") : "") + std::to_string(ms);
    return out;
}

// A span of time for people: "6h30m", "25.0 ms", "41 s".
[[nodiscard]] inline std::string duration_text(std::int64_t ns) {
    if (ns < 1'000'000) return decimal_text(ns / 100, 1) + " us";
    if (ns < kNsPerSec) return decimal_text(ns / 100'000, 1) + " ms";
    const std::int64_t s = ns / kNsPerSec;
    if (s < 120) return std::to_string(s) + " s";
    if (s < 7200) return std::to_string(s / 60) + "m" + std::to_string(s % 60) + "s";
    return std::to_string(s / 3600) + "h" + std::to_string((s / 60) % 60) + "m";
}

// ---- checksums -------------------------------------------------------------------

// The POSIX `cksum` CRC: the same number `cksum FILE` prints, so anyone can
// check which file a tape or a market came from without this program.
class Cksum {
public:
    void add(const void* data, std::size_t n) noexcept {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) crc_ = (crc_ << 8) ^ table()[((crc_ >> 24) ^ p[i]) & 0xFF];
        len_ += n;
    }
    void add(std::string_view s) noexcept { add(s.data(), s.size()); }

    [[nodiscard]] std::uint32_t value() const noexcept {
        std::uint32_t c = crc_;
        for (std::uint64_t n = len_; n != 0; n >>= 8) c = (c << 8) ^ table()[((c >> 24) ^ (n & 0xFF)) & 0xFF];
        return ~c;
    }
    [[nodiscard]] std::uint64_t length() const noexcept { return len_; }
    // "3015617425 6" — exactly what cksum prints before the file name.
    [[nodiscard]] std::string text() const { return std::to_string(value()) + " " + std::to_string(len_); }

private:
    static const std::array<std::uint32_t, 256>& table() noexcept {
        static const std::array<std::uint32_t, 256> t = [] {
            std::array<std::uint32_t, 256> r{};
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t c = i << 24;
                for (int k = 0; k < 8; ++k) c = (c & 0x80000000U) != 0 ? (c << 1) ^ 0x04C11DB7U : c << 1;
                r[i] = c;
            }
            return r;
        }();
        return t;
    }
    std::uint32_t crc_ = 0;
    std::uint64_t len_ = 0;
};

} // namespace px::data
