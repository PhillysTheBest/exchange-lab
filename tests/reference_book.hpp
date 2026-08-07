// tests/reference_book.hpp — deliberately naive book used ONLY as a test oracle.
//
// This is explicitly not the A/B performance twin (that was declined — see the
// plan doc): it never runs in a benchmark, only in fuzz_vs_reference.cpp. It uses
// std::map<Price, std::deque<...>> (tree + node-per-order + heap allocation
// throughout) precisely because that combination is boringly obviously correct —
// insertion order within a std::deque is FIFO, std::map keeps price levels
// sorted, and there is no cleverness anywhere to doubt. If OrderBook (the array +
// bitmask + intrusive-FIFO version) disagrees with this on any trade sequence,
// OrderBook is wrong.
#pragma once

#include "px/normalize.hpp"
#include "px/trade.hpp"
#include "px/types.hpp"

#include <deque>
#include <map>
#include <unordered_map>
#include <utility>

namespace px::test {

class ReferenceBook {
public:
    struct SubmitResult {
        OrderId order_id  = kInvalidOrderId;
        Qty     filled_qty = 0;
        Qty     rested_qty = 0;
        RejectReason reject = RejectReason::None;
    };

    explicit ReferenceBook(MarketId market_id) : market_id_(market_id) {}

    SubmitResult submit(OrderId id, TraderId trader, Contract contract, Side side,
                         OrderType type, TimeInForce tif, Price price, Qty qty,
                         const TradeSink& sink) {
        SubmitResult result;
        result.order_id = id;

        if (qty == 0) {
            result.reject = RejectReason::InvalidQty;
            return result;
        }

        const auto [nside, nprice] = normalize(contract, side, price);

        Price eff_price = nprice;
        TimeInForce eff_tif = tif;
        if (type == OrderType::Market) {
            eff_price = (nside == Side::Buy) ? kMaxPrice : kMinPrice;
            eff_tif = TimeInForce::IOC;
        } else if (!valid_price(nprice)) {
            result.reject = RejectReason::InvalidPrice;
            return result;
        }

        Qty remaining = qty;

        if (nside == Side::Buy) {
            remaining = cross(asks_, remaining, eff_price, /*taker_wants_le=*/true,
                               id, trader, nside, sink, result);
        } else {
            remaining = cross(bids_, remaining, eff_price, /*taker_wants_le=*/false,
                               id, trader, nside, sink, result);
        }

        if (remaining > 0 && eff_tif == TimeInForce::GTC) {
            RefOrder o{id, trader, remaining, eff_price};
            if (nside == Side::Buy) bids_[eff_price].push_back(o);
            else                    asks_[eff_price].push_back(o);
            loc_[id] = {nside, eff_price};
            result.rested_qty = remaining;
        }
        return result;
    }

    [[nodiscard]] RejectReason cancel(OrderId id) {
        auto it = loc_.find(id);
        if (it == loc_.end()) return RejectReason::UnknownOrder;
        const auto [side, price] = it->second;
        if (side == Side::Buy) erase_from(bids_, price, id);
        else                   erase_from(asks_, price, id);
        loc_.erase(it);
        return RejectReason::None;
    }

    [[nodiscard]] bool best_bid(Price& out) const {
        if (bids_.empty()) return false;
        out = bids_.begin()->first;
        return true;
    }
    [[nodiscard]] bool best_ask(Price& out) const {
        if (asks_.empty()) return false;
        out = asks_.begin()->first;
        return true;
    }

private:
    struct RefOrder {
        OrderId id;
        TraderId trader_id;
        Qty qty;
        Price price;
    };

    template <typename Book>
    void erase_from(Book& book, Price price, OrderId id) {
        auto lvl = book.find(price);
        if (lvl == book.end()) return;
        auto& dq = lvl->second;
        for (auto dit = dq.begin(); dit != dq.end(); ++dit) {
            if (dit->id == id) { dq.erase(dit); break; }
        }
        if (dq.empty()) book.erase(lvl);
    }

    // bids_: highest price first. asks_: lowest price first (std::map default).
    template <typename Book>
    Qty cross(Book& opposite, Qty remaining, Price eff_price, bool taker_wants_le,
              OrderId taker_id, TraderId trader, Side taker_side, const TradeSink& sink,
              SubmitResult& result) {
        while (remaining > 0 && !opposite.empty()) {
            auto lvl_it = opposite.begin();
            const Price lvl_price = lvl_it->first;
            const bool crosses = taker_wants_le ? (lvl_price <= eff_price) : (lvl_price >= eff_price);
            if (!crosses) break;

            auto& dq = lvl_it->second;
            while (remaining > 0 && !dq.empty()) {
                RefOrder& maker = dq.front();
                if (maker.trader_id == trader) {
                    loc_.erase(maker.id);
                    dq.pop_front();
                    continue;
                }
                const Qty tq = remaining < maker.qty ? remaining : maker.qty;

                Trade t;
                t.market_id = market_id_;
                t.taker_order_id = taker_id;
                t.maker_order_id = maker.id;
                t.taker_id = trader;
                t.maker_id = maker.trader_id;
                t.price = lvl_price;
                t.qty = tq;
                t.taker_side = taker_side;
                t.ts = 0;
                sink(t);

                remaining -= tq;
                maker.qty -= tq;
                result.filled_qty += tq;
                if (maker.qty == 0) {
                    loc_.erase(maker.id);
                    dq.pop_front();
                }
            }
            if (dq.empty()) opposite.erase(lvl_it);
        }
        return remaining;
    }

    MarketId market_id_;
    std::map<Price, std::deque<RefOrder>, std::greater<Price>> bids_;
    std::map<Price, std::deque<RefOrder>> asks_;
    std::unordered_map<OrderId, std::pair<Side, Price>> loc_;
};

} // namespace px::test
