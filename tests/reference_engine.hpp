// tests/reference_engine.hpp — the test oracle: a deliberately naive matching
// engine built from std::map, std::deque and std::unordered_map, written to be
// obviously correct rather than fast. It implements the same event protocol
// (px/messages.hpp) and the same admission rule (px/matching_engine.hpp),
// including BookFull under the same capacities, so the real engine's output can
// be compared against it event by event. Never used outside tests.
#pragma once

#include "px/matching_engine.hpp"
#include "px/messages.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <unordered_map>
#include <vector>

namespace px::test {

class ReferenceEngine {
public:
    explicit ReferenceEngine(const EngineConfig& cfg) : cfg_(cfg) {}

    void add_instrument(Price tick) { books_.push_back(Book{tick, {}, {}}); }

    std::vector<ExecEvent> process(const OrderRequest& req) {
        out_.clear();
        if (req.type == ReqType::New) {
            process_new(req);
        } else {
            process_cancel(req);
        }
        return out_;
    }

    struct Top {
        bool present = false;
        Price price = 0;
        std::uint64_t qty = 0;
        std::uint32_t orders = 0;
    };

    [[nodiscard]] std::size_t depth(InstrumentId i, Side s) const {
        return s == Side::Buy ? books_[i].bids.size() : books_[i].asks.size();
    }

    // rank 0 == best
    [[nodiscard]] Top level(InstrumentId i, Side s, std::size_t rank) const {
        if (s == Side::Buy) return level_of(books_[i].bids, rank);
        return level_of(books_[i].asks, rank);
    }

    [[nodiscard]] std::size_t resting_orders() const { return live_.size(); }

    // The whole side, best level first, in one pass (level() is O(rank) per call).
    void ladder(InstrumentId i, Side s, std::vector<Top>& out) const {
        out.clear();
        const auto fill = [&out](const auto& m) {
            for (const auto& [px, q] : m) {
                Top t;
                t.present = true;
                t.price = px;
                for (const Resting& r : q) t.qty += r.leaves;
                t.orders = static_cast<std::uint32_t>(q.size());
                out.push_back(t);
            }
        };
        if (s == Side::Buy) {
            fill(books_[i].bids);
        } else {
            fill(books_[i].asks);
        }
    }

    // Where a live order rests: its level's rank from the best (0 == best), the
    // side's depth, and how many orders share its level. False if not live.
    bool locate(OrderId id, std::size_t& rank, std::size_t& depth, std::size_t& level_orders) const {
        const auto it = live_.find(id);
        if (it == live_.end()) return false;
        const Where w = it->second;
        const auto find_in = [&](const auto& m) {
            const auto lvl = m.find(w.price);
            rank = static_cast<std::size_t>(std::distance(m.begin(), lvl));
            depth = m.size();
            level_orders = lvl->second.size();
        };
        if (w.side == Side::Buy) {
            find_in(books_[w.instr].bids);
        } else {
            find_in(books_[w.instr].asks);
        }
        return true;
    }

    // True if a GTC limit at `price` would open a new level behind every existing
    // level on its side (the far-end insert, the most expensive index shift).
    [[nodiscard]] bool is_far_end_price(InstrumentId i, Side s, Price price, std::size_t& depth) const {
        if (i >= books_.size()) return false;
        if (s == Side::Buy) {
            const auto& m = books_[i].bids;
            depth = m.size();
            return !m.empty() && price < m.rbegin()->first;
        }
        const auto& m = books_[i].asks;
        depth = m.size();
        return !m.empty() && price > m.rbegin()->first;
    }

private:
    struct Resting {
        OrderId id;
        Qty leaves;
        OwnerId owner;
    };
    using Queue = std::deque<Resting>;
    struct Book {
        Price tick;
        std::map<Price, Queue, std::greater<Price>> bids; // best (highest) first
        std::map<Price, Queue> asks;                      // best (lowest) first
    };
    struct Where {
        InstrumentId instr;
        Side side;
        Price price;
    };

    template <typename Map>
    static Top level_of(const Map& m, std::size_t rank) {
        Top t;
        if (rank >= m.size()) return t;
        auto it = m.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(rank));
        t.present = true;
        t.price = it->first;
        for (const Resting& r : it->second) t.qty += r.leaves;
        t.orders = static_cast<std::uint32_t>(it->second.size());
        return t;
    }

    ExecEvent next(EventType type) {
        ExecEvent e;
        e.seq = seq_++;
        e.type = type;
        return e;
    }

    void reject(const OrderRequest& req, RejectReason why) {
        ExecEvent e = next(EventType::Rejected);
        e.order_id = req.order_id;
        e.price = req.price;
        e.qty = req.qty;
        e.owner = req.owner;
        e.instr = req.instr;
        e.reason = static_cast<std::uint8_t>(why);
        e.side = req.side;
        out_.push_back(e);
    }

    [[nodiscard]] std::size_t total_levels() const {
        std::size_t n = 0;
        for (const Book& b : books_) n += b.bids.size() + b.asks.size();
        return n;
    }

