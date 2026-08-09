// telemetry/telemetry.hpp — Stage 6, Part G: the metrics model. A read-only
// snapshot the trading engine (Brain, T2) writes to and ANY other thread (this
// process's own periodic JSON publisher today; nothing stops a future in-process
// reader) may read concurrently, racily, by design — see CLAUDE.md's Part F/G:
// "The dashboard should be allowed to become stale. The trading engine must never
// wait for the dashboard," and "Separate: Trading State from Telemetry/Presentation
// State."
//
// TWO TIERS OF STATE, KEPT DELIBERATELY SEPARATE, matching that instruction exactly:
//
//   1. TRADING-ADJACENT ACCOUNTING (telemetry::PositionAccount, position_account.hpp)
//      — plain (non-atomic) fields, mutated ONLY from Brain's own thread (T2), never
//      read cross-thread. This is where average-entry-price / realized-P&L
//      bookkeeping actually lives — real state, real invariants, same "single
//      writer, no locks" discipline Brain itself already holds for ShadowBook/
//      RiskEngine/OrderTracker (see pipeline/brain.hpp's own file header).
//   2. PUBLISHED TELEMETRY (TelemetryState's atomic fields + RecentRing buffers
//      below) — only ever WRITTEN by T2 (via the on_* methods, called from
//      pipeline/brain.cpp), but safe for ANY OTHER thread to READ at any time
//      without coordinating with T2 at all. Every field is either a
//      std::atomic<T> (relaxed ops throughout — this is metrics, not a
//      correctness-critical handoff) or a RecentRing (same relaxed-atomics
//      discipline, one field wider). Nothing here can ever block, stall, or slow
//      down T2's own dispatch loop: every on_* call is O(1), allocation-free, and
//      touches only this object's own memory — never a lock, never I/O.
//
// Brain holds an OPTIONAL `TelemetryState*` (nullptr by default — see
// pipeline/brain.hpp's own comment on attach_telemetry) so every existing test and
// benchmark that doesn't attach one pays a single null-check per call site and
// nothing else; attaching telemetry never changes Brain's own decision-path
// behavior, only what gets additionally recorded alongside it.
#pragma once

#include "px/types.hpp"
#include "telemetry/position_account.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace px::telemetry {

inline constexpr std::size_t kMaxTelemetryMarkets = 64; // dashboard-relevant bound, not
                                                          // kMaxMarkets(1024) -- a dashboard
                                                          // showing 1024 rows would defeat its
                                                          // own "high-signal" purpose (the
                                                          // plan's Part E intro)
inline constexpr std::size_t kRecentOrderEvents = 64;
inline constexpr std::size_t kRecentDecisions = 64;
inline constexpr std::size_t kNameLen = 24;
inline constexpr std::size_t kReasonLen = 48;

enum class Mode : std::uint8_t { DryRun, Live };
enum class ConnStatus : std::uint8_t { Disconnected, Connecting, Connected };
enum class OrderEventKind : std::uint8_t { Submit, Ack, Fill, Cancel, Reject };

struct OrderEventRecord {
    std::int64_t ts_ms = 0;
    MarketId market = 0;
    OrderEventKind kind = OrderEventKind::Submit;
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
};

struct StrategyDecisionRecord {
    std::int64_t ts_ms = 0;
    MarketId market = 0;
    char strategy_name[kNameLen] = {};
    std::uint8_t trigger = 0; // strategy::TriggerKind, stored as a raw byte so this
                               // header doesn't need to include strategy.hpp
    std::uint8_t action = 0;  // strategy::ExecutionAction, same reason
    Price price = 0;
    Qty qty = 0;
    char reason[kReasonLen] = {}; // e.g. "imbalance=+0.71" -- the plan's Strategy
                                    // panel "reason/features" column. A short fixed
                                    // field, not a general log -- same POD-everywhere
                                    // discipline this project holds for ring payloads.
};

// Fixed-capacity ring any number of readers can poll concurrently with the single
// writer — racy by construction (no synchronization between the writer's multi-field
// record write and a reader mid-read), which is fine: telemetry is explicitly
// allowed to be stale or even briefly torn, never a source of truth anything
// trading-critical reads. Same tradeoff SpscRing::size_approx() already documents
// for itself (px/spsc_ring.hpp), generalized to whole records instead of just a size.
template <typename T, std::size_t N>
class RecentRing {
public:
    void push(const T& item) noexcept {
        const auto idx = next_.fetch_add(1, std::memory_order_relaxed);
        items_[idx % N] = item;
    }

