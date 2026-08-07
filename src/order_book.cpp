#include "px/order_book.hpp"

#include <algorithm>

#ifdef PX_DEBUG
#include <cassert>
#endif

namespace px {

void OrderBook::insert_into_level(Slot s, Price p, Side side) noexcept {
    PriceLevel& lvl = levels_[static_cast<std::size_t>(p)];
    const bool was_empty = (lvl.tail == kNullSlot);

    Order& o = pool_[s];
    o.prev = lvl.tail;
    o.next = kNullSlot;
    if (lvl.tail != kNullSlot) pool_[lvl.tail].next = s;
    else                       lvl.head = s;
    lvl.tail = s;
    lvl.total_qty += o.qty;

    if (was_empty) {
        if (side == Side::Buy) bid_mask_.set(p);
        else                   ask_mask_.set(p);
    }
}

void OrderBook::unlink_from_level(Slot s) noexcept {
    Order& o = pool_[s];
    const Price p = o.price;
    PriceLevel& lvl = levels_[static_cast<std::size_t>(p)];

    if (o.prev != kNullSlot) pool_[o.prev].next = o.next;
    else                     lvl.head = o.next;
    if (o.next != kNullSlot) pool_[o.next].prev = o.prev;
    else                     lvl.tail = o.prev;
    o.prev = kNullSlot;
    o.next = kNullSlot;

    if (lvl.head == kNullSlot) {
        if (o.side == Side::Buy) bid_mask_.clear(p);
        else                     ask_mask_.clear(p);
    }
}

bool OrderBook::best_bid(Price& out) const noexcept {
    const int hi = bid_mask_.highest_set();
    if (hi < 0) return false;
    out = static_cast<Price>(hi);
    return true;
}

bool OrderBook::best_ask(Price& out) const noexcept {
    const int lo = ask_mask_.lowest_set();
    if (lo < 0) return false;
    out = static_cast<Price>(lo);
    return true;
}

const Order* OrderBook::find_order(OrderId id) const noexcept {
    const Slot s = id_map_.find(id);
    if (s == kNullSlot) return nullptr;
    return &pool_[s];
}

void OrderBook::shadow_set_level(Side side, Price price, Qty new_total_qty) noexcept {
    PriceLevel& lvl = levels_[static_cast<std::size_t>(price)];
    lvl.total_qty = new_total_qty;
    detail::Mask128& mask = (side == Side::Buy) ? bid_mask_ : ask_mask_;
    if (new_total_qty > 0) mask.set(price);
    else                   mask.clear(price);
}

OrderBook::SubmitResult OrderBook::submit(OrderId id, TraderId trader, Contract contract,
                                           Side side, OrderType type, TimeInForce tif,
                                           Price price, Qty qty, Tsc submit_ts,
                                           const TradeSink& sink) noexcept {
    SubmitResult result;
    result.order_id = id;

    if (qty == 0) {
        result.reject = RejectReason::InvalidQty;
        return result;
    }

    // Collapse YES/NO to a single asset before anything else touches side/price.
    const auto [norm_side, norm_price] = normalize(contract, side, price);

    // MARKET == LIMIT at the extreme tick, forced IOC — one matching path below
    // serves both order types.
    Price effective_price = norm_price;
    TimeInForce effective_tif = tif;
    if (type == OrderType::Market) {
        effective_price = (norm_side == Side::Buy) ? kMaxPrice : kMinPrice;
        effective_tif = TimeInForce::IOC;
    } else if (!valid_price(norm_price)) {
        result.reject = RejectReason::InvalidPrice;
        return result;
    }

    Qty remaining = qty;

    // --- sweep the opposite side while the incoming order crosses it ----------
    for (;;) {
        if (remaining == 0) break;

        Price best;
        bool crosses;
        if (norm_side == Side::Buy) {
            crosses = best_ask(best) && best <= effective_price;
        } else {
            crosses = best_bid(best) && best >= effective_price;
        }
        if (!crosses) break;

        PriceLevel& lvl = levels_[static_cast<std::size_t>(best)];
        while (remaining > 0 && lvl.head != kNullSlot) {
            const Slot maker_slot = lvl.head;
            Order& maker = pool_[maker_slot];

            // Self-trade prevention: cancel the resting order, keep sweeping.
            if (maker.trader_id == trader) {
                lvl.total_qty -= maker.qty;
                const OrderId maker_id = maker.id;
                unlink_from_level(maker_slot);
                maker.status = OrderStatus::Cancelled;
                id_map_.erase(maker_id);
                pool_.release(maker_slot);
                continue;
            }

            const Qty trade_qty = std::min(remaining, maker.qty);

            Trade t;
            t.market_id = market_id_;
            t.taker_order_id = id;
            t.maker_order_id = maker.id;
            t.taker_id = trader;
            t.maker_id = maker.trader_id;
            t.price = best; // resting (maker) order's price — standard price improvement
            t.qty = trade_qty;
            t.taker_side = norm_side;
            t.ts = submit_ts;
            sink(t);

            remaining -= trade_qty;
            maker.qty -= trade_qty;
            lvl.total_qty -= trade_qty;
            result.filled_qty += trade_qty;

            if (maker.qty == 0) {
                maker.status = OrderStatus::Filled;
                const OrderId maker_id = maker.id;
                unlink_from_level(maker_slot);
                id_map_.erase(maker_id);
                pool_.release(maker_slot);
            } else {
                maker.status = OrderStatus::PartiallyFilled;
            }
        }
    }

    // --- rest any remainder (GTC only; IOC/Market never rest) -----------------
    if (remaining > 0 && effective_tif == TimeInForce::GTC) {
        const Slot s = pool_.acquire();
        if (s == kNullSlot) {
            result.reject = RejectReason::PoolExhausted;
            return result;
        }
        Order& o = pool_[s];
        o = Order{};
        o.id = id;
        o.trader_id = trader;
        o.market_id = market_id_;
        o.price = effective_price;
        o.qty = remaining;
        o.orig_qty = qty;
        o.submit_ts = submit_ts;
        o.side = norm_side;
        o.contract = contract;
        o.type = type;
        o.tif = tif;
        o.status = (remaining == qty) ? OrderStatus::New : OrderStatus::PartiallyFilled;

        if (!id_map_.insert(id, s)) {
            // Duplicate order id — release the slot we just took and reject.
            pool_.release(s);
            result.reject = RejectReason::DuplicateOrderId;
            return result;
        }
        insert_into_level(s, effective_price, norm_side);
        result.rested_qty = remaining;
    }

    return result;
}

RejectReason OrderBook::cancel(OrderId id) noexcept {
    const Slot s = id_map_.find(id);
    if (s == kNullSlot) return RejectReason::UnknownOrder;

    Order& o = pool_[s];
    levels_[static_cast<std::size_t>(o.price)].total_qty -= o.qty;
    unlink_from_level(s);
    o.status = OrderStatus::Cancelled;
    id_map_.erase(id);
    pool_.release(s);
    return RejectReason::None;
}

#ifdef PX_DEBUG
void OrderBook::check_invariants() const {
    Price bb, ba;
    const bool have_bb = best_bid(bb);
    const bool have_ba = best_ask(ba);
    if (have_bb && have_ba) assert(bb < ba && "book is crossed");

    for (int p = 0; p < kNumLevels; ++p) {
        const PriceLevel& lvl = levels_[static_cast<std::size_t>(p)];
        assert(bid_mask_.test(static_cast<Price>(p)) == (lvl.head != kNullSlot && pool_[lvl.head].side == Side::Buy));
        assert(ask_mask_.test(static_cast<Price>(p)) == (lvl.head != kNullSlot && pool_[lvl.head].side == Side::Sell));

        Qty sum = 0;
        Slot cur = lvl.head;
        Slot prev = kNullSlot;
        while (cur != kNullSlot) {
            const Order& o = pool_[cur];
            assert(o.price == p);
            assert(o.prev == prev);
            sum += o.qty;
            prev = cur;
            cur = o.next;
        }
        assert(prev == lvl.tail);
        assert(sum == lvl.total_qty);
    }
}
#endif

} // namespace px
