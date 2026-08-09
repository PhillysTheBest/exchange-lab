// telemetry/telemetry.hpp — Stage 6, Part G (extended Stage 6A): the metrics model. A
// read-only snapshot the trading engine (Brain, T2) writes to and ANY other thread
// (this process's own periodic JSON publisher today; nothing stops a future in-process
// reader) may read concurrently, racily, by design — see docs/CLAUDE.md's Part F/G:
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
//      below) — only ever WRITTEN by T2 (via the on_*/set_* methods, called from
//      pipeline/brain.cpp or, for the handful of once-at-startup config values,
//      tools/kalshi_pipeline.cpp's main thread BEFORE T2 starts — see
//      set_risk_limits's own comment for why that one exception is still safe),
//      but safe for ANY OTHER thread to READ at any time without coordinating with
//      T2 at all. Every field is either a std::atomic<T> (relaxed ops throughout —
//      this is metrics, not a correctness-critical handoff) or a RecentRing (same
//      relaxed-atomics discipline, one field wider). Nothing here can ever block,
//      stall, or slow down T2's own dispatch loop: every on_* call is O(1) or
//      bounded-O(kDepthLevels) — never unbounded, never a scan of all markets —
//      allocation-free, and touches only this object's own memory — never a lock,
//      never I/O.
//
// Brain holds an OPTIONAL `TelemetryState*` (nullptr by default — see
// pipeline/brain.hpp's own comment on attach_telemetry) so every existing test and
// benchmark that doesn't attach one pays a single null-check per call site and
// nothing else; attaching telemetry never changes Brain's own decision-path
// behavior, only what gets additionally recorded alongside it.
//
// Stage 6A (dashboard redesign, see docs/DASHBOARD_DATA_CONTRACT.md for the full
// field-by-field inventory this pass added): extends the original Part G model with
// per-market order-book depth (bounded top-N, not the full 99-level ladder), a public
// trade tape, a session P&L history for charting, and real risk-limit/rate-limit
// figures — every one of them real data Brain or RiskEngine already computes, none
// fabricated for the frontend's benefit (see that same contract doc's "what was
// deliberately NOT added" section for the handful of fields that WOULD require
// inventing data, and were left out instead).
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
inline constexpr std::size_t kRecentTrades = 32; // public trade tape -- Stage 6A
// Session-scoped P&L history for the dashboard's trajectory chart (Stage 6A). Sampled
// on every fill (unthrottled -- fills are inherently infrequent) and on book updates
// throttled to at most one sample per kPnlSampleMinIntervalMs (book updates are NOT
// inherently infrequent). At the throttle rate, 512 samples covers at least ~17
// minutes of session history -- "Session" only, deliberately not a longer-window
// time-range selector (1H/6H/24H): that would need on-disk time-series retention,
// out of scope for an in-memory ring built for a live dashboard's own session view.
inline constexpr std::size_t kPnlHistoryCapacity = 512;
inline constexpr std::int64_t kPnlSampleMinIntervalMs = 2000;
// Top-of-book depth published per market per side (Stage 6A) -- bounded, NOT the full
// [1,99] ladder: a "current market" panel showing 99 rows per side would defeat its
// own purpose the same way kMaxTelemetryMarkets already reasons about market COUNT.
// 8 is enough to show real shape (multiple levels of size) without turning into a
// second full order book implementation living inside telemetry.
inline constexpr std::size_t kDepthLevels = 8;
inline constexpr std::size_t kNameLen = 24;
inline constexpr std::size_t kReasonLen = 48;

enum class Mode : std::uint8_t { DryRun, Live };
enum class ConnStatus : std::uint8_t { Disconnected, Connecting, Connected };
// RiskBlocked (Stage 6A): Brain's OWN pre-send rejection (RiskEngine::check/
// check_cancel, or the market-lifecycle gate in Brain::handle_decision) -- distinct
// from Reject, which is Kalshi's POST-send rejection of an order Brain already
// approved and sent. Conflating the two would hide exactly the distinction
// risk/risk_engine.hpp's own Verdict enum and Brain::handle_decision already keep
// separate (see pipeline/brain.cpp) -- see TelemetryState::on_risk_rejected.
enum class OrderEventKind : std::uint8_t { Submit, Ack, Fill, Cancel, Reject, RiskBlocked };

