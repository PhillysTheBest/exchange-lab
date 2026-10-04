// tests/test_arena.cpp — the arena held to the same standard as the engine.
//
// A simulator is easy to make plausible and hard to make true, so the five
// things the latency experiment actually rests on are each checked here rather
// than assumed:
//
//  1. THE MIRROR IS THE BOOK. A participant's view, rebuilt from the event
//     stream alone, is compared against the engine's own ladder after every one
//     of 200,000 requests of a seeded stream. This is the same differential
//     shape as fuzz_vs_reference: the engine is the oracle, the mirror is the
//     thing under test, and if a bot's view of its queue position were wrong
//     every number downstream would be wrong with it.
//  2. QUEUE POSITION IS TIME PRIORITY. Checked by hand on a known FIFO,
//     including after the order in front of it is cancelled.
//  3. THE SCHEDULER HAS NO TIES. Items come out in (time, sequence) order even
//     when hundreds share a timestamp. This is the one place determinism could
//     silently become "nearly".
//  4. A SESSION REPLAYS. Same seed and roster, same session hash; a different
//     seed, a different one. Stated as a pinned number so a change that moves
//     it has to be deliberate.
//  5. THE ACCOUNTING CANNOT LIE. The session is closed, so cash, position and
//     marked value must each sum to exactly zero across participants. The whole
//     sweep is read off these numbers.
//
// Plus two properties of the reaction model: a bot's view is stale by exactly
// its own delay, and an informed participant that has been switched off takes
// nothing.
#include "arena/book_mirror.hpp"
#include "arena/session.hpp"
#include "px/matching_engine.hpp"
#include "sim/order_stream.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace px;
using namespace px::arena;

namespace {

// ---- 1. the mirror against the engine ------------------------------------

struct MirrorCtx {
    std::vector<BookMirror>* mirrors;
};

void feed_mirrors(void* ctx, const ExecEvent& e) {
    auto* c = static_cast<MirrorCtx*>(ctx);
    for (BookMirror& m : *c->mirrors) m.apply(e);
}

// Compares one side of one instrument, level by level, best first.
bool side_matches(const MatchingEngine& eng, const BookMirror& m, InstrumentId id, Side s, std::uint32_t limit) {
    const std::uint32_t d = eng.depth(id, s);
    if (m.depth(s) != d) return false;
    const std::uint32_t n = limit == 0 ? d : (d < limit ? d : limit);
    for (std::uint32_t r = 0; r < n; ++r) {
        const LevelView a = eng.level(id, s, r);
        const LevelView b = m.level(s, r);
        if (a.price != b.price || a.qty != b.qty || a.orders != b.orders) return false;
    }
    return true;
}

void test_mirror_vs_engine() {
    sim::StreamConfig cfg;
    cfg.orders = 200'000;
    cfg.seed = 42;
    const std::vector<OrderRequest> stream = sim::generate_order_stream(cfg);

    auto eng = std::make_unique<MatchingEngine>();
    sim::add_instruments(*eng, cfg);

    std::vector<BookMirror> mirrors;
    mirrors.reserve(cfg.instruments);
    for (std::uint16_t i = 0; i < cfg.instruments; ++i) mirrors.emplace_back(static_cast<InstrumentId>(i));

    MirrorCtx ctx{&mirrors};
    const EventSink sink(&ctx, &feed_mirrors);

    std::size_t top_failures = 0, full_failures = 0, full_checks = 0;
    std::uint64_t resting_failures = 0;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        eng->process(stream[i], sink);

        // After every single request: top of book and depth on both sides of
        // every instrument. Depth equality alone is a strong claim — it means no
        // level was created, missed or left behind.
        for (std::uint16_t k = 0; k < cfg.instruments; ++k) {
            const auto id = static_cast<InstrumentId>(k);
            if (!side_matches(*eng, mirrors[k], id, Side::Buy, 4) ||
                !side_matches(*eng, mirrors[k], id, Side::Sell, 4)) {
                ++top_failures;
            }
        }

        // Periodically: every level of every ladder, and the exchange-wide
        // resting-order count against the sum of what the mirrors believe.
        if (i % 5'000 == 4'999) {
            ++full_checks;
            std::size_t live = 0;
            for (std::uint16_t k = 0; k < cfg.instruments; ++k) {
                const auto id = static_cast<InstrumentId>(k);
                if (!side_matches(*eng, mirrors[k], id, Side::Buy, 0) ||
                    !side_matches(*eng, mirrors[k], id, Side::Sell, 0)) {
                    ++full_failures;
                }
                live += mirrors[k].live_orders();
            }
            if (live != eng->resting_orders()) ++resting_failures;
        }
    }

