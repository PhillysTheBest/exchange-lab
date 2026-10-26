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
//     thing under test, and if a strategy's view of its queue position were wrong
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
// Plus two properties of the reaction model: a strategy's view is stale by exactly
// its own delay, and an informed participant that has been switched off takes
// nothing.
#include "arena/book_mirror.hpp"
#include "arena/strategies/script.hpp"
#include "arena/session.hpp"
#include "arena/suite.hpp"
#include "arena/suite_report.hpp"
#include "arena/controls.hpp"
#include "arena/ui.hpp"
#include "px/matching_engine.hpp"
#include "sim/order_stream.hpp"
#include "test_harness.hpp"

// A session's time is simulated and must never read hardware. Everything a
// session, the suite or the report needs is included above, so if any of it
// pulled in the cycle counter or the spin-wait/affinity header, this fails to
// compile (docs/PLATFORMS.md).
#if defined(PX_CYCLE_CLOCK_HPP) || defined(PX_PLATFORM_HPP)
#error "an arena header includes px/cycle_clock.hpp or px/platform.hpp: a session must not read hardware time"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace px;
using namespace px::arena;

namespace {

// The composed bytes of the fixed scene test_ui_grid draws. Pinned so that an
// accidental change to the palette, a drawing primitive or the escape-emitting
// fails here; a deliberate visual change updates this number along with it.
constexpr std::uint64_t kPinnedFrameHash = 0xddfa48c44bff4de8ULL;

// The standard suite's fingerprint over the smoke block: its conditions plus
// every placebo session hash. Pinned so that a change to the engine, the
// session, a reference strategy, a default or the suite's own settings fails
// here and forces a deliberate suite version bump (docs/RUNNER.md).
constexpr std::uint64_t kPinnedSmokeFingerprint = 0xd461c30ab2446acfULL;
// The scenario families, likewise pinned: a change needs a version bump.
constexpr std::uint64_t kPinnedWideFingerprint = 0x9bb1a7c184ccb2d7ULL;
constexpr std::uint64_t kPinnedBusyFingerprint = 0x5d735a9faa384bf5ULL;
// Each family over the holdout block, from the placebo arm alone: no test runs a
// candidate there, which is what keeps it a holdout.
constexpr std::uint64_t kPinnedHoldoutFingerprint = 0x6e1fe05931a6f560ULL;
constexpr std::uint64_t kPinnedHoldoutWideFingerprint = 0x8d95ab9f5a9e2d80ULL;
constexpr std::uint64_t kPinnedHoldoutBusyFingerprint = 0xb66de703b5562711ULL;

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
        pushed_seq.push_back(s.push(at, WakeKind::StrategyTimer, static_cast<std::uint16_t>(i % 8)));
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
    // anything: trades happened, the book never filled up, and no strategy asked for
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
    roster.strategies.push_back(make_strategy(probe));
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

// ---- the two configurations that used to go wrong silently ---------------
//
// Both were found by testing the arena rather than reasoning about it, and both
// are pinned here because a bug that was found and not pinned gets found again
// by a reader, in a repository whose whole pitch is evidence.

void test_session_refuses_bad_configs() {
    // A roster past the participant ceiling. It used to be accepted, and the
    // participants past index 63 then never received an own-event wake: with 104
    // identical noise traders, the first 64 took 151.9 fills each and the rest
    // took 55.9. Wrong numbers that looked entirely plausible.
    {
        Roster big = build_roster(RosterConfig{});
        strategies::Noise filler;
        while (big.strategies.size() <= Session::kMaxParticipants) {
            big.strategies.push_back(make_strategy(filler));
            big.informed.push_back(false);
        }
        Session s;
        const Session::StartError e = s.start(small_session(), std::move(big));
        PX_CHECK(e == Session::StartError::TooManyParticipants);
        // A refused session is inert rather than half-built, so a caller that
        // ignores the result gets nothing instead of nonsense.
        PX_CHECK(s.done());
        PX_CHECK(s.participants() == 0);
        PX_CHECK(s.stats().requests == 0);
    }
    // Exactly at the ceiling is still allowed: the guard must bite at 65, not 64.
    {
        Roster full = build_roster(RosterConfig{});
        strategies::Noise filler;
        while (full.strategies.size() < Session::kMaxParticipants) {
            full.strategies.push_back(make_strategy(filler));
            full.informed.push_back(false);
        }
        PX_CHECK(full.strategies.size() == Session::kMaxParticipants);
        Session s;
        PX_CHECK(s.start(small_session(), std::move(full)) == Session::StartError::None);
        PX_CHECK(s.participants() == Session::kMaxParticipants);
    }
    // No bound of any kind. A session ends on max_requests, max_time_ns or the
    // stall limit; with the last two switched off a session that goes quiet
    // would run forever, so the configuration is refused instead.
    {
        SessionConfig c = small_session();
        c.max_time_ns = 0;
        c.stall_limit_ns = 0;
        Session s;
        PX_CHECK(s.start(c, RosterConfig{}) == Session::StartError::NoStallBound);
        PX_CHECK(s.done());
    }
    PX_CHECK(std::string(Session::explain(Session::StartError::None)) == "ok");
}

// A market with nobody to trade against. This used to hang: the maker quotes
// once, has nothing further to do, and no request ever arrives again — so
// max_requests stayed unreachable while the fundamental kept rescheduling
// itself. It now ends on the stall limit and says so.
void test_quiet_session_terminates() {
    RosterConfig rc;
    rc.rival_enabled = false;
    rc.informed_enabled = false;
    rc.momentum_enabled = false;
    rc.noise_count = 0; // nothing left but the maker
    SessionConfig sc = small_session();
    sc.max_requests = 100'000; // far more than this roster can ever produce

    Session s;
    PX_CHECK(s.start(sc, rc) == Session::StartError::None);
    s.run();

    std::printf("  quiet session: stalled=%d after %zu requests, clock %lld ns\n", s.stats().stalled ? 1 : 0,
                s.stats().requests, static_cast<long long>(s.now()));
    PX_CHECK(s.done());
    PX_CHECK(s.stats().stalled);
    PX_CHECK(s.stats().requests < 100);
    // It stopped shortly after the stall limit rather than running on, and the
    // bound is in simulated time, so this is exact rather than timing-dependent.
    PX_CHECK(s.now() <= sc.stall_limit_ns + sc.fair_step_mean_ns * 4);
    // And a normal session must never trip it.
    Session busy;
    PX_CHECK(busy.start(small_session(), RosterConfig{}) == Session::StartError::None);
    busy.run();
    PX_CHECK(!busy.stats().stalled);
}

// ---- the renderer ---------------------------------------------------------
//
// It makes no measured claim, so what is pinned is not how it looks but the
// invariants a layout edit could break without anyone noticing.
void test_ui_grid() {
    ui::Screen s(40, 12);
    s.desktop();

    // One cell per code point. The first version of text() advanced one byte at
    // a time, so every multi-byte glyph was torn across two or three cells and
    // rendered as replacement characters. This is that bug, pinned.
    s.text(0, 0, "ab\xe2\x96\x88\xe2\x96\x8c", ui::pal::ink, ui::pal::panel);
    PX_CHECK(s.glyph_at(0, 0) == "a");
    PX_CHECK(s.glyph_at(1, 0) == "b");
    PX_CHECK(s.glyph_at(2, 0) == "\xe2\x96\x88"); // U+2588, one cell
    PX_CHECK(s.glyph_at(3, 0) == "\xe2\x96\x8c"); // U+258C, one cell

    // `limit` counts columns, not bytes.
    s.text(0, 1, "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88", ui::pal::ink, ui::pal::panel, 2);
    PX_CHECK(s.glyph_at(0, 1) == "\xe2\x96\x88");
    PX_CHECK(s.glyph_at(1, 1) == "\xe2\x96\x88");
    PX_CHECK(s.glyph_at(2, 1) != "\xe2\x96\x88");

    // Drawing off every edge is clipped, not written out of bounds. Under ASan
    // this is the check that would catch a layout change writing past the grid.
    s.text(-5, 3, "clipped left", ui::pal::ink, ui::pal::panel);
    s.text(38, 4, "clipped right", ui::pal::ink, ui::pal::panel);
    s.text(0, -2, "above", ui::pal::ink, ui::pal::panel);
    s.text(0, 99, "below", ui::pal::ink, ui::pal::panel);
    s.window(ui::Rect{30, 9, 20, 10}, "OFF EDGE", ui::pal::teal);
    PX_CHECK(s.glyph_at(39, 4).size() > 0);
    PX_CHECK(s.glyph_at(40, 4).empty()); // past the right edge: nothing there

    // A bar fills in eighths, so the full blocks are the floor of the fraction.
    s.bar(0, 6, 8, 0.5, ui::pal::teal);
    PX_CHECK(s.glyph_at(0, 6) == "\xe2\x96\x88");
    PX_CHECK(s.glyph_at(3, 6) == "\xe2\x96\x88");
    PX_CHECK(s.glyph_at(4, 6) != "\xe2\x96\x88");
    s.bar(0, 7, 8, 0.0, ui::pal::teal);
    PX_CHECK(s.glyph_at(0, 7) == " ");
    s.bar(0, 8, 8, 1.0, ui::pal::teal);
    PX_CHECK(s.glyph_at(7, 8) == "\xe2\x96\x88");
    // Out-of-range fractions are clamped rather than overflowing the row.
    s.bar(0, 8, 8, 9.0, ui::pal::teal);
    PX_CHECK(s.glyph_at(7, 8) == "\xe2\x96\x88");
    PX_CHECK(s.glyph_at(8, 8) != "\xe2\x96\x88");

    // A signed bar grows from the middle, each way, in its own colour.
    s.signed_bar(0, 10, 11, 0.9);
    PX_CHECK(s.fg_at(6, 10) == ui::pal::up);
    s.signed_bar(0, 11, 11, -0.9);
    PX_CHECK(s.fg_at(4, 11) == ui::pal::down);

    // Composing is deterministic and self-contained: same grid, same bytes, and
    // the frame both starts at home and leaves no colour set behind it.
    const std::string first = s.compose();
    const std::string again = s.compose();
    PX_CHECK(first == again);
    PX_CHECK(first.rfind("\x1b[H", 0) == 0);
    PX_CHECK(first.size() > 4 && first.compare(first.size() - 4, 4, "\x1b[0m") == 0);
    // Every row is emitted, and none of them is left wider than the grid.
    std::size_t rows = 1;
    for (const char ch : first) rows += ch == '\n' ? 1 : 0;
    PX_CHECK(rows == 12);

    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const unsigned char ch : first) {
        h ^= ch;
        h *= 0x100000001b3ULL;
    }
    std::printf("  ui: %zu-byte frame, hash %016llx\n", first.size(), static_cast<unsigned long long>(h));
    // Pinned. If a change to the palette or the drawing primitives moves this,
    // that was a deliberate visual change and this number is updated with it.
    PX_CHECK(h == kPinnedFrameHash);
}

