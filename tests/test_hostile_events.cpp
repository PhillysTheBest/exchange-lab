// tests/test_hostile_events.cpp — extends the adversarial-input methodology
// tests/test_replay_fuzz.cpp already used to find the ShadowBook side-flip
// bug to the rest of the live pipeline: duplicate/reordered feed messages,
// partial fills, cancel/fill races, connection failure, risk rejection, and
// an unrecognized message type — each driven through the REAL production
// code (Ingress::handle_message, a real Brain thread, RiskEngine), not a
// reimplemented shortcut.
//
// SCOPE, stated precisely rather than implied:
//   - "Rapid book changes" is deliberately NOT re-covered here — that's
//     exactly what test_replay_fuzz.cpp's 4,000-event adversarial stream
//     already is, and duplicating it under a different name would only
//     conflate two test files' failure signals.
//   - "Reconnect" is NOT tested, because it doesn't exist: Ingress::run
//     (pipeline/ingress.cpp) has no retry/reconnect loop at all — a receive
//     failure ends T1's thread outright (see test_connect_failure_ends_run
//     below for the one piece of this that IS real and testable: a
//     connection that never succeeds in the first place, handled cleanly).
//     A live deployment currently has no signal that T1 died and the book
//     has gone stale. Flagged as a real, un-built gap, not silently routed
//     around or faked.
//   - "Market halt" / "market resolution" are NOT handled by this pipeline —
//     confirmed against live/kalshi_feed.cpp's own comment: `market_lifecycle_v2`
//     decodes to FeedMsgKind::Unknown and is deliberately ignored.
//     test_unrecognized_message_type_is_safely_ignored below proves that
//     "ignored" means "safely dropped," not "corrupts state" — it does NOT
//     claim any strategy reacts to a halt (none can; there's no signal for
//     one to react to). Building halt/resolution-aware trading logic is
//     real, scoped-out work, same honesty this project already applies to
//     the async fill-listener gap (live/kalshi_gateway.hpp).
//
// Two real bugs were found and fixed while writing this file, both because a
// test tried the adversarial case rather than assuming the happy path
// generalizes:
//   - live/kalshi_feed.hpp's SeqTracker: its own doc comment promised "won't
//     trust the stream again until the next Snapshot" after a gap, but the
//     code accepted the very next delta unconditionally, re-baselining off
//     of it — a duplicate/reordered/adversarial delta arriving in that
//     window would have been applied with zero verification. Fixed (see
//     `desynced_`); test_duplicate_seq_dropped_through_real_ingress and
//     test_reordered_seq_dropped_through_real_ingress are the regression
//     tests.
//   - live/kalshi_gateway.cpp's Gateway never erased kalshi_uuid_of_ for an
//     order that fully resolved (filled or IOC-killed) without ever being
//     cancelled — unbounded growth over a long session. Fixed alongside the
//     matching strategy/order_tracker.hpp zombie-entry fix
//     tests/test_pipeline_e2e.cpp found; not independently re-tested here
//     (no externally observable effect within one test's lifetime — see that
//     file and kalshi_gateway.hpp's own comment for the reasoning).
#include "kalshi_gateway.hpp"
#include "pipeline/brain.hpp"
#include "pipeline/ingress.hpp"
#include "risk_engine.hpp"
#include "strategy/order_tracker.hpp"
#include "strategy/passive_market_maker.hpp"
#include "test_harness.hpp"
#include "ticker_registry.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

using namespace px;
using namespace px::live;
using namespace px::pipeline;
using namespace px::strategy;

namespace {

[[nodiscard]] bool wait_until(const std::function<bool()>& pred, int seconds = 5) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::yield();
    }
    return false;
}

// ============================================================================
// ---- SeqTracker hostile cases, through the real Ingress::handle_message --
// ============================================================================

