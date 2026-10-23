// marketdata/replay.hpp — an order-by-order tape played back through the engine,
// with one strategy trading in it (docs/DATA.md, "Replay").
//
// HISTORY CANNOT REACT. Every order on the tape arrives when it did, whatever the
// strategy does: nobody cancels because it bid, nobody follows its price, and an
// order it took is simply missing when the tape later cancels it. What a strategy
// earns here is what the recorded flow would have given a participant it never
// saw, which is the backtester's flaw the rest of this project is built to avoid.
// The report says so first, and counts every place the tape and the strategy
// collided, so the size of that flaw is on the page beside the result.
//
// HOW THE TAPE BECOMES ORDERS. Three owners share one MatchingEngine:
//   history's book   every add on the tape, as a GTC limit at its recorded time;
//                    a cancel cancels it. Size the tape knew only by level (the
//                    book LOBSTER says was resting before the file began) is one
//                    order a level, ahead of everything added later.
//   history's flow   every execution on the tape, as an IOC at the trade's price
//                    and size, so it trades with whatever is first in the queue
//                    there, which is the recorded order unless the strategy got
//                    ahead of it.
//   the strategy     a third owner, woken the way Session wakes one: on its own
//                    events, when the touch moves, on its timer, and seeing the
//                    book as it was its reaction delay ago.
// A print with no resting order named (a hidden execution) changes no book and is
// not replayed.
//
// THE ENGINE HAS NO PARTIAL CANCEL, and is not given one: the engine is finished.
// A partial cancel cancels that order and every order of history's behind it at
// its price, and adds them back in the same order, so the queue keeps its order.
// An order of the strategy's behind it moves up past them, and that is counted.
//
// THE CONTROL. With no strategy the replayed book must equal the tape's own book
// (TapeBook) after every event, and every execution must find its order first in
// the queue; test_data holds both, with planted defects that break them.
//
// Integer throughout, like the session: no floating point, and the same tape and
// strategy give the same engine event stream, whose hash the report prints.
#pragma once

#include "arena/book_mirror.hpp"
#include "arena/strategy.hpp"
#include "arena/strategies/registry.hpp"
#include "marketdata/stats.hpp"
#include "marketdata/tape.hpp"
#include "px/matching_engine.hpp"
#include "px/messages.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace px::data {

struct ReplayConfig {
    arena::SimTime reaction_ns = 10'000; // the strategy's; the suite's seat delay unless told otherwise
    std::int64_t from_ns = 0;            // tape time the strategy starts at; the tape before it builds the book
    std::int64_t for_ns = 0;             // how long it trades; 0 to the tape's end
    bool check_book = false;             // compare the books after every tape event (the control)
    // Planted defects, for the control's own test: each must make it fail.
    bool defect_full_cancels = false;    // a partial cancel replayed as a full one
    bool defect_requeue_at_back = false; // a partial cancel's order put back behind the others
};

struct ReplayStats {
    // What the tape asked for.
    std::uint64_t events = 0, adds = 0, cancels = 0, executions = 0, levels = 0, resets = 0;
    std::uint64_t hidden_prints = 0;    // prints naming no order: not replayed
    std::uint64_t partial_cancels = 0;  // replayed by putting the queue back in order
    std::uint64_t requeued = 0;         // orders of history's cancelled and added back for them
    std::uint64_t unknown = 0;          // cancels and executions of orders the tape never showed resting
    // What history could not do, because it cannot react.
    std::uint64_t book_full = 0;          // history's orders the engine had no room for
    std::uint64_t cancels_gone = 0;       // cancels whose order the strategy had already taken
    std::uint64_t exec_unfilled = 0;      // quantity of executions that found their price taken
    std::uint64_t exec_out_of_queue = 0;  // executions whose order was not first at its price
    std::uint64_t flow_with_strategy = 0; // quantity of history's flow that traded with the strategy
    std::uint64_t taken_from_book = 0;    // quantity the strategy took from history's book
    std::uint64_t adds_crossed = 0;       // quantity of history's adds that traded on arrival
    std::uint64_t moved_ahead = 0;        // times a requeue moved one of the strategy's orders up
    std::uint64_t self_trades = 0;        // history's orders cancelled by self-trade prevention
    // The control.
    std::uint64_t checked = 0, mismatches = 0;
    std::int64_t first_mismatch_t = -1;
    std::string first_mismatch;
    // The strategy.
    std::uint64_t orders = 0, fills = 0, rejects = 0, wakes = 0, volume = 0, drops = 0;
    std::int64_t position = 0;
    std::int64_t cash = 0;   // ticks x quantity, negative for what it paid
    std::int64_t mark = 0;   // the price it is marked at, in ticks: the last mid, else the last trade
    std::int64_t value = 0;  // cash + position x mark
    std::int64_t start_t = 0, end_t = 0;
    std::uint64_t hash = 0, engine_events = 0;
};

