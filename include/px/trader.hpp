// px/trader.hpp — a trader's account: integer cash plus a flat per-market
// position array, with an O(1) pre-trade risk check.
//
// Simplified risk model, deliberately: cash is checked at order-entry time
// against the order's full notional (price*qty for a buy, (100-price)*qty for a
// sell — the max possible loss on a contract bounded in [0,100]), but is *not*
// escrowed for the lifetime of a resting order. A trader with several large
// resting orders that individually pass the check could in principle have all of
// them fill at once and go cash-negative in aggregate. A production system would
// hold per-order escrow and release it on cancel/fill; that's the natural next
// step, flagged here rather than built, so the risk model stays legible instead
// of accreting a second half-correct margin engine alongside the matching one.
//
// What *is* modeled exactly, and is the invariant the tests check: cash and
// position are zero-sum transfers on every trade — apply_fill moves cash and
// position between exactly the two counterparties, creating or destroying
// neither, independent of the (separate, advisory) pre-trade gate above.
#pragma once

#include "types.hpp"

#include <array>

namespace px {

class Trader {
public:
    explicit Trader(TraderId id, Cash starting_cash) noexcept
        : id_(id), cash_(starting_cash) {}

    [[nodiscard]] TraderId id() const noexcept { return id_; }
    [[nodiscard]] Cash cash() const noexcept { return cash_; }
    [[nodiscard]] Position position(MarketId m) const noexcept { return positions_[m]; }

    // Advisory pre-trade gate — see class comment for what this does and doesn't
    // guarantee. `price` is the order's limit price (or the extreme tick for a
    // translated Market order); the caller is expected to pass that, not the
    // eventual trade price, since the trade price isn't known until matching runs.
    [[nodiscard]] bool can_afford(Side side, Price price, Qty qty) const noexcept {
        const Cash unit_cost = (side == Side::Buy) ? price : (100 - price);
        return cash_ >= unit_cost * static_cast<Cash>(qty);
    }

    // Called once per Trade this trader was a party to (as either taker or
    // maker — the caller passes this trader's own side of that specific trade).
    // Cash and position move by exactly the trade notional; the counterparty's
    // apply_fill call (with the opposite side) moves the exact opposite amount,
    // so summed across both parties cash and position are unchanged.
    void apply_fill(MarketId m, Side side, Price price, Qty qty) noexcept {
        const Cash notional = static_cast<Cash>(price) * static_cast<Cash>(qty);
        if (side == Side::Buy) {
            cash_ -= notional;
            positions_[m] += static_cast<Position>(qty);
        } else {
            cash_ += notional;
            positions_[m] -= static_cast<Position>(qty);
        }
    }

private:
    TraderId id_;
    Cash cash_;
    std::array<Position, kMaxMarkets> positions_{};
};

} // namespace px
