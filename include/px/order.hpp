// px/order.hpp — the resting-order record.
//
// POD, alignas(64): a single cache line, no virtuals, no heap-owning members.
// Orders live inside a Pool<Order, N> slab (see pool.hpp) and are referenced
// everywhere by `Slot` (a u32 index), never by pointer or std::shared_ptr — that's
// what makes cancel O(1) and keeps the price-level FIFO intrusive instead of a
// separate std::list node per order.
#pragma once

#include "types.hpp"

namespace px {

struct alignas(64) Order {
    OrderId  id         = kInvalidOrderId;
    TraderId trader_id  = 0;
    MarketId market_id  = 0;

    Price price     = 0;  // limit price, YES-relative, post-normalization
    Qty   qty       = 0;  // remaining open quantity
    Qty   orig_qty  = 0;  // originally submitted quantity (for fill-ratio reporting)

    Tsc submit_ts = 0;    // TSC at admission — drives time priority and latency stats

    // Intrusive doubly-linked FIFO within a single PriceLevel (see order_book.hpp).
    // Linked by Slot index, not pointer: 4 bytes instead of 8, and the links stay
    // valid even if the backing Pool's storage were ever relocated.
    Slot prev = kNullSlot;
    Slot next = kNullSlot;

    Side        side     = Side::Buy;
    Contract    contract = Contract::Yes; // as originally submitted; reporting only —
                                           // matching always operates post-normalization
    OrderType   type     = OrderType::Limit;
    TimeInForce tif      = TimeInForce::GTC;
    OrderStatus status    = OrderStatus::New;

    [[nodiscard]] Qty filled_qty() const noexcept { return orig_qty - qty; }
};

static_assert(sizeof(Order) == 64, "Order must occupy exactly one cache line");

} // namespace px