class Replay {
public:
    static constexpr OwnerId kBook = 1, kFlow = 2, kStrategy = 3;
    static constexpr std::size_t kMaxMyOrders = 32;

    // `strategy` may be null: the tape alone, which is the control.
    bool run(const Tape& tape, std::unique_ptr<arena::StrategyHandle> strategy, const ReplayConfig& cfg,
             ReplayStats& out, std::string& why) {
        if (tape.h.depth != Depth::L3) {
            why = "replay needs an order-by-order (L3) tape, and " + tape.h.name + " is " +
                  (tape.h.depth == Depth::L2 ? "price levels (L2)" : "trades only") +
                  ": with no orders to play back there is nothing to replay; calibrate a market from it instead";
            return false;
        }
        cfg_ = cfg;
        st_ = ReplayStats{};
        strategy_ = std::move(strategy);
        EngineConfig ec;
        ec.max_orders = 1u << 20;
        ec.max_levels = 1u << 16;
        ec.max_instruments = 1;
        engine_ = std::make_unique<MatchingEngine>(ec);
        instr_ = engine_->add_instrument("REPLAY", 1);
        mirror_ = arena::BookMirror(instr_);
        const std::int64_t start = cfg.from_ns;
        const std::int64_t last = tape.ev.empty() ? 0 : tape.ev.back().t;
        const std::int64_t end = cfg.for_ns > 0 ? start + cfg.for_ns : last;
        st_.start_t = start;
        st_.end_t = end;
        bool started = false;
        std::size_t i = 0;
        while (true) {
            const bool tape_left = i < tape.ev.size() && tape.ev[i].t <= end;
            if (!started && (!tape_left || tape.ev[i].t >= start)) {
                started = true;
                if (strategy_) {
                    strategy_->reset(1);
                    timer_ = strategy_->timer_ns();
                    if (timer_ > 0) next_timer_ = start + timer_;
                    wake_reason_ = arena::WakeReason::Open;
                    next_wake_ = start + cfg_.reaction_ns;
                }
            }
            // At one instant the tape goes first, then the strategy's look, then its timer.
            const std::int64_t wake = started && strategy_ ? std::min(next_wake_, next_timer_) : kNever;
            if (tape_left && (!started || tape.ev[i].t <= wake)) {
                now_ = tape.ev[i].t;
                apply(tape.ev[i]);
                if (cfg_.check_book) check(tape.ev[i], tape.h);
                ++i;
                continue;
            }
            if (wake == kNever || wake > end) break;
            now_ = wake;
            run_strategy(next_wake_ > next_timer_);
        }
        finish();
        out = st_;
        return true;
    }

    [[nodiscard]] const MatchingEngine& engine() const noexcept { return *engine_; }
    [[nodiscard]] InstrumentId instrument() const noexcept { return instr_; }

private:
    static constexpr std::int64_t kNever = INT64_MAX;
    struct QOrder {
        Side side;
        Price price;
        Qty leaves;
        OwnerId owner;
        std::list<OrderId>::iterator at;
    };
    using LevelKey = std::pair<int, Price>;

