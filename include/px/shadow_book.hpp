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

#ifdef PX_DEBUG
#include <cassert>
#endif

namespace px {

class ShadowBook {
public:
    // Sets a level's total resting quantity directly (mask updated to match).
    // Same semantics as OrderBook::shadow_set_level, on a much smaller struct.
    //
    // A single price level shares one `qty_` slot regardless of side — it can
    // only ever represent one side's resting interest at a time (a normalized
    // book is never simultaneously a bid and an ask at the same price; that
    // would just be a cross). So setting a level on one side always
    // proactively clears the OTHER side's occupancy bit at that price,
    // unconditionally, before deciding this side's own bit from
    // new_total_qty: found while writing check_invariants below — the
    // original version only cleared the other side's bit on the
    // new_total_qty > 0 branch, so a level flipping sides *to a zero
    // quantity* (side A held real qty; side B "sets" the same price to 0)
    // still overwrote the shared qty_ slot via the unconditional assignment
    // above, while leaving side A's mask bit stale/set — qty_ said empty,
    // side A's mask said occupied. A second-order version of the same bug
    // the first fix only half-closed. Caught by tests/test_replay_fuzz.cpp's
    // adversarial side-flip fuzzing (which legitimate Kalshi data shouldn't
    // produce, but adversarial/malformed input reaching here could), not by
    // inspection.
    void set_level(Side side, Price price, Qty new_total_qty) noexcept {
        qty_[static_cast<std::size_t>(price)] = new_total_qty;
        detail::Mask128& mask = (side == Side::Buy) ? bid_mask_ : ask_mask_;
        detail::Mask128& other = (side == Side::Buy) ? ask_mask_ : bid_mask_;
        other.clear(price);
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

#ifdef PX_DEBUG
    // Unlike OrderBook::check_invariants (order_book.hpp), there is no order
    // pool/FIFO here to cross-check total_qty against — this is the entire
    // invariant available on a struct that's just a Qty array plus two
    // occupancy bitmasks: a level's mask occupancy must agree with whether
    // its quantity is nonzero, and — since qty_ is shared across sides — a
    // level can never be occupied by both masks at once. Used by
    // tests/test_replay_fuzz.cpp to confirm adversarial input never leaves
    // the shadow book in a state where a mask bit and its qty have silently
    // diverged.
    void check_invariants() const {
        for (int p = 0; p < kNumLevels; ++p) {
            const auto pp = static_cast<Price>(p);
            const bool nonzero = qty_[static_cast<std::size_t>(p)] > 0;
            const bool bid_set = bid_mask_.test(pp);
            const bool ask_set = ask_mask_.test(pp);
            assert(!(bid_set && ask_set) && "a single price level can't be both a bid and an ask level");
            assert((bid_set || ask_set) == nonzero && "mask occupancy must agree with qty > 0");
        }
    }
#endif

private:
    std::array<Qty, kNumLevels> qty_{};
    detail::Mask128 bid_mask_{};
    detail::Mask128 ask_mask_{};
};

} // namespace px
