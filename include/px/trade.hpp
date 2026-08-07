// px/trade.hpp — an execution record. POD, alignas(64), emitted by MatchingEngine
// on every match. Carries a TSC timestamp so end-to-end (submit -> trade) latency
// can be measured from the records themselves, not just from a benchmark harness.
#pragma once

#include "types.hpp"

namespace px {

struct alignas(64) Trade {
    SeqNum seq = 0;         // monotonically increasing, per-engine

    MarketId market_id = 0;
    OrderId  taker_order_id = kInvalidOrderId; // the incoming order that crossed the book
    OrderId  maker_order_id = kInvalidOrderId; // the resting order it matched against
    TraderId taker_id = 0;
    TraderId maker_id = 0;

    Price price = 0; // always the resting (maker) order's price — standard price improvement rule
    Qty   qty   = 0;

    Side taker_side = Side::Buy; // side of the aggressor
    Tsc  ts = 0;
};

static_assert(sizeof(Trade) == 64, "Trade must occupy exactly one cache line");

// Non-owning callback the matching path uses to emit trades as they happen,
// instead of writing into a caller-sized buffer (which would need sizing for a
// worst-case sweep depth) or a virtual Observer interface (vtable indirection).
// A function pointer + context is one predictable indirect call per trade, zero
// allocation, and lets the caller do whatever it wants with each Trade — push it
// onto an SPSC ring, append to a journal, feed a test oracle — without OrderBook
// needing to know which.
class TradeSink {
public:
    using Fn = void (*)(void* ctx, const Trade&);

    constexpr TradeSink(void* ctx, Fn fn) noexcept : ctx_(ctx), fn_(fn) {}

    void operator()(const Trade& t) const noexcept { fn_(ctx_, t); }

private:
    void* ctx_;
    Fn fn_;
};

} // namespace px
