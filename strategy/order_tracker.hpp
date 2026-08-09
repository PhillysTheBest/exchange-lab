// strategy/order_tracker.hpp — Brain's per-market record of ITS OWN resting
// orders, never other traders' — a small, fixed-capacity, allocation-free
// table, not a general order book.
//
// Array-backed and kept partitioned (acked entries always occupy a
// contiguous prefix) specifically so orders_for() can return a real,
// zero-copy, zero-allocation std::span over just the acked entries. That's
// the structural fix to the cancel race described in the plan doc's
// "Order-ID ring safety" section: a Strategy only ever sees orders_for()'s
// output (via MarketState::outstanding_orders — see market_state.hpp), so it
// cannot construct a Cancel decision against an order Gateway hasn't
// confirmed yet, because it can't see it. No runtime race check needed.
//
// Mutations (on_new/on_acked/on_fill/on_cancelled/on_rejected) happen at
// order-placement rate — a handful of times per second at most (Kalshi's own
// rate limit caps this an order of magnitude lower still) — so they can
// afford a little swap-based bookkeeping. Reads (orders_for) happen on every
// BookUpdate trigger, potentially hundreds of times per second, so those stay
// a trivial span slice with no work at all.
#pragma once

#include "px/shadow_book.hpp"
#include "px/types.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <utility>

namespace px::strategy {

// Bound on resting orders any one strategy keeps open on a single market at
// once. Small and fixed deliberately — legitimately needing more than this
// many simultaneous resting orders on one market is almost certainly a
// runaway strategy, and a fixed bound keeps this allocation-free on a path
// that can be hot (see the file header comment).
inline constexpr std::size_t kMaxRestingOrdersPerMarket = 32;

struct RestingOrder {
    OrderId id = kInvalidOrderId;
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;                  // originally requested qty
    Qty filled_qty = 0;
    Qty remaining_qty = 0;
    Qty depth_ahead_estimate = 0; // frozen at ack time — see strategy.hpp/market_state.hpp;
                                  // an explicit upper bound, not a live/exact figure
};

// SIZE NOTE (found while measuring Stage 5's kMaxMarkets bump, the same
// class of hazard pipeline/brain.hpp's own SIZE WARNING documents for
// Brain): OrderTracker embeds by_market_'s full std::array<MarketOrders,
// kMaxMarkets> by value — sizeof(OrderTracker) is ~1.04 MB at kMaxMarkets =
// 1024 (was ~260 KB at the pre-Stage-5 kMaxMarkets = 256). Existing test
// files (tests/test_strategies.cpp, bench/bench_strategy.cpp) construct it as
// a plain stack local dozens of times and still pass at this size — a
// default Windows thread's reserved stack is comfortably larger than the
// oft-quoted 1 MB figure in this toolchain — but that margin is now
// meaningfully thinner than it was, and a future caller constructing one on
// a thread with an explicitly reduced stack size should heap-allocate it
// (std::make_unique<OrderTracker>()) rather than assume a stack local is
// still safe by the same margin Stage 1-4 code relied on.
class OrderTracker {
public:
    // Optimistically records a pending (unacked) order the instant Brain
    // decides to send it — before Gateway has confirmed anything. Returns
    // false (recording nothing) if this market's table is already full.
    bool on_new(MarketId market, OrderId id, Side side, Price price, Qty qty) noexcept {
        MarketOrders& mo = by_market_[market];
        if (mo.total_count >= kMaxRestingOrdersPerMarket) return false;
        RestingOrder& r = mo.orders[mo.total_count];
        r = RestingOrder{};
        r.id = id;
        r.side = side;
        r.price = price;
        r.qty = qty;
        r.remaining_qty = qty;
        ++mo.total_count;
        return true;
    }