    // Oldest-first copy of the last min(pushed_count, N) entries.
    [[nodiscard]] std::vector<T> snapshot() const {
        const std::uint64_t count = next_.load(std::memory_order_relaxed);
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(count, N));
        std::vector<T> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t idx = (count - n + i) % N;
            out.push_back(items_[idx]);
        }
        return out;
    }

private:
    std::array<T, N> items_{};
    std::atomic<std::uint64_t> next_{0};
};

// The cross-thread-visible slice of one market's state — every field atomic,
// derived from (and kept in sync with, on every relevant Brain event) the
// corresponding T2-private PositionAccount. -1 on a price field means "never
// observed," distinguishable from a genuine 0 (which isn't a valid price on this
// project's [1,99] domain anyway — see px/types.hpp — but spelled out via a
// dedicated `has_book` flag below rather than relying on callers to know that).
struct MarketSnapshot {
    std::atomic<bool> in_use{false};
    std::atomic<bool> has_book{false};
    MarketId market = 0; // written once, at first use, before in_use is set true --
                          // safe to read plain once in_use observed true (release/
                          // acquire pair below)
    std::atomic<std::int64_t> position{0};
    std::atomic<std::int32_t> avg_entry_cents{0};
    std::atomic<std::int32_t> yes_bid_cents{0};
    std::atomic<std::int32_t> yes_ask_cents{0};
    std::atomic<std::int64_t> unrealized_pnl_cents{0};
    std::atomic<std::int64_t> realized_pnl_cents{0};
    std::atomic<std::int64_t> exposure_cents{0};
    // px::MarketStatus, default 1 (Open), NOT 0 (PreOpen) -- deliberately matching
    // pipeline::Brain's own market_status_ default (pipeline/brain.hpp's
    // constructor: `market_status_.fill(MarketStatus::Open)`) for the identical
    // reason that comment gives: Kalshi's lifecycle channel reports STATE CHANGES
    // from subscription time onward, not a backfill, so a market that was already
    // open before this connection subscribed never gets an explicit `activated`
    // event. Defaulting this field to PreOpen would show a dashboard viewer "not
    // tradeable yet" for a market Brain itself already considers Open -- a
    // strictly worse, actively misleading failure mode, not a neutral one. Kept in
    // sync with Brain's real status on every book update besides, not just at
    // construction -- see apply_book_event's telemetry_->on_lifecycle call in
    // pipeline/brain.cpp, which doesn't wait for an explicit LifecycleStatus event
    // either.
    std::atomic<std::uint8_t> status{1};
};

class TelemetryState {
public:
    explicit TelemetryState(std::int64_t start_ts_ms) : start_ts_ms_(start_ts_ms) {}

    // ---- called ONLY from Brain's own thread (T2) — see class header comment ----
    void set_connected(bool v) noexcept { connected_.store(v, std::memory_order_relaxed); }
    void set_mode(Mode m) noexcept { mode_.store(static_cast<std::uint8_t>(m), std::memory_order_relaxed); }
    void set_conn_status(ConnStatus s) noexcept { conn_status_.store(static_cast<std::uint8_t>(s), std::memory_order_relaxed); }
    void note_reconnect() noexcept { reconnect_count_.fetch_add(1, std::memory_order_relaxed); }
    void set_active_markets(std::uint32_t n) noexcept { active_markets_.store(n, std::memory_order_relaxed); }

    void on_event_received() noexcept { events_received_.fetch_add(1, std::memory_order_relaxed); }
    void on_event_processed() noexcept { events_processed_.fetch_add(1, std::memory_order_relaxed); }

