// px/order.hpp — the two records the book is built from.
//
// A resting Order is 32 bytes (two per cache line) because everything it shares
// with the other orders at its price — the price itself, side and instrument —
// lives once on its PriceLevel instead of being copied onto every order. Orders
// at a level form an intrusive doubly-linked FIFO by Slot index (4 bytes, not an
// 8-byte pointer), so time priority is list order and cancel is O(1) unlinking.
#pragma once

#include "types.hpp"

#include <cstdint>
#include <type_traits>

namespace px {

struct alignas(32) Order {
    OrderId id = kInvalidOrderId;
    Qty leaves = 0;
    OwnerId owner = 0;
    Slot prev = kNullSlot;  // toward the head (older)
    Slot next = kNullSlot;  // toward the tail (newer)
    Slot level = kNullSlot; // back-pointer to the PriceLevel this order rests on
    std::uint32_t pad = 0;
};
static_assert(sizeof(Order) == 32);
static_assert(std::is_trivially_copyable_v<Order>);

struct alignas(32) PriceLevel {
    Price price = 0;
    std::uint64_t total_qty = 0; // sum of `leaves` over the FIFO
    Slot head = kNullSlot;       // oldest order: first to fill
    Slot tail = kNullSlot;
    std::uint32_t count = 0;     // orders in the FIFO
    InstrumentId instr = 0;
    Side side = Side::Buy;
    std::uint8_t pad = 0;
};
static_assert(sizeof(PriceLevel) == 32);
static_assert(std::is_trivially_copyable_v<PriceLevel>);

} // namespace px