struct OrderEventRecord {
    std::int64_t ts_ms = 0;
    MarketId market = 0;
    OrderId id = kInvalidOrderId; // Stage 6A: lets the dashboard group this market's
                                   // recent_orders entries by order id client-side into
                                   // one order's own lifecycle (submit -> ack -> fill),
                                   // which the flat, unlinked ring couldn't support
                                   // before. kInvalidOrderId for RiskBlocked (no order
                                   // was ever assigned an id -- it never got that far).
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
                                    // Deliberately NOT decomposed into separate
                                    // structured fields (Imbalance/Spread/...) -- see
                                    // docs/DASHBOARD_DATA_CONTRACT.md's "deliberately
                                    // not added" section for why.
};

// A real public trade Brain observed on this market (pipeline::BookEventKind::Trade)
// -- NOT one of this account's own fills (those are OrderEventRecord/Fill above).
// Stage 6A: the plan's "Recent Trades" tape ties directly to strategy/trade_flow.hpp's
// own signal, and until this pass nothing published the market-wide trade tape at all,
// only this account's own order events.
struct TradeRecord {
    std::int64_t ts_ms = 0;
    MarketId market = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
};

// One point on the session P&L trajectory chart -- Stage 6A.
struct PnlSample {
    std::int64_t ts_ms = 0;
    Cash net_pnl_cents = 0; // realized + unrealized - fees, aggregated across every
                              // market this process is tracking, at the moment sampled
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

    // ---- top-of-book depth (Stage 6A) -- nearest-price-first each side ----
    std::array<std::atomic<std::int32_t>, kDepthLevels> bid_depth_price{};
    std::array<std::atomic<std::uint32_t>, kDepthLevels> bid_depth_qty{};
    std::array<std::atomic<std::int32_t>, kDepthLevels> ask_depth_price{};
    std::array<std::atomic<std::uint32_t>, kDepthLevels> ask_depth_qty{};
    std::atomic<std::uint8_t> bid_depth_count{0};
    std::atomic<std::uint8_t> ask_depth_count{0};
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

    // Stage 6A: risk-config figures (risk/risk_engine.hpp's RiskConfig), real numbers
    // this process was actually started with, not fabricated defaults -- called ONCE,
    // from tools/kalshi_pipeline.cpp's main thread, but BEFORE brain_thread starts
    // (same ordering attach_telemetry itself already relies on for its own "T2-only
    // writes" invariant to hold in practice) -- so there is still only ever one
    // logical writer active at any moment this object is reachable from more than one
    // thread. Not re-called after that; RiskConfig itself is fixed for a process's
    // whole lifetime (see risk_engine.hpp -- RiskEngine takes it by value at
    // construction, nothing ever mutates it afterward).
    void set_risk_limits(Cash max_exposure_cents, Price fat_finger_min, Price fat_finger_max,
                          std::int64_t rate_limit_capacity_tokens) noexcept {
        max_exposure_cents_.store(max_exposure_cents, std::memory_order_relaxed);
        fat_finger_min_.store(fat_finger_min, std::memory_order_relaxed);
        fat_finger_max_.store(fat_finger_max, std::memory_order_relaxed);
        rate_limit_capacity_.store(rate_limit_capacity_tokens, std::memory_order_relaxed);
    }
    // Stage 6A: sampled from Brain::handle_decision (T2) right alongside the same
    // risk_.check()/check_cancel() call that already runs there -- RiskEngine's own
    // token bucket is T2-private state (risk/risk_engine.hpp), never read cross-thread
    // any other way; this is the one sanctioned, already-on-T2 read of it.
    void set_rate_limit_tokens(std::int64_t tokens_available) noexcept {
        rate_limit_tokens_.store(tokens_available, std::memory_order_relaxed);
    }
    // Stage 6A: Brain's OWN pre-send rejection (lifecycle gate or RiskEngine verdict)
    // -- see OrderEventKind::RiskBlocked's own comment for why this is kept distinct
    // from on_order_rejected (Kalshi's post-send rejection) rather than merged into
    // the same counter/kind.
    void on_risk_rejected(MarketId market, std::int64_t ts_ms) noexcept {
        risk_rejections_.fetch_add(1, std::memory_order_relaxed);
        recent_orders_.push(
            OrderEventRecord{ts_ms, market, kInvalidOrderId, OrderEventKind::RiskBlocked, Side::Buy, 0, 0});
    }

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

