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
    } else if (req.type == ReqType::Cancel) {
        process_cancel(req, sink);
    } else if (req.type == ReqType::Amend) {
        process_amend(req, sink);
    } else {
        process_auction(req, sink);
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

    const bool may_rest = !is_market && req.tif == Tif::GTC;
    // An auction collects orders to match later: one that cannot wait has no place in it.
    if (book.auction && !may_rest) return reject(req, RejectReason::WrongPhase, sink);

    // ---- admission: guarantee a GTC remainder a place to rest ----
    // Matching only ever touches the OPPOSITE side and only ever frees slots, so
    // this own-side lookup and these capacity checks stay valid through the sweep.
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
    match_and_rest(req, book, is_market, may_rest, own_pos, sink);
}

void MatchingEngine::match_and_rest(const OrderRequest& req, Book& book, bool is_market, bool may_rest,
                                    SortedLevels::Position own_pos, EventSink sink) noexcept {
    SortedLevels& own = book.side(req.side);
    const std::int64_t own_key = SortedLevels::key_of(req.side, req.price);

    // ---- match against the opposite side, best level first, FIFO within it ----
    Qty leaves = req.qty;
    SortedLevels& opp = book.side(opposite(req.side));
    const bool matching = !book.auction; // in an auction the order only rests
    while (matching && leaves > 0 && !opp.empty()) {
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

void MatchingEngine::process_amend(const OrderRequest& req, EventSink sink) noexcept {
    const Slot os = ids_.find(req.order_id);
    // As with cancel, somebody else's order looks exactly like an unknown id.
    if (os == kNullSlot || orders_[os].owner != req.owner) return reject(req, RejectReason::UnknownOrder, sink);
    Order& o = orders_[os];
    PriceLevel& lvl = levels_[o.level];
    Book& book = books_[lvl.instr];
    if (req.qty == 0) return reject(req, RejectReason::InvalidQty, sink);
    if (req.price <= 0 || req.price > kMaxPrice || req.price % book.tick != 0) {
        return reject(req, RejectReason::InvalidPrice, sink);
    }

    ExecEvent a;
    a.order_id = o.id;
    a.price = req.price;
    a.qty = req.qty;
    a.leaves = req.qty;
    a.owner = o.owner;
    a.instr = lvl.instr;
    a.type = EventType::Amended;
    a.side = lvl.side;

    if (req.price == lvl.price && req.qty <= o.leaves) {
        // Smaller at the same price: nobody behind it is disadvantaged, so it keeps its place.
        lvl.total_qty -= o.leaves - req.qty;
        o.leaves = req.qty;
        a.seq = next_seq_++;
        a.reason = static_cast<std::uint8_t>(AmendEffect::KeptPriority);
        sink(a);
        return;
    }

    // A new price or more quantity: to the back of the queue. Decide first that
    // it will have somewhere to rest. Its own order slot comes free, and so does
    // its level (and that level's place in the index) if it is alone there.
    const Side side = lvl.side;
    SortedLevels& own = book.side(side);
    const bool frees_level = lvl.count == 1;
    if (req.price != lvl.price && !own.locate(SortedLevels::key_of(side, req.price)).found &&
        !frees_level && (levels_.full() || own.full())) {
        return reject(req, RejectReason::BookFull, sink);
    }

    OrderRequest again = limit_order(o.id, lvl.instr, o.owner, side, req.price, req.qty);
    remove_order(os, /*drop_empty_level=*/true);
    a.seq = next_seq_++;
    a.reason = static_cast<std::uint8_t>(AmendEffect::LostPriority);
    sink(a);
    match_and_rest(again, book, /*is_market=*/false, /*may_rest=*/true,
                   own.locate(SortedLevels::key_of(side, again.price)), sink);
}

// The price at which the most quantity trades. Walks every price that is a level
// on either side, lowest first, carrying the quantity bid at or above it and the
// quantity offered at or below it; no memory beyond the two cursors.
UncrossView MatchingEngine::indicative(InstrumentId id) const noexcept {
    const Book& book = books_[id];
    const std::uint32_t nb = book.bids.size();
    const std::uint32_t na = book.asks.size();
    UncrossView best;
    if (nb == 0 || na == 0) return best;

    std::uint64_t demand = 0; // bids at or above the price being tried
    for (std::uint32_t r = 0; r < nb; ++r) demand += levels_[book.bids.at_rank(r)].total_qty;
    std::uint64_t supply = 0; // asks at or below it
    std::uint64_t best_gap = 0;

    // Both cursors move up in price: bids from their worst rank, asks from their best.
    std::uint32_t bids_left = nb; // bids at ranks [0, bids_left) are still at or above the price
    std::uint32_t asks_in = 0;    // asks at ranks [0, asks_in) are at or below it
    while (bids_left > 0) {       // above the best bid nothing is bid, so nothing trades
        const Price next_bid = levels_[book.bids.at_rank(bids_left - 1)].price;
        const Price p = asks_in < na ? std::min(next_bid, levels_[book.asks.at_rank(asks_in)].price) : next_bid;
        while (asks_in < na && levels_[book.asks.at_rank(asks_in)].price <= p) {
            supply += levels_[book.asks.at_rank(asks_in)].total_qty;
            ++asks_in;
        }
        const std::uint64_t qty = std::min(demand, supply);
        const std::uint64_t gap = demand > supply ? demand - supply : supply - demand;
        if (qty > best.qty || (qty == best.qty && qty > 0 && gap < best_gap)) {
            best.price = p;
            best.qty = qty;
            best_gap = gap;
        }
        // Past this price, the bids resting exactly at it no longer count.
        while (bids_left > 0 && levels_[book.bids.at_rank(bids_left - 1)].price <= p) {
            demand -= levels_[book.bids.at_rank(bids_left - 1)].total_qty;
            --bids_left;
        }
    }
    return best;
}

void MatchingEngine::process_auction(const OrderRequest& req, EventSink sink) noexcept {
    if (req.instr >= books_.size()) return reject(req, RejectReason::UnknownInstrument, sink);
    Book& book = books_[req.instr];
    const bool opening = req.type == ReqType::OpenAuction;
    if (opening == book.auction) return reject(req, RejectReason::WrongPhase, sink);

    ExecEvent ph;
    ph.instr = req.instr;
    ph.type = EventType::Phase;
    if (opening) {
        book.auction = true;
        ph.seq = next_seq_++;
        ph.reason = static_cast<std::uint8_t>(BookPhase::Auction);
        sink(ph);
        return;
    }

    // ---- uncross: the best bid meets the best ask until the quantity is done ----
    const UncrossView u = indicative(req.instr);
    std::uint64_t left = u.qty;
    while (left > 0) {
        const Slot bls = book.bids.best();
        const Slot als = book.asks.best();
        PriceLevel& bl = levels_[bls];
        PriceLevel& al = levels_[als];
        Order& bid = orders_[bl.head];
        Order& ask = orders_[al.head];
        const Qty q = static_cast<Qty>(std::min<std::uint64_t>(left, std::min(bid.leaves, ask.leaves)));
        left -= q;
        bid.leaves -= q;
        ask.leaves -= q;
        bl.total_qty -= q;
        al.total_qty -= q;
        ExecEvent t;
        t.seq = next_seq_++;
        t.order_id = bid.id;
        t.maker_id = ask.id;
        t.price = u.price;
        t.qty = q;
        t.leaves = bid.leaves;
        t.maker_leaves = ask.leaves;
        t.owner = bid.owner;
        t.maker_owner = ask.owner;
        t.instr = req.instr;
        t.type = EventType::Trade;
        t.side = Side::Buy;
        sink(t);
        if (bid.leaves == 0) {
            remove_order(bl.head, /*drop_empty_level=*/false);
            if (bl.count == 0) {
                book.bids.pop_best();
                levels_.release(bls);
            }
        }
        if (ask.leaves == 0) {
            remove_order(al.head, /*drop_empty_level=*/false);
            if (al.count == 0) {
                book.asks.pop_best();
                levels_.release(als);
            }
        }
    }

    book.auction = false;
    ph.seq = next_seq_++;
    ph.price = u.price;
    ph.reason = static_cast<std::uint8_t>(BookPhase::Continuous);
    sink(ph);
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
        if (!book.auction && !book.bids.empty() && !book.asks.empty() &&
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
