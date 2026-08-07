// px/order_book.hpp — the array-backed, single-asset (normalized YES) book.
//
// This struct does double duty across the project: offline it is the matching
// engine's book (submit() actually crosses orders); online (Phase 3) it is the
// shadow replica of Kalshi's real book, fed by orderbook_snapshot/orderbook_delta
// (apply_snapshot_level / the same insert/unlink mechanics, driven by the feed
// instead of by matching). Same struct, same invariants, in both roles.
//
// Layout: `levels_[101]` is indexed directly by price (0 and 100 are permanent
// empty sentinels). Each PriceLevel is the head/tail of an intrusive FIFO of
// Order slots — insert is push-to-tail, matching consumes from head, so
// time-priority falls out of the data structure for free. Two 64-bit occupancy
// masks (bid/ask) mirror which indices are non-empty; best bid/ask is one
// countl_zero/countr_zero over those masks, not a scan of the ladder.
//
// SIZE WARNING: this class embeds its Order slab and id map by value — the
// "preallocate everything once, zero allocation on the hot path" design point
// — which makes a single instance ~5.7 MB (kMaxOrdersPerMarket * sizeof(Order),
// plus the flat map). That's exactly right as a member of a heap-allocated
// Market (see market.hpp / matching_engine.cpp, which construct Markets via
// make_unique), and exactly wrong as a stack local or by-value parameter: the
// default thread stack on this platform is far smaller than 5.7 MB. Always
// heap-allocate an OrderBook you construct directly (tests, benchmarks) —
// `std::make_unique<OrderBook>(id)` — never declare one as a local variable.
#pragma once

#include "flat_map.hpp"
#include "normalize.hpp"
#include "order.hpp"
#include "pool.hpp"
#include "trade.hpp"
#include "types.hpp"

#include <array>
#include <bit>
#include <cstdint>

namespace px {

namespace detail {

// Occupancy bitmask over price indices [0, 100]. Bit p tracks whether levels_[p]
// is currently non-empty. word0 covers prices 0-63, word1 covers prices 64-100.
struct Mask128 {
    std::uint64_t word0 = 0;
    std::uint64_t word1 = 0;

    void set(Price p) noexcept {
        if (p < 64) word0 |= (std::uint64_t{1} << p);
        else        word1 |= (std::uint64_t{1} << (p - 64));
    }
    void clear(Price p) noexcept {
        if (p < 64) word0 &= ~(std::uint64_t{1} << p);
        else        word1 &= ~(std::uint64_t{1} << (p - 64));
    }
    [[nodiscard]] bool test(Price p) const noexcept {
        return p < 64 ? (word0 >> p) & 1u : (word1 >> (p - 64)) & 1u;
    }

    // Highest set bit == best bid (buyers want the highest price). -1 if empty.
    [[nodiscard]] int highest_set() const noexcept {
        if (word1 != 0) return 64 + (63 - std::countl_zero(word1));
        if (word0 != 0) return 63 - std::countl_zero(word0);
        return -1;
    }
    // Lowest set bit == best ask (sellers want the lowest price). -1 if empty.
    [[nodiscard]] int lowest_set() const noexcept {
        if (word0 != 0) return std::countr_zero(word0);
        if (word1 != 0) return 64 + std::countr_zero(word1);
        return -1;
    }
};

} // namespace detail

class OrderBook {
public:
    struct PriceLevel {
        Slot head = kNullSlot; // oldest resting order — matched first
        Slot tail = kNullSlot; // newest resting order — next insertion point
        Qty  total_qty = 0;    // sum of resting qty at this level; O(1) depth query
    };

    struct SubmitResult {
        OrderId order_id  = kInvalidOrderId;
        Qty     filled_qty = 0;
        Qty     rested_qty = 0;
        RejectReason reject = RejectReason::None;
    };

    explicit OrderBook(MarketId market_id) noexcept : market_id_(market_id) {}

    // Submits a new order. `contract`/`side`/`price` are as the caller specified
    // them (pre-normalization) — normalize() runs on the very first line. `sink`
    // is invoked once per resulting trade, in price-time-priority order.
    //
    // MARKET orders are translated internally to a LIMIT at the extreme tick with
    // TimeInForce forced to IOC, so there is exactly one matching code path below
    // for both order types.
    SubmitResult submit(OrderId id, TraderId trader, Contract contract, Side side,
                         OrderType type, TimeInForce tif, Price price, Qty qty,
                         Tsc submit_ts, const TradeSink& sink) noexcept;

    // RejectReason::None on success, RejectReason::UnknownOrder if `id` isn't resting.
    [[nodiscard]] RejectReason cancel(OrderId id) noexcept;

    [[nodiscard]] bool best_bid(Price& out) const noexcept;
    [[nodiscard]] bool best_ask(Price& out) const noexcept;

    [[nodiscard]] const PriceLevel& level(Price p) const noexcept { return levels_[static_cast<std::size_t>(p)]; }
    [[nodiscard]] const Order* find_order(OrderId id) const noexcept;
    [[nodiscard]] MarketId market_id() const noexcept { return market_id_; }
    [[nodiscard]] std::size_t resting_count() const noexcept { return pool_.live(); }

    // Shadow-book path (Phase 3): Kalshi's public feed reports aggregate depth per
    // level, not individual resting orders, and it never asks us to cross anything
    // — Kalshi already matched. This sets a level's total resting quantity directly
    // (mask updated to match) without touching the per-order FIFO, which stays
    // unused/empty in shadow mode. Not used by the offline matching path.
    void shadow_set_level(Side side, Price price, Qty new_total_qty) noexcept;

#ifdef PX_DEBUG
    // Debug-only invariant checker: masks agree with the ladder, level total_qty
    // equals the sum of its resting orders' qty, and the book is never crossed.
    // Aborts (via assert) on violation — intended for tests and fuzzing, not for
    // the release/bench build.
    void check_invariants() const;
#endif

private:
    void insert_into_level(Slot s, Price p, Side side) noexcept;
    void unlink_from_level(Slot s) noexcept;

    MarketId market_id_;
    std::array<PriceLevel, kNumLevels> levels_{};
    detail::Mask128 bid_mask_{};
    detail::Mask128 ask_mask_{};

    Pool<Order, kMaxOrdersPerMarket> pool_{};
    FlatOrderMap<kMaxOrdersPerMarket * 2> id_map_{};
};

} // namespace px