    void on_order_submitted(MarketId market, OrderId id, Side side, Price price, Qty qty, std::int64_t ts_ms) noexcept {
        orders_submitted_.fetch_add(1, std::memory_order_relaxed);
        recent_orders_.push(OrderEventRecord{ts_ms, market, id, OrderEventKind::Submit, side, price, qty});
    }
    void on_order_acked(MarketId market, OrderId id, Side side, Price price, Qty qty, std::int64_t ts_ms) noexcept {
        orders_acked_.fetch_add(1, std::memory_order_relaxed);
        recent_orders_.push(OrderEventRecord{ts_ms, market, id, OrderEventKind::Ack, side, price, qty});
    }
    void on_order_cancelled(MarketId market, OrderId id, Side side, Price price, Qty qty, std::int64_t ts_ms) noexcept {
        orders_cancelled_.fetch_add(1, std::memory_order_relaxed);
        recent_orders_.push(OrderEventRecord{ts_ms, market, id, OrderEventKind::Cancel, side, price, qty});
    }
    void on_order_rejected(MarketId market, OrderId id, Side side, Price price, Qty qty, std::int64_t ts_ms) noexcept {
        orders_rejected_.fetch_add(1, std::memory_order_relaxed);
        recent_orders_.push(OrderEventRecord{ts_ms, market, id, OrderEventKind::Reject, side, price, qty});
    }

    // The one method that touches PositionAccount (T2-private — see class header)
    // and then republishes the resulting derived values into this market's atomic
    // MarketSnapshot — the ONLY point where "trading-adjacent accounting" crosses
    // into "published telemetry." Also samples the session P&L history (Stage 6A) --
    // unthrottled here, since fills are inherently infrequent relative to book
    // updates (see on_book_update's own throttled sampling).
    void on_order_filled(MarketId market, OrderId id, Side side, Price price, Qty qty, Cash fee_cents,
                          std::int64_t ts_ms) noexcept {
        orders_filled_.fetch_add(1, std::memory_order_relaxed);
        fees_cents_.fetch_add(fee_cents, std::memory_order_relaxed);
        recent_orders_.push(OrderEventRecord{ts_ms, market, id, OrderEventKind::Fill, side, price, qty});

        if (market < kMaxTelemetryMarkets) {
            accounts_[market].apply_fill(side, price, qty);
            publish_account(market);
        }
        sample_pnl(ts_ms);
    }