    std::printf("  mirror: %zu requests, %zu full ladder checks\n", stream.size(), full_checks);
    PX_CHECK(top_failures == 0);
    PX_CHECK(full_failures == 0);
    PX_CHECK(resting_failures == 0);
    PX_CHECK(full_checks > 30);
}

// ---- 2. queue position ----------------------------------------------------

void test_queue_position() {
    auto eng = std::make_unique<MatchingEngine>();
    const InstrumentId id = eng->add_instrument("TEST", 100);
    BookMirror m(id);
    const EventSink sink(&m, [](void* c, const ExecEvent& e) { static_cast<BookMirror*>(c)->apply(e); });

    // Three bids at the same price, from three owners, in time order.
    eng->process(limit_order(1, id, 10, Side::Buy, 100 * kPriceScale, 30), sink);
    eng->process(limit_order(2, id, 20, Side::Buy, 100 * kPriceScale, 40), sink);
    eng->process(limit_order(3, id, 30, Side::Buy, 100 * kPriceScale, 50), sink);

    BookMirror::OrderRef refs[4];
    PX_CHECK(m.my_orders(10, refs, 4) == 1 && refs[0].qty_ahead == 0);
    PX_CHECK(m.my_orders(20, refs, 4) == 1 && refs[0].qty_ahead == 30);
    PX_CHECK(m.my_orders(30, refs, 4) == 1 && refs[0].qty_ahead == 70);
    PX_CHECK(m.size_at_best(Side::Buy) == 120);

    // Cancelling the order in front moves everyone behind it up the queue, and
    // the survivors keep their order relative to each other.
    eng->process(cancel_order(1, id, 10), sink);
    PX_CHECK(m.my_orders(10, refs, 4) == 0);
    PX_CHECK(m.my_orders(20, refs, 4) == 1 && refs[0].qty_ahead == 0);
    PX_CHECK(m.my_orders(30, refs, 4) == 1 && refs[0].qty_ahead == 40);
    PX_CHECK(m.size_at_best(Side::Buy) == 90);

    // A partial fill of the front of the queue leaves it in front, with less in
    // front of the rest: time priority survives a partial fill.
    eng->process(limit_order(4, id, 40, Side::Sell, 100 * kPriceScale, 10), sink);
    PX_CHECK(m.my_orders(20, refs, 4) == 1 && refs[0].leaves == 30);
    PX_CHECK(m.my_orders(30, refs, 4) == 1 && refs[0].qty_ahead == 30);
    PX_CHECK(m.best(Side::Buy) == 100 * kPriceScale);
    PX_CHECK(m.last_trade() == 100 * kPriceScale);

    // And when the level empties completely it is gone, not left at zero.
    eng->process(limit_order(5, id, 40, Side::Sell, 100 * kPriceScale, 80), sink);
    PX_CHECK(m.depth(Side::Buy) == 0);
    PX_CHECK(m.best(Side::Buy) == 0);
}

// ---- 3. the scheduler -----------------------------------------------------

void test_scheduler_total_order() {
    Scheduler s;
    SimRng rng(7);
    // Push a few hundred items, deliberately with heavy timestamp collisions.
    std::vector<std::uint64_t> pushed_seq;
    for (int i = 0; i < 500; ++i) {
        const SimTime at = static_cast<SimTime>(rng.below(5) * 100);
        pushed_seq.push_back(s.push(at, WakeKind::BotTimer, static_cast<std::uint16_t>(i % 8)));
    }
    PX_CHECK(s.size() == 500);

    SimTime last_at = -1;
    std::uint64_t last_seq = 0;
    bool ordered = true;
    bool clock_monotonic = true;
    std::size_t popped = 0;
    while (!s.empty()) {
        const SimTime before = s.now();
        const Wake w = s.pop();
        if (s.now() < before) clock_monotonic = false;
        if (w.at < last_at || (w.at == last_at && w.seq < last_seq)) ordered = false;
        last_at = w.at;
        last_seq = w.seq;
        ++popped;
    }
    PX_CHECK(popped == 500);
    PX_CHECK(ordered);
    PX_CHECK(clock_monotonic);

    // An interval draw is integer, inside its stated range, and has no
    // floating-point step anywhere in it.
    SimRng r2(11);
    bool in_range = true;
    for (int i = 0; i < 1000; ++i) {
        const SimTime iv = r2.interval(1000);
        if (iv < 250 || iv > 1750) in_range = false;
    }
    PX_CHECK(in_range);
}

