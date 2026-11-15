// venue/feed.hpp — what leaves the venue: private reports and a public feed.
//
// The engine emits one stream of events that names both owners of every trade.
// A venue never publishes that. The Publisher here splits it in two:
//
//   private reports   to one owner each, about that owner's orders only. A fill
//                     tells you your order, the price and the size, and never
//                     who was on the other side. Numbered per owner.
//   the public feed   anonymous and order by order (the shape of Nasdaq's ITCH):
//                     Add when an order comes to rest, Exec when a resting order
//                     is filled, Delete when one is cancelled, Reduce when one
//                     is amended smaller in place. An amend that sends an
//                     order to the back is a Delete and a new Add. An auction is
//                     announced when it opens and when it closes, with the price
//                     it uncrossed at; each of its trades is an Exec for the
//                     seller's order and a Paired for the buyer's. Orders are named
//                     by a reference number the feed makes up, never by the
//                     client's order id, and no message has an owner in it: the
//                     32-byte wire format has no field to put one in.
//
// An incoming order that trades and leaves nothing behind never appears on the
// public feed by itself: the executions against resting orders say all that the
// market is entitled to know.
//
// The feed is numbered, so a listener that misses a message knows it. FeedBook
// is such a listener: it builds the book from the public feed alone, refuses to
// apply a message that does not follow the last one, and catches up from a
// snapshot. tests/test_venue.cpp drops messages on purpose and checks the
// rebuilt book against the engine's.
//
// Messages are encoded to and from plain byte buffers. No socket is involved.
#pragma once