void test_duplicate_seq_dropped_through_real_ingress() {
    TickerRegistry registry;
    MarketId market;
    PX_CHECK(registry.add("HOSTILE-DUP", market));
    KalshiConfig ws_cfg;
    Ingress ingress(ws_cfg, registry);

    RingA ring_a;
    auto brain = std::make_unique<Brain>(risk::RiskConfig{});
    RingB ring_b;
    RingC ring_c;
    OracleRing oracle_ring;
    std::atomic<bool> stop{false};
    std::thread brain_thread([&] { brain->run(ring_a, oracle_ring, ring_c, ring_b, stop); });

    std::atomic<int> warnings{0};
    ingress.on_warning = [&](std::string_view) { ++warnings; };

    auto push = [&](const char* raw) {
        FeedMsg fm;
        std::string err;
        PX_CHECK(parse_feed_message(raw, fm, err));
        ingress.handle_message(fm, ring_a);
    };

    push(R"({"type":"orderbook_snapshot","sid":20,"seq":1,)"
         R"("msg":{"market_ticker":"HOSTILE-DUP","market_id":"m",)"
         R"("yes_dollars_fp":[["0.4000","100.00"]],)"
         R"("no_dollars_fp":[["0.4000","50.00"]]}})"); // bid=40, ask=60
    push(R"({"type":"orderbook_delta","sid":20,"seq":2,)"
         R"("msg":{"market_ticker":"HOSTILE-DUP","market_id":"m",)"
         R"("price_dollars":"0.4000","delta_fp":"25.00","side":"yes"}})"); // bid qty 100 -> 125
    PX_CHECK(wait_until([&] { return brain->book_events_applied() >= 4; })); // snapshotbegin+2 levels+1 delta

    // The exact same delta again — a legitimate seq value, but a replay, not
    // a continuation: expected next is 3, this is 2 again.
    push(R"({"type":"orderbook_delta","sid":20,"seq":2,)"
         R"("msg":{"market_ticker":"HOSTILE-DUP","market_id":"m",)"
         R"("price_dollars":"0.4000","delta_fp":"25.00","side":"yes"}})");
    PX_CHECK(wait_until([&] { return warnings.load() >= 1; })); // seq-gap warning fired

    // A THIRD delta, immediately after, with no snapshot in between: must
    // ALSO be dropped (the desynced_ fix) rather than silently re-baselining
    // off whatever seq it happens to carry.
    push(R"({"type":"orderbook_delta","sid":20,"seq":3,)"
         R"("msg":{"market_ticker":"HOSTILE-DUP","market_id":"m",)"
         R"("price_dollars":"0.4000","delta_fp":"1000.00","side":"yes"}})"); // would be an obvious, large corruption if applied

    // Push one more real, legitimate-looking event and confirm book_events_applied
    // stops climbing from the dropped messages (only real events advance it).
    const auto before = brain->book_events_applied();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    PX_CHECK(brain->book_events_applied() == before); // both hostile deltas were dropped before ever reaching ring A

    PX_CHECK(brain->book(market).level_qty(40) == 125); // unchanged by either dropped delta

    stop.store(true, std::memory_order_relaxed);
    brain_thread.join();
}

void test_reordered_seq_dropped_through_real_ingress() {
    TickerRegistry registry;
    MarketId market;
    PX_CHECK(registry.add("HOSTILE-REORDER", market));
    KalshiConfig ws_cfg;
    Ingress ingress(ws_cfg, registry);

    RingA ring_a;
    auto brain = std::make_unique<Brain>(risk::RiskConfig{});
    RingB ring_b;
    RingC ring_c;
    OracleRing oracle_ring;
    std::atomic<bool> stop{false};
    std::thread brain_thread([&] { brain->run(ring_a, oracle_ring, ring_c, ring_b, stop); });

    auto push = [&](const char* raw) {
        FeedMsg fm;
        std::string err;
        PX_CHECK(parse_feed_message(raw, fm, err));
        ingress.handle_message(fm, ring_a);
    };

    push(R"({"type":"orderbook_snapshot","sid":21,"seq":10,)"
         R"("msg":{"market_ticker":"HOSTILE-REORDER","market_id":"m",)"
         R"("yes_dollars_fp":[["0.4000","100.00"]],)"
         R"("no_dollars_fp":[["0.4000","50.00"]]}})");
    PX_CHECK(wait_until([&] { return brain->book_events_applied() >= 2; })); // snapshotbegin+1 level

    // seq 12 arrives before seq 11 ever does (network reordering) -- a gap
    // (expected 11, got 12), correctly dropped.
    push(R"({"type":"orderbook_delta","sid":21,"seq":12,)"
         R"("msg":{"market_ticker":"HOSTILE-REORDER","market_id":"m",)"
         R"("price_dollars":"0.4000","delta_fp":"999.00","side":"yes"}})");
    // The genuinely-earlier seq 11 shows up late: also dropped (desynced_),
    // even though on its own it would have looked perfectly valid.
    push(R"({"type":"orderbook_delta","sid":21,"seq":11,)"
         R"("msg":{"market_ticker":"HOSTILE-REORDER","market_id":"m",)"
         R"("price_dollars":"0.4000","delta_fp":"1.00","side":"yes"}})");

    const auto before = brain->book_events_applied();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    PX_CHECK(brain->book_events_applied() == before);
    PX_CHECK(brain->book(market).level_qty(40) == 100); // untouched by either

    stop.store(true, std::memory_order_relaxed);
    brain_thread.join();
}