// docs/ARENA.md says someone can add a strategy by writing a header and one
// line in the registry, without reading the engine. arena/strategies/fader.hpp is that
// worked example, and this is what keeps the claim honest: it checks that the
// example is really registrable, really trades, and really cannot break anything
// it does not own.
void test_third_party_strategy() {
    // Off by default, which is what let it be added without moving a single
    // published number.
    const Roster plain = build_roster(RosterConfig{});
    bool present = false;
    for (const auto& b : plain.strategies) present = present || std::string(b->name()) == "fader";
    PX_CHECK(!present);

    RosterConfig rc;
    rc.fader_enabled = true;
    rc.fader_trigger_ticks = 2; // keen enough to actually trade in a tight book
    SessionConfig sc = small_session();
    sc.max_requests = 60'000;

    Session s;
    PX_CHECK(s.start(sc, rc) == Session::StartError::None);
    s.run();

    int which = -1;
    for (std::size_t i = 0; i < s.participants(); ++i) {
        if (std::string(s.name_of(i)) == "fader") which = static_cast<int>(i);
    }
    PX_CHECK(which >= 0);
    PX_CHECK(s.participants() == plain.size() + 1);

    const Account& a = s.ledger().at(static_cast<std::size_t>(which));
    std::printf("  third-party strategy: %llu orders, %llu fills, %llu rejects\n",
                static_cast<unsigned long long>(a.orders_sent), static_cast<unsigned long long>(a.fills),
                static_cast<unsigned long long>(a.rejects));
    // It took part: a strategy that is registered but never acts would make the
    // worked example worthless as a worked example.
    PX_CHECK(a.orders_sent > 100);
    PX_CHECK(a.fills > 100);
    // And it was never handed the fundamental value, because the roster did not
    // mark it informed.
    PX_CHECK(!s.informed(static_cast<std::size_t>(which)));

    // The contract holds an arbitrary strategy harmlessly: an extra participant
    // cannot break the closed-world identity, the book's tracking of the
    // fundamental, or the session's ability to end.
    PX_CHECK(s.ledger().cash_sum() == 0);
    PX_CHECK(s.ledger().position_sum() == 0);
    PX_CHECK(s.ledger().value_sum(s.fair_value().value()) == 0);
    PX_CHECK(!s.stats().stalled);
    PX_CHECK(s.stats().book_full_rejects == 0);
    PX_CHECK(s.stats().action_drops == 0);

    // Its parameters reach it through the registry, so it is sweepable like
    // anything else rather than hard-coded.
    RosterConfig shy = rc;
    shy.fader_trigger_ticks = 40; // far wider than this market ever moves
    Session quiet;
    PX_CHECK(quiet.start(sc, shy) == Session::StartError::None);
    quiet.run();
    PX_CHECK(quiet.ledger().at(static_cast<std::size_t>(which)).orders_sent < a.orders_sent);
}

// Composing a line-up by name. The default path (build_roster) is what every
// published number came from; this is the other way in, and what it must not do
// is disagree with itself.
void test_roster_from_spec() {
    const RosterConfig rc;
    std::string why;

    // Every name kStrategyNames advertises must actually be addable, and vice versa.
    // A list that drifts from the code is worse than no list.
    std::size_t listed = 0;
    {
        std::string tok;
        for (const char* p = kStrategyNames;; ++p) {
            if (*p == ' ' || *p == '\0') {
                if (!tok.empty()) {
                    ++listed;
                    Roster one;
                    if (!add_named_strategy(tok, rc, one)) {
                        std::printf("  advertised but not addable: %s\n", tok.c_str());
                        PX_CHECK(false);
                    }
                    PX_CHECK(one.strategies.size() == 1);
                    PX_CHECK(one.informed.size() == 1);
                    tok.clear();
                }
                if (*p == '\0') break;
            } else {
                tok.push_back(*p);
            }
        }
    }
    std::printf("  spec: %zu named participants, all addable\n", listed);
    PX_CHECK(listed == 8);

    // Counts repeat a name, and the roster comes out in the order written.
    Roster r;
    PX_CHECK(roster_from_spec("maker,rival,informed,noise:3,fader", rc, r, why));
    PX_CHECK(r.strategies.size() == 7);
    PX_CHECK(std::string(r.strategies[0]->name()) == "maker");
    PX_CHECK(std::string(r.strategies[1]->name()) == "rival");
    PX_CHECK(std::string(r.strategies[3]->name()) == "noise");
    PX_CHECK(std::string(r.strategies[5]->name()) == "noise");
    PX_CHECK(std::string(r.strategies[6]->name()) == "fader");
    PX_CHECK(r.maker_index == 0);
    PX_CHECK(r.rival_index == 1);
    // Only the one named informed is informed, and the roster decides it.
    PX_CHECK(r.informed.size() == r.strategies.size());
    int informed_count = 0;
    for (std::size_t i = 0; i < r.informed.size(); ++i) informed_count += r.informed[i] ? 1 : 0;
    PX_CHECK(informed_count == 1);
    PX_CHECK(r.informed[2]);

    // Repeated noise traders get staggered heartbeats, so naming three does not
    // put them in lockstep.
    PX_CHECK(r.strategies[3]->timer_ns() != r.strategies[4]->timer_ns());
    PX_CHECK(r.strategies[4]->timer_ns() != r.strategies[5]->timer_ns());

    // Every way of writing it wrong is a message, not a different market.
    Roster junk;
    PX_CHECK(!roster_from_spec("maker,nonsense", rc, junk, why));
    PX_CHECK(why.find("nonsense") != std::string::npos);
    PX_CHECK(!roster_from_spec("maker,noise:x", rc, junk, why));
    PX_CHECK(!roster_from_spec("maker,noise:", rc, junk, why));
    PX_CHECK(!roster_from_spec("maker,noise:0", rc, junk, why));
    PX_CHECK(!roster_from_spec("", rc, junk, why));
    PX_CHECK(!roster_from_spec(",,,", rc, junk, why));
    // Stray commas around real names are tolerated rather than fatal.
    Roster loose;
    PX_CHECK(roster_from_spec("maker,,noise,", rc, loose, why));
    PX_CHECK(loose.strategies.size() == 2);

    // A named line-up runs, and the closed-world identity holds for it just as
    // it does for the default one.
    Roster play;
    PX_CHECK(roster_from_spec("maker,rival,informed,noise:2", rc, play, why));
    Session s;
    PX_CHECK(s.start(small_session(), std::move(play)) == Session::StartError::None);
    s.run();
    PX_CHECK(s.participants() == 5);
    PX_CHECK(s.stats().trades > 100);
    PX_CHECK(!s.stats().stalled);
    PX_CHECK(s.ledger().value_sum(s.fair_value().value()) == 0);

    // And the spec cannot be used to smuggle past the participant ceiling.
    Roster huge;
    PX_CHECK(roster_from_spec("maker,noise:100", rc, huge, why));
    Session over;
    PX_CHECK(over.start(small_session(), std::move(huge)) == Session::StartError::TooManyParticipants);
}

