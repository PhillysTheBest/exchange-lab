// pipeline/events.hpp — the fixed-size POD payloads carried across the three
// SpscRing<T,Capacity> channels that connect T1 Ingress -> T2 Brain -> T3
// Egress -> (back to) T2. SpscRing requires std::is_trivially_copyable_v<T>
// (see px/spsc_ring.hpp) — every type here is checked against that at compile
// time, not just assumed.
#pragma once

#include "px/spsc_ring.hpp"
#include "px/types.hpp"
#include "strategy/strategy.hpp" // px::strategy::OrderIntent — reused as-is for ring B

#include <cstdint>
#include <type_traits>

namespace px::pipeline {

// ---- ring A: T1 Ingress -> T2 Brain -----------------------------------------
//
// A decoded WS message, reduced to instructions ShadowBook already knows how
// to apply (set_level/apply_delta) plus the two things T1 needs to tell T2
// that aren't book mutations: "clear this market's book, a fresh snapshot
// follows" (SnapshotBegin — sent once before the run of LevelSet events that
// make up a snapshot, whether the initial one or a resync) and a ticker
// update (for strategies that key off the ticker channel directly rather
// than the book). One flat struct, not a tagged union of differently-shaped
// types, because SpscRing needs one fixed T; the wasted bytes on a given
// event's unused fields are a few bytes on a struct that's already a fraction
// of a cache line, and it keeps this file boring instead of clever.
//
// Trade (Stage 3): a public trade notification from Kalshi's `trade` WS
// channel (see live/kalshi_feed.cpp). Collapsed into this same BookEvent type
// rather than given its own SpscRing/TriggerKind — Brain's strategy dispatch
// re-reads the full MarketState regardless of which event triggered it (see
// strategy/market_state.hpp), so a separate identity would buy nothing. Uses
// side/price/qty (the trade's own) plus ts_ms (the trade's own exchange
// timestamp — NOT Brain's processing-time clock, needed for TradeWindow's
// time-bounded recency logic in strategy/trade_window.hpp).
//
// LifecycleStatus (Stage 4): a mapped market_lifecycle_v2 event (see
// live/kalshi_feed.hpp's SCHEMA STATUS note for the event_type -> MarketStatus
// mapping — Kalshi's spec has no literal "halted" value). Carries the new
// status in `lifecycle_status`; every other field is unused. Brain applies it
// to market_status_ and dispatches BookUpdate same as any other event, so a
// strategy can react immediately (e.g. cancel a resting quote the moment its
// market halts) — the actual "stop new execution" / "stop trading
// permanently" gating happens in Brain::handle_decision, not here.
enum class BookEventKind : std::uint8_t { SnapshotBegin, LevelSet, LevelDelta, Ticker, Trade, LifecycleStatus };

struct BookEvent {
    BookEventKind kind = BookEventKind::LevelSet;
    MarketId market = 0;

    // LevelSet: qty is the new absolute total. LevelDelta: qty_delta is the
    // signed change (Kalshi contract counts fit comfortably in int32; a
    // per-level count anywhere near INT32_MAX would itself indicate a decode
    // bug upstream). Ticker: price/ask_price carry yes_bid/yes_ask; side and
    // qty/qty_delta are unused. Trade: side/price/qty are the trade's own,
    // ts_ms is its exchange timestamp. LifecycleStatus: lifecycle_status only.
    // SnapshotBegin uses only `market`.
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
    std::int32_t qty_delta = 0;
    Price ask_price = 0;  // Ticker only: yes_ask (price above doubles as yes_bid)
    std::int64_t ts_ms = 0; // Trade only: the trade's own exchange timestamp
    MarketStatus lifecycle_status = MarketStatus::Open; // LifecycleStatus only
};
static_assert(std::is_trivially_copyable_v<BookEvent>, "BookEvent crosses an SpscRing — must stay POD");

// ---- ring B: T2 Brain -> T3 Egress ------------------------------------------
//
// Reused directly rather than redeclared: px::strategy::OrderIntent (see
// strategy/strategy.hpp) is already exactly this — a plain description of
// what a Strategy would like to do, with no engine/network coupling — and is
// already trivially copyable (all scalar/enum fields, no user-declared
// special member functions). Using the same type here is also what actually
// wires the Strategy Engine into the live pipeline instead of leaving it a
// local-only demo (see the plan doc's Stage 2 "what Stage 1 left behind"
// table).
using OrderIntent = px::strategy::OrderIntent;
using IntentKind = px::strategy::IntentKind; // re-exported alongside OrderIntent, same reasoning
static_assert(std::is_trivially_copyable_v<OrderIntent>, "OrderIntent crosses an SpscRing — must stay POD");
// OrderIntent gained IntentKind/OrderId fields in Stage 3 (see strategy/strategy.hpp)
// so a CancelOrder intent can travel ring B alongside NewOrder ones — see the plan
// doc's "Order-ID ring safety" section for why Kalshi's own UUID order_id never
// appears here (or anywhere on any ring): Gateway keeps that mapping privately.

inline constexpr std::size_t kRingBCapacity = 1u << 12; // 4096
using RingB = SpscRing<OrderIntent, kRingBCapacity>;

// ---- ring C: T3 Egress -> T2 Brain (execution reports) ----------------------
//
// What T2 needs to reconcile RiskEngine's inventory (record_fill) and rate
// state (on_rate_limited_response) against reality — see risk/risk_engine.hpp
// — plus (Stage 3) strategy::OrderTracker's view of each order's own resting
// state. `now_ns` is deliberately NOT carried here: T2 reads its own clock
// when it consumes the event, rather than trusting a timestamp minted on T3,
// so the risk engine's time domain has exactly one source.
//
// Acked/Cancelled (Stage 3): pushed from Gateway's create-order/cancel-order
// REST response parsing (live/kalshi_rest.cpp, live/kalshi_gateway.cpp) — see
// the plan doc's "Order-ID ring safety" section. `id` is Brain's own OrderId
// (never Kalshi's UUID, which stays private to Gateway).
enum class ExecKind : std::uint8_t { Fill, Reject, RateLimited, Acked, Cancelled };

struct ExecReport {
    ExecKind kind = ExecKind::Fill;
    MarketId market = 0;
    OrderId id = kInvalidOrderId; // which of Brain's own tracked orders this is about
    Side side = Side::Buy;        // Fill only
    Price price = 0;              // Fill only: average fill price
    Qty qty = 0;                  // Fill: filled amount. Acked: remaining_count. Cancelled: reduced_by.
};
static_assert(std::is_trivially_copyable_v<ExecReport>, "ExecReport crosses an SpscRing — must stay POD");

inline constexpr std::size_t kRingCCapacity = 1u << 12; // 4096
using RingC = SpscRing<ExecReport, kRingCCapacity>;

} // namespace px::pipeline