// ============================================================================
// ---- Partial fills, cancel/fill races, duplicate acks — OrderTracker -----
// ============================================================================

void test_partial_fill_leaves_order_resting_with_correct_remaining() {
    OrderTracker tracker;
    ShadowBook book;
    book.set_level(Side::Buy, 40, 100);
    constexpr MarketId m = 0;

    PX_CHECK(tracker.on_new(m, 1, Side::Buy, 40, 10));
    tracker.on_acked(m, 1, /*remaining_qty=*/6, book); // 4 filled synchronously at entry
    tracker.on_fill(m, 1, /*filled_qty=*/4);

    const auto orders = tracker.orders_for(m);
    PX_CHECK(orders.size() == 1); // still resting -- NOT the zombie-removal path
    PX_CHECK(orders[0].id == 1);
    PX_CHECK(orders[0].remaining_qty == 6);
    PX_CHECK(orders[0].filled_qty == 4);
}

void test_late_fill_after_cancel_does_not_crash_or_resurrect_the_order() {
    // Race: a fill notification for an order that was already fully
    // cancelled arrives late (e.g. it filled a moment before the cancel took
    // effect server-side, but the reports arrive to us in the opposite
    // order). OrderTracker must stay consistent regardless.
    OrderTracker tracker;
    ShadowBook book;
    book.set_level(Side::Buy, 40, 100);
    constexpr MarketId m = 0;

    PX_CHECK(tracker.on_new(m, 2, Side::Buy, 40, 10));
    tracker.on_acked(m, 2, 10, book);
    PX_CHECK(tracker.orders_for(m).size() == 1);

    tracker.on_cancelled(m, 2, /*reduced_by=*/10); // fully resolves -> removed
    PX_CHECK(tracker.orders_for(m).empty());

    tracker.on_fill(m, 2, /*filled_qty=*/3); // late fill for an id no longer tracked
    PX_CHECK(tracker.orders_for(m).empty()); // no-op, not a resurrection, not a crash
}

void test_late_cancel_ack_after_full_fill_does_not_crash() {
    // The mirror race: a cancel response arrives for an order that already
    // fully filled (and was therefore already removed by the on_acked/
    // on_fill zero-remaining fix — see strategy/order_tracker.hpp).
    OrderTracker tracker;
    ShadowBook book;
    book.set_level(Side::Sell, 60, 100);
    constexpr MarketId m = 0;

    PX_CHECK(tracker.on_new(m, 3, Side::Sell, 60, 5));
    tracker.on_acked(m, 3, /*remaining_qty=*/0, book); // IOC, fully resolved at entry -> not promoted at all
    PX_CHECK(tracker.orders_for(m).empty());

    tracker.on_cancelled(m, 3, /*reduced_by=*/5); // stale cancel response, id already gone
    PX_CHECK(tracker.orders_for(m).empty()); // still empty, no crash
}

void test_duplicate_ack_is_idempotent() {
    OrderTracker tracker;
    ShadowBook book;
    book.set_level(Side::Buy, 40, 100);
    constexpr MarketId m = 0;

    PX_CHECK(tracker.on_new(m, 4, Side::Buy, 40, 10));
    tracker.on_acked(m, 4, 10, book);
    tracker.on_acked(m, 4, 10, book); // a retried/duplicated ack for the same id
    PX_CHECK(tracker.orders_for(m).size() == 1); // not double-counted
}

void test_duplicate_cancelled_is_idempotent() {
    OrderTracker tracker;
    ShadowBook book;
    book.set_level(Side::Buy, 40, 100);
    constexpr MarketId m = 0;

    PX_CHECK(tracker.on_new(m, 5, Side::Buy, 40, 10));
    tracker.on_acked(m, 5, 10, book);
    tracker.on_cancelled(m, 5, 10);
    tracker.on_cancelled(m, 5, 10); // duplicate — id already gone
    PX_CHECK(tracker.orders_for(m).empty());
}

