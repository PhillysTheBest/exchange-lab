// px/messages.hpp — the two structs that cross the lock-free rings.
//
// OrderRequest flows gateway -> matching core; ExecEvent flows matching core ->
// publisher. Both are trivially copyable and exactly one 64-byte cache line, so a
// ring slot never shares a line with its neighbour and a producer writing slot N
// never contends with a consumer reading slot N-1.
//
// EVENT PROTOCOL (what MatchingEngine::process emits, in order):
//   New order   -> Rejected(reason)
//               |  Ack, then for each resting maker in price-time order either
//                  Trade(taker, maker) or Cancelled(maker, SelfTrade), then
//                  Cancelled(taker, IocRemainder) if an IOC/Market order has
//                  quantity left. A GTC remainder rests silently: the `leaves`
//                  field of the order's last event is what rested.
//   Cancel      -> Cancelled(User) | Rejected(UnknownOrder)
// Every event carries the next engine sequence number and no timestamp, so the
// same input stream always produces a bit-identical output stream.
#pragma once

#include "types.hpp"

#include <cstdint>
#include <type_traits>

namespace px {

enum class ReqType : std::uint8_t { New = 0, Cancel = 1 };
enum class OrdType : std::uint8_t { Limit = 0, Market = 1 };
enum class Tif : std::uint8_t { GTC = 0, IOC = 1 }; // Market orders are always IOC

enum class EventType : std::uint8_t { Ack = 0, Trade = 1, Cancelled = 2, Rejected = 3, EndOfStream = 4 };

enum class RejectReason : std::uint8_t {
    None = 0,
    UnknownInstrument,
    InvalidQty,
    InvalidPrice,     // <= 0, above kMaxPrice, or not a multiple of the tick size
    InvalidOrderId,   // kInvalidOrderId is reserved
    DuplicateOrderId, // id already belongs to a live order
    BookFull,         // a GTC order could not be guaranteed a resting slot
    UnknownOrder,     // cancel of an id that is not live (or not yours)
};

enum class CancelReason : std::uint8_t { None = 0, User, IocRemainder, SelfTrade };

struct alignas(64) OrderRequest {
    OrderId order_id = 0;  // New: the new order's id. Cancel: the order to cancel.
    Price price = 0;       // Limit only
    Qty qty = 0;           // New only
    OwnerId owner = 0;
    InstrumentId instr = 0;
    ReqType type = ReqType::New;
    OrdType ord = OrdType::Limit;
    Tif tif = Tif::GTC;
    Side side = Side::Buy;
};
static_assert(sizeof(OrderRequest) == 64);
static_assert(std::is_trivially_copyable_v<OrderRequest>);

struct alignas(64) ExecEvent {
    SeqNum seq = 0;
    OrderId order_id = 0;     // Trade: the taker. Otherwise: the order the event is about.
    OrderId maker_id = 0;     // Trade only
    Price price = 0;          // Trade: execution price. Cancelled: the order's price.
    Qty qty = 0;              // Trade: filled qty. Ack: order qty. Cancelled: qty removed.
    Qty leaves = 0;           // order_id's remaining qty after this event
    Qty maker_leaves = 0;     // Trade only
    OwnerId owner = 0;
    OwnerId maker_owner = 0;  // Trade only
    InstrumentId instr = 0;
    EventType type = EventType::Ack;
    std::uint8_t reason = 0;  // RejectReason or CancelReason, per `type`
    Side side = Side::Buy;    // side of order_id
};
static_assert(sizeof(ExecEvent) == 64);
static_assert(std::is_trivially_copyable_v<ExecEvent>);

// ---- request builders (tests, tools, the workload generator) ----------------

[[nodiscard]] constexpr OrderRequest limit_order(OrderId id, InstrumentId instr, OwnerId owner, Side side,
                                                 Price price, Qty qty, Tif tif = Tif::GTC) noexcept {
    OrderRequest r;
    r.order_id = id;
    r.price = price;
    r.qty = qty;
    r.owner = owner;
    r.instr = instr;
    r.type = ReqType::New;
    r.ord = OrdType::Limit;
    r.tif = tif;
    r.side = side;
    return r;
}

[[nodiscard]] constexpr OrderRequest market_order(OrderId id, InstrumentId instr, OwnerId owner, Side side,
                                                  Qty qty) noexcept {
    OrderRequest r = limit_order(id, instr, owner, side, 0, qty, Tif::IOC);
    r.ord = OrdType::Market;
    return r;
}

[[nodiscard]] constexpr OrderRequest cancel_order(OrderId id, InstrumentId instr, OwnerId owner) noexcept {
    OrderRequest r;
    r.order_id = id;
    r.owner = owner;
    r.instr = instr;
    r.type = ReqType::Cancel;
    return r;
}

// ---- event hashing ----------------------------------------------------------
//
// FNV-1a over each field's value, not the struct's raw bytes, so padding can
// never leak into the hash. Used by the determinism test and the benchmark to
// prove that two runs (or a direct run and a threaded run) emitted the same
// event stream.
class EventHasher {
public:
    void add(const ExecEvent& e) noexcept {
        mix(e.seq);
        mix(e.order_id);
        mix(e.maker_id);
        mix(static_cast<std::uint64_t>(e.price));
        mix(e.qty);
        mix(e.leaves);
        mix(e.maker_leaves);
        mix(e.owner);
        mix(e.maker_owner);
        mix(e.instr);
        mix(static_cast<std::uint64_t>(e.type));
        mix(e.reason);
        mix(static_cast<std::uint64_t>(e.side));
        ++count_;
    }
    [[nodiscard]] std::uint64_t value() const noexcept { return h_; }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

private:
    void mix(std::uint64_t v) noexcept {
        for (int i = 0; i < 8; ++i) {
            h_ ^= (v >> (i * 8)) & 0xFF;
            h_ *= 0x100000001b3ULL;
        }
    }
    std::uint64_t h_ = 0xcbf29ce484222325ULL;
    std::uint64_t count_ = 0;
};

} // namespace px
