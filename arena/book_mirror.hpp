// arena/book_mirror.hpp — a participant's own view of the book, rebuilt from the
// event stream alone.
//
// The engine never tells a participant where its order stands in the queue, and
// nothing here asks it to: MatchingEngine's public surface is unchanged. Every
// field below is derived from the ExecEvent stream, which is the only thing a
// real venue sends out, so a bot built on this provably sees no more than a real
// participant would — and queue position is earned by tracking the feed rather
// than handed over by the exchange.
//
// RECONSTRUCTION RULES (the event protocol itself is in px/messages.hpp)
//   Ack        the order enters the book at its own limit price, at the tail of
//              that level's FIFO, with `qty` outstanding. For a crossing taker
//              this momentarily shows a crossed book; the Trades that follow in
//              the same request clear it. Readers only ever observe the mirror
//              between requests (see the timestamp invariant below), so that
//              transient is never visible.
//   Trade      `qty` leaves both the taker (order_id) and the maker (maker_id).
//              The execution price is the maker's, which is why quantity is
//              removed per order id and never per price.
//   Cancelled  `qty` leaves order_id, whatever the reason: a user cancel, an
//              IOC/market remainder, or a resting order killed by self-trade
//              prevention. All three remove liquidity, and a bot must handle the
//              third or its own quotes will appear to vanish without a fill.
//   Rejected   nothing happened. Admission is decided before matching, so a
//              rejected order never acked and never entered the book.
//
// TIMESTAMP INVARIANT. Every event one process() call emits carries a single
// session timestamp, so advancing a mirror "up to time t" can never stop in the
// middle of a request. Session::advance_mirror depends on that, and test_arena
// checks the whole reconstructed ladder against the engine's own after every
// request of a million-order stream.
//
// Sort key: -price for bids, price for asks, so begin() is the best level on
// either side. (SortedLevels uses the mirror image of that convention, best at
// the back, because it is a flat array; here the map's natural order is enough.)
//
// This is arena code, not engine code: it allocates, and that is allowed. The
// zero-allocation guarantee belongs to MatchingEngine, which this never touches.
#pragma once

#include "px/matching_engine.hpp"
#include "px/messages.hpp"
#include "px/types.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <unordered_map>
#include <vector>

namespace px::arena {

class BookMirror {
public:
    // One live order, as a participant is able to know it.
    struct OrderRef {
        OrderId id = 0;
        Side side = Side::Buy;
        Price price = 0;
        Qty leaves = 0;
        // Quantity resting ahead of this order at its own price: time priority
        // made legible. This is what a maker is really competing for.
        std::uint64_t qty_ahead = 0;
    };

    explicit BookMirror(InstrumentId instr = 0) noexcept : instr_(instr) {}

    void reset() noexcept {
        live_.clear();
        side_[0].clear();
        side_[1].clear();
        last_trade_ = 0;
        total_qty_ = 0;
        signed_volume_ = 0;
        trades_seen_ = 0;
    }

    // Applies one event, in engine sequence order. Events for other instruments
    // are ignored, so a mirror may be fed the whole exchange's stream.
    void apply(const ExecEvent& e) {
        if (e.instr != instr_) return;
        switch (e.type) {
            case EventType::Ack:
                insert(e.order_id, e.side, e.price, e.qty, e.owner);
                break;
            case EventType::Trade:
                last_trade_ = e.price;
                // The public feed states the aggressor's side, so the direction
                // of every trade is observable to every participant, not just
                // its two sides. That is what lets a maker learn from the whole
                // tape instead of only from what happens to hit its own quotes.
                last_aggressor_ = e.side;
                signed_volume_ += e.side == Side::Buy ? static_cast<std::int64_t>(e.qty)
                                                      : -static_cast<std::int64_t>(e.qty);
                ++trades_seen_;
                reduce(e.order_id, e.qty);
                reduce(e.maker_id, e.qty);
                break;
            case EventType::Cancelled:
                reduce(e.order_id, e.qty);
                break;
            case EventType::Rejected:
            case EventType::EndOfStream:
                break;
        }
    }

    [[nodiscard]] InstrumentId instrument() const noexcept { return instr_; }
    [[nodiscard]] Price last_trade() const noexcept { return last_trade_; }
    [[nodiscard]] Side last_aggressor() const noexcept { return last_aggressor_; }
    // Cumulative buy volume minus sell volume, by aggressor side, over every
    // trade this view has been shown. A strategy reads the *change* in it since
    // it last looked, which is the order-flow imbalance it missed.
    [[nodiscard]] std::int64_t signed_volume() const noexcept { return signed_volume_; }
    [[nodiscard]] std::uint64_t trades_seen() const noexcept { return trades_seen_; }
    [[nodiscard]] std::size_t live_orders() const noexcept { return live_.size(); }
    [[nodiscard]] std::uint64_t total_qty() const noexcept { return total_qty_; }

    // 0 when that side is empty, which is the convention every bot here uses.
    [[nodiscard]] Price best(Side s) const noexcept {
        const auto& m = side_[idx(s)];
        return m.empty() ? 0 : price_of(s, m.begin()->first);
    }

    [[nodiscard]] std::uint64_t size_at_best(Side s) const noexcept {
        const auto& m = side_[idx(s)];
        return m.empty() ? 0 : m.begin()->second.total;
    }