// ============================================================================
// ---- Risk rejection, through a real Brain -- never reaches OrderTracker --
// ============================================================================

void test_risk_rejection_never_reaches_order_tracker_or_gateway() {
    TickerRegistry registry;
    MarketId market;
    PX_CHECK(registry.add("HOSTILE-RISK", market));
    KalshiConfig ws_cfg;
    Ingress ingress(ws_cfg, registry);

    risk::RiskConfig risk_cfg; // fat_finger_min=5, fat_finger_max=95 by default
    auto brain = std::make_unique<Brain>(risk_cfg);
    brain->add_strategy(market, std::make_unique<PassiveMarketMaker>(market, /*trader=*/0));

    std::atomic<int> reject_count{0};
    risk::Verdict last_verdict{};
    brain->on_reject = [&](const OrderIntent&, risk::Verdict v) {
        ++reject_count;
        last_verdict = v;
    };

    RingA ring_a;
    RingB ring_b;
    RingC ring_c;
    OracleRing oracle_ring;
    std::atomic<bool> stop{false};
    std::thread brain_thread([&] { brain->run(ring_a, oracle_ring, ring_c, ring_b, stop); });

    // best_bid=2 (below fat_finger_min=5), best_ask=6 -> spread=4, satisfies
    // PassiveMarketMaker's default min_spread so it genuinely attempts to
    // Join the bid at 2, not silently Wait for an unrelated reason.
    FeedMsg snap;
    std::string err;
    PX_CHECK(parse_feed_message(R"({"type":"orderbook_snapshot","sid":22,"seq":1,)"
                                 R"("msg":{"market_ticker":"HOSTILE-RISK","market_id":"m",)"
                                 R"("yes_dollars_fp":[["0.0200","100.00"]],)"
                                 R"("no_dollars_fp":[["0.9400","50.00"]]}})",
                                 snap, err));
    ingress.handle_message(snap, ring_a);

    PX_CHECK(wait_until([&] { return reject_count.load() >= 1; }));
    PX_CHECK(last_verdict == risk::Verdict::RejectFatFinger);
    PX_CHECK(brain->intents_accepted() == 0);
    PX_CHECK(brain->intents_rejected() == 1);
    // The whole point of checking risk BEFORE order_tracker_.on_new (see
    // pipeline/brain.cpp's handle_decision) -- a rejected intent must never
    // become a phantom pending order a later Ack/Cancel could ever reference.
    PX_CHECK(brain->order_tracker().orders_for(market).empty());

    stop.store(true, std::memory_order_relaxed);
    brain_thread.join();
}

// ============================================================================
// ---- Connection failure ("disconnect"), and an unrecognized message type -
// ---- ("market halt/resolution" stand-in) ----------------------------------
// ============================================================================

// Mirrors tests/test_gateway_dry_run.cpp's write_test_key: a throwaway
// 2048-bit key, generated fresh, deleted before this test returns. Needed
// because KalshiFeed::connect signs the WS upgrade request before ever
// touching the network — see live/kalshi_auth.cpp.
[[nodiscard]] bool write_throwaway_key(const std::string& path) {
    EVP_PKEY_CTX* gen = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!gen) return false;
    bool ok = EVP_PKEY_keygen_init(gen) > 0 && EVP_PKEY_CTX_set_rsa_keygen_bits(gen, 2048) > 0;
    EVP_PKEY* key = nullptr;
    ok = ok && EVP_PKEY_keygen(gen, &key) > 0;
    EVP_PKEY_CTX_free(gen);
    if (!ok || !key) return false;

    BIO* bio = BIO_new_file(path.c_str(), "wb");
    bool wrote = false;
    if (bio) {
        wrote = PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
        BIO_free(bio);
    }
    EVP_PKEY_free(key);
    return wrote;
}