    ReplayConfig cfg_{};
    ReplayStats st_{};
    std::unique_ptr<arena::StrategyHandle> strategy_;
    std::unique_ptr<MatchingEngine> engine_;
    InstrumentId instr_ = 0;
    OrderId next_id_ = 1;
    std::int64_t now_ = 0;
    std::vector<ExecEvent> buf_;
    EventHasher hasher_{};

    // The engine's queue, rebuilt from its events, as BookMirror does, but keeping
    // each level's orders in time order so a requeue can put them back.
    std::unordered_map<OrderId, QOrder> q_;
    std::map<LevelKey, std::list<OrderId>> levels_;
    // The tape's order ids, and its level-only size, to the engine's orders.
    std::unordered_map<std::uint64_t, OrderId> tape_ids_;
    std::map<LevelKey, OrderId> anon_;
    TapeBook book_;

    // The strategy's side of things.
    arena::BookMirror mirror_{};
    std::deque<std::pair<std::int64_t, ExecEvent>> feed_;
    std::unordered_set<OrderId> awaiting_;
    arena::BookMirror::OrderRef my_buf_[kMaxMyOrders]{};
    arena::SimTime timer_ = 0;
    std::int64_t next_wake_ = kNever, next_timer_ = kNever;
    arena::WakeReason wake_reason_ = arena::WakeReason::Open;
    std::int64_t last_trade_ = 0;

    static int side_ix(Side s) noexcept { return s == Side::Buy ? 0 : 1; }
    static Side side_of(char c) noexcept { return c == 'B' ? Side::Buy : Side::Sell; }

    void send(const OrderRequest& req) {
        buf_.clear();
        engine_->process(req, EventSink(this, [](void* ctx, const ExecEvent& e) {
                             static_cast<Replay*>(ctx)->buf_.push_back(e);
                         }));
        bool own = false;
        for (const ExecEvent& e : buf_) {
            hasher_.add(e);
            ++st_.engine_events;
            follow(e);
            if (e.owner == kStrategy || (e.type == EventType::Trade && e.maker_owner == kStrategy)) own = true;
            if (strategy_) feed_.emplace_back(now_, e);
        }
        if (strategy_) wake_after(own);
    }
    void wake_after(bool own) {
        const bool touch = top(Side::Buy) != seen_bid_ || top(Side::Sell) != seen_ask_;
        seen_bid_ = top(Side::Buy);
        seen_ask_ = top(Side::Sell);
        if ((own || touch) && next_wake_ == kNever && started_strategy()) {
            wake_reason_ = own ? arena::WakeReason::Own : arena::WakeReason::Touch;
            next_wake_ = now_ + cfg_.reaction_ns;
        }
    }
    [[nodiscard]] bool started_strategy() const noexcept { return now_ >= cfg_.from_ns; }
    Price seen_bid_ = 0, seen_ask_ = 0;