    // Promotes a pending entry to acked once Gateway's create-order response
    // confirms it: Kalshi's own authoritative remaining_count, plus a
    // depth-ahead estimate computed right here (not by the caller) from the
    // order's own already-recorded price (set back in on_new) and `book`'s
    // current level_qty at that price — `book` should be the caller's
    // ShadowBook for this same market, already updated with whatever the
    // exchange's aggregate reflects by ack time (which includes our own
    // now-accepted order). See RestingOrder::depth_ahead_estimate's own
    // comment for what this figure actually means (an upper bound, not a
    // live number). No-op if `id` isn't a pending (not-yet-acked) entry.
    //
    // remaining_qty == 0 (a marketable order that fully filled at entry, or
    // an IOC order whose unfilled remainder Kalshi auto-cancels — either way
    // Kalshi's own create-order response reports it with remaining_count 0):
    // dropped outright rather than promoted. Found by
    // tests/test_pipeline_e2e.cpp: promoting it anyway left a permanent
    // zombie entry — nothing ever removes an acked entry on a fill (see
    // on_fill below, before its own fix) — so orders_for() stayed non-empty
    // forever after a strategy's very first IOC trade. OrderBookImbalance and
    // TradeFlow (strategy/order_book_imbalance.hpp, strategy/trade_flow.hpp)
    // both gate on `outstanding_orders.empty()` as their "am I already
    // exposed" check, so this silently disabled either strategy for the rest
    // of that market's lifetime after one trade — a real bug, not a
    // hypothetical one.
    void on_acked(MarketId market, OrderId id, Qty remaining_qty, const ShadowBook& book) noexcept {
        MarketOrders& mo = by_market_[market];
        const std::size_t idx = find_index(mo, id);
        if (idx == kNotFound || idx < mo.acked_count) return; // unknown, or already acked

        if (remaining_qty == 0) {
            remove_at(mo, idx); // idx is in the pending zone here (checked above) — see remove_at's own comment
            return;
        }

        const Price price = mo.orders[idx].price;
        const Qty level_qty = book.level_qty(price);
        const Qty depth_ahead = (level_qty > remaining_qty) ? (level_qty - remaining_qty) : 0;

        std::swap(mo.orders[idx], mo.orders[mo.acked_count]);
        RestingOrder& r = mo.orders[mo.acked_count];
        r.remaining_qty = remaining_qty;
        r.depth_ahead_estimate = depth_ahead;
        ++mo.acked_count;
    }

    // Applies a fill against a tracked order — acked or still-pending (an
    // order can fill synchronously in the same create-order response that
    // acks it, per Kalshi's V2 schema: fill_count > 0 on a 201). No-op if
    // `id` isn't tracked for this market.
    //
    // Accumulates filled_qty only — does NOT independently derive
    // remaining_qty by subtracting from it. Found by
    // tests/test_hostile_events.cpp's partial-fill-at-entry case: Gateway's
    // only current producer of ExecKind::Fill (live/kalshi_gateway.cpp's
    // handle_new_order) ALWAYS pushes an Acked report for the same id
    // immediately first, carrying Kalshi's own authoritative post-fill
    // remaining_count — on_acked (above) already sets remaining_qty
    // correctly from that. A version of this function that ALSO subtracted
    // filled_qty from remaining_qty was double-counting the exact same fill
    // Acked had already reflected (e.g. remaining_count=6, fill_count=4 on a
    // 10-lot order — genuinely 6 left, but the old code computed 6-4=2).
    // Every remaining_qty update in this codebase comes from an authoritative
    // Kalshi-reported number (on_acked's remaining_count, on_cancelled's
    // reduced_by) — never a locally-derived delta — and this keeps that
    // property, rather than having two different code paths disagree about
    // who owns it.
    //
    // NOTE for whoever builds the still-scoped-out async `fill`-channel
    // listener (see kalshi_gateway.hpp's FILL TRACKING comment): a fill
    // arriving there, for an order that's been resting a while with no
    // adjacent Acked/remaining_count update, will need this function to
    // derive remaining_qty itself (there's no other authoritative source for
    // it at that point) — this simple accumulate-only version is correct for
    // every path reachable today, not for that one.
    void on_fill(MarketId market, OrderId id, Qty filled_qty) noexcept {
        MarketOrders& mo = by_market_[market];
        const std::size_t idx = find_index(mo, id);
        if (idx == kNotFound) return;
        mo.orders[idx].filled_qty += filled_qty;
    }