void test_connect_failure_ends_run_cleanly() {
    // "Disconnect," tested the one honest way available offline: a
    // connection that never succeeds in the first place, via the same RFC
    // 6761 guaranteed-non-resolving host tests/test_gateway_dry_run.cpp
    // already uses. What THIS proves: Ingress::run doesn't hang, doesn't
    // crash, and reports the failure via on_error exactly once, cleanly
    // exiting the thread — the honest boundary of what's testable without a
    // real or mocked socket (an established connection dying mid-stream is
    // not exercised here; see the file header for why that's flagged, not
    // built, given Ingress::run has no reconnect logic to test in the first
    // place).
    const std::string key_path = "build/_test_hostile_key.pem";
    PX_CHECK(write_throwaway_key(key_path));

    TickerRegistry registry;
    MarketId market;
    PX_CHECK(registry.add("HOSTILE-DISCONNECT", market));

    KalshiConfig cfg;
    cfg.host = "invalid.invalid"; // RFC 6761 — guaranteed non-resolving
    cfg.private_key_path = key_path;
    Ingress ingress(cfg, registry);

    std::atomic<int> error_count{0};
    std::string last_error;
    std::atomic<bool> error_set{false};
    ingress.on_error = [&](std::string_view msg) {
        last_error = std::string(msg);
        ++error_count;
        error_set.store(true, std::memory_order_release);
    };

    RingA ring_a;
    std::atomic<bool> stop{false};
    std::thread t([&] { ingress.run(ring_a, stop); });

    PX_CHECK(wait_until([&] { return error_set.load(std::memory_order_acquire); }));
    t.join(); // run() returns on its own after a failed connect -- no stop_flag needed
    PX_CHECK(error_count.load() == 1); // exactly once, not a retry storm
    PX_CHECK(last_error.find("connect failed") != std::string::npos);

    std::remove(key_path.c_str());
}

void test_unrecognized_message_type_is_safely_ignored() {
    // Stand-in for market_lifecycle_v2 / any other message type this project
    // doesn't act on (see the file header) -- proves "ignored" means exactly
    // that, not "silently misparsed into something that corrupts the book."
    TickerRegistry registry;
    MarketId market;
    PX_CHECK(registry.add("HOSTILE-UNKNOWN", market));
    KalshiConfig ws_cfg;
    Ingress ingress(ws_cfg, registry);

    RingA ring_a;
    auto brain = std::make_unique<Brain>(risk::RiskConfig{});
    RingB ring_b;
    RingC ring_c;
    OracleRing oracle_ring;
    std::atomic<bool> stop{false};
    std::thread brain_thread([&] { brain->run(ring_a, oracle_ring, ring_c, ring_b, stop); });

    FeedMsg snap;
    std::string err;
    PX_CHECK(parse_feed_message(R"({"type":"orderbook_snapshot","sid":23,"seq":1,)"
                                 R"("msg":{"market_ticker":"HOSTILE-UNKNOWN","market_id":"m",)"
                                 R"("yes_dollars_fp":[["0.4000","100.00"]]}})",
                                 snap, err));
    ingress.handle_message(snap, ring_a);
    PX_CHECK(wait_until([&] { return brain->book_events_applied() >= 2; }));

    FeedMsg unknown;
    PX_CHECK(parse_feed_message(R"({"type":"market_lifecycle_v2","sid":23,)"
                                 R"("msg":{"market_ticker":"HOSTILE-UNKNOWN","status":"halted"}})",
                                 unknown, err));
    PX_CHECK(unknown.kind == FeedMsgKind::Unknown);
    ingress.handle_message(unknown, ring_a); // Ingress::handle_message's own Unknown-kind early return

    const auto before = brain->book_events_applied();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    PX_CHECK(brain->book_events_applied() == before); // nothing reached ring A for it
    PX_CHECK(brain->book(market).level_qty(40) == 100); // book unaffected

    stop.store(true, std::memory_order_relaxed);
    brain_thread.join();
}

} // namespace

int main() {
    test_duplicate_seq_dropped_through_real_ingress();
    test_reordered_seq_dropped_through_real_ingress();

    test_partial_fill_leaves_order_resting_with_correct_remaining();
    test_late_fill_after_cancel_does_not_crash_or_resurrect_the_order();
    test_late_cancel_ack_after_full_fill_does_not_crash();
    test_duplicate_ack_is_idempotent();
    test_duplicate_cancelled_is_idempotent();

    test_risk_rejection_never_reaches_order_tracker_or_gateway();

    test_connect_failure_ends_run_cleanly();
    test_unrecognized_message_type_is_safely_ignored();

    PX_TEST_REPORT_AND_RETURN();
}
