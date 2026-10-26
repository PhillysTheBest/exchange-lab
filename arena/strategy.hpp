// arena/strategy.hpp — the contract between a strategy and the session.
//
// A strategy is a plain struct. It does not inherit from anything, it never touches
// the MatchingEngine, and it cannot read the clock: it is handed a view of the
// market and returns what it would like to do. That makes a strategy testable on its
// own, and it makes the session the only thing that decides when anybody acts.
//
// THE SURFACE a strategy must provide — nothing more, nothing less:
//
//     const char* name;                    // shown in PARTICIPANTS
//     SimTime reaction_ns;                 // its round-trip reaction delay
//     SimTime timer_ns;                    // 0, or a periodic self-wake
//     void reset(std::uint64_t seed);      // called once per session
//     ActionBatch on_event(const MarketView& v);
//
// and, optionally, a sixth: the settings a run may change by name.
//
//     template <class F> void settings(F&& f) {
//         f("ratio", ratio, 1, 100);       // name, field, smallest and largest value
//     }
//
// The harness calls settings() with a visitor and the strategy hands it each
// setting in turn, so one member both names the settings and gives write access
// to them. The harness, never the strategy, checks a value: a whole number inside
// the range and inside the field's integer type, or the run is refused naming the
// token. `name+ratio=5` in a lab program's line-up or candidate list sets it
// (docs/RUNNER.md, "A strategy's own settings"). A strategy without the member
// compiles and behaves exactly as before, and refuses every setting.
//
// `name` is a per-instance field rather than a static constant so that the same
// strategy can appear twice in one roster under two names — which the headline
// experiment needs, because the swept maker is measured against a rival maker
// that is not swept.
//
// Adding a strategy is a header in arena/strategies/ plus one line in registry.hpp. There
// is no plugin interface, no scripting language and no dynamic loading, by
// decision: strategies link into the binary, which is how research platforms
// actually do it and which keeps this a systems-and-strategy project.
//
// THE WAKE SET, fixed here so that the scheduler is implementable at all. A strategy
// is woken when, and only when:
//   * Own — an event about one of its own orders arrives (ack, fill, cancel,
//     reject). This is what a drop copy gives a real participant.
//   * Touch — the best bid or ask of its instrument changed.
//   * Timer — its own timer_ns elapsed since its last timer wake.
//   * Signal — the information it has been given moved, for the one participant
//     that has any (see `signal` below).
// At most one wake is outstanding per strategy. A burst of events inside one reaction
// window therefore collapses into a single decision, which is both what a real
// system does and what keeps a slow strategy slow: with a 1 ms delay a strategy simply
// cannot look at the market more than a thousand times a second.
//
// WHY ActionBatch AND NOT ONE Action. A market maker cancel-replacing both sides
// needs four actions in one reaction — two cancels and two quotes — and if it
// has to spend four reaction windows doing that, the experiment is measuring the
// API rather than the strategy. The batch is a fixed-size array because
// returning a vector from a strategy would put an allocation in the session loop for
// no reason.
//
// WHAT A STRATEGY CANNOT SEE. Not the fundamental value (unless it is the informed
// participant and the roster says so), not the clock, not other strategies' positions,
// not the engine's internals, and not its queue position except by having
// tracked the feed itself (arena/book_mirror.hpp). Those are the asymmetries the
// session exists to study, so they are enforced by what this struct contains.
#pragma once

#include "arena/book_mirror.hpp"
#include "arena/clock.hpp"
#include "px/messages.hpp"
#include "px/types.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace px::arena {

// Rounds a price down onto the instrument's tick grid. Strategies quote through this,
// because the engine rejects an off-tick price outright (InvalidPrice) and a strategy
// whose every quote is rejected is a silent bug rather than a loud one.
[[nodiscard]] inline constexpr Price snap_to_tick(Price p, Price tick) noexcept {
    if (tick <= 0) return p;
    const Price r = p % tick;
    return r == 0 ? p : p - r;
}

