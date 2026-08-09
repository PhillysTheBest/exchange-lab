// telemetry/position_account.hpp — Stage 6, Part G: incremental average-entry-price
// and realized-P&L tracking per market, VWAP-style.
//
// Not reused from px/trader.hpp: Trader deliberately tracks only aggregate cash +
// position (exactly what the offline matching engine's zero-sum risk check needs —
// see its own header comment), with no per-position cost-basis concept at all.
// Telemetry's Position panel explicitly asks for "Average Entry" and "Unrealised
// P&L" per market, neither of which is derivable from cash+position alone once a
// position has been partially closed and reopened — this is the smallest additional
// piece of state that makes those two fields real numbers instead of placeholders.
//
// Standard incremental VWAP cost-basis algorithm, the same one any position-keeping
// system needs: cost_basis carries the same sign as position (positive cost_basis
// paired with a long position means "cash spent to build it"; a short position's
// cost_basis is the mirror — negative, so avg_entry_cents() = cost_basis / position
// comes out positive either way). A fill that EXTENDS the current position (or opens
// one from flat) adds to cost_basis proportionally; a fill that REDUCES it realizes
// P&L on the closed portion and shrinks cost_basis by the same fraction; a fill that
// overshoots through zero closes the old position fully and opens a fresh one in the
// new direction, with a cost_basis that reflects only the new fills — not an
// approximation, the mathematically correct treatment (the old position no longer
// exists to have a blended cost basis with).
#pragma once

#include "px/types.hpp"

#include <algorithm>
#include <cstdint>

namespace px::telemetry {

class PositionAccount {
public:
    void apply_fill(Side side, Price price, Qty qty) noexcept {
        const auto signed_qty = (side == Side::Buy) ? static_cast<Position>(qty) : -static_cast<Position>(qty);
        if (qty == 0) return;

        const bool opening_or_extending =
            position_ == 0 || (position_ > 0) == (signed_qty > 0);

        if (opening_or_extending) {
            cost_basis_cents_ += static_cast<Cash>(price) * static_cast<Cash>(signed_qty);
            position_ += signed_qty;
            return;
        }

        // Reducing (partially or fully) and/or flipping through zero.
        const Position closing_qty = std::min(static_cast<Position>(qty), position_ < 0 ? -position_ : position_);
        const Price avg_entry = avg_entry_cents();
        const Position pos_sign = (position_ > 0) ? 1 : -1;
        // Realized P&L on the closed portion: for a long, profit if fill price >
        // avg entry; for a short, profit if fill price < avg entry -- both captured
        // by (fill_price - avg_entry) * pos_sign * closing_qty.
        realized_pnl_cents_ += (static_cast<Cash>(price) - static_cast<Cash>(avg_entry)) *
                                static_cast<Cash>(pos_sign) * static_cast<Cash>(closing_qty);

        // Shrink cost basis by the same fraction of the position that just closed.
        if (position_ != 0) {
            cost_basis_cents_ -= (cost_basis_cents_ * closing_qty) / (position_ < 0 ? -position_ : position_);
        }
        position_ -= pos_sign * closing_qty;

        const Qty overshoot = qty - static_cast<Qty>(closing_qty);
        if (overshoot > 0) {
            // Flipped through zero -- old position is fully closed (cost_basis_/
            // position_ are both ~0 at this point, modulo integer rounding), open a
            // fresh position in the new direction from the overshoot quantity alone.
            const auto flip_signed_qty = (side == Side::Buy) ? static_cast<Position>(overshoot)
                                                               : -static_cast<Position>(overshoot);
            position_ = flip_signed_qty;
            cost_basis_cents_ = static_cast<Cash>(price) * static_cast<Cash>(flip_signed_qty);
        }
    }

    [[nodiscard]] Position position() const noexcept { return position_; }
    [[nodiscard]] Cash realized_pnl_cents() const noexcept { return realized_pnl_cents_; }
    [[nodiscard]] Price avg_entry_cents() const noexcept {
        return position_ == 0 ? 0 : static_cast<Price>(cost_basis_cents_ / position_);
    }
    [[nodiscard]] Cash unrealized_pnl_cents(Price mark_price) const noexcept {
        if (position_ == 0) return 0;
        return (static_cast<Cash>(mark_price) - static_cast<Cash>(avg_entry_cents())) * position_;
    }

private:
    Position position_ = 0;
    Cash cost_basis_cents_ = 0;
    Cash realized_pnl_cents_ = 0;
};

} // namespace px::telemetry