    // Keeps the queue model, the strategy's accounts and the collision counts.
    void follow(const ExecEvent& e) {
        switch (e.type) {
            case EventType::Ack: {
                auto& lvl = levels_[{side_ix(e.side), e.price}];
                lvl.push_back(e.order_id);
                q_[e.order_id] = QOrder{e.side, e.price, e.qty, e.owner, std::prev(lvl.end())};
                break;
            }
            case EventType::Trade: {
                reduce(e.order_id, e.qty);
                reduce(e.maker_id, e.qty);
                last_trade_ = e.price;
                if (e.owner == kStrategy || e.maker_owner == kStrategy) {
                    const bool taker = e.owner == kStrategy;
                    const bool bought = taker ? e.side == Side::Buy : e.side == Side::Sell;
                    const auto q = static_cast<std::int64_t>(e.qty);
                    st_.position += bought ? q : -q;
                    st_.cash += (bought ? -1 : 1) * e.price * q;
                    ++st_.fills;
                    st_.volume += e.qty;
                    if (taker && e.maker_owner == kBook) st_.taken_from_book += e.qty;
                    if (!taker && e.owner == kFlow) st_.flow_with_strategy += e.qty;
                    if (!taker && e.owner == kBook) st_.adds_crossed += e.qty;
                }
                break;
            }
            case EventType::Cancelled:
                if (static_cast<CancelReason>(e.reason) == CancelReason::SelfTrade && e.owner == kBook) ++st_.self_trades;
                if (static_cast<CancelReason>(e.reason) == CancelReason::IocRemainder && e.owner == kFlow)
                    st_.exec_unfilled += e.qty;
                reduce(e.order_id, e.qty);
                break;
            case EventType::Rejected:
                if (e.owner == kStrategy) ++st_.rejects;
                if (e.owner == kBook && static_cast<RejectReason>(e.reason) == RejectReason::BookFull) ++st_.book_full;
                break;
            case EventType::EndOfStream: break;
        }
    }
    void reduce(OrderId id, Qty n) {
        const auto it = q_.find(id);
        if (it == q_.end()) return;
        if (n >= it->second.leaves) {
            const LevelKey k{side_ix(it->second.side), it->second.price};
            auto lv = levels_.find(k);
            lv->second.erase(it->second.at);
            if (lv->second.empty()) levels_.erase(lv);
            q_.erase(it);
        } else {
            it->second.leaves -= n;
        }
    }
    [[nodiscard]] Price top(Side s) const noexcept {
        return engine_->depth(instr_, s) == 0 ? 0 : engine_->level(instr_, s, 0).price;
    }
    [[nodiscard]] Qty leaves(OrderId id) const noexcept {
        const auto it = q_.find(id);
        return it == q_.end() ? 0 : it->second.leaves;
    }

    OrderId add_book(Side s, Price p, Qty q) {
        const OrderId id = next_id_++;
        send(limit_order(id, instr_, kBook, s, p, q, Tif::GTC));
        return id;
    }

    // The engine order a tape event names: its own, or its level's size the tape knew only by level.
    [[nodiscard]] OrderId engine_id(const TapeEvent& e, Side resting) const {
        if (e.id != 0) {
            const auto it = tape_ids_.find(e.id);
            if (it != tape_ids_.end() && q_.count(it->second) != 0) return it->second;
            if (it != tape_ids_.end()) return it->second; // known, but gone
        }
        const auto a = anon_.find({side_ix(resting), e.price});
        return a != anon_.end() ? a->second : 0;
    }

    // Takes `n` off engine order `id` and keeps its place: the order and every
    // order of history's behind it at its price are cancelled and added back in
    // order. Orders of the strategy's behind it stay, and so move up.
    void partial_cancel(OrderId id, Qty n) {
        const QOrder& o = q_.at(id);
        const Side side = o.side;
        const Price price = o.price;
        if (cfg_.defect_full_cancels) {
            send(cancel_order(id, instr_, kBook));
            return;
        }
        const Qty keep = o.leaves - n;
        std::vector<std::pair<OrderId, Qty>> tail; // history's, from `id` back
        bool strategy_behind = false;
        const auto& lvl = levels_.at({side_ix(side), price});
        for (auto it = o.at; it != lvl.end(); ++it) {
            const QOrder& x = q_.at(*it);
            if (x.owner == kBook) tail.emplace_back(*it, *it == id ? keep : x.leaves);
            else if (x.owner == kStrategy) strategy_behind = true;
        }
        if (cfg_.defect_requeue_at_back) {
            send(cancel_order(id, instr_, kBook));
            if (keep > 0) rename(id, add_book(side, price, keep));
            return;
        }
        if (strategy_behind) ++st_.moved_ahead;
        for (const auto& [old_id, q] : tail) send(cancel_order(old_id, instr_, kBook));
        for (const auto& [old_id, q] : tail) {
            if (q <= 0) continue;
            rename(old_id, add_book(side, price, q));
            if (old_id != id) ++st_.requeued;
        }
    }
    // Points whatever named an engine order at its replacement.
    void rename(OrderId from, OrderId to) {
        const auto t = by_engine_.find(from);
        if (t != by_engine_.end()) {
            tape_ids_[t->second] = to;
            by_engine_[to] = t->second;
            by_engine_.erase(t);
            return;
        }
        for (auto& [k, v] : anon_)
            if (v == from) v = to;
    }
    std::unordered_map<OrderId, std::uint64_t> by_engine_; // engine id -> tape id