#include "px/matching_engine.hpp"
#include "venue/journal.hpp"

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace px::venue {

// ---- the public feed --------------------------------------------------------

enum class FeedType : std::uint8_t {
    Add = 1,
    Exec = 2,
    Delete = 3,
    Reduce = 4,
    AuctionOpen = 5,  // the instrument stops matching and collects orders
    AuctionClose = 6, // it uncrossed at `price` (0: nothing matched) and matches again
    Paired = 7,       // uncross only: the resting order on the other side of the Exec before it
};

struct FeedMsg {
    std::uint64_t seq = 0;   // feed sequence number, gapless from 1
    std::uint64_t ref = 0;   // the feed's own name for a resting order
    Price price = 0;         // Add: the order's price. Exec: the execution price.
    Qty qty = 0;             // Add: resting size. Exec, Paired: size filled. Reduce: new size. Delete: 0.
    InstrumentId instr = 0;
    FeedType type = FeedType::Add;
    Side side = Side::Buy;   // Add only
};

inline constexpr std::size_t kFeedMsgBytes = 32;

inline void encode_feed(std::uint8_t out[kFeedMsgBytes], const FeedMsg& m) noexcept {
    std::uint8_t* p = out;
    detail::put(p, m.seq, 8);
    detail::put(p, m.ref, 8);
    detail::put(p, static_cast<std::uint64_t>(m.price), 8);
    detail::put(p, m.qty, 4);
    detail::put(p, m.instr, 2);
    detail::put(p, static_cast<std::uint64_t>(m.type), 1);
    detail::put(p, static_cast<std::uint64_t>(m.side), 1);
}
[[nodiscard]] inline FeedMsg decode_feed(const std::uint8_t in[kFeedMsgBytes]) noexcept {
    const std::uint8_t* p = in;
    FeedMsg m;
    m.seq = detail::get(p, 8);
    m.ref = detail::get(p, 8);
    m.price = static_cast<Price>(detail::get(p, 8));
    m.qty = static_cast<Qty>(detail::get(p, 4));
    m.instr = static_cast<InstrumentId>(detail::get(p, 2));
    m.type = static_cast<FeedType>(detail::get(p, 1));
    m.side = static_cast<Side>(detail::get(p, 1));
    return m;
}

struct FeedOrder {
    std::uint64_t ref = 0;
    Price price = 0;
    Qty qty = 0;
    InstrumentId instr = 0;
    Side side = Side::Buy;
};

// Every resting order, oldest first, as of feed message `seq`.
struct FeedSnapshot {
    std::uint64_t seq = 0;
    std::vector<FeedOrder> orders;
    std::vector<InstrumentId> auctions; // instruments in an auction at that moment
};

// ---- private reports --------------------------------------------------------

enum class ReportType : std::uint8_t { Accepted, Rejected, Filled, Cancelled, Amended };

struct Report {
    std::uint64_t seq = 0;  // per owner, gapless from 1
    OrderId order_id = 0;   // always one of the receiving owner's own orders
    Price price = 0;        // Filled: execution price
    Qty qty = 0;            // Accepted: order size. Filled: size filled. Cancelled: size removed.
    Qty leaves = 0;
    OwnerId owner = 0;      // who this report is for
    ReportType type = ReportType::Accepted;
    std::uint8_t reason = 0;
};

// ---- the publisher ----------------------------------------------------------

class Publisher {
public:
    using FeedFn = std::function<void(const FeedMsg&)>;
    using ReportFn = std::function<void(const Report&)>;
    Publisher(FeedFn feed, ReportFn report) : feed_(std::move(feed)), report_(std::move(report)) {}

    void begin(const OrderRequest& req) noexcept {
        cur_ = req;
        alive_ = false;
        leaves_ = 0;
    }
    void on_event(const ExecEvent& e) {
        switch (e.type) {
        case EventType::Ack:
            alive_ = true;
            leaves_ = e.leaves;
            send_report(e.owner, ReportType::Accepted, e.order_id, 0, e.qty, e.leaves, 0);
            break;
        case EventType::Rejected:
            alive_ = false;
            send_report(e.owner, ReportType::Rejected, e.order_id, 0, 0, 0, e.reason);
            break;
        case EventType::Trade: {
            leaves_ = e.leaves;
            send_report(e.owner, ReportType::Filled, e.order_id, e.price, e.qty, e.leaves, 0);
            send_report(e.maker_owner, ReportType::Filled, e.maker_id, e.price, e.qty, e.maker_leaves, 0);
            executed(e.maker_id, FeedType::Exec, e.price, e.qty, e.maker_leaves);
            // In an uncross the buyer's order was resting too. It is one trade, so one Exec.
            if (cur_.type == ReqType::Uncross) executed(e.order_id, FeedType::Paired, e.price, e.qty, e.leaves);
            break;
        }
        case EventType::Cancelled: {
            send_report(e.owner, ReportType::Cancelled, e.order_id, e.price, e.qty, 0, e.reason);
            const auto it = ref_of_.find(e.order_id);
            if (it != ref_of_.end()) {
                const FeedOrder o = resting_[it->second];
                send_feed(FeedType::Delete, o.ref, o.instr, o.side, 0, 0);
                drop(it);
            } else if (e.order_id == cur_.order_id) {
                alive_ = false;
            }
            break;
        }
        case EventType::Amended: {
            send_report(e.owner, ReportType::Amended, e.order_id, e.price, e.qty, e.leaves, e.reason);
            const auto it = ref_of_.find(e.order_id);
            if (it == ref_of_.end()) break;
            FeedOrder& o = resting_[it->second];
            if (static_cast<AmendEffect>(e.reason) == AmendEffect::KeptPriority) {
                o.qty = e.leaves;
                send_feed(FeedType::Reduce, o.ref, o.instr, o.side, o.price, e.leaves);
            } else { // the old reference goes; if the order rests again it gets a new one
                send_feed(FeedType::Delete, o.ref, o.instr, o.side, 0, 0);
                drop(it);
                cur_ = limit_order(e.order_id, e.instr, e.owner, e.side, e.price, e.leaves);
                alive_ = true;
                leaves_ = e.leaves;
            }
            break;
        }
        case EventType::Phase:
            if (static_cast<BookPhase>(e.reason) == BookPhase::Auction) {
                auctions_.insert(e.instr);
                send_feed(FeedType::AuctionOpen, 0, e.instr, Side::Buy, 0, 0);
            } else {
                auctions_.erase(e.instr);
                send_feed(FeedType::AuctionClose, 0, e.instr, Side::Buy, e.price, 0);
            }
            break;
        case EventType::EndOfStream:
            break;
        }
    }
    void end() {
        if (cur_.type != ReqType::New || !alive_ || leaves_ == 0) return;
        const std::uint64_t ref = ++next_ref_;
        resting_[ref] = FeedOrder{ref, cur_.price, leaves_, cur_.instr, cur_.side};
        ref_of_[cur_.order_id] = ref;
        send_feed(FeedType::Add, ref, cur_.instr, cur_.side, cur_.price, leaves_);
    }

    [[nodiscard]] FeedSnapshot snapshot() const {
        FeedSnapshot s;
        s.seq = feed_seq_;
        s.orders.reserve(resting_.size());
        for (const auto& [ref, o] : resting_) s.orders.push_back(o);
        s.auctions.assign(auctions_.begin(), auctions_.end());
        return s;
    }
    [[nodiscard]] std::uint64_t feed_seq() const noexcept { return feed_seq_; }

private:
    void executed(OrderId id, FeedType as, Price price, Qty qty, Qty leaves) {
        const auto it = ref_of_.find(id);
        if (it == ref_of_.end()) return;
        FeedOrder& o = resting_[it->second];
        send_feed(as, o.ref, o.instr, o.side, price, qty);
        if (leaves == 0) drop(it);
        else o.qty = leaves;
    }
    void drop(std::unordered_map<OrderId, std::uint64_t>::iterator it) {
        resting_.erase(it->second);
        ref_of_.erase(it);
    }
    void send_feed(FeedType t, std::uint64_t ref, InstrumentId instr, Side side, Price price, Qty qty) {
        FeedMsg m;
        m.seq = ++feed_seq_;
        m.ref = ref;
        m.price = price;
        m.qty = qty;
        m.instr = instr;
        m.type = t;
        m.side = side;
        if (feed_) feed_(m);
    }
    void send_report(OwnerId owner, ReportType t, OrderId id, Price price, Qty qty, Qty leaves, std::uint8_t reason) {
        Report r;
        r.seq = ++report_seq_[owner];
        r.order_id = id;
        r.price = price;
        r.qty = qty;
        r.leaves = leaves;
        r.owner = owner;
        r.type = t;
        r.reason = reason;
        if (report_) report_(r);
    }

    FeedFn feed_;
    ReportFn report_;
    std::map<std::uint64_t, FeedOrder> resting_;          // by ref, which is rest order
    std::unordered_map<OrderId, std::uint64_t> ref_of_;   // private: never leaves this class
    std::unordered_map<OwnerId, std::uint64_t> report_seq_;
    std::set<InstrumentId> auctions_;
    std::uint64_t next_ref_ = 0;
    std::uint64_t feed_seq_ = 0;
    OrderRequest cur_;
    bool alive_ = false;
    Qty leaves_ = 0;
};

// ---- a listener that builds the book from the public feed alone -------------

class FeedBook {
public:
    enum class Result { Applied, Duplicate, Gap, Corrupt };

    // Applies `m` only if it is the next message. On Gap nothing changes and
    // the book is stale until load() gives it a snapshot at or past the gap.
    Result apply(const FeedMsg& m) {
        if (m.seq <= seq_) return Result::Duplicate;
        if (m.seq != seq_ + 1) {
            stale_ = true;
            return Result::Gap;
        }
        switch (m.type) {
        case FeedType::Add:
            if (orders_.count(m.ref) != 0) return Result::Corrupt;
            add(FeedOrder{m.ref, m.price, m.qty, m.instr, m.side});
            break;
        case FeedType::Exec:
        case FeedType::Paired: {
            const auto it = orders_.find(m.ref);
            if (it == orders_.end() || it->second->qty < m.qty) return Result::Corrupt;
            if (m.type == FeedType::Exec) { // a Paired is the same trade seen from the other order
                ++trades_;
                volume_ += m.qty;
            }
            if (it->second->qty == m.qty) remove(it);
            else it->second->qty -= m.qty;
            break;
        }
        case FeedType::AuctionOpen:
            if (!auctions_.insert(m.instr).second) return Result::Corrupt;
            break;
        case FeedType::AuctionClose:
            if (auctions_.erase(m.instr) == 0) return Result::Corrupt;
            last_uncross_[m.instr] = m.price;
            break;
        case FeedType::Delete: {
            const auto it = orders_.find(m.ref);
            if (it == orders_.end()) return Result::Corrupt;
            remove(it);
            break;
        }
        case FeedType::Reduce: {
            const auto it = orders_.find(m.ref);
            if (it == orders_.end() || m.qty == 0 || m.qty > it->second->qty) return Result::Corrupt;
            it->second->qty = m.qty;
            break;
        }
        default:
            return Result::Corrupt;
        }
        seq_ = m.seq;
        return Result::Applied;
    }

    // Replaces the book with a snapshot. Messages up to the snapshot's number
    // are then duplicates, so a listener may replay what it buffered.
    void load(const FeedSnapshot& s) {
        orders_.clear();
        levels_.clear();
        for (const FeedOrder& o : s.orders) add(o);
        auctions_.clear();
        auctions_.insert(s.auctions.begin(), s.auctions.end());
        seq_ = s.seq;
        stale_ = false;
    }

    [[nodiscard]] bool stale() const noexcept { return stale_; }
    [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }
    [[nodiscard]] std::size_t resting_orders() const noexcept { return orders_.size(); }
    [[nodiscard]] std::uint64_t trades() const noexcept { return trades_; }
    [[nodiscard]] std::uint64_t volume() const noexcept { return volume_; }
    [[nodiscard]] bool in_auction(InstrumentId instr) const { return auctions_.count(instr) != 0; }
    // The price of the instrument's last uncross that this listener saw, or 0.
    [[nodiscard]] Price last_uncross(InstrumentId instr) const {
        const auto it = last_uncross_.find(instr);
        return it == last_uncross_.end() ? 0 : it->second;
    }

    // Best price first, like MatchingEngine::level().
    [[nodiscard]] std::vector<LevelView> levels(InstrumentId instr, Side side) const {
        std::vector<LevelView> out;
        const auto it = levels_.find(key(instr, side));
        if (it == levels_.end()) return out;
        const auto emit = [&out](Price price, const std::list<FeedOrder>& q) {
            LevelView v;
            v.price = price;
            for (const FeedOrder& o : q) v.qty += o.qty;
            v.orders = static_cast<std::uint32_t>(q.size());
            out.push_back(v);
        };
        if (side == Side::Buy)
            for (auto l = it->second.rbegin(); l != it->second.rend(); ++l) emit(l->first, l->second);
        else
            for (auto l = it->second.begin(); l != it->second.end(); ++l) emit(l->first, l->second);
        return out;
    }

private:
    using Queue = std::list<FeedOrder>;
    static std::uint32_t key(InstrumentId instr, Side side) noexcept {
        return (static_cast<std::uint32_t>(instr) << 1) | static_cast<std::uint32_t>(side);
    }
    void add(const FeedOrder& o) {
        Queue& q = levels_[key(o.instr, o.side)][o.price];
        q.push_back(o);
        orders_[o.ref] = std::prev(q.end());
    }
    void remove(std::unordered_map<std::uint64_t, Queue::iterator>::iterator it) {
        const FeedOrder o = *it->second;
        auto& side = levels_[key(o.instr, o.side)];
        Queue& q = side[o.price];
        q.erase(it->second);
        if (q.empty()) side.erase(o.price);
        orders_.erase(it);
    }

    std::unordered_map<std::uint32_t, std::map<Price, Queue>> levels_;
    std::unordered_map<std::uint64_t, Queue::iterator> orders_;
    std::uint64_t seq_ = 0;
    std::uint64_t trades_ = 0;
    std::uint64_t volume_ = 0;
    std::set<InstrumentId> auctions_;
    std::map<InstrumentId, Price> last_uncross_;
    bool stale_ = false;
};

} // namespace px::venue