enum class WakeReason : std::uint8_t { Own = 0, Touch, Timer, Signal, Open };

inline constexpr const char* wake_reason_name(WakeReason r) noexcept {
    switch (r) {
        case WakeReason::Own: return "own";
        case WakeReason::Touch: return "touch";
        case WakeReason::Timer: return "timer";
        case WakeReason::Signal: return "signal";
        case WakeReason::Open: return "open";
    }
    return "?";
}

struct Action {
    enum class Kind : std::uint8_t { None = 0, Limit, Market, Cancel };
    Kind kind = Kind::None;
    Side side = Side::Buy;
    Tif tif = Tif::GTC;
    Price price = 0;      // Limit only
    Qty qty = 0;          // Limit and Market
    OrderId cancel_id = 0; // Cancel only; must be an id from `my_orders`

    [[nodiscard]] static Action limit(Side s, Price p, Qty q, Tif t = Tif::GTC) noexcept {
        Action a;
        a.kind = Kind::Limit;
        a.side = s;
        a.tif = t;
        a.price = p;
        a.qty = q;
        return a;
    }
    [[nodiscard]] static Action market(Side s, Qty q) noexcept {
        Action a;
        a.kind = Kind::Market;
        a.side = s;
        a.tif = Tif::IOC;
        a.qty = q;
        return a;
    }
    [[nodiscard]] static Action cancel(OrderId id) noexcept {
        Action a;
        a.kind = Kind::Cancel;
        a.cancel_id = id;
        return a;
    }
};

// Everything one reaction may ask for. Six is enough for a two-sided
// cancel-replace with room to spare; a seventh is dropped rather than growing,
// and the session counts drops so the cap can never hide a bug.
struct ActionBatch {
    static constexpr std::size_t kMax = 6;
    Action a[kMax]{};
    std::uint8_t n = 0;
    std::uint8_t dropped = 0;

    void push(const Action& action) noexcept {
        if (action.kind == Action::Kind::None) return;
        if (n >= kMax) {
            ++dropped;
            return;
        }
        a[n++] = action;
    }
    [[nodiscard]] std::size_t size() const noexcept { return n; }
    [[nodiscard]] bool empty() const noexcept { return n == 0; }
};

// What a participant knows at the instant it decides. Built fresh by the session
// for each wake; nothing in it outlives the on_event() call.
struct MarketView {
    // When the strategy is acting. Its orders carry this timestamp into the engine.
    SimTime now_ns = 0;
    // What it is looking at: the market as of now_ns - reaction_ns. Stale by
    // exactly its own delay, which is the whole point.
    const BookMirror* book = nullptr;
    SimTime as_of_ns = 0;
    WakeReason reason = WakeReason::Open;

    InstrumentId instr = 0;
    Price tick = 100;

    // Its own live orders, with queue position, reconstructed from the feed.
    const BookMirror::OrderRef* my_orders = nullptr;
    std::size_t my_order_count = 0;
    // Orders it has sent that it has not yet seen acknowledged. A strategy that
    // ignores this will quote twice while its first quote is in flight.
    std::uint32_t in_flight = 0;

    std::int64_t position = 0;
    std::int64_t cash = 0;
    std::uint64_t fills = 0;
    std::uint64_t rejects = 0;

    // The information this participant has been given, in price units, or 0 if
    // it has none. Exactly one strategy in the roster is informed; every other strategy
    // sees 0 here and must form its own view from the book. This single field is
    // the information asymmetry the headline experiment prices.
    Price signal = 0;

    [[nodiscard]] Price best(Side s) const noexcept { return book ? book->best(s) : 0; }
    [[nodiscard]] Price mid() const noexcept { return book ? book->mid() : 0; }
    [[nodiscard]] Price last_trade() const noexcept { return book ? book->last_trade() : 0; }
    [[nodiscard]] std::uint64_t size_at_best(Side s) const noexcept { return book ? book->size_at_best(s) : 0; }
};

