// arena/session.hpp — a seeded, replayable trading session inside the engine.
//
// The session owns a MatchingEngine, the simulated clock, the fundamental value,
// the roster and the accounting. It contains no rendering: the renderer is a
// reader of this, and the latency sweep is another, so the experiment can be run
// and verified without a terminal in sight.
//
// THE ENGINE IS UNCHANGED. The session is just another caller of
// MatchingEngine::process(), the way sim/pipeline.hpp and tools/repl.cpp are. It
// adds no engine API, moves no engine code, and cannot affect the measured
// latency path. What it does add is *when* orders arrive, which the engine has
// no opinion about.
//
// BOTS RUN OUTSIDE THE EVENT SINK. The sink the engine is handed appends to a
// buffer and does nothing else. Every consequence — accounting, feeds, wakes — is
// drawn afterwards, once process() has returned. Calling a strategy from inside
// the sink would run arbitrary code inside the engine's matching loop, which is
// the one place in this repository where that is not allowed.
//
// WHAT IS SIMULATED AND WHAT IS MEASURED. Every nanosecond in this file is
// invented. The engine's latency numbers in README.md are measured on hardware;
// a session's reaction delays are inputs the experimenter chooses. A session
// never times itself, and no number it produces belongs anywhere near the
// latency tables.
//
// REPLAY. Same seed plus same roster gives the same session, which hash() makes
// checkable in one number. Three things make that true and all three are load
// bearing: the scheduler's total (time, sequence) order, the absence of any
// floating point or wall-clock read inside a session, and the fact that a bot
// only ever sees its own delayed feed. test_arena asserts it.
//
// ALLOCATION. The arena allocates: maps, deques and vectors, by design. The
// zero-allocation rule is the engine's guarantee about process(), and
// test_alloc_audit still holds it there. Extending that rule up here would buy
// nothing and cost a great deal, so it is stated as not applying rather than
// quietly assumed.
#pragma once

#include "arena/accounting.hpp"
#include "arena/book_mirror.hpp"
#include "arena/bot.hpp"
#include "arena/bots/registry.hpp"
#include "arena/clock.hpp"
#include "arena/fair_value.hpp"
#include "px/matching_engine.hpp"
#include "px/messages.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace px::arena {

struct SessionConfig {
    std::uint64_t seed = 42;
    // The session ends at whichever of these comes first; 0 means "no limit".
    std::size_t max_requests = 200'000; // engine requests processed
    SimTime max_time_ns = 0;

    Price tick = 100;                       // $0.01
    Price start_price = 100 * kPriceScale;  // the previous close
    SimTime fair_step_mean_ns = 100'000;    // mean time between fundamental jumps
    std::int64_t fair_band_ticks = 2'000;

    SimTime markout_horizon_ns = 1'000'000; // 1 ms; see arena/accounting.hpp
    std::int64_t fee_per_qty = 0;

    std::size_t tape_capacity = 512; // recent prints kept for the renderer
};

// One print, for the TAPE window and for anything that wants recent history.
struct TapePrint {
    SimTime at = 0;
    Price price = 0;
    Qty qty = 0;
    Side aggressor = Side::Buy;
    std::uint16_t taker = 0; // roster index
    std::uint16_t maker = 0;
};

struct SessionStats {
    std::size_t requests = 0;
    std::uint64_t events = 0;
    std::uint64_t trades = 0;
    std::uint64_t volume = 0;
    std::uint64_t rejects = 0;
    std::uint64_t self_trade_cancels = 0;
    std::uint64_t book_full_rejects = 0;
    std::uint64_t action_drops = 0;   // batches that wanted more than ActionBatch::kMax
    std::uint64_t sink_growths = 0;   // times the event buffer had to grow
    std::uint64_t fair_steps = 0;
    std::uint64_t wall_hits = 0; // times the fundamental hit its reflecting wall
    SimTime elapsed_ns = 0;