    [[nodiscard]] std::uint32_t depth(Side s) const noexcept {
        return static_cast<std::uint32_t>(side_[idx(s)].size());
    }

    // rank 0 == best. Returns a zeroed view past the end rather than reading out
    // of bounds, because a stale view legitimately asks about levels that have
    // since been consumed.
    [[nodiscard]] LevelView level(Side s, std::uint32_t rank) const noexcept {
        const auto& m = side_[idx(s)];
        if (rank >= m.size()) return {};
        auto it = m.begin();
        std::advance(it, rank);
        LevelView v;
        v.price = price_of(s, it->first);
        v.qty = it->second.total;
        v.orders = it->second.count;
        return v;
    }

    // The mid in price units, or 0 when either side is empty. Integer only:
    // nothing inside a session may depend on floating point (arena/clock.hpp).
    [[nodiscard]] Price mid() const noexcept {
        const Price b = best(Side::Buy);
        const Price a = best(Side::Sell);
        return (b == 0 || a == 0) ? 0 : (b + a) / 2;
    }

    // Writes `owner`'s live orders into `out`, best-priced first within each
    // side, and returns how many were written. Queue position is computed here,
    // which costs one walk of each level the owner is resting on.
    std::size_t my_orders(OwnerId owner, OrderRef* out, std::size_t cap) const {
        std::size_t n = 0;
        for (int si = 0; si < 2; ++si) {
            const Side s = si == 0 ? Side::Buy : Side::Sell;
            for (const auto& entry : side_[si]) {
                std::uint64_t ahead = 0;
                for (const OrderId id : entry.second.fifo) {
                    const auto it = live_.find(id);
                    if (it == live_.end()) continue; // a stale FIFO slot
                    if (it->second.owner == owner) {
                        if (n == cap) return n;
                        out[n++] = OrderRef{id, s, price_of(s, entry.first), it->second.leaves, ahead};
                    }
                    ahead += it->second.leaves;
                }
            }
        }
        return n;
    }

    [[nodiscard]] bool knows(OrderId id) const noexcept { return live_.count(id) != 0; }

    [[nodiscard]] Qty leaves_of(OrderId id) const noexcept {
        const auto it = live_.find(id);
        return it == live_.end() ? 0 : it->second.leaves;
    }

private:
    struct MOrder {
        Price price = 0;
        Qty leaves = 0;
        OwnerId owner = 0;
        Side side = Side::Buy;
    };

    struct MLevel {
        std::uint64_t total = 0;
        std::uint32_t count = 0;
        std::vector<OrderId> fifo; // time priority; may hold ids that are gone
        std::uint32_t dead = 0;    // how many of those, so it can be compacted
    };

    [[nodiscard]] static constexpr int idx(Side s) noexcept { return s == Side::Buy ? 0 : 1; }
    // Bids are keyed by -price so that begin() is the best level on both sides.
    [[nodiscard]] static constexpr std::int64_t key_of(Side s, Price p) noexcept {
        return s == Side::Buy ? -p : p;
    }
    [[nodiscard]] static constexpr Price price_of(Side s, std::int64_t key) noexcept {
        return s == Side::Buy ? -key : key;
    }

    void insert(OrderId id, Side s, Price price, Qty qty, OwnerId owner) {
        if (qty == 0) return;
        live_[id] = MOrder{price, qty, owner, s};
        MLevel& lvl = side_[idx(s)][key_of(s, price)];
        lvl.total += qty;
        ++lvl.count;
        lvl.fifo.push_back(id);
        total_qty_ += qty;
    }

    void reduce(OrderId id, Qty qty) {
        const auto it = live_.find(id);
        if (it == live_.end()) return; // never acked here, or already gone
        MOrder& o = it->second;
        const Qty taken = qty < o.leaves ? qty : o.leaves;
        o.leaves -= taken;
        total_qty_ -= taken;
        auto& m = side_[idx(o.side)];
        const auto lit = m.find(key_of(o.side, o.price));
        if (lit == m.end()) return;
        MLevel& lvl = lit->second;
        lvl.total -= taken;
        if (o.leaves != 0) return;
        --lvl.count;
        ++lvl.dead;
        live_.erase(it);
        if (lvl.count == 0) {
            m.erase(lit);
        } else if (lvl.dead > lvl.fifo.size() / 2) {
            compact(lvl);
        }
    }

    // Drops FIFO slots whose orders are gone. The survivors keep their relative
    // order, so this can never alter a queue position.
    void compact(MLevel& lvl) {
        std::size_t w = 0;
        for (std::size_t r = 0; r < lvl.fifo.size(); ++r) {
            if (live_.count(lvl.fifo[r]) != 0) lvl.fifo[w++] = lvl.fifo[r];
        }
        lvl.fifo.resize(w);
        lvl.dead = 0;
    }

    InstrumentId instr_;
    std::unordered_map<OrderId, MOrder> live_;
    std::map<std::int64_t, MLevel> side_[2]; // [0] bids, [1] asks; begin() == best
    Price last_trade_ = 0;
    std::uint64_t total_qty_ = 0;
    std::int64_t signed_volume_ = 0;
    std::uint64_t trades_seen_ = 0;
    Side last_aggressor_ = Side::Buy;
};

} // namespace px::arena
