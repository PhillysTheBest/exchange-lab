// marketdata/json.hpp — just enough JSON to read an exchange's export.
//
// Coinbase and Kraken write their trades and captured feeds as JSON. This reads
// it into a small tree and keeps every number as the text it was written as,
// so a price goes through parse_decimal like any other and no floating point is
// ever involved. Strings are unescaped; \u escapes become UTF-8. Nesting is
// limited, so a hostile file cannot run the stack out.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace px::data {

struct Json {
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    std::string text;                                  // a string's value, a number's digits, "true"/"false"
    std::vector<Json> items;                           // an array's values, an object's values
    std::vector<std::string> keys;                     // an object's keys, beside items

    [[nodiscard]] bool is_object() const noexcept { return type == Type::Object; }
    [[nodiscard]] bool is_array() const noexcept { return type == Type::Array; }
    [[nodiscard]] bool is_scalar() const noexcept {
        return type == Type::String || type == Type::Number || type == Type::Bool;
    }

    // The value under `key`, or nullptr when there is none (or this is not an object).
    [[nodiscard]] const Json* get(std::string_view key) const noexcept {
        if (type != Type::Object) return nullptr;
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key) return &items[i];
        return nullptr;
    }
    // A scalar's text under `key`; empty when it is missing, null, or not a scalar.
    [[nodiscard]] std::string_view str(std::string_view key) const noexcept {
        const Json* v = get(key);
        return v != nullptr && v->is_scalar() ? std::string_view(v->text) : std::string_view();
    }
};

class JsonReader {
public:
    explicit JsonReader(std::string_view s) : s_(s) {}

    // Parses one value from the start of the text; with `whole`, nothing but
    // white space may follow it.
    [[nodiscard]] bool parse(Json& out, std::string& why, bool whole = true) {
        at_ = 0;
        if (!value(out, 0, why)) return false;
        space();
        if (whole && at_ != s_.size()) {
            why = "JSON: something after the value, at byte " + std::to_string(at_ + 1);
            return false;
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 64;
    std::string_view s_;
    std::size_t at_ = 0;

    void space() noexcept {
        while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\n' || s_[at_] == '\r')) ++at_;
    }
    bool fail(std::string& why, const char* what) const {
        why = std::string("JSON: ") + what + " at byte " + std::to_string(at_ + 1);
        return false;
    }
    bool literal(std::string_view word) noexcept {
        if (s_.substr(at_, word.size()) != word) return false;
        at_ += word.size();
        return true;
    }
    static void utf8(std::uint32_t c, std::string& out) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    bool hex4(std::uint32_t& c) noexcept {
        if (at_ + 4 > s_.size()) return false;
        c = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = s_[at_++];
            c <<= 4;
            if (h >= '0' && h <= '9') c |= static_cast<std::uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') c |= static_cast<std::uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') c |= static_cast<std::uint32_t>(h - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out, std::string& why) {
        ++at_; // the opening quote
        out.clear();
        while (at_ < s_.size()) {
            const char c = s_[at_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail(why, "a control character in a string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= s_.size()) break;
            const char e = s_[at_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t c1 = 0;
                    if (!hex4(c1)) return fail(why, "a bad \\u escape");
                    if (c1 >= 0xD800 && c1 < 0xDC00 && literal("\\u")) {
                        std::uint32_t c2 = 0;
                        if (!hex4(c2) || c2 < 0xDC00 || c2 >= 0xE000) return fail(why, "a bad surrogate pair");
                        c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                    }
                    utf8(c1, out);
                    break;
                }
                default: return fail(why, "a bad escape");
            }
        }
        return fail(why, "a string with no closing quote");
    }
    bool number(Json& out, std::string& why) {
        const std::size_t start = at_;
        if (at_ < s_.size() && s_[at_] == '-') ++at_;
        std::size_t digits = 0;
        while (at_ < s_.size() && s_[at_] >= '0' && s_[at_] <= '9') ++at_, ++digits;
        if (at_ < s_.size() && s_[at_] == '.') {
            ++at_;
            while (at_ < s_.size() && s_[at_] >= '0' && s_[at_] <= '9') ++at_, ++digits;
        }
        if (at_ < s_.size() && (s_[at_] == 'e' || s_[at_] == 'E')) {
            ++at_;
            if (at_ < s_.size() && (s_[at_] == '+' || s_[at_] == '-')) ++at_;
            while (at_ < s_.size() && s_[at_] >= '0' && s_[at_] <= '9') ++at_;
        }
        if (digits == 0) return fail(why, "a bad number");
        out.type = Json::Type::Number;
        out.text = std::string(s_.substr(start, at_ - start));
        return true;
    }
    bool value(Json& out, int depth, std::string& why) {
        out = Json{};
        if (depth > kMaxDepth) return fail(why, "nesting too deep");
        space();
        if (at_ >= s_.size()) return fail(why, "the text ends where a value should be");
        const char c = s_[at_];
        if (c == '"') {
            out.type = Json::Type::String;
            return string(out.text, why);
        }
        if (c == '{' || c == '[') {
            const bool obj = c == '{';
            out.type = obj ? Json::Type::Object : Json::Type::Array;
            ++at_;
            space();
            if (at_ < s_.size() && s_[at_] == (obj ? '}' : ']')) {
                ++at_;
                return true;
            }
            while (true) {
                space();
                if (obj) {
                    if (at_ >= s_.size() || s_[at_] != '"') return fail(why, "an object key that is not a string");
                    std::string key;
                    if (!string(key, why)) return false;
                    space();
                    if (at_ >= s_.size() || s_[at_] != ':') return fail(why, "no : after an object key");
                    ++at_;
                    out.keys.push_back(std::move(key));
                }
                out.items.emplace_back();
                if (!value(out.items.back(), depth + 1, why)) return false;
                space();
                if (at_ < s_.size() && s_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (at_ < s_.size() && s_[at_] == (obj ? '}' : ']')) {
                    ++at_;
                    return true;
                }
                return fail(why, obj ? "no , or } in an object" : "no , or ] in an array");
            }
        }
        if (literal("true")) {
            out.type = Json::Type::Bool;
            out.text = "true";
            return true;
        }
        if (literal("false")) {
            out.type = Json::Type::Bool;
            out.text = "false";
            return true;
        }
        if (literal("null")) return true;
        return number(out, why);
    }
};

[[nodiscard]] inline bool parse_json(std::string_view s, Json& out, std::string& why) {
    return JsonReader(s).parse(out, why);
}

} // namespace px::data
