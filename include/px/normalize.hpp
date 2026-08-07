// px/normalize.hpp — the YES/NO duality collapses to one function.
//
// A Kalshi-style contract pays $1 if YES, $0 if NO; the two prices always sum to
// 100 cents. That means "BUY NO @ p" is mathematically identical to "SELL YES @
// (100-p)", and "SELL NO @ p" is identical to "BUY YES @ (100-p)". Rather than
// maintain two books (and synchronize them on every match), every order is
// rewritten to its YES-equivalent right here, at the entry boundary, and the
// matching engine only ever sees one asset, one book, one side of bids and asks.
#pragma once

#include "types.hpp"

#include <utility>

namespace px {

[[nodiscard]] constexpr std::pair<Side, Price> normalize(Contract c, Side side, Price price) noexcept {
    if (c == Contract::Yes) return {side, price};
    return {opposite(side), static_cast<Price>(100 - price)};
}

// The original (contract, side) an order was submitted with, recovered from its
// normalized (YES-relative) side plus the contract it was submitted against.
// Round-trips with normalize(): denormalize(c, normalize(c, side, price).first) == side.
[[nodiscard]] constexpr Side denormalize_side(Contract c, Side normalized_side) noexcept {
    return c == Contract::Yes ? normalized_side : opposite(normalized_side);
}

} // namespace px
