// arena/strategy_settings.hpp — a strategy named with its settings: name+key=value.
//
// The syntax, and why it cannot be read two ways (docs/RUNNER.md, "A strategy's
// own settings"):
//
//     imbalance+ratio=5            one setting
//     imbalance+ratio=5+qty=20     two
//     imbalance+ratio=5:2          two of them in a line-up, both with ratio 5
//
// A comma still separates names and :N still repeats one. No name contains '+'
// (built-in names are words, and `make strategy` takes [a-z][a-z0-9_]*), and a
// count is read from the right before any setting, so every spec that parsed
// before parses the same way now.
//
// THE LABEL sorts the keys, so imbalance+qty=20+ratio=5 and imbalance+ratio=5+qty=20
// are one candidate under one name. The ID writes each '=' as '_', for the places
// where a '=' would end a key: a suite.cfg line and a directory name.
#pragma once

#include "arena/strategy.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace px::arena {

struct StrategySpec {
    std::string name;
    std::vector<std::pair<std::string, std::int64_t>> settings; // sorted by key

    [[nodiscard]] std::string label() const {
        std::string s = name;
        for (const auto& [k, v] : settings) s += "+" + k + "=" + std::to_string(v);
        return s;
    }
    [[nodiscard]] std::string id() const {
        std::string s = label();
        std::replace(s.begin(), s.end(), '=', '_');
        return s;
    }
};

namespace detail {

[[nodiscard]] inline bool setting_key_ok(std::string_view k) noexcept {
    if (k.empty() || !((k[0] >= 'a' && k[0] <= 'z') || k[0] == '_')) return false;
    for (const char c : k)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

// A whole number: an optional '-' and 1 to 18 digits, nothing else.
[[nodiscard]] inline bool setting_value_ok(std::string_view s, std::int64_t& out) noexcept {
    const bool neg = !s.empty() && s[0] == '-';
    const std::string_view d = neg ? s.substr(1) : s;
    if (d.empty() || d.size() > 18) return false;
    std::int64_t v = 0;
    for (const char c : d) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = neg ? -v : v;
    return true;
}

} // namespace detail

// Splits `token` (no count) into a name and its settings. A token with no '+'
// is a bare name and always parses, so this never refuses what parsed before.
[[nodiscard]] inline bool parse_strategy_spec(std::string_view token, StrategySpec& out, std::string& why) {
    out = StrategySpec{};
    const std::size_t plus = token.find('+');
    out.name = std::string(token.substr(0, plus));
    if (plus == std::string_view::npos) return true;
    if (out.name.empty()) {
        why = "'" + std::string(token) + "': no strategy name before '+'";
        return false;
    }
    std::string_view rest = token.substr(plus + 1);
    for (;;) {
        const std::size_t next = rest.find('+');
        const std::string_view kv = rest.substr(0, next);
        const std::size_t eq = kv.find('=');
        const std::string_view key = kv.substr(0, eq);
        std::int64_t v = 0;
        if (eq == std::string_view::npos || !detail::setting_key_ok(key)) {
            why = "'" + std::string(kv) + "' in '" + std::string(token) + "': a setting is key=value, the key " +
                  "lower-case letters, digits and _";
            return false;
        }
        if (!detail::setting_value_ok(kv.substr(eq + 1), v)) {
            why = "'" + std::string(kv) + "' in '" + std::string(token) + "': the value is not a whole number";
            return false;
        }
        for (const auto& [k, _] : out.settings) {
            if (k == key) {
                why = "'" + std::string(token) + "' sets " + std::string(key) + " twice";
                return false;
            }
        }
        out.settings.emplace_back(std::string(key), v);
        if (next == std::string_view::npos) break;
        rest = rest.substr(next + 1);
    }
    std::sort(out.settings.begin(), out.settings.end());
    return true;
}

// Every setting of `spec`, onto a freshly built handle; the first refusal wins.
[[nodiscard]] inline bool apply_settings(StrategyHandle& h, const StrategySpec& spec, std::string& why) {
    for (const auto& [k, v] : spec.settings) {
        if (!h.apply_setting(k, v, why)) return false;
    }
    return true;
}

// A strategy shown under its label, so two instances with different settings
// are told apart in every table and CSV. Names are not part of any hash, so
// wrapping moves nothing.
class LabelledHandle final : public StrategyHandle {
public:
    LabelledHandle(std::unique_ptr<StrategyHandle> inner, std::string label)
        : inner_(std::move(inner)), label_(std::move(label)) {}
    [[nodiscard]] const char* name() const noexcept override { return label_.c_str(); }
    [[nodiscard]] SimTime reaction_ns() const noexcept override { return inner_->reaction_ns(); }
    [[nodiscard]] SimTime timer_ns() const noexcept override { return inner_->timer_ns(); }
    void reset(std::uint64_t seed) override { inner_->reset(seed); }
    ActionBatch on_event(const MarketView& v) override { return inner_->on_event(v); }
    [[nodiscard]] bool has_settings() const noexcept override { return inner_->has_settings(); }
    [[nodiscard]] std::string setting_names() const override { return inner_->setting_names(); }
    [[nodiscard]] bool apply_setting(std::string_view key, std::int64_t value, std::string& why) override {
        return inner_->apply_setting(key, value, why);
    }

private:
    std::unique_ptr<StrategyHandle> inner_;
    std::string label_;
};

} // namespace px::arena