    void apply(const TapeEvent& e) {
        ++st_.events;
        book_.apply(e);
        switch (e.kind) {
            case 'A': {
                ++st_.adds;
                if (e.side != 'B' && e.side != 'S') return;
                if (e.id != 0) {
                    const auto was = tape_ids_.find(e.id);
                    if (was != tape_ids_.end() && q_.count(was->second) != 0)
                        send(cancel_order(was->second, instr_, kBook));
                }
                const OrderId id = add_book(side_of(e.side), e.price, static_cast<Qty>(e.qty));
                if (e.id != 0) {
                    tape_ids_[e.id] = id;
                    by_engine_[id] = e.id;
                } else {
                    anon_[{side_ix(side_of(e.side)), e.price}] = id;
                }
                return;
            }
            case 'L': {
                // Size known only by level: one order a level, set to the size given.
                ++st_.levels;
                if (e.side != 'B' && e.side != 'S') return;
                const Side s = side_of(e.side);
                const LevelKey k{side_ix(s), e.price};
                // What the level holds of orders the tape named, which an L row does not replace.
                std::int64_t named = 0;
                const auto lv = levels_.find(k);
                OrderId anon = 0;
                if (const auto a = anon_.find(k); a != anon_.end() && q_.count(a->second) != 0) anon = a->second;
                if (lv != levels_.end())
                    for (const OrderId x : lv->second)
                        if (x != anon && q_.at(x).owner == kBook) named += q_.at(x).leaves;
                const std::int64_t want = e.qty > named ? e.qty - named : 0;
                const std::int64_t have = anon != 0 ? leaves(anon) : 0;
                if (want == have) return;
                if (anon != 0 && want > 0 && want < have) {
                    partial_cancel(anon, static_cast<Qty>(have - want));
                    return;
                }
                if (anon != 0) send(cancel_order(anon, instr_, kBook));
                anon_.erase(k);
                if (want > 0) anon_[k] = add_book(s, e.price, static_cast<Qty>(want));
                return;
            }
            case 'C': {
                ++st_.cancels;
                const Side resting = side_of(e.side);
                const OrderId id = engine_id(e, resting);
                if (id == 0) {
                    ++st_.unknown;
                    return;
                }
                const Qty have = leaves(id);
                if (have == 0) {
                    ++st_.cancels_gone;
                    return;
                }
                if (static_cast<Qty>(e.qty) >= have) send(cancel_order(id, instr_, kBook));
                else {
                    ++st_.partial_cancels;
                    partial_cancel(id, static_cast<Qty>(e.qty));
                }
                return;
            }
            case 'T': {
                if (e.id == 0) {
                    ++st_.hidden_prints;
                    return;
                }
                ++st_.executions;
                const char rest = TapeBook::opposite(e.side);
                if (rest == '-') return;
                const Side resting = side_of(rest);
                const OrderId id = engine_id(e, resting);
                if (id == 0) ++st_.unknown;
                // Is its order first in the queue at that price, as time priority says it must be?
                if (id != 0 && leaves(id) > 0) {
                    const auto lv = levels_.find({side_ix(resting), e.price});
                    if (lv != levels_.end() && lv->second.front() != id && q_.at(lv->second.front()).owner == kBook)
                        ++st_.exec_out_of_queue;
                }
                const OrderId flow = next_id_++;
                send(limit_order(flow, instr_, kFlow, resting == Side::Buy ? Side::Sell : Side::Buy, e.price,
                                 static_cast<Qty>(e.qty), Tif::IOC));
                return;
            }
            case 'R': {
                ++st_.resets;
                std::vector<OrderId> mine;
                for (const auto& [id, o] : q_)
                    if (o.owner == kBook) mine.push_back(id);
                std::sort(mine.begin(), mine.end());
                for (const OrderId id : mine) send(cancel_order(id, instr_, kBook));
                anon_.clear();
                return;
            }
            default: return;
        }
    }

