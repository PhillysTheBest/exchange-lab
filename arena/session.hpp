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
// STRATEGIES RUN OUTSIDE THE EVENT SINK. The sink the engine is handed appends to a
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
// floating point or wall-clock read inside a session, and the fact that a strategy
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
#include "arena/strategy.hpp"
#include "arena/strategies/registry.hpp"
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

    // How much simulated time may pass with no request reaching the engine
    // before the session is declared over. A backstop, not a knob: a session
    // with flow sees a request every few microseconds. 0 disables it, which
    // Session::check() then refuses unless max_time_ns bounds the session.
    SimTime stall_limit_ns = 100'000'000; // 100 ms of simulated time

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
    // True when the session ended because no request arrived for
    // stall_limit_ns of simulated time, rather than because it finished. Its
    // numbers are not comparable with a session that ran to completion.
    bool stalled = false;
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
    // The most participants one session can hold. Part of the contract rather
    // than an implementation detail: see check() for why it is a hard ceiling
    // and not a resizable buffer.
    static constexpr std::size_t kMaxParticipants = 64;

    Session() { buf_.reserve(kEventBufferReserve); }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Why a session can be refused, rather than silently misbehaving. Both of
    // these were found by testing the arena rather than reasoning about it, and
    // both are rejected at construction on purpose: see the note on each.
    enum class StartError : std::uint8_t {
        None = 0,
        TooManyParticipants, // roster larger than kMaxParticipants
        NoStallBound,        // nothing would ever stop a session that goes quiet
    };

    [[nodiscard]] static const char* explain(StartError e) noexcept {
        switch (e) {
            case StartError::None:
                return "ok";
            case StartError::TooManyParticipants:
                static_assert(kMaxParticipants == 64, "update this message with the limit");
                return "too many participants: the session supports at most 64";
            case StartError::NoStallBound:
                return "session.max_time_ns and session.stall_limit_ns are both 0, so a session "
                       "that goes quiet would never end: set one of them";
        }
        return "?";
    }

    // Checks a roster and config without building anything, so a caller can
    // refuse before it starts rather than hang or mis-trade.
    //
    // TOO MANY PARTICIPANTS. deliver_order() marks which strategies an event concerned
    // in a fixed `bool own[kMaxParticipants]` array. Past that bound the marks
    // were silently dropped, so those strategies never got an own-event wake: measured
    // with 104 identical noise traders, participants 0-63 took 151.9 fills each
    // and the rest took 55.9. Rejecting is deliberately chosen over widening the
    // array into a dynamic bitset, because the bound is not the real cost —
    // changing how wakes are marked would change wake *ordering* for large
    // rosters, which moves every session hash and invalidates the committed
    // 24-seed evidence. Refusing at the boundary is provably no behavioural
    // change at or below 64. Raising the ceiling is a separate change with its
    // own re-measurement.
    //
    // A SESSION THAT GOES QUIET. A session ends on max_requests or max_time_ns.
    // If the roster stops submitting orders, max_requests is unreachable, and
    // the fundamental-value step always reschedules itself, so the scheduler
    // never empties and run() never returns. Confirmed by testing: a roster of
    // nothing but the market maker hangs, because once its quotes are where it
    // wants them it has nothing left to do.
    //
    // A static check cannot catch this, and the first attempt at one was wrong.
    // Whether a participant ever submits an order is a property of its code, not
    // of its configuration — the lone maker in that hang *has* a heartbeat
    // timer, it simply returns an empty batch every time it fires. So the bound
    // is `stall_limit_ns`: if that much *simulated* time passes with no request
    // reaching the engine, the session is over and says so in `stats().stalled`.
    //
    // That is a quiescence detector, and it is deterministic, which is the only
    // reason it is allowed here. It compares two simulated timestamps, so it
    // triggers at exactly the same point on every machine and every replay; a
    // wall-clock timeout would not, and would have broken the replay contract.
    // A session that does generate flow never approaches it: requests arrive
    // every few microseconds against a default limit of 100 ms.
    [[nodiscard]] static StartError check(const SessionConfig& scfg, const Roster& roster) {
        if (roster.size() > kMaxParticipants) return StartError::TooManyParticipants;
        if (scfg.max_time_ns <= 0 && scfg.stall_limit_ns <= 0) return StartError::NoStallBound;
        return StartError::None;
    }

    // The usual way in: build the registry's roster from its config.
    StartError start(const SessionConfig& scfg, const RosterConfig& rcfg) {
        RosterConfig r = rcfg;
        r.anchor = scfg.start_price;
        r.tick = scfg.tick;
        return start(scfg, build_roster(r));
    }

    // The same session with a roster assembled by the caller. This is how
    // test_arena puts a probe participant in the market, and it is the proof
    // that the strategy contract is genuinely open: nothing in here knows which
    // strategies it is running.
    //
    // Returns the reason it refused, or StartError::None. A refused session is
    // left done() and empty, so a caller that ignores the result gets an
    // immediately-finished session rather than a hang or wrong numbers.
    StartError start(const SessionConfig& scfg, Roster roster) {
        const StartError bad = check(scfg, roster);
        if (bad != StartError::None) {
            parts_.clear();
            roster_ = Roster{};
            stats_ = SessionStats{};
            done_ = true;
            return bad;
        }
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
            p.reaction = roster_.strategies[i]->reaction_ns();
            if (p.reaction < 0) p.reaction = 0;
            p.timer = roster_.strategies[i]->timer_ns();
            p.informed = roster_.informed[i];
            p.signal_known = p.informed ? cfg_.start_price : 0;
            roster_.strategies[i]->reset(cfg_.seed + 0x9E37U * (i + 1));
        }

        ev_hash_ = EventHasher{};
        time_hash_ = 0xcbf29ce484222325ULL;
        stats_ = SessionStats{};
        next_order_id_ = 1;
        last_request_at_ = 0;
        done_ = false;

        // The first fundamental jump, each strategy's heartbeat, and one opening look
        // per strategy so that somebody quotes into an empty book.
        sched_.push(fv_.next_interval(), WakeKind::FairValueStep);
        for (std::size_t i = 0; i < parts_.size(); ++i) {
            Participant& p = parts_[i];
            const auto who = static_cast<std::uint16_t>(i);
            if (p.timer > 0) {
                p.pending_timer = p.timer;
                sched_.push(p.pending_timer, WakeKind::StrategyTimer, who);
            }
            p.pending_reason = WakeReason::Open;
            p.pending_event = p.reaction;
            sched_.push(p.pending_event, WakeKind::StrategyEvent, who);
        }
        return StartError::None;
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
        // Quiet for long enough that nothing more is going to happen. Compared
        // in simulated time, so it fires at the same instant on every replay.
        if (cfg_.stall_limit_ns > 0 && sched_.next_time() - last_request_at_ > cfg_.stall_limit_ns) {
            stats_.stalled = true;
            done_ = true;
            return false;
        }

        const Wake w = sched_.pop();
        switch (w.kind) {
            case WakeKind::FairValueStep: on_fair_value_step(w.at); break;
            case WakeKind::StrategyEvent:
            case WakeKind::StrategyTimer:
            case WakeKind::StrategySignal: run_strategy(w); break;
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
    [[nodiscard]] const char* name_of(std::size_t i) const noexcept { return roster_.strategies[i]->name(); }
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
        else if (key == "session.stall_limit_ns") c.stall_limit_ns = value;
        else if (key == "session.fee_per_qty") c.fee_per_qty = value;
        else if (key == "fair.step_mean_ns") c.fair_step_mean_ns = value;
        else if (key == "fair.band_ticks") c.fair_band_ticks = value;
        else return false;
        return true;
    }

    // Reads one session field by name, the counterpart of set_param(), so a run
    // can record every field it ran with under the names --set= takes.
    [[nodiscard]] static bool get_param(const SessionConfig& c, std::string_view key, std::int64_t& out) {
        if (key == "session.seed") out = static_cast<std::int64_t>(c.seed);
        else if (key == "session.max_requests") out = static_cast<std::int64_t>(c.max_requests);
        else if (key == "session.max_time_ns") out = c.max_time_ns;
        else if (key == "session.markout_horizon_ns") out = c.markout_horizon_ns;
        else if (key == "session.stall_limit_ns") out = c.stall_limit_ns;
        else if (key == "session.fee_per_qty") out = c.fee_per_qty;
        else if (key == "fair.step_mean_ns") out = c.fair_step_mean_ns;
        else if (key == "fair.band_ticks") out = c.fair_band_ticks;
        else return false;
        return true;
    }

    static constexpr const char* kSessionParamNames =
        "session.seed session.max_requests session.max_time_ns session.stall_limit_ns\n"
        "  session.markout_horizon_ns session.fee_per_qty fair.step_mean_ns fair.band_ticks";

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
                sched_.push(p.pending_signal, WakeKind::StrategySignal, static_cast<std::uint16_t>(i));
            }
        }
    }

    // Brings a participant's view up to `cutoff` — and no further. Everything a
    // strategy knows passes through here.
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

    void run_strategy(const Wake& w) {
        Participant& p = parts_[w.who];
        const SimTime now = w.at;

        WakeReason reason = p.pending_reason;
        switch (w.kind) {
            case WakeKind::StrategyTimer:
                reason = WakeReason::Timer;
                p.pending_timer = kNever;
                if (p.timer > 0) {
                    p.pending_timer = now + p.timer;
                    sched_.push(p.pending_timer, WakeKind::StrategyTimer, w.who);
                }
                break;
            case WakeKind::StrategySignal:
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

        const ActionBatch batch = roster_.strategies[w.who]->on_event(v);
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
        // The order reaches the engine at the instant the strategy acted: the delay
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
        last_request_at_ = now;

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
            if (p.pending_event != kNever) continue; // one outstanding look per strategy
            p.pending_reason = mine ? WakeReason::Own : WakeReason::Touch;
            p.pending_event = now + p.reaction;
            sched_.push(p.pending_event, WakeKind::StrategyEvent, static_cast<std::uint16_t>(i));
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
    SimTime last_request_at_ = 0;
    bool done_ = false;
};

} // namespace px::arena
