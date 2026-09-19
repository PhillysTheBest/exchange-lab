// px/types.hpp — scalar types shared by the engine, the simulator and the tests.
//
// Design rule: no floating point on the price or matching path. A price is a
// signed 64-bit count of 1/10,000ths of a currency unit (kPriceScale), the same
// fixed-point convention as NASDAQ ITCH's "Price(4)": $1.00 == 10'000. Every
// instrument then declares its own tick size in those units (e.g. 100 == $0.01).
// Integer prices compare, hash and order exactly, and replay bit-for-bit.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace px {

using Price        = std::int64_t;   // fixed-point, kPriceScale units per 1.0
using Qty          = std::uint32_t;  // shares / contracts
using OrderId      = std::uint64_t;  // exchange-unique among live orders
using OwnerId      = std::uint32_t;  // submitting participant (self-trade prevention)
using InstrumentId = std::uint16_t;
using Slot         = std::uint32_t;  // index into a Pool — never a pointer
using SeqNum       = std::uint64_t;  // engine event sequence number
using Tsc          = std::uint64_t;  // raw time-stamp-counter ticks

inline constexpr Price kPriceScale = 10'000;
// Upper bound on any limit price ($1,000,000). Keeps `-price` (the ask-side sort
// key, see sorted_levels.hpp) and every price*qty product far from overflow.
inline constexpr Price kMaxPrice = 1'000'000 * kPriceScale;

inline constexpr Slot kNullSlot = std::numeric_limits<Slot>::max();
// Reserved: the id map uses it as its empty-bucket marker, so no order may carry it.
inline constexpr OrderId kInvalidOrderId = std::numeric_limits<OrderId>::max();
inline constexpr InstrumentId kInvalidInstrument = std::numeric_limits<InstrumentId>::max();

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

} // namespace px