    // Sweeps `opp` (the side opposite the taker). Returns the taker's leaves.
    template <typename Map>
    Qty sweep(Map& opp, const OrderRequest& req, bool is_market) {
        Qty leaves = req.qty;
        while (leaves > 0 && !opp.empty()) {
            auto lvl = opp.begin();
            const Price px = lvl->first;
            const bool crosses = req.side == Side::Buy ? px <= req.price : px >= req.price;
            if (!is_market && !crosses) break;
            Queue& q = lvl->second;
            while (!q.empty() && leaves > 0) {
                Resting& m = q.front();
                if (m.owner == req.owner) {
                    ExecEvent c = next(EventType::Cancelled);
                    c.order_id = m.id;
                    c.price = px;
                    c.qty = m.leaves;
                    c.owner = m.owner;
                    c.instr = req.instr;
                    c.reason = static_cast<std::uint8_t>(CancelReason::SelfTrade);
                    c.side = opposite(req.side);
                    out_.push_back(c);
                    live_.erase(m.id);
                    q.pop_front();
                    continue;
                }
                const Qty fill = std::min(leaves, m.leaves);
                leaves -= fill;
                m.leaves -= fill;
                ExecEvent t = next(EventType::Trade);
                t.order_id = req.order_id;
                t.maker_id = m.id;
                t.price = px;
                t.qty = fill;
                t.leaves = leaves;
                t.maker_leaves = m.leaves;
                t.owner = req.owner;
                t.maker_owner = m.owner;
                t.instr = req.instr;
                t.side = req.side;
                out_.push_back(t);
                if (m.leaves == 0) {
                    live_.erase(m.id);
                    q.pop_front();
                }
            }
            if (!q.empty()) break;
            opp.erase(lvl);
        }
        return leaves;
    }

    void process_new(const OrderRequest& req) {
        if (req.instr >= books_.size()) return reject(req, RejectReason::UnknownInstrument);
        Book& book = books_[req.instr];
        if (req.qty == 0) return reject(req, RejectReason::InvalidQty);
        const bool is_market = req.ord == OrdType::Market;
        if (!is_market && (req.price <= 0 || req.price > kMaxPrice || req.price % book.tick != 0)) {
            return reject(req, RejectReason::InvalidPrice);
        }
        if (req.order_id == kInvalidOrderId) return reject(req, RejectReason::InvalidOrderId);
        if (live_.count(req.order_id) != 0) return reject(req, RejectReason::DuplicateOrderId);

        const bool may_rest = !is_market && req.tif == Tif::GTC;
        if (may_rest) {
            if (live_.size() >= cfg_.max_orders) return reject(req, RejectReason::BookFull);
            const bool level_exists = req.side == Side::Buy ? book.bids.count(req.price) != 0
                                                            : book.asks.count(req.price) != 0;
            const std::size_t own_levels = req.side == Side::Buy ? book.bids.size() : book.asks.size();
            if (!level_exists && (total_levels() >= cfg_.max_levels || own_levels >= cfg_.levels_per_side)) {
                return reject(req, RejectReason::BookFull);
            }
        }

        ExecEvent ack = next(EventType::Ack);
        ack.order_id = req.order_id;
        ack.price = is_market ? 0 : req.price;
        ack.qty = req.qty;
        ack.leaves = req.qty;
        ack.owner = req.owner;
        ack.instr = req.instr;
        ack.side = req.side;
        out_.push_back(ack);

        const Qty leaves = req.side == Side::Buy ? sweep(book.asks, req, is_market) : sweep(book.bids, req, is_market);
        if (leaves == 0) return;
        if (!may_rest) {
            ExecEvent c = next(EventType::Cancelled);
            c.order_id = req.order_id;
            c.price = is_market ? 0 : req.price;
            c.qty = leaves;
            c.owner = req.owner;
            c.instr = req.instr;
            c.reason = static_cast<std::uint8_t>(CancelReason::IocRemainder);
            c.side = req.side;
            out_.push_back(c);
            return;
        }
        Queue& q = req.side == Side::Buy ? book.bids[req.price] : book.asks[req.price];
        q.push_back(Resting{req.order_id, leaves, req.owner});
        live_[req.order_id] = Where{req.instr, req.side, req.price};
    }

    void process_cancel(const OrderRequest& req) {
        const auto it = live_.find(req.order_id);
        if (it == live_.end()) return reject(req, RejectReason::UnknownOrder);
        const Where w = it->second;
        Book& book = books_[w.instr];
        auto erase_from = [&](auto& side_map) -> bool {
            auto lvl = side_map.find(w.price);
            Queue& q = lvl->second;
            for (auto o = q.begin(); o != q.end(); ++o) {
                if (o->id != req.order_id) continue;
                if (o->owner != req.owner) return false;
                ExecEvent c = next(EventType::Cancelled);
                c.order_id = o->id;
                c.price = w.price;
                c.qty = o->leaves;
                c.owner = o->owner;
                c.instr = w.instr;
                c.reason = static_cast<std::uint8_t>(CancelReason::User);
                c.side = w.side;
                out_.push_back(c);
                q.erase(o);
                if (q.empty()) side_map.erase(lvl);
                return true;
            }
            return false;
        };
        const bool done = w.side == Side::Buy ? erase_from(book.bids) : erase_from(book.asks);
        if (!done) return reject(req, RejectReason::UnknownOrder);
        live_.erase(it);
    }

    EngineConfig cfg_;
    std::vector<Book> books_;
    std::unordered_map<OrderId, Where> live_;
    std::vector<ExecEvent> out_;
    SeqNum seq_ = 1;
};

} // namespace px::test