    void on_strategy_decision(MarketId market, const char* strategy_name, std::uint8_t trigger, std::uint8_t action,
                               Price price, Qty qty, const char* reason, std::int64_t ts_ms) noexcept {
        strategy_decisions_.fetch_add(1, std::memory_order_relaxed);
        StrategyDecisionRecord rec;
        rec.ts_ms = ts_ms;
        rec.market = market;
        std::strncpy(rec.strategy_name, strategy_name, kNameLen - 1);
        rec.trigger = trigger;
        rec.action = action;
        rec.price = price;
        rec.qty = qty;
        if (reason) std::strncpy(rec.reason, reason, kReasonLen - 1);
        recent_decisions_.push(rec);
    }

    void on_order_submitted(MarketId market, Side side, Price price, Qty qty, std::int64_t ts_ms) noexcept {
        orders_submitted_.fetch_add(1, std::memory_order_relaxed);
        recent_orders_.push(OrderEventRecord{ts_ms, market, OrderEventKind::Submit, side, price, qty});
    }
    void on_order_acked(MarketId market, Side side, Price price, Qty qty, std::int64_t ts_ms) noexcept {
        orders_acked_.fetch_add(1, std::memory_order_relaxed);
        recent_orders_.push(OrderEventRecord{ts_ms, market, OrderEventKind::Ack, side, price, qty});
    }
    void on_order_cancelled(MarketId market, Side side, Price price, Qty qty, std::int64_t ts_ms) noexcept {
        orders_cancelled_.fetch_add(1, std::memory_order_relaxed);
        recent_orders_.push(OrderEventRecord{ts_ms, market, OrderEventKind::Cancel, side, price, qty});
    }
    void on_order_rejected(MarketId market, Side side, Price price, Qty qty, std::int64_t ts_ms) noexcept {
        orders_rejected_.fetch_add(1, std::memory_order_relaxed);
        recent_orders_.push(OrderEventRecord{ts_ms, market, OrderEventKind::Reject, side, price, qty});
    }

    // The one method that touches PositionAccount (T2-private — see class header)
    // and then republishes the resulting derived values into this market's atomic
    // MarketSnapshot — the ONLY point where "trading-adjacent accounting" crosses
    // into "published telemetry."
    void on_order_filled(MarketId market, Side side, Price price, Qty qty, Cash fee_cents,
                          std::int64_t ts_ms) noexcept {
        orders_filled_.fetch_add(1, std::memory_order_relaxed);
        fees_cents_.fetch_add(fee_cents, std::memory_order_relaxed);
        recent_orders_.push(OrderEventRecord{ts_ms, market, OrderEventKind::Fill, side, price, qty});

        if (market >= kMaxTelemetryMarkets) return; // beyond the dashboard's bound -- see kMaxTelemetryMarkets
        accounts_[market].apply_fill(side, price, qty);
        publish_account(market);
    }

    // Called on every book update Brain already processes (apply_book_event) --
    // keeps yes_bid/yes_ask and this market's unrealized P&L current even between
    // fills.
    void on_book_update(MarketId market, bool have_bid, Price bid, bool have_ask, Price ask) noexcept {
        if (market >= kMaxTelemetryMarkets) return;
        MarketSnapshot& m = markets_[market];
        if (!m.in_use.load(std::memory_order_relaxed)) {
            m.market = market;
            m.in_use.store(true, std::memory_order_relaxed);
        }
        m.has_book.store(have_bid || have_ask, std::memory_order_relaxed);
        if (have_bid) m.yes_bid_cents.store(bid, std::memory_order_relaxed);
        if (have_ask) m.yes_ask_cents.store(ask, std::memory_order_relaxed);
        if (have_bid || have_ask) {
            const Price mid = have_bid && have_ask ? static_cast<Price>((bid + ask) / 2) : (have_bid ? bid : ask);
            m.unrealized_pnl_cents.store(accounts_[market].unrealized_pnl_cents(mid), std::memory_order_relaxed);
        }
    }

    void on_lifecycle(MarketId market, std::uint8_t status) noexcept {
        if (market >= kMaxTelemetryMarkets) return;
        markets_[market].status.store(status, std::memory_order_relaxed);
    }