    // Does the book track the fundamental? Sampled at every fundamental jump:
    // the absolute gap between the mid and V, in ticks. This is the session's
    // own sanity check, and it is reported with every result rather than kept
    // private, because a market whose price has wandered off is not a market and
    // every number read off it would be a measurement of the drift. Samples
    // where one side of the book is empty are counted separately.
    std::uint64_t gap_samples = 0;
    std::uint64_t gap_sum_ticks = 0;
    std::uint64_t gap_max_ticks = 0;
    std::uint64_t gap_unpriced = 0; // samples with no two-sided market

    [[nodiscard]] double mean_gap_ticks() const noexcept {
        return gap_samples == 0 ? 0.0 : static_cast<double>(gap_sum_ticks) / static_cast<double>(gap_samples);
    }
};

class Session {
public:
    static constexpr std::size_t kMaxMyOrders = 32;
    static constexpr std::size_t kEventBufferReserve = 4096;

    Session() { buf_.reserve(kEventBufferReserve); }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // The usual way in: build the registry's roster from its config.
    void start(const SessionConfig& scfg, const RosterConfig& rcfg) {
        RosterConfig r = rcfg;
        r.anchor = scfg.start_price;
        r.tick = scfg.tick;
        start(scfg, build_roster(r));
    }

    // The same session with a roster assembled by the caller. This is how
    // test_arena puts a probe participant in the market, and it is the proof
    // that the bot contract is genuinely open: nothing in here knows which
    // strategies it is running.
    void start(const SessionConfig& scfg, Roster roster) {
        cfg_ = scfg;

        EngineConfig ec;
        ec.max_orders = 1u << 13;
        ec.max_levels = 1u << 11;
        ec.levels_per_side = 512;
        ec.max_instruments = 1;
        engine_ = std::make_unique<MatchingEngine>(ec);
        instr_ = engine_->add_instrument("ARENA", cfg_.tick);

        roster_ = std::move(roster);

        FairValueConfig fv;
        fv.start = cfg_.start_price;
        fv.tick = cfg_.tick;
        fv.step_mean_ns = cfg_.fair_step_mean_ns;
        fv.band_ticks = cfg_.fair_band_ticks;
        // A derived seed, so that changing the roster's seeds cannot accidentally
        // change the value path and confound a sweep.
        fv_.reset(fv, cfg_.seed * 6364136223846793005ULL + 1442695040888963407ULL);

        sched_.reset();
        ledger_.reset(roster_.size(), cfg_.markout_horizon_ns, cfg_.fee_per_qty);
        pending_req_.clear();
        tape_.clear();
        tape_.shrink_to_fit();
        buf_.clear();

        parts_.clear();
        parts_.resize(roster_.size());
        for (std::size_t i = 0; i < roster_.size(); ++i) {
            Participant& p = parts_[i];
            p.mirror = BookMirror(instr_);
            p.owner = static_cast<OwnerId>(i + 1); // distinct owners: see registry.hpp
            p.reaction = roster_.bots[i]->reaction_ns();
            if (p.reaction < 0) p.reaction = 0;
            p.timer = roster_.bots[i]->timer_ns();
            p.informed = roster_.informed[i];
            p.signal_known = p.informed ? cfg_.start_price : 0;
            roster_.bots[i]->reset(cfg_.seed + 0x9E37U * (i + 1));
        }

        ev_hash_ = EventHasher{};
        time_hash_ = 0xcbf29ce484222325ULL;
        stats_ = SessionStats{};
        next_order_id_ = 1;
        done_ = false;

        // The first fundamental jump, each bot's heartbeat, and one opening look
        // per bot so that somebody quotes into an empty book.
        sched_.push(fv_.next_interval(), WakeKind::FairValueStep);
        for (std::size_t i = 0; i < parts_.size(); ++i) {
            Participant& p = parts_[i];
            const auto who = static_cast<std::uint16_t>(i);
            if (p.timer > 0) {
                p.pending_timer = p.timer;
                sched_.push(p.pending_timer, WakeKind::BotTimer, who);
            }
            p.pending_reason = WakeReason::Open;
            p.pending_event = p.reaction;
            sched_.push(p.pending_event, WakeKind::BotEvent, who);
        }
    }