// The scripted participant: the replayable answer to trading by hand. Live
// keystrokes cannot go into a session without reading a wall clock, which would
// break replay; a file stamped in simulated time has none of that problem, and
// these checks are what make that claim hold.
void test_scripted_participant() {
    std::string why;
    std::vector<strategies::ScriptedOrder> orders;
    const Price tick = 100;

    // The format, including comments, blank lines, units and the tick grid.
    const char* good = "# a comment\n"
                       "\n"
                       "0      buy  50  99.95\n"
                       "250us  sell 50 100.05\n"
                       "2ms    buy  25  market   # pay the spread\n"
                       "3ms    cancel 1\n"
                       "4000000 sell 10 100.08\n";
    PX_CHECK(parse_script(good, tick, orders, why));
    std::printf("  script: %zu orders parsed\n", orders.size());
    PX_CHECK(orders.size() == 5);
    PX_CHECK(orders[0].at == 0);
    PX_CHECK(orders[1].at == 250'000);   // us
    PX_CHECK(orders[2].at == 2'000'000); // ms
    PX_CHECK(orders[3].at == 3'000'000);
    PX_CHECK(orders[4].at == 4'000'000); // bare ns
    PX_CHECK(orders[0].action.kind == Action::Kind::Limit);
    PX_CHECK(orders[0].action.side == Side::Buy);
    PX_CHECK(orders[0].action.price == 99 * kPriceScale + 9500);
    PX_CHECK(orders[0].action.qty == 50);
    PX_CHECK(orders[2].action.kind == Action::Kind::Market);
    PX_CHECK(orders[2].action.tif == Tif::IOC);
    PX_CHECK(orders[3].action.kind == Action::Kind::Cancel);

    // Every way of writing it wrong is a refusal naming the line, not a silently
    // different session.
    const char* bad[] = {
        "0 buy 50 99.955\n",       // off the tick grid
        "0 buy 50 99.000005\n",    // too many decimals
        "0 buy 50 banana\n",       // not a price
        "0 buy 0 99.95\n",         // zero quantity
        "0 buy x 99.95\n",         // not a quantity
        "0 hop 50 99.95\n",        // not a verb
        "0 buy 50\n",              // no price
        "later buy 50 99.95\n",    // not a timestamp
        "0 cancel 0\n",            // order numbers start at 1
        "0 cancel x\n",            // not an order number
        "5ms buy 1 99.95\n1ms buy 1 99.95\n", // time runs backwards
        "0 buy 50 0.00\n",         // non-positive price
    };
    std::size_t refused = 0;
    for (const char* b : bad) {
        std::vector<strategies::ScriptedOrder> junk;
        if (!parse_script(b, tick, junk, why)) {
            ++refused;
        } else {
            std::printf("  accepted what it should not: %s", b);
        }
    }
    PX_CHECK(refused == sizeof bad / sizeof bad[0]);
    // A file of nothing but comments parses to nothing, which the loader treats
    // as an error but the parser does not: parsing and meaning are separate.
    std::vector<strategies::ScriptedOrder> empty;
    PX_CHECK(parse_script("# nothing here\n\n", tick, empty, why));
    PX_CHECK(empty.empty());

    // And it really trades, in a real market, without breaking anything.
    Roster r = build_roster(RosterConfig{});
    strategies::Scripted you;
    you.orders = &orders;
    r.strategies.push_back(make_strategy(you));
    r.informed.push_back(false);
    const std::size_t which = r.strategies.size() - 1;

    SessionConfig sc = small_session();
    sc.max_requests = 60'000;
    Session s;
    PX_CHECK(s.start(sc, std::move(r)) == Session::StartError::None);
    s.run();
    const Account& a = s.ledger().at(which);
    std::printf("  scripted participant: %llu orders sent, %llu fills\n",
                static_cast<unsigned long long>(a.orders_sent), static_cast<unsigned long long>(a.fills));
    PX_CHECK(std::string(s.name_of(which)) == "you");
    // Four of the five are new orders; the cancel may or may not find a live
    // order to cancel, so the floor is what matters.
    PX_CHECK(a.orders_sent >= 4);
    PX_CHECK(a.fills > 0);
    PX_CHECK(s.ledger().value_sum(s.fair_value().value()) == 0);

    // What the viewer's `f` key does: start the same session again over a
    // freshly loaded copy of the orders. It must land on exactly the session a
    // headless run would produce, because a restart that drifted would make the
    // viewer a participant rather than a reader.
    {
        std::vector<strategies::ScriptedOrder> reloaded;
        PX_CHECK(parse_script(good, tick, reloaded, why));
        Roster again = build_roster(RosterConfig{});
        strategies::Scripted you_again;
        you_again.orders = &reloaded;
        again.strategies.push_back(make_strategy(you_again));
        again.informed.push_back(false);
        Session restarted;
        PX_CHECK(restarted.start(sc, std::move(again)) == Session::StartError::None);
        restarted.run();
        PX_CHECK(restarted.hash() == s.hash());
    }

    // Same script, same seed, same session: scripted trading is replayable,
    // which is the entire reason it is a file and not a keyboard.
    std::vector<strategies::ScriptedOrder> again = orders;
    Roster r2 = build_roster(RosterConfig{});
    strategies::Scripted you2;
    you2.orders = &again;
    r2.strategies.push_back(make_strategy(you2));
    r2.informed.push_back(false);
    Session s2;
    PX_CHECK(s2.start(sc, std::move(r2)) == Session::StartError::None);
    s2.run();
    PX_CHECK(s2.hash() == s.hash());
}


// ---- the standard experiment runner (arena/suite.hpp, docs/RUNNER.md) --------

// Sends nothing, ever, but wakes on its own timer: an inert seat that is busier
// than the placebo, to show the seat is invisible however often it looks.
struct Idle {
    const char* name = "idle";
    SimTime reaction_ns = 10'000;
    SimTime timer_ns = 7'000;
    void reset(std::uint64_t) {}
    ActionBatch on_event(const MarketView&) { return ActionBatch{}; }
};

// Posts a far-away bid and cancels it again on every wake of a 1 us timer: a
// runaway the step ceiling must catch.
struct Churn {
    const char* name = "churn";
    SimTime reaction_ns = 10'000;
    SimTime timer_ns = 1'000;
    void reset(std::uint64_t) {}
    ActionBatch on_event(const MarketView& v) {
        ActionBatch out;
        for (std::size_t i = 0; i < v.my_order_count; ++i) out.push(Action::cancel(v.my_orders[i].id));
        if (v.in_flight == 0) out.push(Action::limit(Side::Buy, 90 * kPriceScale, 1));
        return out;
    }
};

// Records how stale its view is on every wake, to show the seat runs at the
// suite's delay rather than the one it declares.
struct LagState {
    std::uint64_t wakes = 0;
    SimTime min_lag = kNever;
    SimTime max_lag = 0;
};
struct Lag {
    const char* name = "lag";
    SimTime reaction_ns = 30'000;
    SimTime timer_ns = 5'000;
    LagState* st = nullptr;
    void reset(std::uint64_t) {}
    ActionBatch on_event(const MarketView& v) {
        ++st->wakes;
        const SimTime lag = v.now_ns - v.as_of_ns;
        if (lag < st->min_lag) st->min_lag = lag;
        if (lag > st->max_lag) st->max_lag = lag;
        return ActionBatch{};
    }
};

// The default roster with anchor and tick synced to the session, exactly as
// Session::start(scfg, rcfg) builds it.
Roster default_roster(const SessionConfig& sc) {
    RosterConfig rc;
    rc.anchor = sc.start_price;
    rc.tick = sc.tick;
    return build_roster(rc);
}

bool same_account(const Account& a, const Account& b) {
    return a.position == b.position && a.cash == b.cash && a.fees == b.fees && a.fills == b.fills &&
           a.maker_fills == b.maker_fills && a.taker_fills == b.taker_fills && a.filled_qty == b.filled_qty &&
           a.orders_sent == b.orders_sent && a.rejects == b.rejects && a.self_trade_cancels == b.self_trade_cancels &&
           a.edge_sum == b.edge_sum && a.spread_sum == b.spread_sum && a.adverse_sum == b.adverse_sum &&
           a.markout_n == b.markout_n && a.adverse_fills == b.adverse_fills && a.unresolved_fills == b.unresolved_fills;
}

void test_placebo_is_inert() {
    // (a) It is a nameable participant that never acts.
    Roster one;
    PX_CHECK(add_named_strategy("placebo", RosterConfig{}, one));
    PX_CHECK(std::string(one.strategies[0]->name()) == "placebo");
    PX_CHECK(one.strategies[0]->timer_ns() == 0);
    PX_CHECK(!one.informed[0]);
    PX_CHECK(one.strategies[0]->on_event(MarketView{}).empty());

    // (b) Appended last, it leaves the session exactly as it was.
    const SessionConfig sc = small_session();
    Session bare;
    bare.start(sc, default_roster(sc));
    bare.run();
    PX_CHECK(bare.hash() == 0x2853d2127a3486c5ULL);

    auto with_seat = [&](std::unique_ptr<StrategyHandle> seat, Session& s) {
        Roster r = default_roster(sc);
        r.strategies.push_back(std::move(seat));
        r.informed.push_back(false);
        s.start(sc, std::move(r));
        s.run();
    };
    auto placebo = [] {
        Roster p;
        (void)add_named_strategy("placebo", RosterConfig{}, p);
        return std::move(p.strategies.front());
    };

    Session a;
    with_seat(placebo(), a);
    PX_CHECK(a.hash() == bare.hash());
    PX_CHECK(a.event_hash() == bare.event_hash());
    PX_CHECK(a.event_count() == bare.event_count());
    const std::size_t n = bare.participants();
    PX_CHECK(a.participants() == n + 1);
    for (std::size_t i = 0; i < n; ++i) {
        PX_CHECK(same_account(a.ledger().at(i), bare.ledger().at(i)));
        PX_CHECK(a.wakes_of(i) == bare.wakes_of(i));
    }
    PX_CHECK(same_account(a.ledger().at(n), Account{}));
    PX_CHECK(a.wakes_of(n) > 0);

    // (c) Whatever its delay, and however often an inert seat looks.
    Session b, c;
    with_seat(std::make_unique<suite::SeatOverride>(placebo(), 0), b);
    with_seat(make_strategy(Idle{}), c);
    PX_CHECK(b.hash() == bare.hash());
    PX_CHECK(c.hash() == bare.hash());
    PX_CHECK(c.wakes_of(n) > a.wakes_of(n));

    // (d) Inserted earlier it is not invisible: every later owner id and reset
    // seed shift. The check above could fail.
    Roster r = default_roster(sc);
    r.strategies.insert(r.strategies.begin() + 3, placebo());
    r.informed.insert(r.informed.begin() + 3, false);
    Session d;
    d.start(sc, std::move(r));
    d.run();
    PX_CHECK(d.hash() != bare.hash());
    std::printf("  placebo: appended %016llx (bare %016llx), inserted at 3 %016llx\n",
                static_cast<unsigned long long>(a.hash()), static_cast<unsigned long long>(bare.hash()),
                static_cast<unsigned long long>(d.hash()));
}

const suite::SeedBlock& smoke_block() { return *suite::find_block("smoke"); }

suite::ArmRecord run_named(const suite::SuiteDef& def, std::uint64_t seed, std::string_view candidate) {
    Roster r;
    suite::Origin origin = suite::Origin::Reference;
    std::string why;
    suite::ArmRecord rec;
    const bool built = suite::build_arm_roster(def, candidate, r, origin, why);
    PX_CHECK(built);
    const bool ran = suite::run_arm(def, smoke_block(), seed, candidate.empty() ? "none" : candidate, origin,
                                    std::move(r), rec, why);
    PX_CHECK(ran);
    return rec;
}

suite::ArmRecord run_seated(const suite::SuiteDef& def, std::uint64_t seed, std::unique_ptr<StrategyHandle> h,
                            std::string_view label) {
    Roster r;
    std::string why;
    suite::ArmRecord rec;
    PX_CHECK(suite::build_arm_roster(def, std::move(h), r, why));
    PX_CHECK(suite::run_arm(def, smoke_block(), seed, label, suite::Origin::Builtin, std::move(r), rec, why));
    return rec;
}

void test_suite_reference_is_default() {
    const suite::SuiteDef def = suite::standard_suite();
    const SessionConfig sc = suite::suite_session_config(def, 42);
    PX_CHECK(sc.max_requests == 0);
    PX_CHECK(sc.stall_limit_ns == 0);
    PX_CHECK(sc.max_time_ns == 25'000'000);
    Roster check_roster;
    std::string why;
    PX_CHECK(roster_from_spec(def.lineup, suite::suite_roster_config(def), check_roster, why));
    PX_CHECK(Session::check(sc, check_roster) == Session::StartError::None);

    const suite::SeedBlock& blk = smoke_block();
    for (std::uint64_t seed = blk.first; seed < blk.first + blk.count; ++seed) {
        const SessionConfig s = suite::suite_session_config(def, seed);
        // The line-up spec is the published default roster ...
        Roster spec;
        PX_CHECK(roster_from_spec(def.lineup, suite::suite_roster_config(def), spec, why));
        Session x, y;
        x.start(s, std::move(spec));
        x.run();
        y.start(s, RosterConfig{});
        y.run();
        PX_CHECK(x.hash() == y.hash());
        // ... and driving step() under the ceiling is run().
        const suite::ArmRecord none = run_named(def, seed, "");
        PX_CHECK(none.session.session_hash == y.hash());
        PX_CHECK(none.session.ended_by == suite::EndedBy::Time);
        PX_CHECK(none.parts.size() == 6);
    }
}

void test_suite_common_conditions() {
    const suite::SuiteDef def = suite::standard_suite();
    const suite::SeedBlock& blk = smoke_block();
    int request_bound_differs = 0;
    for (std::uint64_t seed = blk.first; seed < blk.first + blk.count; ++seed) {
        const suite::ArmRecord p = run_named(def, seed, "placebo");
        for (const char* arm : {"maker", "noise", "momentum"}) {
            const suite::ArmRecord a = run_named(def, seed, arm);
            // The fundamental's path and closing value are common to every arm.
            PX_CHECK(a.session.v_final == p.session.v_final);
            PX_CHECK(a.session.fair_steps == p.session.fair_steps);
            PX_CHECK(a.session.ended_by == suite::EndedBy::Time);
            PX_CHECK(a.session.steps < def.step_ceiling);

            // Under a request bound they would not be: the candidate's own
            // orders spend the shared budget and end the session earlier.
            SessionConfig rb = suite::suite_session_config(def, seed);
            rb.max_requests = 7'000;
            rb.max_time_ns = 0;
            rb.stall_limit_ns = SessionConfig{}.stall_limit_ns;
            Roster r0, r1;
            suite::Origin o;
            std::string why;
            PX_CHECK(suite::build_arm_roster(def, "placebo", r0, o, why));
            PX_CHECK(suite::build_arm_roster(def, arm, r1, o, why));
            Session s0, s1;
            s0.start(rb, std::move(r0));
            s0.run();
            s1.start(rb, std::move(r1));
            s1.run();
            if (s0.fair_value().value() != s1.fair_value().value()) ++request_bound_differs;
        }
    }
    std::printf("  suite: closing value common to every arm under the time bound; %d/12 differ under 7,000 requests\n",
                request_bound_differs);
    PX_CHECK(request_bound_differs > 0);
}

void test_suite_health_gate() {
    using suite::Health;
    const suite::HealthGate g;
    SessionStats st;
    st.gap_samples = 100;
    st.gap_sum_ticks = 400; // a mean of exactly 4 ticks passes
    PX_CHECK(suite::judge(st, suite::EndedBy::Time, g) == Health::Ok);
    st.gap_sum_ticks = 401;
    PX_CHECK(suite::judge(st, suite::EndedBy::Time, g) == Health::Gap);
    st.gap_sum_ticks = 100;
    st.gap_samples = 95;
    st.gap_unpriced = 5; // exactly 5% one-sided passes
    PX_CHECK(suite::judge(st, suite::EndedBy::Time, g) == Health::Ok);
    st.gap_samples = 94;
    st.gap_unpriced = 6;
    PX_CHECK(suite::judge(st, suite::EndedBy::Time, g) == Health::OneSided);
    SessionStats ok;
    ok.gap_samples = 10;
    ok.gap_sum_ticks = 10;
    SessionStats bf = ok;
    bf.book_full_rejects = 1;
    PX_CHECK(suite::judge(bf, suite::EndedBy::Time, g) == Health::BookFull);
    SessionStats wall = ok;
    wall.wall_hits = 1;
    PX_CHECK(suite::judge(wall, suite::EndedBy::Time, g) == Health::Wall);
    PX_CHECK(suite::judge(SessionStats{}, suite::EndedBy::Time, g) == Health::NoSamples);
    PX_CHECK(suite::judge(ok, suite::EndedBy::Ceiling, g) == Health::Ceiling);
    PX_CHECK(suite::judge(ok, suite::EndedBy::Time, g) == Health::Ok);
}

void test_suite_reference_admissible() {
    const suite::SuiteDef def = suite::standard_suite();
    const suite::SeedBlock& blk = smoke_block();
    for (std::uint64_t seed = blk.first; seed < blk.first + blk.count; ++seed) {
        const suite::ArmRecord p = run_named(def, seed, "placebo");
        PX_CHECK(p.session.healthy());
        PX_CHECK(p.session.inert);
        PX_CHECK(p.session.seat_value == 0);
    }
}

void test_suite_seat_rules() {
    std::string why;
    using V = std::vector<std::string>;
    PX_CHECK(!suite::check_candidates(V{}, why));
    PX_CHECK(!suite::check_candidates(V{"informed"}, why) && why.find("informed") != std::string::npos);
    PX_CHECK(!suite::check_candidates(V{"placebo"}, why) && why.find("placebo") != std::string::npos);
    PX_CHECK(!suite::check_candidates(V{"nonsense"}, why) && why.find("nonsense") != std::string::npos);
    PX_CHECK(!suite::check_candidates(V{"maker", "maker"}, why) && why.find("twice") != std::string::npos);
    PX_CHECK(!suite::check_candidates(V{""}, why));
    PX_CHECK(!suite::check_candidates(V(9, "maker"), why));
    PX_CHECK(suite::check_candidates(V{"maker", "noise", "momentum", "fader", "rival"}, why));

    const suite::SuiteDef def = suite::standard_suite();
    Roster r;
    suite::Origin origin;
    PX_CHECK(suite::build_arm_roster(def, "maker", r, origin, why));
    PX_CHECK(origin == suite::Origin::Builtin);
    PX_CHECK(r.size() == 7);
    PX_CHECK(!r.informed[6]); // never informed, whatever sits there
    std::size_t seat = 0;
    PX_CHECK(suite::lineup_size(def, seat, why) && seat == 6);

    // The seat runs at the suite's delay, not the one the candidate declares.
    const suite::ArmRecord f = run_named(def, 42, "fader");
    PX_CHECK(f.session.declared_reaction_ns == 30'000);
    PX_CHECK(f.session.seat_reaction_ns == 10'000);
    PX_CHECK(f.parts[6].reaction_ns == 10'000);
    PX_CHECK(f.parts[6].seat);
    LagState st;
    Lag lag;
    lag.st = &st;
    const suite::ArmRecord l = run_seated(def, 42, make_strategy(lag), "lag");
    PX_CHECK(st.wakes > 100);
    PX_CHECK(st.min_lag == 10'000 && st.max_lag == 10'000);
    PX_CHECK(l.session.declared_reaction_ns == 30'000);
}

void test_suite_arm_determinism() {
    const suite::SuiteDef def = suite::standard_suite();
    auto text = [](const suite::ArmRecord& r) {
        std::string s;
        suite::append_session_row(s, r.session);
        suite::append_participant_rows(s, r.parts);
        return s;
    };
    const std::string m1 = text(run_named(def, 42, "maker"));
    const std::string m2 = text(run_named(def, 42, "maker"));
    PX_CHECK(m1 == m2);
    // Arms are independent sessions: running another one in between changes nothing.
    const std::string n1 = text(run_named(def, 43, "noise"));
    const std::string m3 = text(run_named(def, 42, "maker"));
    const std::string n2 = text(run_named(def, 43, "noise"));
    PX_CHECK(m3 == m1);
    PX_CHECK(n2 == n1);
}

void test_suite_inert_arm() {
    const suite::SuiteDef def = suite::standard_suite();
    const suite::SeedBlock& blk = smoke_block();
    for (std::uint64_t seed = blk.first; seed < blk.first + blk.count; ++seed) {
        const suite::ArmRecord p = run_named(def, seed, "placebo");
        const suite::ArmRecord i = run_seated(def, seed, make_strategy(Idle{}), "idle");
        PX_CHECK(i.session.inert);
        PX_CHECK(i.session.session_hash == p.session.session_hash);
        PX_CHECK(i.parts[6].wakes > p.parts[6].wakes);
    }
}

void test_suite_ceiling() {
    suite::SuiteDef def = suite::standard_suite();
    def.step_ceiling = 5'000;
    const suite::ArmRecord c = run_seated(def, 42, make_strategy(Churn{}), "churn");
    PX_CHECK(c.session.ended_by == suite::EndedBy::Ceiling);
    PX_CHECK(c.session.health == suite::Health::Ceiling);
    PX_CHECK(c.session.steps == 5'000);
    PX_CHECK(c.session.value_sum == 0);
    PX_CHECK(c.session.seat_orders > 100);
}

void test_suite_closed_identity() {
    const suite::SuiteDef def = suite::standard_suite();
    const suite::SeedBlock& blk = smoke_block();
    for (std::uint64_t seed = blk.first; seed < blk.first + blk.count; ++seed) {
        const suite::ArmRecord p = run_named(def, seed, "placebo");
        for (const char* arm : {"placebo", "maker", "momentum"}) {
            const suite::ArmRecord a = run_named(def, seed, arm);
            PX_CHECK(a.session.cash_sum == 0);
            PX_CHECK(a.session.position_sum == 0);
            PX_CHECK(a.session.value_sum == 0);
            // The seat's value is exactly what the reference participants lost
            // against their own placebo-arm selves: attribution with no model.
            std::int64_t shift = 0;
            for (std::size_t i = 0; i < 6; ++i) shift += a.parts[i].value - p.parts[i].value;
            PX_CHECK(a.session.seat_value == -shift);
        }
    }
}

void test_suite_param_dump() {
    // Every advertised name is gettable and round-trips through its setter.
    for (const std::string& k : suite::split_names(kRosterParamNames)) {
        RosterConfig c;
        std::int64_t before = -1, after = -1;
        PX_CHECK(get_roster_param(c, k, before));
        const bool is_bool = k.ends_with(".enabled");
        const std::int64_t x = is_bool ? (before == 0 ? 1 : 0) : before + 3;
        PX_CHECK(set_roster_param(c, k, x));
        PX_CHECK(get_roster_param(c, k, after));
        PX_CHECK(after == x);
    }
    for (const std::string& k : suite::split_names(Session::kSessionParamNames)) {
        SessionConfig c;
        std::int64_t before = -1, after = -1;
        PX_CHECK(Session::get_param(c, k, before));
        PX_CHECK(Session::set_param(c, k, before + 3));
        PX_CHECK(Session::get_param(c, k, after));
        PX_CHECK(after == before + 3);
    }
    std::int64_t dummy = 0;
    PX_CHECK(!get_roster_param(RosterConfig{}, "nonsense", dummy));
    PX_CHECK(!Session::get_param(SessionConfig{}, "nonsense", dummy));

    // suite.cfg names every field once, and applying it to fresh configs and
    // dumping again reproduces it byte for byte: it can be read back.
    const suite::SuiteDef def = suite::standard_suite();
    const suite::RunInfo info;
    const std::string cfg = suite::suite_cfg_text(def, smoke_block(), info);
    auto count = [&cfg](const std::string& key) {
        std::size_t n = 0, at = 0;
        const std::string needle = "\n" + key + "=";
        while ((at = cfg.find(needle, at)) != std::string::npos) {
            ++n;
            ++at;
        }
        return n;
    };
    for (const std::string& k : suite::split_names(kRosterParamNames)) PX_CHECK(count(k) == 1);
    for (const std::string& k : suite::split_names(Session::kSessionParamNames)) {
        PX_CHECK(count(k) == (k == "session.seed" ? 0u : 1u));
    }
    RosterConfig rc;
    SessionConfig sc;
    std::size_t line_start = 0;
    while (line_start < cfg.size()) {
        const std::size_t end = cfg.find('\n', line_start);
        const std::string line = cfg.substr(line_start, end - line_start);
        line_start = end + 1;
        const std::size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::int64_t v = std::strtoll(line.c_str() + eq + 1, nullptr, 10);
        if (set_roster_param(rc, key, v)) continue;
        (void)Session::set_param(sc, key, v);
    }
    suite::SuiteDef again = def;
    again.roster = rc;
    PX_CHECK(suite::roster_text(again) == suite::roster_text(def));
    std::int64_t mt = 0;
    PX_CHECK(Session::get_param(sc, "session.max_time_ns", mt) && mt == def.session_ns);
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> smoke_placebo_hashes(const suite::SuiteDef& def) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    const suite::SeedBlock& blk = smoke_block();
    for (std::uint64_t seed = blk.first; seed < blk.first + blk.count; ++seed) {
        out.emplace_back(seed, run_named(def, seed, "placebo").session.session_hash);
    }
    return out;
}

void test_suite_fingerprint() {
    const suite::SuiteDef def = suite::standard_suite();
    const auto hashes = smoke_placebo_hashes(def);
    const std::uint64_t fp = suite::suite_fingerprint(def, smoke_block(), hashes);
    std::printf("  suite: smoke fingerprint %016llx\n", static_cast<unsigned long long>(fp));
    PX_CHECK(fp == kPinnedSmokeFingerprint);

    // A change to the suite's own settings moves it, even with every placebo
    // hash unchanged ...
    suite::SuiteDef seat = def;
    seat.seat_reaction_ns = 10'001;
    PX_CHECK(suite::suite_fingerprint(seat, smoke_block(), hashes) != fp);
    // ... and a change to a reference strategy moves it through the hashes.
    suite::SuiteDef ref = def;
    ref.roster.maker_impact_centiticks = 16;
    const auto moved = smoke_placebo_hashes(ref);
    PX_CHECK(moved != hashes);
    PX_CHECK(suite::suite_fingerprint(ref, smoke_block(), moved) != fp);
}

void test_paired_stats() {
    const suite::Paired p = suite::paired({1, 2, 3, 4}, {2, 4, 6, 8});
    auto near = [](double a, double b) { return a - b < 1e-3 && b - a < 1e-3; };
    PX_CHECK(p.n == 4);
    PX_CHECK(near(p.mean, 2.5));
    PX_CHECK(near(p.sd, 1.2910));
    PX_CHECK(near(p.se, 0.6455));
    PX_CHECK(near(p.t, 3.182));
    PX_CHECK(near(p.lo, 0.4460) && near(p.hi, 4.5540));
    PX_CHECK(near(p.median, 2.5) && near(p.min, 1) && near(p.max, 4));
    PX_CHECK(p.ahead == 4);
    PX_CHECK(p.var_removed_defined && near(p.var_removed, 0.8));
    PX_CHECK(p.separated() && !p.straddles && !p.identical);

    const suite::Paired c = suite::paired({1, 2, 3}, {4, 5, 6}); // constant difference
    PX_CHECK(c.se == 0 && near(c.mean, 3) && c.separated());
    const suite::Paired z = suite::paired({5, 7}, {5, 7});
    PX_CHECK(z.identical && !z.separated());
    for (const suite::SeedBlock& b : suite::kSeedBlocks) PX_CHECK(suite::t975(b.count - 1) > 0);
}

void test_suite_compare() {
    // A synthetic run: an A/A pair, a pair separated by its interval although
    // the range straddles zero, and an arm that failed one seed.
    suite::RunRecord run;
    run.block = suite::SeedBlock{"synthetic", 1, 8};
    for (const char* n : {"a", "b", "c", "d"}) {
        suite::CandidateInfo ci;
        ci.name = n;
        run.info.candidates.push_back(ci);
    }
    const std::int64_t a[] = {10, 20, 30, 40, 50, 60, 70, 80};
    const std::int64_t c[] = {30, 41, 49, 61, 70, 79, 91, 79}; // a + ~20, but one seed far below
    for (std::uint64_t k = 0; k < 8; ++k) {
        auto row = [&](const char* arm, std::int64_t v, bool healthy) {
            suite::SessionRow r;
            r.arm = arm;
            r.seed = k + 1;
            r.seat_value = v * kPriceScale;
            r.health = healthy ? suite::Health::Ok : suite::Health::Gap;
            run.sessions.push_back(r);
        };
        row("placebo", 0, true);
        row("a", a[k], true);
        row("b", a[k], true);
        row("c", c[k], true);
        row("d", a[k], k != 5);
    }
    const std::vector<suite::ComparisonRow> rows = suite::compare(run);
    PX_CHECK(rows.size() == 4 + 3);
    auto find = [&rows](const std::string& base, const std::string& arm) -> const suite::ComparisonRow* {
        for (const auto& r : rows) {
            if (r.baseline == base && r.arm == arm) return &r;
        }
        return nullptr;
    };
    const suite::ComparisonRow* aa = find("a", "b");
    const suite::ComparisonRow* ac = find("a", "c");
    const suite::ComparisonRow* ad = find("a", "d");
    PX_CHECK(aa != nullptr && aa->verdict == "identical");
    PX_CHECK(ac != nullptr && ac->verdict == "separated" && ac->stats.straddles);
    PX_CHECK(ad != nullptr && ad->verdict == "not_admissible");
    PX_CHECK(ad != nullptr && ad->stats.n == 8); // the failing seed is not dropped
    PX_CHECK(find("placebo", "d")->verdict == "not_admissible");
    PX_CHECK(suite::outcome_of(run).exit_code() == 0);
}

void test_suite_rows() {
    const suite::SuiteDef def = suite::standard_suite();
    const suite::ArmRecord m = run_named(def, 42, "maker");
    std::string sessions = suite::kSessionsHeader;
    suite::append_session_row(sessions, m.session);
    std::string parts = suite::kParticipantsHeader;
    suite::append_participant_rows(parts, m.parts);
    auto columns = [](const std::string& line) { return 1 + std::count(line.begin(), line.end(), ','); };
    auto lines = [](const std::string& text) {
        std::vector<std::string> out;
        std::size_t at = 0;
        while (at < text.size()) {
            const std::size_t end = text.find('\n', at);
            out.push_back(text.substr(at, end - at));
            at = end + 1;
        }
        return out;
    };
    for (const std::string* t : {&sessions, &parts}) {
        const std::vector<std::string> ls = lines(*t);
        PX_CHECK(ls.size() >= 2);
        for (const std::string& l : ls) PX_CHECK(columns(l) == columns(ls[0]));
        PX_CHECK(t->find('.') == std::string::npos); // integers and identifiers only
        PX_CHECK(t->find('\r') == std::string::npos);
    }
    PX_CHECK(lines(parts).size() == 1 + 7);
}

// ---- the evaluation report (docs/REPORT.md) ----------------------------------

// A whole smoke run of `def` for the named candidates, as suite.cpp builds it.
suite::RunRecord smoke_run(const suite::SuiteDef& def, const std::vector<std::string>& names) {
    suite::RunRecord run;
    run.def = def;
    run.block = smoke_block();
    for (const std::string& n : names) {
        suite::CandidateInfo c;
        c.name = n;
        run.info.candidates.push_back(c);
    }
    const suite::SeedBlock& blk = smoke_block();
    for (std::uint64_t seed = blk.first; seed < blk.first + blk.count; ++seed) {
        for (const std::string& arm : suite::arm_names(run)) {
            suite::ArmRecord a = run_named(def, seed, arm);
            run.sessions.push_back(a.session);
            for (suite::ParticipantRow& p : a.parts) run.parts.push_back(std::move(p));
        }
    }
    return run;
}

void test_report_path() {
    const suite::SuiteDef def = suite::standard_suite();
    const suite::ArmRecord m = run_named(def, 42, "maker");
    const suite::SessionRow& r = m.session;
    const Account& a = m.parts.back().account;
    PX_CHECK(m.parts.back().seat);
    // One sample per step that moved the fundamental, and one at the end.
    PX_CHECK(r.seat_path_samples > 1 && r.seat_path_samples <= r.fair_steps + 1);
    const std::uint64_t final_abs = static_cast<std::uint64_t>(a.position < 0 ? -a.position : a.position);
    PX_CHECK(r.seat_max_abs_position >= final_abs && r.seat_max_abs_position > 0);
    PX_CHECK(r.seat_abs_position_sum <= r.seat_max_abs_position * r.seat_path_samples);
    PX_CHECK(r.seat_max_drawdown > 0);
    // The empty seat's path is flat.
    const suite::ArmRecord p = run_named(def, 42, "placebo");
    PX_CHECK(p.session.seat_path_samples > 1);
    PX_CHECK(p.session.seat_abs_position_sum == 0 && p.session.seat_max_abs_position == 0);
    PX_CHECK(p.session.seat_max_drawdown == 0);
    // Sampling reads the session and never steers it: the no-seat market has no
    // path at all, and the placebo's hash is still the no-seat market's.
    const suite::ArmRecord none = run_named(def, 42, "");
    PX_CHECK(none.session.seat_path_samples == 0);
    PX_CHECK(none.session.session_hash == p.session.session_hash);
}

void test_report_metrics() {
    const suite::RunRecord run = smoke_run(suite::standard_suite(), {"maker", "momentum"});
    const std::vector<suite::MetricRow> rows = suite::evaluate(run);
    PX_CHECK(rows.size() == 3 * suite::kMetricCount); // two levels, one pair
    for (const suite::MetricRow& r : rows) PX_CHECK(r.stats.n == 4 || std::string(suite::kMetrics[r.metric].key) == "maker_share");
    // The value level is the step-2 statistic, and value decomposes exactly.
    const std::vector<std::int64_t> v = suite::seat_values(run, "maker");
    const suite::Paired direct = suite::paired(std::vector<std::int64_t>(v.size(), 0), v);
    PX_CHECK(rows[0].arm == "maker" && rows[0].metric == 0);
    const double dm = rows[0].stats.mean - suite::to_currency(direct.mean);
    PX_CHECK(dm < 1e-9 && -dm < 1e-9);
    for (const char* arm : {"maker", "momentum"}) {
        for (const suite::SeatSeed& x : suite::seat_series(run, arm)) {
            const double sum = x.v[9] + x.v[10] + x.v[11];
            PX_CHECK(sum - x.v[0] < 1e-6 && x.v[0] - sum < 1e-6);
            PX_CHECK(x.v[3] >= x.v[2]); // max |inventory| >= mean |inventory|
        }
    }
    // Pairs run on the seeds where both are defined, and say so.
    for (const suite::MetricRow& r : rows) {
        if (r.baseline.empty()) continue;
        PX_CHECK(r.baseline == "maker" && r.arm == "momentum");
        PX_CHECK(!r.verdict.empty());
    }
    // The CSV has one header and one row per metric row, all the same width.
    const std::string csv = suite::evaluation_csv(rows);
    PX_CHECK(static_cast<std::size_t>(std::count(csv.begin(), csv.end(), '\n')) == 1 + rows.size());

    // A ratio undefined on one seed drops that seed from that metric only.
    suite::RunRecord syn;
    syn.block = suite::SeedBlock{"synthetic", 1, 4};
    suite::CandidateInfo ci;
    ci.name = "a";
    syn.info.candidates.push_back(ci);
    for (std::uint64_t k = 0; k < 4; ++k) {
        for (const char* arm : {"placebo", "a"}) {
            suite::SessionRow s;
            s.arm = arm;
            s.seed = k + 1;
            s.seat = 0;
            syn.sessions.push_back(s);
            suite::ParticipantRow p;
            p.arm = arm;
            p.seed = k + 1;
            p.index = 0;
            p.seat = true;
            if (std::string(arm) == "a" && k != 2) {
                p.account.orders_sent = 4;
                p.account.fills = 2 + k;
            }
            syn.parts.push_back(p);
        }
    }
    for (const suite::MetricRow& r : suite::evaluate(syn)) {
        const std::string key = suite::kMetrics[r.metric].key;
        if (key == "fills_per_order") PX_CHECK(r.stats.n == 3);
        if (key == "fills") PX_CHECK(r.stats.n == 4);
    }
}

void test_report_attribution() {
    const suite::RunRecord run = smoke_run(suite::standard_suite(), {"maker", "momentum"});
    bool identity = false;
    const std::vector<suite::AttributionRow> rows = suite::attribute(run, identity);
    PX_CHECK(identity);
    PX_CHECK(rows.size() == 2 * 6);
    // Per candidate, the reference participants' changes undo the seat exactly.
    for (const char* arm : {"maker", "momentum"}) {
        double sum = 0;
        for (const suite::AttributionRow& r : rows) {
            if (r.arm == arm) sum += r.stats.mean;
        }
        const std::vector<std::int64_t> v = suite::seat_values(run, arm);
        double seat = 0;
        for (const std::int64_t x : v) seat += suite::to_currency(static_cast<double>(x));
        seat /= static_cast<double>(v.size());
        PX_CHECK(sum + seat < 1e-6 && -(sum + seat) < 1e-6);
    }
    PX_CHECK(rows[0].name == "maker" && rows[5].name == "momentum");
    // A broken identity is reported, not hidden.
    suite::RunRecord bad = run;
    for (suite::ParticipantRow& p : bad.parts) {
        if (p.arm == "maker" && p.index == 1 && p.seed == 43) p.value += 1;
    }
    bool still = true;
    (void)suite::attribute(bad, still);
    PX_CHECK(!still);
}

void test_report_t_table() {
    // Past the table, within 0.0005 of the exact quantiles.
    auto near = [](double a, double b) { return a - b < 5e-4 && b - a < 5e-4; };
    PX_CHECK(near(suite::t975(31), 2.0395));
    PX_CHECK(near(suite::t975(40), 2.0211));
    PX_CHECK(near(suite::t975(60), 2.0003));
    PX_CHECK(near(suite::t975(120), 1.9799));
    // Non-increasing, give or take the table's three decimals at 63 and 127.
    for (std::size_t df = 31; df < 200; ++df) PX_CHECK(suite::t975(df + 1) < suite::t975(df) + 5e-4);
}

void test_report_families() {
    const std::vector<std::string> names = suite::split_names(suite::kSuiteNames);
    PX_CHECK(names.size() == 3 && names[0] == "standard");
    suite::SuiteDef d = suite::standard_suite();
    PX_CHECK(!suite::find_suite("null", d) && !suite::find_suite("bogus", d));
    std::vector<std::uint64_t> fps;
    for (const std::string& n : names) {
        suite::SuiteDef f = suite::standard_suite();
        PX_CHECK(suite::find_suite(n, f));
        PX_CHECK(n == f.name);
        PX_CHECK((n == "standard") == (f.change[0] == '\0'));
        const auto hashes = smoke_placebo_hashes(f);
        for (std::uint64_t seed = smoke_block().first; seed < smoke_block().first + smoke_block().count; ++seed) {
            PX_CHECK(run_named(f, seed, "placebo").session.healthy());
        }
        fps.push_back(suite::suite_fingerprint(f, smoke_block(), hashes));
    }
    PX_CHECK(fps[0] == kPinnedSmokeFingerprint);
    PX_CHECK(fps[1] == kPinnedWideFingerprint);
    PX_CHECK(fps[2] == kPinnedBusyFingerprint);
    std::printf("  families: smoke fingerprints wide %016llx, busy %016llx\n", static_cast<unsigned long long>(fps[1]),
                static_cast<unsigned long long>(fps[2]));
    // The cross-family table names every family and never pools them.
    suite::FamilyResult a, b;
    a.suite = "standard";
    b.suite = "wide";
    b.change = "maker.half_spread_ticks=4";
    const std::string t = suite::families_text({"maker"}, {a, b});
    PX_CHECK(t.find("not pooled") != std::string::npos);
    PX_CHECK(t.find("maker.half_spread_ticks=4") != std::string::npos);
}

// The holdout is checked from the placebo arm only: the market with an empty
// seat says nothing about any strategy, so pinning it spends none of the block.
void test_holdout_block() {
    const suite::SeedBlock* hold = suite::find_block("holdout");
    const suite::SeedBlock* std_blk = suite::find_block("standard");
    PX_CHECK(hold != nullptr && std_blk != nullptr);
    if (hold == nullptr || std_blk == nullptr) return;
    PX_CHECK(hold->confirmatory && !std_blk->confirmatory && !smoke_block().confirmatory);
    PX_CHECK(hold->count == std_blk->count);
    for (const suite::SeedBlock& b : suite::kSeedBlocks) {
        if (b.confirmatory) continue;
        PX_CHECK(hold->first >= b.first + b.count || hold->first + hold->count <= b.first);
    }

    // Confirmatory is not a condition: clearing it changes no condition line.
    const suite::SuiteDef def = suite::standard_suite();
    suite::SeedBlock plain = *hold;
    plain.confirmatory = false;
    PX_CHECK(suite::conditions_text(def, plain) == suite::conditions_text(def, *hold));

    PX_CHECK(suite::block_note(*hold).find("confirmatory") != std::string::npos);
    PX_CHECK(suite::block_note(*hold).find("Never pool it with standard") != std::string::npos);
    PX_CHECK(suite::block_note(*std_blk).empty());
    PX_CHECK(suite::block_note(smoke_block()).find("cannot separate anything") != std::string::npos);

    const std::uint64_t pins[] = {kPinnedHoldoutFingerprint, kPinnedHoldoutWideFingerprint,
                                  kPinnedHoldoutBusyFingerprint};
    const std::vector<std::string> names = suite::split_names(suite::kSuiteNames);
    PX_CHECK(names.size() == 3);
    for (std::size_t k = 0; k < names.size() && k < 3; ++k) {
        suite::SuiteDef f = suite::standard_suite();
        PX_CHECK(suite::find_suite(names[k], f));
        std::vector<std::pair<std::uint64_t, std::uint64_t>> hashes;
        std::uint32_t healthy = 0;
        for (std::uint64_t seed = hold->first; seed < hold->first + hold->count; ++seed) {
            Roster r;
            suite::Origin origin = suite::Origin::Reference;
            std::string why;
            suite::ArmRecord rec;
            PX_CHECK(suite::build_arm_roster(f, "placebo", r, origin, why));
            PX_CHECK(suite::run_arm(f, *hold, seed, "placebo", origin, std::move(r), rec, why));
            if (rec.session.healthy()) ++healthy;
            hashes.emplace_back(seed, rec.session.session_hash);
        }
        PX_CHECK(healthy == hold->count);
        const std::uint64_t fp = suite::suite_fingerprint(f, *hold, hashes);
        std::printf("  holdout: %s fingerprint %016llx, the reference admissible on %u/%u seeds\n", f.name,
                    static_cast<unsigned long long>(fp), healthy, hold->count);
        PX_CHECK(fp == pins[k]);
    }
}

// The p-values and Holm's adjustment, held to answers known in advance
// (docs/REPORT.md, "Many comparisons").
void test_holm() {
    auto close = [](double a, double b) { return a - b < 1e-12 && b - a < 1e-12; };
    // The hand-written atan against the library's, which only this test calls:
    // within 4.4e-16 when measured, two units in the last place near pi/2.
    double worst = 0;
    for (int i = -20000; i <= 20000; ++i) {
        const double x = i / 100.0;
        worst = std::max(worst, std::fabs(suite::portable_atan(x) - std::atan(x)));
    }
    PX_CHECK(worst < 1e-15);
    // Closed forms: one degree of freedom is the Cauchy, two is 1 - t / sqrt(2 + t^2).
    for (const double t : {0.0, 0.5, 1.0, 3.0, 40.0}) {
        PX_CHECK(std::fabs(suite::t_two_sided_p(t, 1) - (1 - 2 / 3.141592653589793 * std::atan(t))) < 1e-15);
        PX_CHECK(std::fabs(suite::t_two_sided_p(t, 2) - (1 - t / std::sqrt(2 + t * t))) < 1e-15);
    }
    // Each row of the t table is a published 95% critical value, so p there is 0.05
    // to within what three decimals of t can move it (4.9e-5 at worst, measured).
    for (std::size_t df = 1; df <= 30; ++df) {
        PX_CHECK(std::fabs(suite::t_two_sided_p(suite::t975(df), df) - 0.05) < 1e-4);
    }
    PX_CHECK(std::fabs(suite::t_two_sided_p(suite::t975(63), 63) - 0.05) < 1e-4);
    PX_CHECK(std::fabs(suite::t_two_sided_p(suite::t975(127), 127) - 0.05) < 1e-4);
    PX_CHECK(suite::t_two_sided_p(-2.5, 63) == suite::t_two_sided_p(2.5, 63));
    PX_CHECK(suite::t_two_sided_p(0, 63) == 1.0);

    // Holm on a textbook family: 0.005 and 0.01 are separated, 0.03 and 0.04 are not.
    const std::vector<double> h = suite::holm_adjust({0.01, 0.04, 0.03, 0.005});
    PX_CHECK(close(h[0], 0.03) && close(h[1], 0.06) && close(h[2], 0.06) && close(h[3], 0.02));
    PX_CHECK(suite::holm_adjust({0.2}) == std::vector<double>{0.2});
    PX_CHECK(suite::holm_adjust({0.5, 0.6}) == (std::vector<double>{1.0, 1.0}));
    const std::vector<double> tie = suite::holm_adjust({0.02, 0.02, 0.9});
    PX_CHECK(close(tie[0], 0.06) && close(tie[1], 0.06) && close(tie[2], 0.9));
    const std::vector<double> raw = {0.3, 0.001, 0.04, 0.012, 0.2, 0.049};
    const std::vector<double> adj = suite::holm_adjust(raw);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        PX_CHECK(adj[i] >= raw[i] && adj[i] <= 1);
        for (std::size_t j = 0; j < raw.size(); ++j) {
            if (raw[i] < raw[j]) PX_CHECK(adj[i] <= adj[j]);
        }
    }

    // The family is the seat-value rows the report counts as tests, and the
    // report shows the correction only when there are two or more of them.
    const suite::RunRecord run = smoke_run(suite::standard_suite(), {"maker", "momentum"});
    const std::vector<suite::ComparisonRow> rows = suite::compare(run);
    std::size_t tests = 0;
    for (const suite::ComparisonRow& r : rows) tests += suite::is_value_test(r) ? 1 : 0;
    PX_CHECK(tests >= 2);
    const std::vector<suite::HolmRow> hr = suite::holm_rows({{"standard", &rows}});
    PX_CHECK(hr.size() == rows.size());
    for (const suite::HolmRow& r : hr) {
        PX_CHECK(r.family == tests);
        if (r.tested) PX_CHECK(r.holm_p >= r.p && r.holm_p <= 1);
    }
    const std::string csv = suite::holm_csv(hr);
    PX_CHECK(csv.rfind("suite,baseline,arm,seeds,t_stat,df,p_value,family_tests,holm_p,holm_verdict\n", 0) == 0);
    PX_CHECK(static_cast<std::size_t>(std::count(csv.begin(), csv.end(), '\n')) == rows.size() + 1);
    const std::string report = suite::report_text(run, rows, 0);
    PX_CHECK(report.find("Holm, over the " + std::to_string(tests) + " seat-value tests above") != std::string::npos);
    PX_CHECK(report.find("Holm-corrected above") != std::string::npos);
    const suite::RunRecord one = smoke_run(suite::standard_suite(), {"maker"});
    const std::string single = suite::report_text(one, suite::compare(one), 0);
    PX_CHECK(single.find("Holm") == std::string::npos);
    PX_CHECK(single.find("no correction is applied") != std::string::npos);
    std::printf("  holm: %zu seat-value tests in a two-candidate smoke run; atan within %.2g of the library's\n", tests,
                worst);
}

// ---- the evaluator's controls, and the defects they catch (docs/CONTROLS.md) ----

// Keeps a count in static storage, so a second session in the same process
// behaves differently from the first: the defect replay exists to catch.
struct Leaky {
    const char* name = "leaky";
    SimTime reaction_ns = 10'000;
    SimTime timer_ns = 50'000;
    static inline int sessions = 0;
    bool active = false;
    void reset(std::uint64_t) { active = (++sessions % 2) == 1; }
    ActionBatch on_event(const MarketView& v) {
        ActionBatch out;
        if (active && v.reason == WakeReason::Timer && v.in_flight == 0 && v.best(Side::Sell) != 0) {
            out.push(Action::market(Side::Buy, 5));
        }
        return out;
    }
};

// A smoke run as arena/controls.cpp builds one: placebo checked against the
// no-seat market, every candidate replayed.
suite::RunRecord controls_run(const std::vector<std::pair<std::string, std::string>>& arms) {
    const suite::SuiteDef def = suite::standard_suite();
    suite::RunRecord run;
    run.def = def;
    run.block = smoke_block();
    for (const auto& a : arms) {
        suite::CandidateInfo c;
        c.name = a.first;
        run.info.candidates.push_back(c);
    }
    const suite::SeedBlock& blk = smoke_block();
    for (std::uint64_t seed = blk.first; seed < blk.first + blk.count; ++seed) {
        suite::ArmRecord p = run_named(def, seed, "placebo");
        const suite::ArmRecord bare = run_named(def, seed, "");
        p.session.check_kind = "no_seat";
        p.session.check_hash = bare.session.session_hash;
        p.session.check_ok = bare.session.session_hash == p.session.session_hash;
        run.sessions.push_back(p.session);
        for (suite::ParticipantRow& x : p.parts) run.parts.push_back(std::move(x));
        for (const auto& [label, strategy] : arms) {
            suite::ArmRecord a = run_named(def, seed, strategy);
            const suite::ArmRecord again = run_named(def, seed, strategy);
            a.session.arm = label;
            for (suite::ParticipantRow& x : a.parts) x.arm = label;
            a.session.check_kind = "replay";
            a.session.check_hash = again.session.session_hash;
            a.session.check_ok = again.session.session_hash == a.session.session_hash;
            run.sessions.push_back(a.session);
            for (suite::ParticipantRow& x : a.parts) run.parts.push_back(std::move(x));
        }
    }
    return run;
}

void test_controls_hold() {
    const suite::RunRecord run = controls_run({{"roundtrip", "roundtrip"}, {"maker", "maker"}, {"maker-copy", "maker"}});
    PX_CHECK(suite::control_placebo_zero(run).pass);
    PX_CHECK(suite::control_placebo_inert(run).pass);
    PX_CHECK(suite::control_common_close(run).pass);
    PX_CHECK(suite::control_replay(run).pass);
    PX_CHECK(suite::control_identities(run).pass);
    PX_CHECK(suite::control_aa(run, "maker", "maker-copy").pass);
    PX_CHECK(suite::control_positive(run, "roundtrip").pass);
    // Each control can fail on a real arm, not only on a planted defect.
    PX_CHECK(!suite::control_positive(run, "maker").pass);            // maker makes money
    PX_CHECK(!suite::control_aa(run, "maker", "roundtrip").pass);     // different strategies
    PX_CHECK(!suite::control_noise_floor(run, "placebo", "roundtrip", "x").pass); // a real, large effect
    PX_CHECK(suite::controls_text({suite::control_replay(run)}).find("1/1 controls held") != std::string::npos);
}

void test_controls_catch_defects() {
    const suite::SuiteDef def = suite::standard_suite();
    const suite::RunRecord good = controls_run({{"maker", "maker"}, {"maker-copy", "maker"}});
    const suite::SeedBlock& blk = smoke_block();

    // 1. Pairing off by one seed: the A/A pair is no longer identical.
    suite::RunRecord shifted = good;
    std::vector<std::int64_t> copy = suite::seat_values(good, "maker-copy");
    std::size_t k = 0;
    for (suite::SessionRow& r : shifted.sessions) {
        if (r.arm == "maker-copy") r.seat_value = copy[(k++ + 1) % copy.size()];
    }
    PX_CHECK(!suite::control_aa(shifted, "maker", "maker-copy").pass);

    // 2. A request bound instead of the time bound: arms close at different
    // fundamentals, which breaks pairing.
    suite::RunRecord bounded;
    bounded.block = blk;
    for (std::uint64_t seed = blk.first; seed < blk.first + blk.count; ++seed) {
        for (const char* arm : {"placebo", "maker"}) {
            Roster r;
            suite::Origin origin = suite::Origin::Reference;
            std::string why;
            PX_CHECK(suite::build_arm_roster(def, arm, r, origin, why));
            SessionConfig sc = suite::suite_session_config(def, seed);
            sc.max_requests = 7'000;
            sc.max_time_ns = 0;
            sc.stall_limit_ns = SessionConfig{}.stall_limit_ns;
            Session s;
            PX_CHECK(s.start(sc, std::move(r)) == Session::StartError::None);
            s.run();
            suite::SessionRow row;
            row.arm = arm;
            row.seed = seed;
            row.v_final = s.fair_value().value();
            row.fair_steps = s.stats().fair_steps;
            bounded.sessions.push_back(row);
        }
    }
    PX_CHECK(!suite::control_common_close(bounded).pass);

    // 3. The seat inserted inside the line-up instead of after it: the placebo
    // is no longer invisible.
    suite::RunRecord inserted = good;
    for (suite::SessionRow& r : inserted.sessions) {
        if (r.arm != "placebo") continue;
        Roster ro;
        std::string why;
        PX_CHECK(roster_from_spec(def.lineup, suite::suite_roster_config(def), ro, why));
        ro.strategies.insert(ro.strategies.begin() + 3, make_strategy(strategies::Placebo{}));
        ro.informed.insert(ro.informed.begin() + 3, false);
        Session s;
        PX_CHECK(s.start(suite::suite_session_config(def, r.seed), std::move(ro)) == Session::StartError::None);
        s.run();
        r.session_hash = s.hash();
        r.check_ok = r.session_hash == r.check_hash;
    }
    PX_CHECK(!suite::control_placebo_inert(inserted).pass);

    // 4. State that survives between sessions: replay differs.
    suite::RunRecord leaky = good;
    for (suite::SessionRow& r : leaky.sessions) {
        if (r.arm != "maker-copy") continue;
        const suite::ArmRecord a = run_seated(def, r.seed, make_strategy(Leaky{}), "leaky");
        const suite::ArmRecord b = run_seated(def, r.seed, make_strategy(Leaky{}), "leaky");
        r.session_hash = a.session.session_hash;
        r.check_hash = b.session.session_hash;
        r.check_ok = r.session_hash == r.check_hash;
    }
    PX_CHECK(!suite::control_replay(leaky).pass);

    // 5. A broken ledger: one participant's value off by one price unit.
    suite::RunRecord ledger = good;
    for (suite::ParticipantRow& p : ledger.parts) {
        if (p.arm == "maker" && p.index == 2 && p.seed == blk.first) p.value += 1;
    }
    PX_CHECK(!suite::control_identities(ledger).pass);
    suite::RunRecord sessions = good;
    for (suite::SessionRow& r : sessions.sessions) {
        if (r.arm == "maker" && r.seed == blk.first) r.value_sum = 1;
    }
    PX_CHECK(!suite::control_identities(sessions).pass);

    // 6. A placebo that trades.
    suite::RunRecord busy = good;
    for (suite::SessionRow& r : busy.sessions) {
        if (r.arm == "placebo" && r.seed == blk.first) r.seat_orders = 1;
    }
    PX_CHECK(!suite::control_placebo_zero(busy).pass);
}

void test_suite_outcomes() {
    // Exit codes, from records alone: 0, then 1 for a replay, then 3 for the reference.
    suite::RunRecord run;
    suite::SessionRow p, c;
    p.arm = "placebo";
    c.arm = "x";
    run.sessions = {p, c};
    PX_CHECK(suite::outcome_of(run).exit_code() == 0);
    run.sessions[1].check_ok = false;
    PX_CHECK(suite::outcome_of(run).exit_code() == 1);
    run.sessions[0].health = suite::Health::Gap;
    PX_CHECK(suite::outcome_of(run).exit_code() == 3);
    run.sessions[0].health = suite::Health::Ok;
    run.sessions[1].check_ok = true;
    run.sessions[1].value_sum = 1;
    PX_CHECK(suite::outcome_of(run).exit_code() == 3);

    // An inadmissible reference makes candidate-against-candidate rows
    // inadmissible too, and a pair that does not separate says so.
    suite::RunRecord r2;
    r2.block = suite::SeedBlock{"synthetic", 1, 4};
    for (const char* n : {"a", "b"}) {
        suite::CandidateInfo ci;
        ci.name = n;
        r2.info.candidates.push_back(ci);
    }
    const std::int64_t a[] = {10, 20, 30, 40};
    const std::int64_t b[] = {12, 17, 33, 39}; // differences 2, -3, 3, -1
    for (std::uint64_t k = 0; k < 4; ++k) {
        for (const auto& [arm, v] : {std::pair<const char*, std::int64_t>{"placebo", 0}, {"a", a[k]}, {"b", b[k]}}) {
            suite::SessionRow s;
            s.arm = arm;
            s.seed = k + 1;
            s.seat_value = v * kPriceScale;
            r2.sessions.push_back(s);
        }
    }
    for (const suite::ComparisonRow& row : suite::compare(r2)) {
        if (row.baseline == "a") PX_CHECK(row.verdict == "not_separated");
    }
    r2.sessions[0].health = suite::Health::Gap;
    for (const suite::ComparisonRow& row : suite::compare(r2)) PX_CHECK(row.verdict == "not_admissible");
}

void test_suite_cfg_reading() {
    const suite::SuiteDef def = suite::standard_suite();
    suite::RunInfo info;
    suite::CandidateInfo c;
    c.name = "maker";
    info.candidates.push_back(c);
    const std::string bare = suite::suite_cfg_text(def, smoke_block(), info);
    PX_CHECK(bare.find("build.") == std::string::npos); // no build recorded, no build lines
    info.build.commit = "0123456789ab";
    info.build.compiler = "x";
    info.binary = "suite_lab";
    info.candidates[0].digest = "42";
    const std::string full = suite::suite_cfg_text(def, smoke_block(), info);
    PX_CHECK(suite::cfg_value(full, "build.commit") == "0123456789ab");
    PX_CHECK(suite::cfg_value(full, "run.candidate.maker.digest") == "42");
    PX_CHECK(suite::cfg_value(full, "suite.name") == "standard");
    PX_CHECK(suite::cfg_value(full, "suite") == "");           // a prefix is not a key
    PX_CHECK(suite::cfg_value(full, "no.such.key") == "");
    // What ran and on which build never counts as a condition.
    PX_CHECK(suite::condition_lines(full) == suite::condition_lines(bare));
    PX_CHECK(!suite::condition_lines(full).empty());
    for (const std::string& l : suite::condition_lines(full)) {
        PX_CHECK(!l.starts_with("run.") && !l.starts_with("build.") && !l.starts_with("#"));
    }
    // A different family is a different set of conditions.
    suite::SuiteDef wide = def;
    PX_CHECK(suite::find_suite("wide", wide));
    PX_CHECK(suite::condition_lines(suite::suite_cfg_text(wide, smoke_block(), info)) != suite::condition_lines(full));
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
    test_third_party_strategy();
    test_roster_from_spec();
    test_scripted_participant();
    test_session_refuses_bad_configs();
    test_quiet_session_terminates();
    test_ui_grid();
    test_speed_beats_slow();
    test_placebo_is_inert();
    test_suite_reference_is_default();
    test_suite_common_conditions();
    test_suite_health_gate();
    test_suite_reference_admissible();
    test_suite_seat_rules();
    test_suite_arm_determinism();
    test_suite_inert_arm();
    test_suite_ceiling();
    test_suite_closed_identity();
    test_suite_param_dump();
    test_suite_fingerprint();
    test_paired_stats();
    test_suite_compare();
    test_suite_rows();
    test_report_path();
    test_report_metrics();
    test_report_attribution();
    test_report_t_table();
    test_report_families();
    test_holdout_block();
    test_holm();
    test_controls_hold();
    test_controls_catch_defects();
    test_suite_outcomes();
    test_suite_cfg_reading();
    PX_TEST_REPORT_AND_RETURN();
}