// ---- how the session holds a heterogeneous roster -------------------------
//
// Strategies are plain structs, so the session wraps each one in this adapter. The
// virtual call costs nothing that matters here: it happens once per reaction, in
// simulated time, nowhere near anything the benchmarks measure.

class StrategyHandle {
public:
    virtual ~StrategyHandle() = default;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
    [[nodiscard]] virtual SimTime reaction_ns() const noexcept = 0;
    [[nodiscard]] virtual SimTime timer_ns() const noexcept = 0;
    virtual void reset(std::uint64_t seed) = 0;
    virtual ActionBatch on_event(const MarketView& v) = 0;
    // The optional sixth member, reached without knowing the strategy's type.
    // A handle with no settings refuses every one; `why` names what was wrong.
    [[nodiscard]] virtual bool has_settings() const noexcept { return false; }
    [[nodiscard]] virtual std::string setting_names() const { return {}; }
    [[nodiscard]] virtual bool apply_setting(std::string_view key, std::int64_t value, std::string& why) {
        (void)key;
        (void)value;
        why = std::string(name()) + " has no settings member, so it takes no settings";
        return false;
    }
};

// The visitor shape settings() is called with, as a named type: detecting the
// member with a lambda in an unevaluated context is not portable to every
// compiler this builds on.
struct SettingProbe {
    template <class T>
    void operator()(const char*, T&, std::int64_t, std::int64_t) const {}
};

template <class S>
concept HasSettings = requires(S& s, SettingProbe p) { s.settings(p); };

template <class Strategy>
class StrategyAdapter final : public StrategyHandle {
public:
    explicit StrategyAdapter(Strategy strategy) : strategy_(strategy) {}
    [[nodiscard]] const char* name() const noexcept override { return strategy_.name; }
    [[nodiscard]] SimTime reaction_ns() const noexcept override { return strategy_.reaction_ns; }
    [[nodiscard]] SimTime timer_ns() const noexcept override { return strategy_.timer_ns; }
    void reset(std::uint64_t seed) override { strategy_.reset(seed); }
    ActionBatch on_event(const MarketView& v) override { return strategy_.on_event(v); }
    [[nodiscard]] Strategy& get() noexcept { return strategy_; }

    [[nodiscard]] bool has_settings() const noexcept override { return HasSettings<Strategy>; }

    [[nodiscard]] std::string setting_names() const override {
        std::string out;
        if constexpr (HasSettings<Strategy>) {
            Strategy copy = strategy_; // settings() takes its fields by reference
            copy.settings([&out](const char* n, auto&, std::int64_t, std::int64_t) {
                if (!out.empty()) out += ' ';
                out += n;
            });
        }
        return out;
    }

    [[nodiscard]] bool apply_setting(std::string_view key, std::int64_t value, std::string& why) override {
        if constexpr (!HasSettings<Strategy>) {
            return StrategyHandle::apply_setting(key, value, why);
        } else {
            bool found = false, ok = false;
            strategy_.settings([&](const char* n, auto& field, std::int64_t lo, std::int64_t hi) {
                using T = std::remove_cvref_t<decltype(field)>;
                static_assert(std::integral<T> && !std::same_as<T, bool>, "a setting is an integer field");
                if (found || key != n) return;
                found = true;
                if (value < lo || value > hi || !std::in_range<T>(value)) {
                    why = std::string(strategy_.name) + "+" + std::string(key) + "=" + std::to_string(value) +
                          ": outside " + std::to_string(lo) + ".." + std::to_string(hi);
                    return;
                }
                field = static_cast<T>(value);
                ok = true;
            });
            if (!found)
                why = std::string(strategy_.name) + " has no setting '" + std::string(key) + "'; its settings: " +
                      setting_names();
            return ok;
        }
    }

private:
    Strategy strategy_;
};

template <class Strategy>
[[nodiscard]] std::unique_ptr<StrategyHandle> make_strategy(Strategy strategy) {
    return std::make_unique<StrategyAdapter<Strategy>>(strategy);
}

} // namespace px::arena
