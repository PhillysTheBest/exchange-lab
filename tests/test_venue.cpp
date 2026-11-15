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
#include "venue/venue.hpp"

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

} // namespace

int main() {
    test_journal_round_trip_and_torn_tail();
    test_crash_recovery();
    test_snapshot_is_the_book();
    test_standby_takes_over();
    test_feed_and_reports();
    test_risk_gate();
    test_sequencer();
    PX_TEST_REPORT_AND_RETURN();
}
