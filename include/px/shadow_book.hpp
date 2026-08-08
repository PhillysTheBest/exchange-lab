// px/shadow_book.hpp — a slim, multi-market-friendly Kalshi shadow book.
//
// OrderBook (order_book.hpp) already has a shadow mode (shadow_set_level), and
// it works, but it's the wrong type to hold one-per-market for Stage 2's live
// pipeline: it embeds a full Order slab + id map for the offline matching path
// it also serves, which makes a single instance ~5.7 MB (see order_book.hpp's
// SIZE WARNING). Tracking even 20 markets that way is 114 MB — well past L3,
// let alone the L1 residency a real depth-limited watch list should have.
//
// In shadow mode none of that machinery is ever touched: Kalshi has already
// matched, so this side only ever *sets* a level's aggregate resting quantity,
// never runs the FIFO/pool/id-map matching mechanics at all. ShadowBook is
// exactly that subset — a Qty[101] ladder plus the two occupancy bitmasks,
// nothing else — reusing detail::Mask128 and the same countl_zero/countr_zero
// BBO trick as OrderBook. One instance is ~(101*4 + 2*16) bytes ≈ 436 bytes;
// 20 markets is under 9 KB, comfortably L1-resident on this machine (32 KB/core).
#pragma once

#include "order_book.hpp" // for detail::Mask128 — the one piece worth sharing
#include "types.hpp"

#include <array>

namespace px {

class ShadowBook {
public:
    // Sets a level's total resting quantity directly (mask updated to match).
    // Same semantics as OrderBook::shadow_set_level, on a much smaller struct.
    void set_level(Side side, Price price, Qty new_total_qty) noexcept {
        qty_[static_cast<std::size_t>(price)] = new_total_qty;
        detail::Mask128& mask = (side == Side::Buy) ? bid_mask_ : ask_mask_;
        if (new_total_qty > 0) mask.set(price);
        else                   mask.clear(price);
    }

    // Applies a signed change against the level's current total, clamped at 0
    // (a delta arriving before its base snapshot, or a rounding artifact at
    // the edge, should never wrap an unsigned quantity negative).
    void apply_delta(Side side, Price price, long long delta) noexcept {
        const long long updated = static_cast<long long>(qty_[static_cast<std::size_t>(price)]) + delta;
        set_level(side, price, static_cast<Qty>(updated < 0 ? 0 : updated));
    }

    // Zeroes every level on both sides — used before replaying a fresh/resync
    // snapshot so stale levels from before a gap don't linger (see
    // kalshi_feed.cpp's pump_once for the OrderBook-based equivalent).
    void clear() noexcept {
        qty_.fill(0);
        bid_mask_ = detail::Mask128{};
        ask_mask_ = detail::Mask128{};
    }

    [[nodiscard]] Qty level_qty(Price price) const noexcept { return qty_[static_cast<std::size_t>(price)]; }

    [[nodiscard]] bool best_bid(Price& out) const noexcept {
        const int hi = bid_mask_.highest_set();
        if (hi < 0) return false;
        out = static_cast<Price>(hi);
        return true;
    }
    [[nodiscard]] bool best_ask(Price& out) const noexcept {
        const int lo = ask_mask_.lowest_set();
        if (lo < 0) return false;
        out = static_cast<Price>(lo);
        return true;
    }

private:
    std::array<Qty, kNumLevels> qty_{};
    detail::Mask128 bid_mask_{};
    detail::Mask128 ask_mask_{};
};

} // namespace px