    // ---- safe to call from ANY thread (racy-by-design reads) ----
    [[nodiscard]] bool connected() const noexcept { return connected_.load(std::memory_order_relaxed); }
    [[nodiscard]] Mode mode() const noexcept { return static_cast<Mode>(mode_.load(std::memory_order_relaxed)); }
    [[nodiscard]] ConnStatus conn_status() const noexcept {
        return static_cast<ConnStatus>(conn_status_.load(std::memory_order_relaxed));
    }
    [[nodiscard]] std::uint32_t reconnect_count() const noexcept { return reconnect_count_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint32_t active_markets() const noexcept { return active_markets_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::int64_t start_ts_ms() const noexcept { return start_ts_ms_; }

    [[nodiscard]] std::uint64_t events_received() const noexcept { return events_received_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t events_processed() const noexcept { return events_processed_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t strategy_decisions() const noexcept { return strategy_decisions_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t orders_submitted() const noexcept { return orders_submitted_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t orders_acked() const noexcept { return orders_acked_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t orders_filled() const noexcept { return orders_filled_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t orders_cancelled() const noexcept { return orders_cancelled_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t orders_rejected() const noexcept { return orders_rejected_.load(std::memory_order_relaxed); }
    [[nodiscard]] Cash fees_cents() const noexcept { return fees_cents_.load(std::memory_order_relaxed); }

    [[nodiscard]] const std::array<MarketSnapshot, kMaxTelemetryMarkets>& markets() const noexcept { return markets_; }
    [[nodiscard]] std::vector<OrderEventRecord> recent_orders() const { return recent_orders_.snapshot(); }
    [[nodiscard]] std::vector<StrategyDecisionRecord> recent_decisions() const { return recent_decisions_.snapshot(); }

private:
    void publish_account(MarketId market) noexcept {
        const PositionAccount& acc = accounts_[market];
        MarketSnapshot& m = markets_[market];
        if (!m.in_use.load(std::memory_order_relaxed)) {
            m.market = market;
            m.in_use.store(true, std::memory_order_relaxed);
        }
        m.position.store(acc.position(), std::memory_order_relaxed);
        m.avg_entry_cents.store(acc.avg_entry_cents(), std::memory_order_relaxed);
        m.realized_pnl_cents.store(acc.realized_pnl_cents(), std::memory_order_relaxed);
        m.exposure_cents.store((acc.position() < 0 ? -acc.position() : acc.position()) * 100, std::memory_order_relaxed);
        const Price bid = static_cast<Price>(m.yes_bid_cents.load(std::memory_order_relaxed));
        const Price ask = static_cast<Price>(m.yes_ask_cents.load(std::memory_order_relaxed));
        if (m.has_book.load(std::memory_order_relaxed)) {
            const Price mid = (bid > 0 && ask > 0) ? static_cast<Price>((bid + ask) / 2) : (bid > 0 ? bid : ask);
            m.unrealized_pnl_cents.store(acc.unrealized_pnl_cents(mid), std::memory_order_relaxed);
        }
    }

    std::int64_t start_ts_ms_;
    std::atomic<bool> connected_{false};
    std::atomic<std::uint8_t> mode_{static_cast<std::uint8_t>(Mode::DryRun)};
    std::atomic<std::uint8_t> conn_status_{static_cast<std::uint8_t>(ConnStatus::Disconnected)};
    std::atomic<std::uint32_t> reconnect_count_{0};
    std::atomic<std::uint32_t> active_markets_{0};

    std::atomic<std::uint64_t> events_received_{0};
    std::atomic<std::uint64_t> events_processed_{0};
    std::atomic<std::uint64_t> strategy_decisions_{0};
    std::atomic<std::uint64_t> orders_submitted_{0};
    std::atomic<std::uint64_t> orders_acked_{0};
    std::atomic<std::uint64_t> orders_filled_{0};
    std::atomic<std::uint64_t> orders_cancelled_{0};
    std::atomic<std::uint64_t> orders_rejected_{0};
    std::atomic<Cash> fees_cents_{0};

    // T2-THREAD-PRIVATE — see class header. Never read cross-thread; only the
    // derived values publish_account() copies out of it are.
    std::array<PositionAccount, kMaxTelemetryMarkets> accounts_{};

    std::array<MarketSnapshot, kMaxTelemetryMarkets> markets_{};
    RecentRing<OrderEventRecord, kRecentOrderEvents> recent_orders_;
    RecentRing<StrategyDecisionRecord, kRecentDecisions> recent_decisions_;
};

} // namespace px::telemetry
