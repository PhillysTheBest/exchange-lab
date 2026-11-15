// tests/test_venue.cpp — the stages around the engine (venue/): the journal,
// snapshots and crash recovery, a standby, and the planted defects that show
// each check can fail.
#include "px/matching_engine.hpp"
#include "sim/order_stream.hpp"
#include "sim/pipeline.hpp"
#include "test_harness.hpp"
#include "venue/book_state.hpp"
#include "venue/journal.hpp"
#include "venue/recovery.hpp"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
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

} // namespace

int main() {
    test_journal_round_trip_and_torn_tail();
    test_crash_recovery();
    test_snapshot_is_the_book();
    test_standby_takes_over();
    PX_TEST_REPORT_AND_RETURN();
}