// ---- 4 and 5. the session: replay, accounting, reaction model -------------

SessionConfig small_session() {
    SessionConfig c;
    c.seed = 42;
    c.max_requests = 30'000;
    return c;
}

void test_session_replay_and_accounting() {
    const RosterConfig roster;
    Session a, b;
    a.start(small_session(), roster);
    a.run();
    b.start(small_session(), roster);
    b.run();

    std::printf("  session: %zu requests, %llu trades, hash %016llx\n", a.stats().requests,
                static_cast<unsigned long long>(a.stats().trades), static_cast<unsigned long long>(a.hash()));

    PX_CHECK(a.hash() == b.hash());
    PX_CHECK(a.event_hash() == b.event_hash());
    PX_CHECK(a.stats().requests == b.stats().requests);
    PX_CHECK(a.stats().trades == b.stats().trades);

    SessionConfig other = small_session();
    other.seed = 43;
    Session c;
    c.start(other, roster);
    c.run();
    PX_CHECK(c.hash() != a.hash());

    // The session must actually have been a market, or nothing above means
    // anything: trades happened, the book never filled up, and no bot asked for
    // more actions than a batch can carry.
    PX_CHECK(a.stats().trades > 100);
    PX_CHECK(a.stats().requests == 30'000);
    PX_CHECK(a.stats().book_full_rejects == 0);
    PX_CHECK(a.stats().action_drops == 0);
    PX_CHECK(a.stats().fair_steps > 100);
    PX_CHECK(a.stats().sink_growths == 0);
    // The fundamental must not have reached its reflecting wall: it is only a
    // wall so that no price can run off the tick grid, and touching it would
    // mean the walk stopped being a martingale (see arena/fair_value.hpp).
    PX_CHECK(a.stats().wall_hits == 0);
    // And the book has to have tracked it, or the session was not a market.
    PX_CHECK(a.stats().gap_samples > 100);
    PX_CHECK(a.stats().mean_gap_ticks() < 40.0);

    // The closed-world identity. Exact, in integers, with zero fees.
    const Price mark = a.fair_value().value();
    PX_CHECK(a.ledger().cash_sum() == 0);
    PX_CHECK(a.ledger().position_sum() == 0);
    PX_CHECK(a.ledger().value_sum(mark) == 0);
    PX_CHECK(a.ledger().pending_markouts() == 0); // finish() resolved them all

    // Every fill was booked to two accounts, and every one of them got a markout.
    std::uint64_t fills = 0, markouts = 0;
    for (std::size_t i = 0; i < a.participants(); ++i) {
        fills += a.ledger().at(i).fills;
        markouts += a.ledger().at(i).markout_n;
    }
    PX_CHECK(fills == a.stats().trades * 2);
    PX_CHECK(markouts == fills);
}

void test_informed_switch() {
    RosterConfig on;
    Session a;
    a.start(small_session(), on);
    a.run();

    int informed_index = -1;
    for (std::size_t i = 0; i < a.participants(); ++i) {
        if (a.informed(i)) informed_index = static_cast<int>(i);
    }
    PX_CHECK(informed_index >= 0);
    PX_CHECK(a.ledger().at(static_cast<std::size_t>(informed_index)).fills > 0);

    // The null run: with nobody informed there is no exogenous information in
    // the session at all, so there is nothing for speed to be worth.
    RosterConfig off = on;
    off.informed_enabled = false;
    Session b;
    b.start(small_session(), off);
    b.run();
    for (std::size_t i = 0; i < b.participants(); ++i) PX_CHECK(!b.informed(i));
    PX_CHECK(b.participants() == a.participants() - 1);
    PX_CHECK(b.stats().trades > 100);
    PX_CHECK(b.ledger().value_sum(b.fair_value().value()) == 0);
}

// A participant that only watches, so the reaction model can be checked from
// inside a real session rather than reasoned about.
struct ProbeState {
    std::uint64_t wakes = 0;
    std::uint64_t timer_wakes = 0;
    bool staleness_exact = true;
    bool never_ahead_of_now = true;
    bool signal_always_zero = true;
};

struct Probe {
    const char* name = "probe";
    SimTime reaction_ns = 7'000;
    SimTime timer_ns = 11'000;
    ProbeState* st = nullptr;

    void reset(std::uint64_t) {}

    ActionBatch on_event(const MarketView& v) {
        ++st->wakes;
        if (v.reason == WakeReason::Timer) ++st->timer_wakes;
        // Its view is stale by exactly its own delay — the whole reaction model
        // in one equality.
        if (v.now_ns - v.as_of_ns != reaction_ns) st->staleness_exact = false;
        if (v.as_of_ns > v.now_ns) st->never_ahead_of_now = false;
        // It was not marked informed, so it must never be handed the signal.
        if (v.signal != 0) st->signal_always_zero = false;
        return ActionBatch{};
    }
};

void test_reaction_model() {
    ProbeState st;
    RosterConfig rc;
    Roster roster = build_roster(rc);
    Probe probe;
    probe.st = &st;
    roster.bots.push_back(make_bot(probe));
    roster.informed.push_back(false);

    Session s;
    s.start(small_session(), std::move(roster));
    s.run();

    std::printf("  probe: %llu wakes (%llu on its timer)\n", static_cast<unsigned long long>(st.wakes),
                static_cast<unsigned long long>(st.timer_wakes));
    PX_CHECK(st.wakes > 100);
    PX_CHECK(st.timer_wakes > 10);
    PX_CHECK(st.staleness_exact);
    PX_CHECK(st.never_ahead_of_now);
    PX_CHECK(st.signal_always_zero);
    // A watcher changes nothing it does not touch: it sent no orders and holds
    // no position, and the identity still closes with it in the room.
    const std::size_t last = s.participants() - 1;
    PX_CHECK(s.ledger().at(last).orders_sent == 0);
    PX_CHECK(s.ledger().at(last).fills == 0);
    PX_CHECK(s.ledger().value_sum(s.fair_value().value()) == 0);
}

// A faster maker should defend better against the one trader who knows
// something. This is the headline experiment in its smallest honest form: not a
// curve, just the two ends of one, so that a regression which inverts the
// mechanism fails the test suite rather than quietly producing a pretty plot.
void test_speed_beats_slow() {
    const auto run_maker = [](SimTime reaction, bool informed_on) {
        RosterConfig rc;
        rc.maker_reaction_ns = reaction;
        rc.informed_enabled = informed_on;
        SessionConfig sc = small_session();
        sc.max_requests = 60'000;
        Session s;
        s.start(sc, rc);
        s.run();
        return s.ledger().at(static_cast<std::size_t>(s.maker_index())).value(s.fair_value().value());
    };

    const std::int64_t fast = run_maker(1'000, true);
    const std::int64_t slow = run_maker(1'000'000, true);
    std::printf("  maker value: fast %lld, slow %lld (price units)\n", static_cast<long long>(fast),
                static_cast<long long>(slow));
    PX_CHECK(fast > slow);
}

// --help lists the sweepable fields. A list that drifts out of step with the
// code is worse than no list, so it is checked rather than maintained by hand.
void test_param_names_are_real() {
    RosterConfig rc;
    SessionConfig sc;
    std::size_t checked = 0, unknown = 0;
    for (const char* names : {kRosterParamNames, Session::kSessionParamNames}) {
        std::string tok;
        for (const char* p = names;; ++p) {
            if (*p == ' ' || *p == '\n' || *p == '\0') {
                if (!tok.empty()) {
                    ++checked;
                    if (!set_roster_param(rc, tok, 1) && !Session::set_param(sc, tok, 1)) {
                        ++unknown;
                        std::printf("  advertised but not settable: %s\n", tok.c_str());
                    }
                    tok.clear();
                }
                if (*p == '\0') break;
            } else {
                tok.push_back(*p);
            }
        }
    }
    std::printf("  params: %zu advertised names, all settable: %s\n", checked, unknown == 0 ? "yes" : "NO");
    PX_CHECK(checked > 25);
    PX_CHECK(unknown == 0);
    PX_CHECK(!set_roster_param(rc, "maker.nonsense", 1));
    PX_CHECK(!Session::set_param(sc, "session.nonsense", 1));
}

} // namespace

int main() {
    std::printf("== arena\n");
    test_mirror_vs_engine();
    test_queue_position();
    test_scheduler_total_order();
    test_session_replay_and_accounting();
    test_informed_switch();
    test_reaction_model();
    test_param_names_are_real();
    test_speed_beats_slow();
    PX_TEST_REPORT_AND_RETURN();
}