    // The control: the engine's book, by level, against the tape's.
    void check(const TapeEvent& e, const TapeHeader& h) {
        ++st_.checked;
        for (int s = 0; s < 2; ++s) {
            const Side side = s == 0 ? Side::Buy : Side::Sell;
            const auto want = book_.top(s, 1u << 30);
            const std::uint32_t depth = engine_->depth(instr_, side);
            bool same = want.size() == depth;
            for (std::uint32_t r = 0; same && r < depth; ++r) {
                const LevelView lv = engine_->level(instr_, side, r);
                same = lv.price == want[r].first && static_cast<std::int64_t>(lv.qty) == want[r].second;
            }
            if (!same) {
                if (st_.mismatches++ == 0) {
                    st_.first_mismatch_t = e.t;
                    st_.first_mismatch = std::string(s == 0 ? "bids" : "asks") + " differ after the " + std::string(1, e.kind) +
                                         " at " + time_text(h.origin_ns + e.t, h.origin_kind);
                }
                return;
            }
        }
    }

    void run_strategy(bool timer) {
        arena::WakeReason why = wake_reason_;
        if (timer) {
            why = arena::WakeReason::Timer;
            next_timer_ = now_ + timer_;
        } else {
            next_wake_ = kNever;
        }
        const std::int64_t cutoff = now_ - cfg_.reaction_ns;
        while (!feed_.empty() && feed_.front().first <= cutoff) {
            const ExecEvent& e = feed_.front().second;
            mirror_.apply(e);
            if (e.owner == kStrategy && (e.type == EventType::Ack || e.type == EventType::Rejected))
                awaiting_.erase(e.order_id);
            feed_.pop_front();
        }
        arena::MarketView v;
        v.now_ns = now_;
        v.as_of_ns = cutoff;
        v.book = &mirror_;
        v.reason = why;
        v.instr = instr_;
        v.tick = 1;
        v.my_order_count = mirror_.my_orders(kStrategy, my_buf_, kMaxMyOrders);
        v.my_orders = my_buf_;
        v.in_flight = static_cast<std::uint32_t>(awaiting_.size());
        v.position = st_.position;
        v.cash = st_.cash;
        v.fills = st_.fills;
        v.rejects = st_.rejects;
        ++st_.wakes;
        const arena::ActionBatch batch = strategy_->on_event(v);
        st_.drops += batch.dropped;
        for (std::uint8_t i = 0; i < batch.n; ++i) submit(batch.a[i]);
    }
    void submit(const arena::Action& a) {
        OrderRequest req;
        switch (a.kind) {
            case arena::Action::Kind::Limit:
                if (a.qty == 0) return;
                req = limit_order(next_id_++, instr_, kStrategy, a.side, a.price, a.qty, a.tif);
                break;
            case arena::Action::Kind::Market:
                if (a.qty == 0) return;
                req = market_order(next_id_++, instr_, kStrategy, a.side, a.qty);
                break;
            case arena::Action::Kind::Cancel:
                if (a.cancel_id == 0) return;
                req = cancel_order(a.cancel_id, instr_, kStrategy);
                break;
            case arena::Action::Kind::None: return;
        }
        ++st_.orders;
        if (a.kind != arena::Action::Kind::Cancel) awaiting_.insert(req.order_id);
        send(req);
    }

    void finish() {
        const Price b = top(Side::Buy), a = top(Side::Sell);
        st_.mark = b != 0 && a != 0 ? (a + b) / 2 : last_trade_;
        st_.value = st_.cash + st_.position * st_.mark;
        st_.hash = hasher_.value();
    }
};

} // namespace px::data