    // Advances the session by one scheduled item. Returns false once the session
    // is over; the item that ended it has already been applied.
    bool step() {
        if (done_) return false;
        if (sched_.empty()) {
            done_ = true;
            return false;
        }
        if (cfg_.max_time_ns > 0 && sched_.next_time() > cfg_.max_time_ns) {
            done_ = true;
            return false;
        }

        const Wake w = sched_.pop();
        switch (w.kind) {
            case WakeKind::FairValueStep: on_fair_value_step(w.at); break;
            case WakeKind::BotEvent:
            case WakeKind::BotTimer:
            case WakeKind::BotSignal: run_bot(w); break;
            case WakeKind::OrderArrival: deliver_order(w); break;
        }
        stats_.elapsed_ns = w.at;
        if (cfg_.max_requests > 0 && stats_.requests >= cfg_.max_requests) done_ = true;
        return !done_;
    }

    void run() {
        while (step()) {
        }
        finish();
    }

    // Runs until the clock would pass `until_ns`. Used by the renderer to pace a
    // session for human eyes without changing a single thing about it.
    void run_until(SimTime until_ns) {
        while (!done_ && !sched_.empty() && sched_.next_time() <= until_ns) {
            if (!step()) break;
        }
    }

    // Closes the books: resolves every markout still inside its horizon against
    // the final fundamental value, counting them so a reader can see how many.
    void finish() {
        ledger_.resolve_all(fv_.value());
        done_ = true;
    }

    // ---- what the sweep and the renderer read ----

    [[nodiscard]] bool done() const noexcept { return done_; }
    [[nodiscard]] SimTime now() const noexcept { return sched_.now(); }
    [[nodiscard]] SimTime next_time() const noexcept { return sched_.next_time(); }
    [[nodiscard]] const MatchingEngine& engine() const noexcept { return *engine_; }
    [[nodiscard]] InstrumentId instrument() const noexcept { return instr_; }
    [[nodiscard]] const Ledger& ledger() const noexcept { return ledger_; }
    [[nodiscard]] const FairValue& fair_value() const noexcept { return fv_; }
    [[nodiscard]] const SessionConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] const SessionStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const std::vector<TapePrint>& tape() const noexcept { return tape_; }
    [[nodiscard]] std::size_t participants() const noexcept { return parts_.size(); }
    [[nodiscard]] const char* name_of(std::size_t i) const noexcept { return roster_.bots[i]->name(); }
    [[nodiscard]] SimTime reaction_of(std::size_t i) const noexcept { return parts_[i].reaction; }
    [[nodiscard]] bool informed(std::size_t i) const noexcept { return parts_[i].informed; }
    [[nodiscard]] std::uint64_t wakes_of(std::size_t i) const noexcept { return parts_[i].wakes; }
    [[nodiscard]] int maker_index() const noexcept { return roster_.maker_index; }
    [[nodiscard]] int rival_index() const noexcept { return roster_.rival_index; }
    // The view the renderer should draw for a focused participant: its own
    // mirror, stale by its own delay, which is what it actually traded on.
    [[nodiscard]] const BookMirror& mirror_of(std::size_t i) const noexcept { return parts_[i].mirror; }

    // One number per session. Mixes the event stream (through the engine's own
    // hasher, so it is the same kind of number as the determinism test's) with
    // the arrival time of every event, which is what the scheduler decides.
    [[nodiscard]] std::uint64_t hash() const noexcept {
        std::uint64_t h = ev_hash_.value();
        h ^= time_hash_;
        h *= 0x100000001b3ULL;
        return h;
    }
    [[nodiscard]] std::uint64_t event_hash() const noexcept { return ev_hash_.value(); }
    [[nodiscard]] std::uint64_t event_count() const noexcept { return ev_hash_.count(); }

    // Sets one session field by name, the counterpart of set_roster_param().
    [[nodiscard]] static bool set_param(SessionConfig& c, std::string_view key, std::int64_t value) {
        if (key == "session.seed") c.seed = static_cast<std::uint64_t>(value);
        else if (key == "session.max_requests") c.max_requests = static_cast<std::size_t>(value);
        else if (key == "session.max_time_ns") c.max_time_ns = value;
        else if (key == "session.markout_horizon_ns") c.markout_horizon_ns = value;
        else if (key == "session.fee_per_qty") c.fee_per_qty = value;
        else if (key == "fair.step_mean_ns") c.fair_step_mean_ns = value;
        else if (key == "fair.band_ticks") c.fair_band_ticks = value;
        else return false;
        return true;
    }

    static constexpr const char* kSessionParamNames =
        "session.seed session.max_requests session.max_time_ns session.markout_horizon_ns\n"
        "  session.fee_per_qty fair.step_mean_ns fair.band_ticks";

