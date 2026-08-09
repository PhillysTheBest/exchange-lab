// eval/fees.hpp — Kalshi's real trading-fee formula, integer-cent, no floating point.
//
// SCHEMA STATUS (Stage 6): verified against TWO independent sources, not assumed —
// Kalshi's own published fee schedule (kalshi.com/docs/kalshi-fee-schedule.pdf) and
// the identical formula in Kalshi's CFTC DCM rule filing
// (cftc.gov/sites/default/files/filings/orgrules/22/09/rule091222kexdcm003.pdf), both
// independently found to state:
//
//   taker fee (dollars) = round_up(0.07   * C * P * (1 - P))
//   maker fee (dollars) = round_up(0.0175 * C * P * (1 - P))
//
// where C = contract count, P = price in dollars (0..1). Fee peaks at a 50c price and
// decays toward both tails — a contract priced at 5c or 95c costs much less to trade
// than one priced at 50c, same shape either formula. The 0.0175 maker rate is exactly
// 1/4 of the 0.07 taker rate.
//
// INTEGER-CENT ADAPTATION, stated explicitly rather than silently assumed: this
// project's Cash type is integer cents (px/types.hpp's own "no floating point on any
// price path" rule) — nothing anywhere in this codebase represents a fractional
// cent. Kalshi's own schedule describes rounding fine enough to reach a "centicent"
// (1/100 of a cent) for its own internal accounting; this project does not replicate
// that sub-cent precision, and rounds up to the nearest whole cent instead — a
// deliberately coarser, always-at-least-as-conservative (never under-charges) estimate
// of the real fee, consistent with this project's existing "when a boundary is
// unconfirmed, pick the direction that fails safe" discipline (see kalshi_feed.hpp's
// SEQ SCOPING comment for the same discipline applied elsewhere).
//
// Computed entirely in int64_t: 0.07 = 7/100, 0.0175 = 7/400 — both exact fractions,
// so `rate_num`/`rate_den` below carries the formula with zero rounding error until
// the final ceiling-divide. price_cents in [1,99] and qty is a Qty (uint32), so
// 7 * qty * P * (100-P) is bounded well under int64_t's range even at qty ~ 4 billion
// (P*(100-P) <= 2500), and every real order size in this codebase is orders of
// magnitude smaller than that.
#pragma once

#include "px/types.hpp"

#include <cstdint>

namespace px::eval {

namespace detail {

[[nodiscard]] inline Cash fee_cents(Price price_cents, Qty qty, std::int64_t rate_num,
                                     std::int64_t rate_den) noexcept {
    if (qty == 0) return 0;
    const std::int64_t p = static_cast<std::int64_t>(price_cents);
    const std::int64_t q = static_cast<std::int64_t>(qty);
    // fee_dollars = (rate_num/rate_den) * q * (p/100) * ((100-p)/100)
    // fee_cents   = fee_dollars * 100 = rate_num * q * p * (100-p) / (rate_den * 100)
    const std::int64_t numerator = rate_num * q * p * (100 - p);
    const std::int64_t denominator = rate_den * 100;
    // Ceiling division on non-negative operands (numerator >= 0 always: p in [1,99],
    // q >= 0, rate_num > 0) — round UP to the next whole cent, per this file's own
    // INTEGER-CENT ADAPTATION note above.
    return static_cast<Cash>((numerator + denominator - 1) / denominator);
}

} // namespace detail

// Taker fee — an order that crosses the spread and removes liquidity (a marketable
// IOC/MARKET fill, or a GTC limit that crossed at entry). rate = 7%.
[[nodiscard]] inline Cash taker_fee_cents(Price price_cents, Qty qty) noexcept {
    return detail::fee_cents(price_cents, qty, /*rate_num=*/7, /*rate_den=*/100);
}

// Maker fee — a resting order that provided liquidity and was later hit by someone
// else's marketable order. rate = 1.75% (exactly 1/4 of the taker rate).
[[nodiscard]] inline Cash maker_fee_cents(Price price_cents, Qty qty) noexcept {
    return detail::fee_cents(price_cents, qty, /*rate_num=*/7, /*rate_den=*/400);
}

[[nodiscard]] inline Cash fee_cents_for(bool is_maker, Price price_cents, Qty qty) noexcept {
    return is_maker ? maker_fee_cents(price_cents, qty) : taker_fee_cents(price_cents, qty);
}

} // namespace px::eval