    // Called on every book update Brain already processes (apply_book_event) --
    // keeps yes_bid/yes_ask and this market's unrealized P&L current even between
    // fills. `ts_ms` (Stage 6A addition) drives the throttled P&L-history sample
    // below -- book updates are NOT inherently infrequent the way fills are, so
    // sampling every single one would blow through kPnlHistoryCapacity in seconds
    // under real book-update traffic and leave the chart showing only the last few
    // moments instead of the session.
    void on_book_update(MarketId market, bool have_bid, Price bid, bool have_ask, Price ask,
                         std::int64_t ts_ms) noexcept {
        if (market < kMaxTelemetryMarkets) {
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
                const Cash old_unrealized = m.unrealized_pnl_cents.load(std::memory_order_relaxed);
                const Cash new_unrealized = accounts_[market].unrealized_pnl_cents(mid);
                m.unrealized_pnl_cents.store(new_unrealized, std::memory_order_relaxed);
                total_unrealized_pnl_cents_.fetch_add(new_unrealized - old_unrealized, std::memory_order_relaxed);
            }
        }
        if (ts_ms - last_pnl_sample_ts_ms_ >= kPnlSampleMinIntervalMs) {
            last_pnl_sample_ts_ms_ = ts_ms; // T2-thread-private, never read cross-thread -- see its own field comment
            sample_pnl(ts_ms);
        }
    }

    // Stage 6A: top-of-book depth, bounded to kDepthLevels per side, nearest-price-
    // first -- called from Brain right alongside on_book_update, with arrays it
    // already scanned out of its own ShadowBook (see pipeline/brain.cpp). Plain
    // pointers, not a ShadowBook reference, so this header stays decoupled from
    // px/shadow_book.hpp the same way strategy/strategy.hpp forward-declares
    // MarketState instead of including market_state.hpp -- Brain already has the
    // book open at the call site, there's no reason telemetry needs to know its type.
    void on_depth_update(MarketId market, const Price* bid_prices, const Qty* bid_qtys, std::size_t bid_n,
                          const Price* ask_prices, const Qty* ask_qtys, std::size_t ask_n) noexcept {
        if (market >= kMaxTelemetryMarkets) return;
        MarketSnapshot& m = markets_[market];
        const std::size_t bn = std::min(bid_n, kDepthLevels);
        for (std::size_t i = 0; i < bn; ++i) {
            m.bid_depth_price[i].store(bid_prices[i], std::memory_order_relaxed);
            m.bid_depth_qty[i].store(bid_qtys[i], std::memory_order_relaxed);
        }
        m.bid_depth_count.store(static_cast<std::uint8_t>(bn), std::memory_order_relaxed);
        const std::size_t an = std::min(ask_n, kDepthLevels);
        for (std::size_t i = 0; i < an; ++i) {
            m.ask_depth_price[i].store(ask_prices[i], std::memory_order_relaxed);
            m.ask_depth_qty[i].store(ask_qtys[i], std::memory_order_relaxed);
        }
        m.ask_depth_count.store(static_cast<std::uint8_t>(an), std::memory_order_relaxed);
    }

    // Stage 6A: a real public trade (pipeline::BookEventKind::Trade), not one of this
    // account's own fills -- see TradeRecord's own comment.
    void on_public_trade(MarketId market, Side side, Price price, Qty qty, std::int64_t ts_ms) noexcept {
        recent_trades_.push(TradeRecord{ts_ms, market, side, price, qty});
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
    [[nodiscard]] std::uint64_t risk_rejections() const noexcept { return risk_rejections_.load(std::memory_order_relaxed); }
    [[nodiscard]] Cash fees_cents() const noexcept { return fees_cents_.load(std::memory_order_relaxed); }

    // Stage 6A: O(1) aggregate P&L, maintained incrementally (see publish_account/
    // on_book_update's delta-fetch_add calls) rather than summed over markets() on
    // every read -- the read side (publish_snapshot, polled at dashboard rates) could
    // afford an O(kMaxTelemetryMarkets) scan, but this also lets on_order_filled/
    // on_book_update sample net_pnl_cents() for the history ring at T2 rates without
    // a scan on the hot(ter) write side.
    [[nodiscard]] Cash total_realized_pnl_cents() const noexcept { return total_realized_pnl_cents_.load(std::memory_order_relaxed); }
    [[nodiscard]] Cash total_unrealized_pnl_cents() const noexcept { return total_unrealized_pnl_cents_.load(std::memory_order_relaxed); }
    [[nodiscard]] Cash net_pnl_cents() const noexcept {
        return total_realized_pnl_cents_.load(std::memory_order_relaxed) +
               total_unrealized_pnl_cents_.load(std::memory_order_relaxed) - fees_cents_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] Cash max_exposure_cents() const noexcept { return max_exposure_cents_.load(std::memory_order_relaxed); }
    [[nodiscard]] Price fat_finger_min() const noexcept { return static_cast<Price>(fat_finger_min_.load(std::memory_order_relaxed)); }
    [[nodiscard]] Price fat_finger_max() const noexcept { return static_cast<Price>(fat_finger_max_.load(std::memory_order_relaxed)); }
    [[nodiscard]] std::int64_t rate_limit_tokens() const noexcept { return rate_limit_tokens_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::int64_t rate_limit_capacity() const noexcept { return rate_limit_capacity_.load(std::memory_order_relaxed); }

    [[nodiscard]] const std::array<MarketSnapshot, kMaxTelemetryMarkets>& markets() const noexcept { return markets_; }
    [[nodiscard]] std::vector<OrderEventRecord> recent_orders() const { return recent_orders_.snapshot(); }
    [[nodiscard]] std::vector<StrategyDecisionRecord> recent_decisions() const { return recent_decisions_.snapshot(); }
    [[nodiscard]] std::vector<TradeRecord> recent_trades() const { return recent_trades_.snapshot(); }
    [[nodiscard]] std::vector<PnlSample> pnl_history() const { return pnl_history_.snapshot(); }

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

        const Cash old_realized = m.realized_pnl_cents.load(std::memory_order_relaxed);
        const Cash new_realized = acc.realized_pnl_cents();
        m.realized_pnl_cents.store(new_realized, std::memory_order_relaxed);
        total_realized_pnl_cents_.fetch_add(new_realized - old_realized, std::memory_order_relaxed);

        m.exposure_cents.store((acc.position() < 0 ? -acc.position() : acc.position()) * 100, std::memory_order_relaxed);
        const Price bid = static_cast<Price>(m.yes_bid_cents.load(std::memory_order_relaxed));
        const Price ask = static_cast<Price>(m.yes_ask_cents.load(std::memory_order_relaxed));
        if (m.has_book.load(std::memory_order_relaxed)) {
            const Price mid = (bid > 0 && ask > 0) ? static_cast<Price>((bid + ask) / 2) : (bid > 0 ? bid : ask);
            const Cash old_unrealized = m.unrealized_pnl_cents.load(std::memory_order_relaxed);
            const Cash new_unrealized = acc.unrealized_pnl_cents(mid);
            m.unrealized_pnl_cents.store(new_unrealized, std::memory_order_relaxed);
            total_unrealized_pnl_cents_.fetch_add(new_unrealized - old_unrealized, std::memory_order_relaxed);
        }
    }

    void sample_pnl(std::int64_t ts_ms) noexcept { pnl_history_.push(PnlSample{ts_ms, net_pnl_cents()}); }

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
    std::atomic<std::uint64_t> risk_rejections_{0};
    std::atomic<Cash> fees_cents_{0};

    std::atomic<Cash> total_realized_pnl_cents_{0};
    std::atomic<Cash> total_unrealized_pnl_cents_{0};

    std::atomic<Cash> max_exposure_cents_{0};
    std::atomic<std::int32_t> fat_finger_min_{0};
    std::atomic<std::int32_t> fat_finger_max_{0};
    std::atomic<std::int64_t> rate_limit_tokens_{-1}; // -1 == "never sampled yet"
    std::atomic<std::int64_t> rate_limit_capacity_{0};

    // T2-THREAD-PRIVATE — see class header. Never read cross-thread; only the
    // derived values publish_account() copies out of it are.
    std::array<PositionAccount, kMaxTelemetryMarkets> accounts_{};

    std::array<MarketSnapshot, kMaxTelemetryMarkets> markets_{};
    RecentRing<OrderEventRecord, kRecentOrderEvents> recent_orders_;
    RecentRing<StrategyDecisionRecord, kRecentDecisions> recent_decisions_;
    RecentRing<TradeRecord, kRecentTrades> recent_trades_;
    RecentRing<PnlSample, kPnlHistoryCapacity> pnl_history_;

    // T2-thread-private throttle state for on_book_update's P&L sampling -- never
    // read cross-thread (only ever compared/assigned from within on_book_update,
    // always called from T2), so plain, not atomic, same as accounts_ above.
    std::int64_t last_pnl_sample_ts_ms_ = 0;
};

} // namespace px::telemetry