private:
    struct Delivery {
        SimTime at = 0;
        ExecEvent e{};
    };

    struct Participant {
        BookMirror mirror{};
        std::deque<Delivery> feed;                       // events not yet visible to it
        std::deque<std::pair<SimTime, Price>> signal_feed; // same, for the informed one
        std::unordered_set<OrderId> awaiting;            // sent, not yet seen acked
        OwnerId owner = 0;
        SimTime reaction = 0;
        SimTime timer = 0;
        SimTime pending_event = kNever;
        SimTime pending_timer = kNever;
        SimTime pending_signal = kNever;
        WakeReason pending_reason = WakeReason::Open;
        Price signal_known = 0;
        std::uint64_t wakes = 0;
        bool informed = false;
    };

    static void sink_fn(void* ctx, const ExecEvent& e) {
        auto* s = static_cast<Session*>(ctx);
        if (s->buf_.size() == s->buf_.capacity()) ++s->stats_.sink_growths;
        s->buf_.push_back(e);
    }

    static std::uint64_t mix(std::uint64_t h, std::uint64_t v) noexcept {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 0x100000001b3ULL;
        }
        return h;
    }

    void on_fair_value_step(SimTime at) {
        fv_.step(at);
        ++stats_.fair_steps;
        stats_.wall_hits = fv_.wall_hits();
        sample_gap();
        ledger_.resolve_due(at, fv_.value());
        sched_.push(at + fv_.next_interval(), WakeKind::FairValueStep);
        for (std::size_t i = 0; i < parts_.size(); ++i) {
            Participant& p = parts_[i];
            if (!p.informed) continue;
            // The new value is delivered to it like anything else: with its own
            // delay. It never reads fv_ directly, so it is informed but not
            // clairvoyant.
            p.signal_feed.push_back({at, fv_.value()});
            if (p.pending_signal == kNever) {
                p.pending_signal = at + p.reaction;
                sched_.push(p.pending_signal, WakeKind::BotSignal, static_cast<std::uint16_t>(i));
            }
        }
    }

    // Brings a participant's view up to `cutoff` — and no further. Everything a
    // bot knows passes through here.
    void advance_feed(Participant& p, SimTime cutoff) {
        while (!p.feed.empty() && p.feed.front().at <= cutoff) {
            const ExecEvent& e = p.feed.front().e;
            p.mirror.apply(e);
            // An order it sent is no longer in flight once it has seen the
            // outcome, whichever way that went.
            if (e.owner == p.owner && (e.type == EventType::Ack || e.type == EventType::Rejected)) {
                p.awaiting.erase(e.order_id);
            }
            p.feed.pop_front();
        }
        while (!p.signal_feed.empty() && p.signal_feed.front().first <= cutoff) {
            p.signal_known = p.signal_feed.front().second;
            p.signal_feed.pop_front();
        }
    }

    void run_bot(const Wake& w) {
        Participant& p = parts_[w.who];
        const SimTime now = w.at;

        WakeReason reason = p.pending_reason;
        switch (w.kind) {
            case WakeKind::BotTimer:
                reason = WakeReason::Timer;
                p.pending_timer = kNever;
                if (p.timer > 0) {
                    p.pending_timer = now + p.timer;
                    sched_.push(p.pending_timer, WakeKind::BotTimer, w.who);
                }
                break;
            case WakeKind::BotSignal:
                reason = WakeReason::Signal;
                p.pending_signal = kNever;
                break;
            default:
                p.pending_event = kNever;
                break;
        }

        const SimTime cutoff = now - p.reaction;
        advance_feed(p, cutoff);

        MarketView v;
        v.now_ns = now;
        v.as_of_ns = cutoff;
        v.book = &p.mirror;
        v.reason = reason;
        v.instr = instr_;
        v.tick = cfg_.tick;
        v.my_order_count = p.mirror.my_orders(p.owner, my_buf_, kMaxMyOrders);
        v.my_orders = my_buf_;
        v.in_flight = static_cast<std::uint32_t>(p.awaiting.size());
        const Account& a = ledger_.at(w.who);
        v.position = a.position;
        v.cash = a.cash;
        v.fills = a.fills;
        v.rejects = a.rejects;
        v.signal = p.informed ? p.signal_known : 0;
        ++p.wakes;

        const ActionBatch batch = roster_.bots[w.who]->on_event(v);
        stats_.action_drops += batch.dropped;
        for (std::uint8_t i = 0; i < batch.n; ++i) submit(w.who, batch.a[i], now);
    }

    void submit(std::size_t who, const Action& act, SimTime at) {
        Participant& p = parts_[who];
        OrderRequest req;
        bool is_new = true;
        switch (act.kind) {
            case Action::Kind::Limit:
                if (act.qty == 0) return;
                req = limit_order(next_order_id_++, instr_, p.owner, act.side, act.price, act.qty, act.tif);
                break;
            case Action::Kind::Market:
                if (act.qty == 0) return;
                req = market_order(next_order_id_++, instr_, p.owner, act.side, act.qty);
                break;
            case Action::Kind::Cancel:
                if (act.cancel_id == 0) return;
                req = cancel_order(act.cancel_id, instr_, p.owner);
                is_new = false;
                break;
            case Action::Kind::None:
                return;
        }
        // The order reaches the engine at the instant the bot acted: the delay
        // is already spent, in the wake that produced this decision.
        const std::uint64_t seq = sched_.push(at, WakeKind::OrderArrival, static_cast<std::uint16_t>(who));
        pending_req_.emplace(seq, req);
        ++ledger_.at(who).orders_sent;
        if (is_new) p.awaiting.insert(req.order_id);
    }

    void deliver_order(const Wake& w) {
        const auto it = pending_req_.find(w.seq);
        if (it == pending_req_.end()) return; // cannot happen; cheaper than asserting
        const OrderRequest req = it->second;
        pending_req_.erase(it);
        const SimTime now = w.at;

        const Price pre_bid = top(Side::Buy);
        const Price pre_ask = top(Side::Sell);

        buf_.clear();
        engine_->process(req, EventSink(this, &sink_fn));
        ++stats_.requests;

        bool own[kMaxParticipants] = {};
        for (const ExecEvent& e : buf_) {
            ev_hash_.add(e);
            time_hash_ = mix(time_hash_, static_cast<std::uint64_t>(now));
            ++stats_.events;

            switch (e.type) {
                case EventType::Trade: {
                    const std::size_t taker = account_of(e.owner);
                    const std::size_t maker = account_of(e.maker_owner);
                    ledger_.on_trade(taker, maker, e.side, e.price, e.qty, now, fv_.value());
                    ++stats_.trades;
                    stats_.volume += e.qty;
                    push_tape(now, e, taker, maker);
                    mark(own, taker);
                    mark(own, maker);
                    break;
                }
                case EventType::Rejected: {
                    const std::size_t who = account_of(e.owner);
                    if (who < parts_.size()) ++ledger_.at(who).rejects;
                    ++stats_.rejects;
                    if (static_cast<RejectReason>(e.reason) == RejectReason::BookFull) ++stats_.book_full_rejects;
                    mark(own, who);
                    break;
                }
                case EventType::Cancelled: {
                    const std::size_t who = account_of(e.owner);
                    if (static_cast<CancelReason>(e.reason) == CancelReason::SelfTrade) {
                        if (who < parts_.size()) ++ledger_.at(who).self_trade_cancels;
                        ++stats_.self_trade_cancels;
                    }
                    mark(own, who);
                    break;
                }
                case EventType::Ack:
                    mark(own, account_of(e.owner));
                    break;
                case EventType::EndOfStream:
                    break;
            }

            // Every participant's feed gets every event. Each will see it at its
            // own delay, which is the only thing that differs between them.
            for (Participant& p : parts_) p.feed.push_back(Delivery{now, e});
        }

        const bool touch_moved = top(Side::Buy) != pre_bid || top(Side::Sell) != pre_ask;
        for (std::size_t i = 0; i < parts_.size(); ++i) {
            Participant& p = parts_[i];
            const bool mine = i < kMaxParticipants && own[i];
            if (!mine && !touch_moved) continue;
            if (p.pending_event != kNever) continue; // one outstanding look per bot
            p.pending_reason = mine ? WakeReason::Own : WakeReason::Touch;
            p.pending_event = now + p.reaction;
            sched_.push(p.pending_event, WakeKind::BotEvent, static_cast<std::uint16_t>(i));
        }

        ledger_.resolve_due(now, fv_.value());
    }

    // One sample of |mid - V| in ticks, taken when the fundamental moves.
    void sample_gap() {
        const Price b = top(Side::Buy);
        const Price a = top(Side::Sell);
        if (b == 0 || a == 0) {
            ++stats_.gap_unpriced;
            return;
        }
        const Price mid = (a + b) / 2;
        const Price d = mid > fv_.value() ? mid - fv_.value() : fv_.value() - mid;
        const auto ticks = static_cast<std::uint64_t>(cfg_.tick <= 0 ? d : d / cfg_.tick);
        ++stats_.gap_samples;
        stats_.gap_sum_ticks += ticks;
        if (ticks > stats_.gap_max_ticks) stats_.gap_max_ticks = ticks;
    }

    [[nodiscard]] Price top(Side s) const noexcept {
        return engine_->depth(instr_, s) == 0 ? 0 : engine_->level(instr_, s, 0).price;
    }

    // Owner ids are roster index + 1, so 0 is "nobody" and never an account.
    [[nodiscard]] std::size_t account_of(OwnerId owner) const noexcept {
        return owner == 0 ? parts_.size() : static_cast<std::size_t>(owner - 1);
    }

    static void mark(bool* own, std::size_t who) noexcept {
        if (who < kMaxParticipants) own[who] = true;
    }

    void push_tape(SimTime at, const ExecEvent& e, std::size_t taker, std::size_t maker) {
        if (cfg_.tape_capacity == 0) return;
        if (tape_.size() >= cfg_.tape_capacity) tape_.erase(tape_.begin());
        tape_.push_back(TapePrint{at, e.price, e.qty, e.side, static_cast<std::uint16_t>(taker),
                                  static_cast<std::uint16_t>(maker)});
    }

    static constexpr std::size_t kMaxParticipants = 64;

    SessionConfig cfg_{};
    std::unique_ptr<MatchingEngine> engine_;
    InstrumentId instr_ = 0;
    Scheduler sched_;
    FairValue fv_;
    Ledger ledger_;
    Roster roster_;
    std::vector<Participant> parts_;
    std::unordered_map<std::uint64_t, OrderRequest> pending_req_;
    std::vector<ExecEvent> buf_;                 // one request's events
    BookMirror::OrderRef my_buf_[kMaxMyOrders]{};
    std::vector<TapePrint> tape_;
    EventHasher ev_hash_{};
    std::uint64_t time_hash_ = 0xcbf29ce484222325ULL;
    SessionStats stats_{};
    OrderId next_order_id_ = 1;
    bool done_ = false;
};

} // namespace px::arena
