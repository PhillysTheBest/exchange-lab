// px/types.hpp — fundamental scalar types and enums shared across the engine.
//
// Design rule: no floating point anywhere on the price/matching path. Prices are
// integer cents on [1, 99] (a prediction-market contract always settles at 0 or 100).
// Money is integer cents. This eliminates an entire class of rounding bugs and keeps
// the hot path branch-friendly and reproducible bit-for-bit across runs.
#pragma once

#include <cstdint>
#include <limits>

namespace px {

// ---- scalar aliases -------------------------------------------------------

using Price    = std::int32_t;   // cents, valid range [kMinPrice, kMaxPrice]
using Qty      = std::uint32_t;  // contract count
using Cash     = std::int64_t;   // integer cents; signed so P&L / margin can go negative
using OrderId  = std::uint64_t;  // externally-visible, caller-assigned or engine-assigned
using TraderId = std::uint32_t;
using MarketId = std::uint32_t;
using Slot     = std::uint32_t;  // index into a pool — NOT a pointer
using Tsc      = std::uint64_t;  // raw TSC tick count (see tsc_clock.hpp)
using SeqNum   = std::uint64_t;  // monotonically increasing event/trade sequence number

inline constexpr Slot kNullSlot = std::numeric_limits<Slot>::max();

// Reserved sentinel — never assign this as a real OrderId. Used internally by
// FlatOrderMap as the "empty slot" marker so lookups need no separate tombstone bit.
inline constexpr OrderId kInvalidOrderId = std::numeric_limits<OrderId>::max();

// ---- price ladder bounds ---------------------------------------------------
//
// A contract trades on integer cents 1..99 (0 and 100 are the settlement values,
// never live order prices). The ladder array is sized 0..100 inclusive (101 slots)
// so a price can index it directly with no offset arithmetic; slots 0 and 100 are
// permanently empty sentinels, which also gives MARKET-order sweeps a natural
// "one past the edge" stop without a separate bounds check.
inline constexpr Price kMinPrice   = 1;
inline constexpr Price kMaxPrice   = 99;
inline constexpr int   kNumLevels  = 101; // indices [0, 100]

[[nodiscard]] constexpr bool valid_price(Price p) noexcept {
    return p >= kMinPrice && p <= kMaxPrice;
}

// Per-market resting-order capacity. Fixed at compile time so Pool/FlatOrderMap
// preallocate once at construction and never resize/allocate again.
inline constexpr std::size_t kMaxOrdersPerMarket = std::size_t{1} << 16; // 65536

// Ceiling on how many distinct markets a single MatchingEngine instance manages.
// Bounds Trader's flat per-market position array — no per-trader allocation either.
inline constexpr std::size_t kMaxMarkets = 256;

using Position = std::int64_t; // signed net YES-contracts held; negative == net short

// ---- enums ------------------------------------------------------------------

// Which token the *caller* is trading. The engine internally normalizes every NO
// order into an equivalent YES order (see OrderBook::normalize) and only ever
// matches on a single YES book — Contract is retained on the Order record purely
// for reporting/fills back to the trader, it plays no role in matching.
enum class Contract : std::uint8_t { Yes, No };

// Post-normalization side, always relative to the YES token.
enum class Side : std::uint8_t { Buy, Sell };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

enum class OrderType : std::uint8_t { Limit, Market };

// GTC rests on the book after any partial fill. IOC fills what it can immediately
// against the opposite side and cancels any remainder. A Market order is modeled
// as a Limit at the extreme tick (kMaxPrice for a buy, kMinPrice for a sell) with
// TimeInForce::IOC forced — see MatchingEngine — so there is exactly one matching
// code path in OrderBook, not two.
enum class TimeInForce : std::uint8_t { GTC, IOC };

enum class OrderStatus : std::uint8_t {
    New,
    PartiallyFilled,
    Filled,
    Cancelled,
    Rejected,
};

enum class MarketStatus : std::uint8_t {
    PreOpen,
    Open,
    Halted,
    Resolved,
};

// What a resolved market settled to. Undefined/meaningless while status != Resolved.
enum class Settlement : std::uint8_t { Unresolved, Yes, No };

// Reason a command was rejected — surfaced to the caller, never silently dropped.
enum class RejectReason : std::uint8_t {
    None,
    MarketNotOpen,
    InvalidPrice,
    InvalidQty,
    UnknownOrder,
    DuplicateOrderId,
    InsufficientCash,
    InsufficientPosition,
    PoolExhausted,
};

} // namespace px
