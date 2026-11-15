// venue/book_state.hpp — the resting orders, known from the event stream alone,
// and a snapshot of them on disk.
//
// The engine exposes price levels but not the orders in them, and it is not given
// a new API for this. So the venue keeps its own list of what rests, built from
// the engine's events the way arena/book_mirror.hpp builds a book: an order rests
// when its request ends with quantity left and nothing cancelled or rejected it.
// Each resting order keeps the number of the moment it rested, which is its time
// priority.
//
// A snapshot is that list, in rest order, plus two counters: the last inbound
// sequence number applied and the engine's next event number. Restoring sends
// the orders into a fresh engine in rest order. Nothing can trade, because a
// book at rest is never crossed, so the result is the same book with the same
// queue at every price. The restored engine numbers its events from a different
// start, so the caller adds `seq_offset` to every event it emits afterwards.
#pragma once

#include "px/matching_engine.hpp"
#include "venue/journal.hpp"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace px::venue {

struct RestingOrder {
    OrderId id = 0;
    Price price = 0;
    Qty leaves = 0;
    OwnerId owner = 0;
    InstrumentId instr = 0;
    Side side = Side::Buy;
};

class RestingBook {
public:
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
            break;
        case EventType::Trade: {
            leaves_ = e.leaves;
            const auto it = by_id_.find(e.maker_id);
            if (it != by_id_.end()) {
                if (e.maker_leaves == 0) erase(it);
                else by_rest_[it->second].leaves = e.maker_leaves;
            }
            break;
        }
        case EventType::Cancelled: {
            const auto it = by_id_.find(e.order_id);
            if (it != by_id_.end()) erase(it);          // a resting order went
            else if (e.order_id == cur_.order_id) alive_ = false; // the incoming order's remainder went
            break;
        }
        case EventType::Rejected:
            alive_ = false;
            break;
        case EventType::EndOfStream:
            break;
        }
    }
    void end() {
        if (cur_.type != ReqType::New || !alive_ || leaves_ == 0) return;
        const std::uint64_t n = ++rest_no_;
        by_rest_[n] = RestingOrder{cur_.order_id, cur_.price, leaves_, cur_.owner, cur_.instr, cur_.side};
        by_id_[cur_.order_id] = n;
    }

    [[nodiscard]] std::size_t size() const noexcept { return by_rest_.size(); }
    // In rest order: oldest first, which is time priority within every level.
    [[nodiscard]] std::vector<RestingOrder> in_rest_order() const {
        std::vector<RestingOrder> out;
        out.reserve(by_rest_.size());
        for (const auto& [n, o] : by_rest_) out.push_back(o);
        return out;
    }
    // One owner's resting orders, oldest first (the kill switch cancels these).
    [[nodiscard]] std::vector<RestingOrder> of_owner(OwnerId owner) const {
        std::vector<RestingOrder> out;
        for (const auto& [n, o] : by_rest_)
            if (o.owner == owner) out.push_back(o);
        return out;
    }
    [[nodiscard]] const RestingOrder* find(OrderId id) const {
        const auto it = by_id_.find(id);
        return it == by_id_.end() ? nullptr : &by_rest_.at(it->second);
    }

private:
    void erase(std::unordered_map<OrderId, std::uint64_t>::iterator it) {
        by_rest_.erase(it->second);
        by_id_.erase(it);
    }
    std::map<std::uint64_t, RestingOrder> by_rest_;
    std::unordered_map<OrderId, std::uint64_t> by_id_;
    std::uint64_t rest_no_ = 0;
    OrderRequest cur_;
    bool alive_ = false;
    Qty leaves_ = 0;
};

struct Snapshot {
    std::uint64_t in_seq = 0;      // last journal record already applied
    SeqNum next_event_seq = 1;     // the engine's next event number at that moment
    std::vector<RestingOrder> orders;
};

inline constexpr char kSnapshotMagic[8] = {'P', 'X', 'S', 'N', 'A', 'P', '0', '1'};

// Written to a temporary name and renamed, so a crash mid-write leaves the
// previous snapshot in place.
[[nodiscard]] inline bool save_snapshot(const std::string& path, const Snapshot& s) {
    std::vector<std::uint8_t> buf(sizeof(kSnapshotMagic) + 24 + s.orders.size() * 27 + 8);
    std::uint8_t* p = buf.data();
    for (const char c : kSnapshotMagic) *p++ = static_cast<std::uint8_t>(c);
    detail::put(p, s.in_seq, 8);
    detail::put(p, s.next_event_seq, 8);
    detail::put(p, s.orders.size(), 8);
    for (const RestingOrder& o : s.orders) {
        detail::put(p, o.id, 8);
        detail::put(p, static_cast<std::uint64_t>(o.price), 8);
        detail::put(p, o.leaves, 4);
        detail::put(p, o.owner, 4);
        detail::put(p, o.instr, 2);
        detail::put(p, static_cast<std::uint64_t>(o.side), 1);
    }
    detail::put(p, detail::fnv(buf.data(), buf.size() - 8), 8);
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) return false;
    const bool wrote = std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !closed) return false;
    std::remove(path.c_str()); // Windows rename does not replace
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// False if the file is missing, short, or fails its checksum.
[[nodiscard]] inline bool load_snapshot(const std::string& path, Snapshot& s) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::vector<std::uint8_t> buf;
    std::uint8_t chunk[4096];
    for (std::size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) buf.insert(buf.end(), chunk, chunk + n);
    std::fclose(f);
    const std::size_t head = sizeof(kSnapshotMagic) + 24;
    if (buf.size() < head + 8) return false;
    for (std::size_t i = 0; i < sizeof(kSnapshotMagic); ++i)
        if (buf[i] != static_cast<std::uint8_t>(kSnapshotMagic[i])) return false;
    const std::uint8_t* p = buf.data() + sizeof(kSnapshotMagic);
    s.in_seq = detail::get(p, 8);
    s.next_event_seq = detail::get(p, 8);
    const std::uint64_t n = detail::get(p, 8);
    if (buf.size() != head + n * 27 + 8) return false;
    s.orders.clear();
    for (std::uint64_t i = 0; i < n; ++i) {
        RestingOrder o;
        o.id = detail::get(p, 8);
        o.price = static_cast<Price>(detail::get(p, 8));
        o.leaves = static_cast<Qty>(detail::get(p, 4));
        o.owner = static_cast<OwnerId>(detail::get(p, 4));
        o.instr = static_cast<InstrumentId>(detail::get(p, 2));
        o.side = static_cast<Side>(detail::get(p, 1));
        s.orders.push_back(o);
    }
    return detail::get(p, 8) == detail::fnv(buf.data(), buf.size() - 8);
}

} // namespace px::venue
