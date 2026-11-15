// tests/test_venue.cpp — the stages around the engine (venue/): the journal,
// snapshots and crash recovery, a standby, and the planted defects that show
// each check can fail.
#include "px/matching_engine.hpp"
#include "sim/order_stream.hpp"
#include "sim/pipeline.hpp"
#include "test_harness.hpp"
#include "venue/book_state.hpp"
#include "venue/feed.hpp"
#include "venue/journal.hpp"
#include "venue/recovery.hpp"
#include "venue/risk.hpp"
#include "venue/sequencer.hpp"
#include "venue/shards.hpp"
#include "venue/venue.hpp"
#include "venue/wire.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace px;
using namespace px::venue;

namespace {

std::string tmp(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "px-test-venue";
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

std::unique_ptr<MatchingEngine> fresh(const sim::StreamConfig& cfg) {
    auto e = std::make_unique<MatchingEngine>();
    sim::add_instruments(*e, cfg);
    return e;
}

bool same_books(const MatchingEngine& a, const MatchingEngine& b) {
    if (a.resting_orders() != b.resting_orders() || a.live_levels() != b.live_levels()) return false;
    for (InstrumentId i = 0; i < a.instrument_count(); ++i) {
        for (const Side s : {Side::Buy, Side::Sell}) {
            if (a.depth(i, s) != b.depth(i, s)) return false;
            for (std::uint32_t r = 0; r < a.depth(i, s); ++r) {
                const LevelView x = a.level(i, s, r);
                const LevelView y = b.level(i, s, r);
                if (x.price != y.price || x.qty != y.qty || x.orders != y.orders) return false;
            }
        }
    }
    return true;
}

const EventSink kDiscard(nullptr, [](void*, const ExecEvent&) {});
EventSink hash_into(EventHasher& h) {
    return EventSink(&h, [](void* c, const ExecEvent& e) { static_cast<EventHasher*>(c)->add(e); });
}

void test_journal_round_trip_and_torn_tail() {
    const std::string path = tmp("roundtrip.jrnl");
    std::remove(path.c_str());
    sim::StreamConfig cfg;
    cfg.orders = 1000;
    const auto stream = sim::generate_order_stream(cfg);
    {
        JournalWriter w(path);
        PX_CHECK(w.ok());
        for (std::size_t i = 0; i < stream.size(); ++i) PX_CHECK(w.append(i + 1, stream[i]));
    }
    std::vector<JournalRecord> recs;
    JournalReader r(path);
    PX_CHECK(r.read_more(recs));
    PX_CHECK(recs.size() == stream.size());
    bool same = recs.size() == stream.size();
    for (std::size_t i = 0; same && i < recs.size(); ++i) {
        const OrderRequest& a = recs[i].req;
        const OrderRequest& b = stream[i];
        same = recs[i].in_seq == i + 1 && a.order_id == b.order_id && a.price == b.price && a.qty == b.qty &&
               a.owner == b.owner && a.instr == b.instr && a.type == b.type && a.ord == b.ord && a.tif == b.tif &&
               a.side == b.side;
    }
    PX_CHECK(same);

    // A crash mid-write: half a record at the end. It is not returned.
    {
        std::FILE* f = std::fopen(path.c_str(), "ab");
        const std::uint8_t half[20] = {1, 2, 3};
        std::fwrite(half, 1, sizeof(half), f);
        std::fclose(f);
    }
    std::vector<JournalRecord> again;
    JournalReader r2(path);
    PX_CHECK(r2.read_more(again));
    PX_CHECK(again.size() == stream.size());
    PX_CHECK(r2.good_bytes() == sizeof(kJournalMagic) + stream.size() * kJournalRecordBytes);

    // A flipped bit inside a record: reading stops there.
    {
        std::FILE* f = std::fopen(path.c_str(), "r+b");
        std::fseek(f, static_cast<long>(sizeof(kJournalMagic) + 10 * kJournalRecordBytes + 12), SEEK_SET);
        const std::uint8_t x = 0xff;
        std::fwrite(&x, 1, 1, f);
        std::fclose(f);
    }
    std::vector<JournalRecord> cut;
    JournalReader r3(path);
    PX_CHECK(r3.read_more(cut));
    PX_CHECK(cut.size() == 10);
}

// Runs `stream` to `crash_at`, snapshotting at `snap_at`, loses the engine,
// recovers from the files alone, finishes the stream, and compares everything
// with a run that was never interrupted.
void recover_and_compare(const std::vector<OrderRequest>& stream, const sim::StreamConfig& cfg, std::size_t snap_at,
                         std::size_t crash_at, std::uint64_t want_hash, std::uint64_t want_events,
                         const MatchingEngine& want_book) {
    const std::string jpath = tmp("crash.jrnl");
    const std::string spath = tmp("crash.snap");
    std::remove(jpath.c_str());
    std::remove(spath.c_str());

    EventHasher seen; // every event a client of the venue would have received
    {
        auto engine = fresh(cfg);
        DurableEngine d(*engine, jpath);
        for (std::size_t i = 0; i < crash_at; ++i) {
            if (i == snap_at) PX_CHECK(save_snapshot(spath, d.snapshot()));
            PX_CHECK(d.process(stream[i], hash_into(seen)));
        }
        // the process dies here: engine, book and DurableEngine are gone
    }
    {
        std::FILE* f = std::fopen(jpath.c_str(), "ab"); // and it died mid-write
        const std::uint8_t torn[17] = {9, 9, 9};
        std::fwrite(torn, 1, sizeof(torn), f);
        std::fclose(f);
    }

    auto engine = fresh(cfg);
    DurableEngine d(*engine, ""); // recovered engine; journalling the tail is not what is under test
    Snapshot snap;
    if (snap_at < crash_at) {
        PX_CHECK(load_snapshot(spath, snap));
        PX_CHECK(snap.in_seq == snap_at);
        PX_CHECK(d.restore(snap));
    }
    JournalReader reader(jpath);
    const long replayed = d.follow(reader, kDiscard); // these events were already sent before the crash
    PX_CHECK(replayed == static_cast<long>(crash_at - (snap_at < crash_at ? snap_at : 0)));
    PX_CHECK(d.in_seq() == crash_at);
    for (std::size_t i = crash_at; i < stream.size(); ++i) PX_CHECK(d.process(stream[i], hash_into(seen)));

    PX_CHECK(seen.value() == want_hash);
    PX_CHECK(seen.count() == want_events);
    PX_CHECK(same_books(*engine, want_book));
    const char* why = nullptr;
    PX_CHECK(engine->check_invariants(&why));
}

void test_crash_recovery() {
    sim::StreamConfig cfg;
    cfg.orders = 60'000;
    cfg.seed = 11;
    const auto stream = sim::generate_order_stream(cfg);
    auto direct = fresh(cfg);
    const EventHasher want = sim::run_direct(*direct, stream);

    recover_and_compare(stream, cfg, 20'000, 41'234, want.value(), want.count(), *direct); // snapshot, then journal tail
    recover_and_compare(stream, cfg, 59'999, 60'000, want.value(), want.count(), *direct); // crash after the last order
    recover_and_compare(stream, cfg, 0, 1, want.value(), want.count(), *direct);           // empty snapshot
    recover_and_compare(stream, cfg, 70'000, 33'333, want.value(), want.count(), *direct); // no snapshot: journal only
    std::printf("recovery  hash=%016llx events=%llu (matches the uninterrupted run at 4 cut points)\n",
                static_cast<unsigned long long>(want.value()), static_cast<unsigned long long>(want.count()));
}

void test_snapshot_is_the_book() {
    sim::StreamConfig cfg;
    cfg.orders = 30'000;
    cfg.seed = 5;
    const auto stream = sim::generate_order_stream(cfg);
    auto engine = fresh(cfg);
    DurableEngine d(*engine, "");
    for (const OrderRequest& r : stream) d.process(r, kDiscard);
    PX_CHECK(d.resting().size() == engine->resting_orders()); // the event-built list agrees with the engine

    const std::string spath = tmp("book.snap");
    PX_CHECK(save_snapshot(spath, d.snapshot()));
    Snapshot s;
    PX_CHECK(load_snapshot(spath, s));
    auto copy = fresh(cfg);
    DurableEngine d2(*copy, "");
    PX_CHECK(d2.restore(s));
    PX_CHECK(same_books(*engine, *copy));
    PX_CHECK(!d2.restore(s)); // only into a fresh engine

    // Planted defect: one byte of the snapshot changed. It must be refused.
    {
        std::FILE* f = std::fopen(spath.c_str(), "r+b");
        std::fseek(f, 40, SEEK_SET);
        const std::uint8_t x = 0x5a;
        std::fwrite(&x, 1, 1, f);
        std::fclose(f);
    }
    Snapshot bad;
    PX_CHECK(!load_snapshot(spath, bad));
}

void test_standby_takes_over() {
    sim::StreamConfig cfg;
    cfg.orders = 40'000;
    cfg.seed = 23;
    const auto stream = sim::generate_order_stream(cfg);
    auto direct = fresh(cfg);
    const EventHasher want = sim::run_direct(*direct, stream);

    const std::string jpath = tmp("standby.jrnl");
    std::remove(jpath.c_str());
    auto primary_engine = fresh(cfg);
    auto standby_engine = fresh(cfg);
    EventHasher standby_events;
    DurableEngine standby(*standby_engine, "");
    JournalReader reader(jpath);
    const std::size_t fail_at = 27'001;
    {
        DurableEngine primary(*primary_engine, jpath);
        for (std::size_t i = 0; i < fail_at; ++i) {
            PX_CHECK(primary.process(stream[i], kDiscard));
            if (i % 5000 == 4999) PX_CHECK(standby.follow(reader, hash_into(standby_events)) >= 0);
        }
    } // the primary fails
    PX_CHECK(standby.follow(reader, hash_into(standby_events)) >= 0); // drain what it had written
    PX_CHECK(standby.in_seq() == fail_at);
    for (std::size_t i = fail_at; i < stream.size(); ++i) PX_CHECK(standby.process(stream[i], hash_into(standby_events)));
    PX_CHECK(standby_events.value() == want.value());
    PX_CHECK(same_books(*standby_engine, *direct));

    // Planted defect: a record lost from the stream a standby applies. It must
    // stop, not carry on with a book that has silently diverged.
    auto e = fresh(cfg);
    DurableEngine lossy(*e, "");
    PX_CHECK(lossy.apply_recorded(JournalRecord{1, stream[0]}, kDiscard));
    PX_CHECK(!lossy.apply_recorded(JournalRecord{3, stream[2]}, kDiscard));
    PX_CHECK(lossy.apply_recorded(JournalRecord{1, stream[0]}, kDiscard)); // a repeat is skipped, not applied twice
    PX_CHECK(lossy.in_seq() == 1);
}

bool feed_matches_engine(const FeedBook& fb, const MatchingEngine& e) {
    if (fb.resting_orders() != e.resting_orders()) return false;
    for (InstrumentId i = 0; i < e.instrument_count(); ++i) {
        for (const Side s : {Side::Buy, Side::Sell}) {
            const auto lv = fb.levels(i, s);
            if (lv.size() != e.depth(i, s)) return false;
            for (std::uint32_t r = 0; r < lv.size(); ++r) {
                const LevelView x = e.level(i, s, r);
                if (x.price != lv[r].price || x.qty != lv[r].qty || x.orders != lv[r].orders) return false;
            }
        }
    }
    return true;
}

struct PubCtx {
    Publisher* pub;
    std::uint64_t trades = 0, volume = 0;
};

void test_feed_and_reports() {
    sim::StreamConfig cfg;
    cfg.orders = 50'000;
    cfg.seed = 31;
    const auto stream = sim::generate_order_stream(cfg);
    auto engine = fresh(cfg);

    // The wire: every public message goes through its 32 bytes and back.
    std::vector<FeedMsg> wire;
    std::map<OwnerId, std::vector<Report>> inbox;
    Publisher pub(
        [&wire](const FeedMsg& m) {
            std::uint8_t b[kFeedMsgBytes];
            encode_feed(b, m);
            wire.push_back(decode_feed(b));
        },
        [&inbox](const Report& r) { inbox[r.owner].push_back(r); });

    FeedBook clean;        // hears everything
    FeedBook lossy;        // loses about one message in 200 and must notice every time
    std::mt19937_64 rng(99);
    std::size_t next = 0, gaps = 0, recoveries = 0, checked = 0;
    bool clean_ok = true, lossy_ok = true, lossy_never_wrong = true;
    PubCtx ctx{&pub};
    const EventSink sink(&ctx, [](void* c, const ExecEvent& e) {
        auto* p = static_cast<PubCtx*>(c);
        if (e.type == EventType::Trade) {
            ++p->trades;
            p->volume += e.qty;
        }
        p->pub->on_event(e);
    });

    for (std::size_t i = 0; i < stream.size(); ++i) {
        pub.begin(stream[i]);
        engine->process(stream[i], sink);
        pub.end();
        for (; next < wire.size(); ++next) {
            const FeedMsg& m = wire[next];
            clean_ok = clean_ok && clean.apply(m) == FeedBook::Result::Applied;
            if (rng() % 200 == 0) continue; // lost on the way
            const bool was_stale = lossy.stale();
            const FeedBook::Result r = lossy.apply(m);
            if (r == FeedBook::Result::Gap) gaps += was_stale ? 0 : 1;
            else if (r != FeedBook::Result::Applied) lossy_ok = false;
        }
        if (lossy.stale()) { // caught up from a snapshot, as a listener would on the recovery channel
            lossy.load(pub.snapshot());
            ++recoveries;
        }
        if (i % 500 == 499) {
            ++checked;
            clean_ok = clean_ok && feed_matches_engine(clean, *engine);
            // A listener that lost a message without yet seeing the next one is behind,
            // and knows nothing is wrong. It must never be wrong once it is current.
            if (lossy.seq() == pub.feed_seq()) lossy_never_wrong = lossy_never_wrong && feed_matches_engine(lossy, *engine);
        }
    }
    PX_CHECK(clean_ok);
    PX_CHECK(lossy_ok);
    PX_CHECK(lossy_never_wrong);
    PX_CHECK(gaps > 50 && recoveries == gaps); // every loss was noticed and repaired
    PX_CHECK(feed_matches_engine(clean, *engine));
    PX_CHECK(clean.trades() == ctx.trades && clean.volume() == ctx.volume); // the feed alone gives every trade
    PX_CHECK(checked == 100);

    // Planted defect: a listener that ignores the gap check ends with the wrong book.
    {
        FeedBook naive;
        bool wrong = false;
        std::uint64_t skipped = 0;
        for (const FeedMsg& m : wire) {
            if (m.seq == 1000) {
                ++skipped;
                continue;
            }
            FeedMsg forced = m;
            forced.seq -= skipped; // pretend nothing was missed
            if (naive.apply(forced) != FeedBook::Result::Applied) wrong = true;
        }
        PX_CHECK(wrong || !feed_matches_engine(naive, *engine));
    }

    // Private reports: only about the receiver's own orders, numbered without
    // gaps, and each fill reported once to each side.
    std::uint64_t fills = 0, fill_qty = 0;
    bool private_ok = true;
    std::map<OrderId, OwnerId> owner_of;
    for (const OrderRequest& r : stream)
        if (r.type == ReqType::New) owner_of[r.order_id] = r.owner;
    for (const auto& [owner, reports] : inbox) {
        std::uint64_t n = 0;
        for (const Report& r : reports) {
            private_ok = private_ok && r.owner == owner && r.seq == ++n;
            if (r.type != ReportType::Rejected) private_ok = private_ok && owner_of[r.order_id] == owner;
            if (r.type == ReportType::Filled) {
                ++fills;
                fill_qty += r.qty;
            }
        }
    }
    PX_CHECK(private_ok);
    PX_CHECK(fills == 2 * ctx.trades && fill_qty == 2 * ctx.volume);
    std::printf("feed      %zu public messages, %zu gaps noticed and recovered, %llu trades\n", wire.size(), gaps,
                static_cast<unsigned long long>(ctx.trades));
}

void test_risk_gate() {
    auto engine = std::make_unique<MatchingEngine>();
    const InstrumentId x = engine->add_instrument("X", 100);
    RiskLimits lim;
    lim.max_order_qty = 500;
    lim.max_order_value = 100 * kPriceScale * 400; // 400 units at 100.00
    lim.collar = 50 * 100;                         // 50 ticks
    lim.max_open_orders = 3;
    lim.max_messages = 6;
    lim.throttle_window = 100;
    std::vector<Report> reports;
    std::vector<FeedMsg> feed;
    Venue v(*engine, 2, lim, "", [&feed](const FeedMsg& m) { feed.push_back(m); },
            [&reports](const Report& r) { reports.push_back(r); });
    std::uint64_t cs[2] = {0, 0};
    OrderId id = 0;
    const auto send = [&](std::uint32_t session, const OrderRequest& r) { return v.submit(session, ++cs[session], r); };
    const Price px100 = 100 * kPriceScale;

    // Before any trade there is no reference price, so the collar cannot bind.
    PX_CHECK(send(0, limit_order(++id, x, 1, Side::Sell, px100, 100)).risk == RiskReject::None);
    PX_CHECK(send(1, limit_order(++id, x, 2, Side::Buy, px100, 10)).risk == RiskReject::None); // trades at 100.00
    PX_CHECK(send(1, limit_order(++id, x, 2, Side::Buy, px100, 501)).risk == RiskReject::OrderTooLarge);
    PX_CHECK(send(1, limit_order(++id, x, 2, Side::Buy, px100, 450)).risk == RiskReject::ValueTooLarge);
    PX_CHECK(send(1, limit_order(++id, x, 2, Side::Buy, px100 + 51 * 100, 10)).risk == RiskReject::PriceOutsideCollar);
    PX_CHECK(send(1, limit_order(++id, x, 2, Side::Buy, px100 - 50 * 100, 10)).risk == RiskReject::None); // on the collar
    PX_CHECK(engine->resting_orders() == 2);
    PX_CHECK(!reports.empty() && reports.back().type == ReportType::Accepted);

    // A market order may not trade beyond the collar: the far ask is left alone.
    {
        auto e = std::make_unique<MatchingEngine>();
        const InstrumentId m = e->add_instrument("M", 100);
        RiskLimits only_collar;
        only_collar.collar = 50 * 100;
        Venue vc(*e, 1, only_collar, "", nullptr, nullptr);
        std::uint64_t c = 0;
        vc.submit(0, ++c, limit_order(1, m, 1, Side::Sell, px100 + 80 * 100, 20)); // posted before any reference price
        vc.submit(0, ++c, limit_order(2, m, 1, Side::Sell, px100, 10));
        vc.submit(0, ++c, limit_order(3, m, 2, Side::Buy, px100, 10));             // trades: the reference is 100.00
        vc.submit(0, ++c, limit_order(4, m, 1, Side::Sell, px100 + 30 * 100, 20));
        PX_CHECK(e->depth(m, Side::Sell) == 2);
        PX_CHECK(vc.submit(0, ++c, market_order(5, m, 2, Side::Buy, 100)).risk == RiskReject::None);
        PX_CHECK(e->depth(m, Side::Sell) == 1 && e->level(m, Side::Sell, 0).price == px100 + 80 * 100);
        PX_CHECK(e->resting_orders() == 1); // and the market order's remainder did not rest

        // Planted defect: with no collar the same market order takes the far ask too.
        auto e0 = std::make_unique<MatchingEngine>();
        const InstrumentId m0 = e0->add_instrument("M", 100);
        Venue v0(*e0, 1, RiskLimits{}, "", nullptr, nullptr);
        c = 0;
        v0.submit(0, ++c, limit_order(1, m0, 1, Side::Sell, px100 + 80 * 100, 20));
        v0.submit(0, ++c, limit_order(4, m0, 1, Side::Sell, px100 + 30 * 100, 20));
        v0.submit(0, ++c, market_order(5, m0, 2, Side::Buy, 100));
        PX_CHECK(e0->depth(m0, Side::Sell) == 0);
    }

    // The throttle counts venue messages, so it decides the same way on replay.
    {
        auto e = std::make_unique<MatchingEngine>();
        const InstrumentId t = e->add_instrument("T", 100);
        RiskLimits th;
        th.max_messages = 3;
        th.throttle_window = 10;
        Venue vt(*e, 1, th, "", nullptr, nullptr);
        std::uint64_t c = 0;
        int passed = 0, throttled = 0;
        for (OrderId i = 1; i <= 9; ++i) {
            const RiskReject r = vt.submit(0, ++c, limit_order(i, t, 1, Side::Buy, px100, 1)).risk;
            passed += r == RiskReject::None;
            throttled += r == RiskReject::Throttled;
        }
        PX_CHECK(passed == 3 && throttled == 6);
        PX_CHECK(vt.submit(0, ++c, limit_order(10, t, 1, Side::Buy, px100, 1)).risk == RiskReject::None); // next window
    }

    // Open-order cap, and cancels are never refused.
    auto e2 = std::make_unique<MatchingEngine>();
    const InstrumentId y = e2->add_instrument("Y", 100);
    RiskLimits cap;
    cap.max_open_orders = 3;
    cap.max_messages = 4;
    cap.throttle_window = 1'000'000;
    Venue v2(*e2, 1, cap, "", nullptr, nullptr);
    std::uint64_t c2 = 0;
    PX_CHECK(v2.submit(0, ++c2, limit_order(1, y, 7, Side::Buy, px100, 1)).risk == RiskReject::None);
    PX_CHECK(v2.submit(0, ++c2, limit_order(2, y, 7, Side::Buy, px100, 1)).risk == RiskReject::None);
    PX_CHECK(v2.submit(0, ++c2, limit_order(3, y, 7, Side::Buy, px100, 1)).risk == RiskReject::None);
    PX_CHECK(v2.submit(0, ++c2, limit_order(4, y, 7, Side::Buy, px100, 1)).risk == RiskReject::TooManyOpenOrders);
    PX_CHECK(v2.submit(0, ++c2, limit_order(5, y, 7, Side::Buy, px100, 1)).risk == RiskReject::Throttled);
    PX_CHECK(v2.submit(0, ++c2, cancel_order(1, y, 7)).risk == RiskReject::None); // a cancel always passes
    PX_CHECK(e2->resting_orders() == 2);

    // Kill switch: resting orders pulled, new orders refused, cancels still pass.
    PX_CHECK(v2.kill(7) == 2);
    PX_CHECK(e2->resting_orders() == 0);
    PX_CHECK(v2.submit(0, ++c2, limit_order(6, y, 7, Side::Buy, px100, 1)).risk == RiskReject::Killed);
    v2.risk().revive(7);

    // Planted defect: with no gate at all, the oversized order reaches the book.
    auto e3 = std::make_unique<MatchingEngine>();
    const InstrumentId z = e3->add_instrument("Z", 100);
    Venue open(*e3, 1, RiskLimits{}, "", nullptr, nullptr);
    PX_CHECK(open.submit(0, 1, limit_order(1, z, 1, Side::Buy, px100, 1'000'000)).risk == RiskReject::None);
    PX_CHECK(e3->resting_orders() == 1);
}

void test_sequencer() {
    // Session numbering: a resend is dropped, a skipped number is refused.
    Sequencer s(2);
    std::uint64_t n = 0;
    PX_CHECK(s.admit(0, 1, n) == Admit::Accepted && n == 1);
    PX_CHECK(s.admit(1, 1, n) == Admit::Accepted && n == 2);
    PX_CHECK(s.admit(0, 1, n) == Admit::Duplicate);
    PX_CHECK(s.admit(0, 3, n) == Admit::Gap);
    PX_CHECK(s.expected(0) == 2);
    PX_CHECK(s.admit(0, 2, n) == Admit::Accepted && n == 3);
    PX_CHECK(s.admit(5, 1, n) == Admit::UnknownSession);

    // A resent order cannot enter the book twice.
    auto engine = std::make_unique<MatchingEngine>();
    const InstrumentId x = engine->add_instrument("X", 100);
    Venue v(*engine, 1, RiskLimits{}, "", nullptr, nullptr);
    const OrderRequest o = limit_order(1, x, 1, Side::Buy, 100 * kPriceScale, 5);
    PX_CHECK(v.submit(0, 1, o).admit == Admit::Accepted);
    PX_CHECK(v.submit(0, 1, o).admit == Admit::Duplicate);
    PX_CHECK(engine->resting_orders() == 1 && engine->next_seq() == 2);

    // Fairness: with every ring full, each pass takes exactly one from each
    // session, so a session with 10x the backlog gets no more than its turn.
    {
        MergeSequencer<64> m(3);
        const std::size_t backlog[3] = {40, 4, 4};
        for (std::uint32_t r = 0; r < 3; ++r)
            for (std::size_t i = 0; i < backlog[r]; ++i) {
                Inbound in;
                in.client_seq = i + 1;
                PX_CHECK(m.ring(r).try_push(in));
            }
        std::vector<std::uint32_t> order;
        for (int pass = 0; pass < 4; ++pass) m.poll([&order](std::uint64_t, const Inbound& in) { order.push_back(in.session); });
        std::size_t per[3] = {0, 0, 0};
        for (const std::uint32_t sess : order) ++per[sess];
        PX_CHECK(order.size() == 12 && per[0] == 4 && per[1] == 4 && per[2] == 4);
    }

    // Four producer threads, one sequencer thread: everything arrives once,
    // numbered without a gap, and each session's own order is kept.
    constexpr std::uint32_t kSessions = 4;
    constexpr std::uint64_t kEach = 50'000;
    MergeSequencer<> m(kSessions);
    std::vector<std::thread> producers;
    for (std::uint32_t sess = 0; sess < kSessions; ++sess) {
        producers.emplace_back([&m, sess] {
            for (std::uint64_t i = 1; i <= kEach; ++i) {
                Inbound in;
                in.client_seq = i;
                in.req.order_id = (static_cast<std::uint64_t>(sess) << 32) | i;
                while (!m.ring(sess).try_push(in)) std::this_thread::yield();
                if (i % 1000 == 0) { // a client that retries: the same message again
                    while (!m.ring(sess).try_push(in)) std::this_thread::yield();
                }
            }
        });
    }
    std::uint64_t last_num = 0, received = 0;
    std::uint64_t last_client[kSessions] = {0, 0, 0, 0};
    bool gapless = true, in_order = true;
    while (received < kSessions * kEach) {
        m.poll([&](std::uint64_t num, const Inbound& in) {
            gapless = gapless && num == last_num + 1;
            last_num = num;
            in_order = in_order && in.client_seq == last_client[in.session] + 1 &&
                       in.req.order_id == ((static_cast<std::uint64_t>(in.session) << 32) | in.client_seq);
            last_client[in.session] = in.client_seq;
            ++received;
        });
    }
    for (std::thread& t : producers) t.join();
    while (m.poll([&](std::uint64_t, const Inbound&) { ++received; }) != 0) {} // the last resends
    PX_CHECK(gapless && in_order);
    PX_CHECK(received == kSessions * kEach);
    PX_CHECK(m.duplicates() == kSessions * (kEach / 1000) && m.gaps() == 0);
}

void test_stage_stamps() {
    sim::StreamConfig cfg;
    cfg.orders = 50'000;
    cfg.seed = 3;
    const auto stream = sim::generate_order_stream(cfg);
    auto direct = fresh(cfg);
    const EventHasher want = sim::run_direct(*direct, stream);
    auto engine = fresh(cfg);
    sim::PipelineOptions opt;
    opt.warmup = 1000;
    opt.pin_threads = false;
    opt.record_enqueue_times = true;
    const sim::PipelineResult res = sim::run_pipeline(*engine, stream, opt);
    PX_CHECK(res.event_hash == want.value()); // stamping changes nothing that is matched
    PX_CHECK(res.enqueue_tsc.size() == res.recorded && res.start_tsc.size() == res.recorded);
    bool stamped = true, ordered = true;
    for (std::size_t i = 0; i < res.recorded; ++i) {
        stamped = stamped && res.enqueue_tsc[i] != 0 && res.start_tsc[i] != 0;
        if (i > 0) ordered = ordered && res.enqueue_tsc[i] >= res.enqueue_tsc[i - 1] && res.start_tsc[i] >= res.start_tsc[i - 1];
    }
    PX_CHECK(stamped && ordered);
}

void test_shards() {
    sim::StreamConfig cfg;
    cfg.orders = 120'000;
    cfg.seed = 17;
    cfg.instruments = 8;
    const auto stream = sim::generate_order_stream(cfg);
    auto whole = fresh(cfg);
    (void)sim::run_direct(*whole, stream);

    constexpr std::uint32_t kShards = 3;
    ShardedEngines sharded(kShards, EngineConfig{}, [&cfg](MatchingEngine& e) { sim::add_instruments(e, cfg); });
    for (const OrderRequest& r : stream) sharded.route(r);
    sharded.finish();

    std::uint64_t total = 0;
    for (std::uint32_t k = 0; k < kShards; ++k) {
        // The reference: this shard's requests, in order, on one thread.
        auto alone = fresh(cfg);
        EventHasher h;
        std::uint64_t n = 0;
        for (const OrderRequest& r : stream) {
            if (sharded.shard_of(r.instr) != k) continue;
            alone->process(r, hash_into(h));
            ++n;
        }
        total += n;
        PX_CHECK(sharded.processed(k) == n && n > 0);
        PX_CHECK(sharded.event_hash(k) == h.value() && sharded.events(k) == h.count());
        const char* why = nullptr;
        PX_CHECK(sharded.engine(k).check_invariants(&why));
    }
    PX_CHECK(total == stream.size());

    // Each instrument's book is what the single engine holding all of them has.
    bool same = true;
    for (InstrumentId i = 0; i < cfg.instruments; ++i) {
        const MatchingEngine& e = sharded.engine(sharded.shard_of(i));
        for (const Side s : {Side::Buy, Side::Sell}) {
            same = same && e.depth(i, s) == whole->depth(i, s);
            for (std::uint32_t r = 0; same && r < e.depth(i, s); ++r) {
                const LevelView x = e.level(i, s, r), y = whole->level(i, s, r);
                same = x.price == y.price && x.qty == y.qty && x.orders == y.orders;
            }
            // and a shard holds nothing for an instrument it does not own
            for (std::uint32_t k = 0; k < kShards; ++k)
                if (k != sharded.shard_of(i)) same = same && sharded.engine(k).depth(i, s) == 0;
        }
    }
    PX_CHECK(same);
}

void test_wire() {
    // A stream of mixed messages, delivered one byte at a time, then in odd chunks.
    std::vector<std::uint8_t> bytes;
    const OrderRequest o = limit_order(77, 3, 9, Side::Sell, 1234500, 250, Tif::IOC);
    Report rep;
    rep.seq = 5;
    rep.order_id = 77;
    rep.price = 1234500;
    rep.qty = 100;
    rep.leaves = 150;
    rep.type = ReportType::Filled;
    frame_logon(bytes, 2, 9);
    frame_order(bytes, 41, o);
    frame_heartbeat(bytes);
    frame_logon_ack(bytes, 42);
    frame_not_taken(bytes, 40, static_cast<std::uint8_t>(Admit::Duplicate));
    frame_report(bytes, rep);

    for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, bytes.size()}) {
        FrameReader r;
        std::vector<Frame> got;
        for (std::size_t i = 0; i < bytes.size(); i += chunk) {
            r.feed(bytes.data() + i, std::min(chunk, bytes.size() - i));
            Frame f;
            while (r.next(f)) got.push_back(f);
        }
        PX_CHECK(got.size() == 6 && !r.broken());
        if (got.size() != 6) continue;
        std::uint32_t session = 0;
        OwnerId owner = 0;
        PX_CHECK(parse_logon(got[0], session, owner) && session == 2 && owner == 9);
        std::uint64_t cs = 0;
        OrderRequest back;
        PX_CHECK(parse_order(got[1], cs, back) && cs == 41 && back.order_id == 77 && back.instr == 3 && back.owner == 9 &&
                 back.side == Side::Sell && back.price == 1234500 && back.qty == 250 && back.tif == Tif::IOC);
        PX_CHECK(got[2].type == 'H' && got[2].body.empty());
        std::uint64_t v = 0;
        PX_CHECK(parse_u64(got[3], 'A', v) && v == 42);
        PX_CHECK(parse_u64(got[4], 'N', v) && v == 40);
        Report rb;
        PX_CHECK(parse_report(got[5], rb) && rb.seq == 5 && rb.order_id == 77 && rb.qty == 100 && rb.leaves == 150 &&
                 rb.type == ReportType::Filled);
    }

