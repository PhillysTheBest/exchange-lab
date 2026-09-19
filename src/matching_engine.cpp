#include "px/matching_engine.hpp"

#include <algorithm>
#include <cstring>
#include <initializer_list>

namespace px {

namespace {

[[nodiscard]] std::size_t next_pow2(std::size_t v) noexcept {
    std::size_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

// Does a taker on `side` with limit `limit` trade against a level at `resting`?
[[nodiscard]] constexpr bool crosses(Side side, Price limit, Price resting) noexcept {
    return side == Side::Buy ? resting <= limit : resting >= limit;
}

} // namespace

MatchingEngine::MatchingEngine(const EngineConfig& cfg)
    : cfg_(cfg),
      orders_(cfg.max_orders),
      levels_(cfg.max_levels),
      // At most max_orders live ids, so a 2x table never exceeds 50% load.
      ids_(next_pow2(std::size_t{cfg.max_orders} * 2)) {
    books_.reserve(cfg.max_instruments); // Book addresses never move after this
}

InstrumentId MatchingEngine::add_instrument(std::string_view symbol, Price tick_size) {
    if (books_.size() >= cfg_.max_instruments || tick_size <= 0) return kInvalidInstrument;
    books_.emplace_back(cfg_.levels_per_side);
    Book& b = books_.back();
    const std::size_t n = std::min(symbol.size(), sizeof(b.symbol) - 1);
    std::memcpy(b.symbol, symbol.data(), n);
    b.tick = tick_size;
    return static_cast<InstrumentId>(books_.size() - 1);
}

void MatchingEngine::process(const OrderRequest& req, EventSink sink) noexcept {
    if (req.type == ReqType::New) {
        process_new(req, sink);
    } else {
        process_cancel(req, sink);
    }
}

void MatchingEngine::reject(const OrderRequest& req, RejectReason why, EventSink sink) noexcept {
    ExecEvent e;
    e.seq = next_seq_++;
    e.order_id = req.order_id;
    e.price = req.price;
    e.qty = req.qty;
    e.owner = req.owner;
    e.instr = req.instr;
    e.type = EventType::Rejected;
    e.reason = static_cast<std::uint8_t>(why);
    e.side = req.side;
    sink(e);
}

void MatchingEngine::process_new(const OrderRequest& req, EventSink sink) noexcept {
    // ---- validation: every reject happens here, before any state changes ----
    if (req.instr >= books_.size()) return reject(req, RejectReason::UnknownInstrument, sink);
    Book& book = books_[req.instr];
    if (req.qty == 0) return reject(req, RejectReason::InvalidQty, sink);
    const bool is_market = req.ord == OrdType::Market;
    if (!is_market && (req.price <= 0 || req.price > kMaxPrice || req.price % book.tick != 0)) {
        return reject(req, RejectReason::InvalidPrice, sink);
    }
    if (req.order_id == kInvalidOrderId) return reject(req, RejectReason::InvalidOrderId, sink);
    if (ids_.find(req.order_id) != kNullSlot) return reject(req, RejectReason::DuplicateOrderId, sink);

    // ---- admission: guarantee a GTC remainder a place to rest ----
    // Matching only ever touches the OPPOSITE side and only ever frees slots, so
    // this own-side lookup and these capacity checks stay valid through the sweep.
    const bool may_rest = !is_market && req.tif == Tif::GTC;
    SortedLevels& own = book.side(req.side);
    const std::int64_t own_key = SortedLevels::key_of(req.side, req.price);
    SortedLevels::Position own_pos{0, false};
    if (may_rest) {
        if (orders_.full()) return reject(req, RejectReason::BookFull, sink);
        own_pos = own.locate(own_key);
        if (!own_pos.found && (levels_.full() || own.full())) return reject(req, RejectReason::BookFull, sink);
    }

    ExecEvent ack;
    ack.seq = next_seq_++;
    ack.order_id = req.order_id;
    ack.price = is_market ? 0 : req.price;
    ack.qty = req.qty;
    ack.leaves = req.qty;
    ack.owner = req.owner;
    ack.instr = req.instr;
    ack.type = EventType::Ack;
    ack.side = req.side;
    sink(ack);

    // ---- match against the opposite side, best level first, FIFO within it ----
    Qty leaves = req.qty;
    SortedLevels& opp = book.side(opposite(req.side));
    while (leaves > 0 && !opp.empty()) {
        const Slot ls = opp.best();
        PriceLevel& lvl = levels_[ls];
        if (!is_market && !crosses(req.side, req.price, lvl.price)) break;

        Slot os = lvl.head;
        while (os != kNullSlot && leaves > 0) {
            Order& maker = orders_[os];
            const Slot next = maker.next;
            if (maker.owner == req.owner) {
                // Self-trade prevention: the resting order is cancelled, and the
                // taker keeps sweeping behind it.
                ExecEvent c;
                c.seq = next_seq_++;
                c.order_id = maker.id;
                c.price = lvl.price;
                c.qty = maker.leaves;
                c.owner = maker.owner;
                c.instr = req.instr;
                c.type = EventType::Cancelled;
                c.reason = static_cast<std::uint8_t>(CancelReason::SelfTrade);
                c.side = lvl.side;
                sink(c);
                remove_order(os, /*drop_empty_level=*/false);
            } else {
                const Qty q = std::min(leaves, maker.leaves);
                leaves -= q;
                maker.leaves -= q;
                lvl.total_qty -= q;
                ExecEvent t;
                t.seq = next_seq_++;
                t.order_id = req.order_id;
                t.maker_id = maker.id;
                t.price = lvl.price;
                t.qty = q;
                t.leaves = leaves;
                t.maker_leaves = maker.leaves;
                t.owner = req.owner;
                t.maker_owner = maker.owner;
                t.instr = req.instr;
                t.type = EventType::Trade;
                t.side = req.side;
                sink(t);
                if (maker.leaves == 0) remove_order(os, /*drop_empty_level=*/false);
            }
            os = next;
        }

        if (lvl.count != 0) break; // the taker ran out first
        opp.pop_best();
        levels_.release(ls);
    }

    if (leaves == 0) return;

    if (!may_rest) {
        ExecEvent c;
        c.seq = next_seq_++;
        c.order_id = req.order_id;
        c.price = is_market ? 0 : req.price;
        c.qty = leaves;
        c.owner = req.owner;
        c.instr = req.instr;
        c.type = EventType::Cancelled;
        c.reason = static_cast<std::uint8_t>(CancelReason::IocRemainder);
        c.side = req.side;
        sink(c);
        return;
    }

    // ---- rest the GTC remainder at the tail of its level ----
    Slot ls;
    if (own_pos.found) {
        ls = own.slot_at_index(own_pos.index);
    } else {
        ls = levels_.acquire();
        PriceLevel& fresh = levels_[ls];
        fresh = PriceLevel{};
        fresh.price = req.price;
        fresh.instr = req.instr;
        fresh.side = req.side;
        own.insert_at(own_pos.index, own_key, ls);
    }

    const Slot os = orders_.acquire();
    Order& o = orders_[os];
    PriceLevel& lvl = levels_[ls];
    o.id = req.order_id;
    o.leaves = leaves;
    o.owner = req.owner;
    o.level = ls;
    o.next = kNullSlot;
    o.prev = lvl.tail;
    if (lvl.tail != kNullSlot) {
        orders_[lvl.tail].next = os;
    } else {
        lvl.head = os;
    }
    lvl.tail = os;
    ++lvl.count;
    lvl.total_qty += leaves;
    ids_.insert(req.order_id, os);
}

void MatchingEngine::process_cancel(const OrderRequest& req, EventSink sink) noexcept {
    const Slot os = ids_.find(req.order_id);
    // Only the owner may cancel; a foreign cancel looks exactly like an unknown id.
    if (os == kNullSlot || orders_[os].owner != req.owner) return reject(req, RejectReason::UnknownOrder, sink);

    const Order& o = orders_[os];
    const PriceLevel& lvl = levels_[o.level];
    ExecEvent c;
    c.seq = next_seq_++;
    c.order_id = o.id;
    c.price = lvl.price;
    c.qty = o.leaves;
    c.owner = o.owner;
    c.instr = lvl.instr;
    c.type = EventType::Cancelled;
    c.reason = static_cast<std::uint8_t>(CancelReason::User);
    c.side = lvl.side;
    sink(c);
    remove_order(os, /*drop_empty_level=*/true);
}

void MatchingEngine::remove_order(Slot os, bool drop_empty_level) noexcept {
    const Order& o = orders_[os];
    const Slot ls = o.level;
    PriceLevel& lvl = levels_[ls];
    if (o.prev != kNullSlot) {
        orders_[o.prev].next = o.next;
    } else {
        lvl.head = o.next;
    }
    if (o.next != kNullSlot) {
        orders_[o.next].prev = o.prev;
    } else {
        lvl.tail = o.prev;
    }
    --lvl.count;
    lvl.total_qty -= o.leaves;
    ids_.erase(o.id);
    orders_.release(os);

    if (drop_empty_level && lvl.count == 0) {
        SortedLevels& idx = books_[lvl.instr].side(lvl.side);
        const SortedLevels::Position pos = idx.locate(SortedLevels::key_of(lvl.side, lvl.price));
        idx.erase_at(pos.index); // pos.found is guaranteed: the level is live
        levels_.release(ls);
    }
}

std::string_view MatchingEngine::symbol(InstrumentId id) const noexcept {
    return std::string_view(books_[id].symbol);
}

std::uint32_t MatchingEngine::depth(InstrumentId id, Side side) const noexcept {
    return books_[id].side(side).size();
}

LevelView MatchingEngine::level(InstrumentId id, Side side, std::uint32_t rank) const noexcept {
    const PriceLevel& l = levels_[books_[id].side(side).at_rank(rank)];
    return LevelView{l.price, l.total_qty, l.count};
}

bool MatchingEngine::check_invariants(const char** why) const {
    const auto fail = [why](const char* msg) {
        if (why) *why = msg;
        return false;
    };

    std::size_t orders_seen = 0;
    std::size_t levels_seen = 0;
    for (std::size_t b = 0; b < books_.size(); ++b) {
        const Book& book = books_[b];
        for (const Side s : {Side::Buy, Side::Sell}) {
            const SortedLevels& idx = book.side(s);
            for (std::uint32_t i = 0; i < idx.size(); ++i) {
                if (i > 0 && !(idx.key_at_index(i - 1) < idx.key_at_index(i))) {
                    return fail("level keys not strictly ascending");
                }
                const Slot ls = idx.slot_at_index(i);
                if (ls >= levels_.capacity()) return fail("level slot out of range");
                const PriceLevel& l = levels_[ls];
                if (SortedLevels::key_of(s, l.price) != idx.key_at_index(i)) return fail("level key/price mismatch");
                if (l.side != s || l.instr != b) return fail("level side/instrument mismatch");
                if (l.price <= 0 || l.price % book.tick != 0) return fail("level price off the tick grid");
                if (l.count == 0 || l.head == kNullSlot) return fail("empty level left in the index");

                std::uint64_t qty = 0;
                std::uint32_t n = 0;
                Slot prev = kNullSlot;
                for (Slot os = l.head; os != kNullSlot; os = orders_[os].next) {
                    if (os >= orders_.capacity()) return fail("order slot out of range");
                    const Order& o = orders_[os];
                    if (o.prev != prev) return fail("FIFO prev link broken");
                    if (o.level != ls) return fail("order level back-pointer wrong");
                    if (o.leaves == 0) return fail("zero-quantity order resting");
                    if (ids_.find(o.id) != os) return fail("id map disagrees with the book");
                    qty += o.leaves;
                    prev = os;
                    if (++n > l.count) return fail("FIFO longer than its count (cycle?)");
                }
                if (prev != l.tail) return fail("FIFO tail wrong");
                if (n != l.count) return fail("level order count wrong");
                if (qty != l.total_qty) return fail("level total_qty wrong");
                orders_seen += n;
                ++levels_seen;
            }
        }
        if (!book.bids.empty() && !book.asks.empty() &&
            levels_[book.bids.best()].price >= levels_[book.asks.best()].price) {
            return fail("book crossed");
        }
    }
    if (orders_seen != orders_.live()) return fail("order pool live count != orders in books");
    if (orders_seen != ids_.size()) return fail("id map size != orders in books");
    if (levels_seen != levels_.live()) return fail("level pool live count != levels in books");
    return true;
}

} // namespace px