    // Reduces remaining_qty by `reduced_by`; removes the entry entirely once
    // it hits zero (nothing left resting to track).
    void on_cancelled(MarketId market, OrderId id, Qty reduced_by) noexcept {
        MarketOrders& mo = by_market_[market];
        const std::size_t idx = find_index(mo, id);
        if (idx == kNotFound) return;
        RestingOrder& r = mo.orders[idx];
        r.remaining_qty = (reduced_by >= r.remaining_qty) ? 0 : r.remaining_qty - reduced_by;
        if (r.remaining_qty == 0) remove_at(mo, idx);
    }

    // Kalshi rejected an order Brain's own risk check had already accepted
    // (price moved, market halted, ...) — drops the pending entry. No-op if
    // `id` isn't tracked (already acked, or never existed).
    void on_rejected(MarketId market, OrderId id) noexcept {
        MarketOrders& mo = by_market_[market];
        const std::size_t idx = find_index(mo, id);
        if (idx == kNotFound || idx < mo.acked_count) return; // only ever drops a pending entry
        remove_at(mo, idx);
    }

    // Acked-only view — see the class comment. Unacked (pending) orders are
    // never visible here, by construction, not by filtering.
    [[nodiscard]] std::span<const RestingOrder> orders_for(MarketId market) const noexcept {
        const MarketOrders& mo = by_market_[market];
        return std::span<const RestingOrder>(mo.orders.data(), mo.acked_count);
    }

    // Read-only lookup by id (acked OR still-pending) — nullptr if `id` isn't
    // currently tracked for `market`. Added for Stage 6, Part G: Brain's
    // optional telemetry hooks need this order's own side/price when an
    // ExecReport (Acked/Cancelled/Reject) arrives, and those reports don't
    // carry either field themselves (see pipeline/events.hpp's ExecReport —
    // side/price are documented "Fill only"). Never on any decision-path —
    // only called from Brain's telemetry_ != nullptr branches, themselves
    // O(1)/allocation-free and gated on order-lifecycle events, not every
    // BookUpdate — so this doesn't change orders_for()'s own "acked-only, by
    // construction" guarantee for what a Strategy can see.
    [[nodiscard]] const RestingOrder* find(MarketId market, OrderId id) const noexcept {
        const MarketOrders& mo = by_market_[market];
        const std::size_t idx = find_index(mo, id);
        return idx == kNotFound ? nullptr : &mo.orders[idx];
    }

private:
    static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

    struct MarketOrders {
        std::array<RestingOrder, kMaxRestingOrdersPerMarket> orders{};
        std::size_t acked_count = 0; // orders[0, acked_count) are acked
        std::size_t total_count = 0; // orders[acked_count, total_count) are pending
    };

    [[nodiscard]] static std::size_t find_index(const MarketOrders& mo, OrderId id) noexcept {
        for (std::size_t i = 0; i < mo.total_count; ++i) {
            if (mo.orders[i].id == id) return i;
        }
        return kNotFound;
    }

    // Removes the entry at `idx`, preserving the acked-prefix/pending-suffix
    // partition. `idx < acked_count` (an acked entry): move the last acked
    // entry into the hole, then move the last pending entry into what was
    // the last acked slot, then shrink both counts by one — keeps both zones
    // contiguous with their remaining members, order within a zone being
    // irrelevant (each zone is a bag, never indexed positionally elsewhere).
    // `idx >= acked_count` (a pending entry): swap with the last pending
    // entry and shrink total_count — the acked zone is untouched.
    static void remove_at(MarketOrders& mo, std::size_t idx) noexcept {
        if (idx < mo.acked_count) {
            mo.orders[idx] = mo.orders[mo.acked_count - 1];
            mo.orders[mo.acked_count - 1] = mo.orders[mo.total_count - 1];
            --mo.acked_count;
            --mo.total_count;
        } else {
            mo.orders[idx] = mo.orders[mo.total_count - 1];
            --mo.total_count;
        }
    }

    std::array<MarketOrders, kMaxMarkets> by_market_{};
};

} // namespace px::strategy