    // A length no message can have: the reader gives up, and stays given up.
    FrameReader bad;
    const std::uint8_t junk[4] = {0xff, 0xff, 'O', 0};
    bad.feed(junk, sizeof(junk));
    Frame f;
    PX_CHECK(!bad.next(f) && bad.broken());
    // A message of the wrong size for its type is not parsed.
    Frame shortf;
    shortf.type = 'O';
    shortf.body.assign(10, 0);
    std::uint64_t cs = 0;
    OrderRequest q;
    PX_CHECK(!parse_order(shortf, cs, q));
}

// Amends through every stage that keeps its own picture of the book: the
// resting list (so a snapshot is right), the public feed and the journal.
void test_amend_through_the_venue() {
    sim::StreamConfig cfg;
    cfg.orders = 40'000;
    cfg.seed = 41;
    const auto base = sim::generate_order_stream(cfg);
    const std::string jpath = tmp("amend.jrnl");
    std::remove(jpath.c_str());

    auto engine = fresh(cfg);
    DurableEngine d(*engine, jpath);
    FeedBook fb;
    bool feed_ok = true;
    std::uint64_t amended = 0, kept = 0;
    Publisher pub([&](const FeedMsg& m) { feed_ok = feed_ok && fb.apply(m) == FeedBook::Result::Applied; }, nullptr);
    EventHasher all;
    struct Ctx {
        Publisher* pub;
        EventHasher* h;
        std::uint64_t* amended;
        std::uint64_t* kept;
    } ctx{&pub, &all, &amended, &kept};
    const EventSink sink(&ctx, [](void* c, const ExecEvent& e) {
        auto* x = static_cast<Ctx*>(c);
        x->pub->on_event(e);
        x->h->add(e);
        if (e.type == EventType::Amended) {
            ++*x->amended;
            *x->kept += e.reason == static_cast<std::uint8_t>(AmendEffect::KeptPriority);
        }
    });
    const auto run = [&](const OrderRequest& r) {
        pub.begin(r);
        d.process(r, sink);
        pub.end();
    };
    std::mt19937_64 rng(7);
    bool books_ok = true;
    for (std::size_t i = 0; i < base.size(); ++i) {
        run(base[i]);
        if (i % 5 == 4 && d.resting().size() > 0) {
            const auto resting = d.resting().in_rest_order();
            const RestingOrder& o = resting[rng() % resting.size()];
            const Price step = static_cast<Price>(rng() % 7) - 3; // -3..+3 ticks; 0 keeps the price
            const Qty q = rng() % 2 == 0 ? std::max<Qty>(1, o.leaves / 2) : o.leaves + 5;
            run(amend_order(o.id, o.instr, o.owner, o.price + step * cfg.tick, q));
        }
        if (i % 2000 == 1999) books_ok = books_ok && feed_matches_engine(fb, *engine) && d.resting().size() == engine->resting_orders();
    }
    PX_CHECK(feed_ok && books_ok);
    PX_CHECK(amended > 5000 && kept > 500 && kept < amended);

    // A snapshot taken after all that restores the same book, and the journal
    // (amends included) replays to the same event stream.
    auto copy = fresh(cfg);
    DurableEngine d2(*copy, "");
    PX_CHECK(d2.restore(d.snapshot()));
    PX_CHECK(same_books(*engine, *copy));
    auto replayed = fresh(cfg);
    DurableEngine d3(*replayed, "");
    EventHasher again;
    JournalReader reader(jpath);
    PX_CHECK(d3.follow(reader, hash_into(again)) == static_cast<long>(d.in_seq()));
    PX_CHECK(again.value() == all.value() && same_books(*engine, *replayed));
}

} // namespace

int main() {
    test_journal_round_trip_and_torn_tail();
    test_crash_recovery();
    test_snapshot_is_the_book();
    test_standby_takes_over();
    test_feed_and_reports();
    test_risk_gate();
    test_sequencer();
    test_stage_stamps();
    test_shards();
    test_wire();
    test_amend_through_the_venue();
    PX_TEST_REPORT_AND_RETURN();
}
