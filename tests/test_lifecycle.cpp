// tests/test_lifecycle.cpp — market halt/resolution awareness (Stage 4,
// second pass): market_lifecycle_v2 decoding, the event_type -> MarketStatus
// mapping, Ingress turning a status-changing event into a BookEvent, and
// Brain's gate against it (OPEN -> HALTED: stop new execution; RESOLVED:
// stop trading permanently) — proven through the REAL PassiveMarketMaker
// strategy reacting to REAL book movement, not a scripted stand-in, and
// through a real Brain thread, not a reimplemented shortcut.
//
// Kalshi's real schema has NO "halted" event_type — see live/kalshi_feed.hpp's
// SCHEMA STATUS note for the verified mapping (`deactivated` -> Halted,
// `determined`/`settled` -> Resolved, `activated` -> Open). This file's tests
// use those real, verified values throughout, not an invented "halted" string.
#include "kalshi_gateway.hpp"
#include "pipeline/brain.hpp"
#include "pipeline/ingress.hpp"
#include "risk_engine.hpp"
#include "strategy/passive_market_maker.hpp"
#include "test_harness.hpp"
#include "ticker_registry.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

// Same role as tests/test_pipeline_e2e.cpp's SimulatedExchangeSink (not
// shared across files — each test file defines its own, matching this
// codebase's existing convention, e.g. write_test_key/write_throwaway_key):
// a Sink standing in for a well-behaved exchange's REST responses, so the
// Ack/Cancelled loop this file's Halted scenario needs can actually close
// without a network. Always acks a new order in full (never fills) and
// always fully honors a cancel -- these tests are about lifecycle gating,
// not fill/partial-fill behavior (see test_pipeline_e2e.cpp and
// test_hostile_events.cpp for that).
class SimulatedExchangeSink final : public Sink {
public:
    bool place(const OrderRequest& req, bool& did_send, std::string& response, std::string&) override {
        did_send = true;
        last_place_uuid = "sim-uuid-" + std::to_string(++place_call_count);
        last_place_qty_ = req.qty;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "HTTP/1.1 201 Created\r\n\r\n"
                      "{\"order_id\":\"%s\",\"client_order_id\":\"px-sim\","
                      "\"fill_count\":\"0.00\",\"remaining_count\":\"%u.00\",\"ts_ms\":1}",
                      last_place_uuid.c_str(), req.qty);
        response = buf;
        return true;
    }

    bool cancel(const std::string& kalshi_order_id, bool& did_send, std::string& response, std::string&) override {
        did_send = true;
        ++cancel_call_count;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "HTTP/1.1 200 OK\r\n\r\n"
                      "{\"order_id\":\"%s\",\"client_order_id\":\"px-sim\",\"reduced_by\":\"%u.00\",\"ts_ms\":2}",
                      kalshi_order_id.c_str(), last_place_qty_);
        response = buf;
        return true;
    }

    int place_call_count = 0;
    int cancel_call_count = 0;
    std::string last_place_uuid;

private:
    Qty last_place_qty_ = 0;
};

// ============================================================================
// ---- parse_feed_message: the event_type -> MarketStatus mapping ----------
// ============================================================================

void test_activated_maps_to_open() {
    FeedMsg fm;
    std::string err;
    PX_CHECK(parse_feed_message(R"({"type":"market_lifecycle_v2","sid":1,)"
                                 R"("msg":{"market_ticker":"T","event_type":"activated"}})",
                                 fm, err));
    PX_CHECK(fm.kind == FeedMsgKind::Lifecycle);
    PX_CHECK(fm.lifecycle_event_type == "activated");
    PX_CHECK(fm.has_status_change);
    PX_CHECK(fm.lifecycle_status == MarketStatus::Open);
}

void test_deactivated_maps_to_halted() {
    FeedMsg fm;
    std::string err;
    PX_CHECK(parse_feed_message(R"({"type":"market_lifecycle_v2","sid":1,)"
                                 R"("msg":{"market_ticker":"T","event_type":"deactivated"}})",
                                 fm, err));
    PX_CHECK(fm.has_status_change);
    PX_CHECK(fm.lifecycle_status == MarketStatus::Halted);
}

void test_determined_and_settled_map_to_resolved() {
    for (const char* event : {"determined", "settled"}) {
        FeedMsg fm;
        std::string err;
        const std::string raw = std::string(R"({"type":"market_lifecycle_v2","sid":1,)") +
                                 R"("msg":{"market_ticker":"T","event_type":")" + event + R"("}})";
        PX_CHECK(parse_feed_message(raw, fm, err));
        PX_CHECK(fm.has_status_change);
        PX_CHECK(fm.lifecycle_status == MarketStatus::Resolved);
    }
}

void test_informational_event_types_carry_no_status_change() {
    for (const char* event : {"created", "close_date_updated", "price_level_structure_updated", "metadata_updated"}) {
        FeedMsg fm;
        std::string err;
        const std::string raw = std::string(R"({"type":"market_lifecycle_v2","sid":1,)") +
                                 R"("msg":{"market_ticker":"T","event_type":")" + event + R"("}})";
        PX_CHECK(parse_feed_message(raw, fm, err));
        PX_CHECK(fm.kind == FeedMsgKind::Lifecycle); // decoded, not Unknown --
        PX_CHECK(!fm.has_status_change);              // but implies no status change
        PX_CHECK(fm.lifecycle_event_type == event);   // raw value still preserved for logging
    }
}

void test_lifecycle_missing_event_type_is_a_parse_error() {
    FeedMsg fm;
    std::string err;
    PX_CHECK(!parse_feed_message(R"({"type":"market_lifecycle_v2","sid":1,)"
                                  R"("msg":{"market_ticker":"T"}})",
                                  fm, err));
    PX_CHECK(!err.empty());
}

// ============================================================================
// ---- Ingress::handle_message: FeedMsg -> BookEvent ------------------------
// ============================================================================

void test_ingress_maps_status_change_to_book_event() {
    TickerRegistry registry;
    MarketId market;
    PX_CHECK(registry.add("LIFECYCLE-INGRESS", market));
    KalshiConfig ws_cfg;
    Ingress ingress(ws_cfg, registry);
    RingA ring_a;

    FeedMsg fm;
    std::string err;
    PX_CHECK(parse_feed_message(R"({"type":"market_lifecycle_v2","sid":1,)"
                                 R"("msg":{"market_ticker":"LIFECYCLE-INGRESS","event_type":"deactivated"}})",
                                 fm, err));
    ingress.handle_message(fm, ring_a);

    BookEvent got;
    PX_CHECK(ring_a.try_pop(got));
    PX_CHECK(got.kind == BookEventKind::LifecycleStatus);
    PX_CHECK(got.market == market);
    PX_CHECK(got.lifecycle_status == MarketStatus::Halted);
    PX_CHECK(!ring_a.try_pop(got)); // exactly one event, nothing else queued
}

void test_ingress_drops_informational_lifecycle_events() {
    TickerRegistry registry;
    MarketId market;
    PX_CHECK(registry.add("LIFECYCLE-INGRESS-2", market));
    KalshiConfig ws_cfg;
    Ingress ingress(ws_cfg, registry);
    RingA ring_a;

    FeedMsg fm;
    std::string err;
    PX_CHECK(parse_feed_message(R"({"type":"market_lifecycle_v2","sid":1,)"
                                 R"("msg":{"market_ticker":"LIFECYCLE-INGRESS-2","event_type":"metadata_updated"}})",
                                 fm, err));
    ingress.handle_message(fm, ring_a);

    BookEvent got;
    PX_CHECK(!ring_a.try_pop(got)); // nothing pushed -- no status change to report
}

// ============================================================================
// ---- Full pipeline: OPEN (default) -> HALTED -> stop new execution -------
// ---- (but cancels still allowed), through the real PassiveMarketMaker ----
// ============================================================================

void test_open_by_default_then_halted_blocks_new_orders_but_allows_cancel() {
    TickerRegistry registry;
    MarketId market;
    PX_CHECK(registry.add("LIFECYCLE-HALT", market));
    KalshiConfig ws_cfg;
    Ingress ingress(ws_cfg, registry);

    risk::RiskConfig risk_cfg;
    auto brain = std::make_unique<Brain>(risk_cfg);
    brain->add_strategy(market, std::make_unique<PassiveMarketMaker>(market, /*trader=*/0));

    std::vector<risk::Verdict> rejects;
    brain->on_reject = [&](const OrderIntent&, risk::Verdict v) { rejects.push_back(v); };

    Gateway gateway(std::make_unique<SimulatedExchangeSink>(), registry);

    RingA ring_a;
    RingB ring_b;
    RingC ring_c;
    OracleRing oracle_ring;
    std::atomic<bool> stop{false};
    std::thread brain_thread([&] { brain->run(ring_a, oracle_ring, ring_c, ring_b, stop); });
    std::thread gateway_thread([&] { gateway.run(ring_b, ring_c, stop); });

    auto push = [&](const std::string& raw) {
        FeedMsg fm;
        std::string err;
        PX_CHECK(parse_feed_message(raw, fm, err));
        ingress.handle_message(fm, ring_a);
    };

    // No lifecycle event has arrived yet -- market_status_ defaults to Open
    // (see brain.hpp's own comment for why not PreOpen). PMM should trade
    // completely normally.
    PX_CHECK(brain->market_status(market) == MarketStatus::Open);
    push(R"({"type":"orderbook_snapshot","sid":40,"seq":1,)"
         R"("msg":{"market_ticker":"LIFECYCLE-HALT","market_id":"m",)"
         R"("yes_dollars_fp":[["0.4000","100.00"]],)"
         R"("no_dollars_fp":[["0.4000","100.00"]]}})"); // bid=40, ask=60
    // PMM joins the bid off the snapshot itself, but per its own design (see
    // passive_market_maker.hpp: "the next BookUpdate re-evaluates quoting
    // from scratch") an OwnOrderUpdate (the ack) alone does NOT make it
    // re-quote the missing ask side -- it waits for the next real BookUpdate.
    // Wait for the bid to actually ack first (so the nudge below finds
    // have_bid_quote already true, not a race where an unacked bid makes PMM
    // re-attempt the BID again instead of the ask), then send a harmless
    // nudge delta (ask depth +1, price unchanged) purely to give PMM that
    // next BookUpdate.
    PX_CHECK(wait_until([&] { return brain->order_tracker().orders_for(market).size() == 1; }));
    push(R"({"type":"orderbook_delta","sid":40,"seq":2,)"
         R"("msg":{"market_ticker":"LIFECYCLE-HALT","market_id":"m",)"
         R"("price_dollars":"0.4000","delta_fp":"1.00","side":"no"}})"); // NO@40 -> YES ask@60, +1 qty
    PX_CHECK(wait_until([&] { return brain->order_tracker().orders_for(market).size() == 2; }));

    // deactivated -> Halted. The book itself hasn't moved, so PMM's existing
    // quotes are still at the current best -- this alone must NOT force a
    // spurious cancel (proving the gate doesn't misfire on its own trigger).
    push(R"({"type":"market_lifecycle_v2","sid":40,)"
         R"("msg":{"market_ticker":"LIFECYCLE-HALT","event_type":"deactivated"}})");
    PX_CHECK(wait_until([&] { return brain->market_status(market) == MarketStatus::Halted; }));
    const auto rejected_before_move = brain->intents_rejected();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    PX_CHECK(brain->intents_rejected() == rejected_before_move); // no spurious action from halting alone

    // Now the book moves: best_bid 40 -> 42. PMM's resting bid quote (still
    // at 40) is stale -- it issues a Cancel first, same as
    // test_pmm_cancels_stale_quote_when_book_moves (tests/test_strategies.cpp).
    // A Cancel must still be accepted while Halted (risk-reducing, same
    // reasoning RiskEngine's own check_cancel already gets for fat-finger/
    // exposure).
    push(R"({"type":"orderbook_delta","sid":40,"seq":3,)"
         R"("msg":{"market_ticker":"LIFECYCLE-HALT","market_id":"m",)"
         R"("price_dollars":"0.4000","delta_fp":"-100.00","side":"yes"}})"); // remove the 40c level entirely
    push(R"({"type":"orderbook_delta","sid":40,"seq":4,)"
         R"("msg":{"market_ticker":"LIFECYCLE-HALT","market_id":"m",)"
         R"("price_dollars":"0.4200","delta_fp":"50.00","side":"yes"}})"); // new bid at 42
    // The stale (price==40) bid quote is gone -- the ask quote (price==60)
    // is untouched and still resting throughout.
    PX_CHECK(wait_until(
        [&] {
            for (const auto& o : brain->order_tracker().orders_for(market)) {
                if (o.price == 40) return false;
            }
            return true;
        },
        5));

    // Same "PMM only re-quotes on the next real BookUpdate, not the ack/
    // cancel OwnOrderUpdate that made room for it" pattern as the initial
    // quoting nudge above -- one more harmless nudge so PMM actually
    // attempts the re-Join rather than sitting idle until whatever event
    // happens to arrive next.
    push(R"({"type":"orderbook_delta","sid":40,"seq":5,)"
         R"("msg":{"market_ticker":"LIFECYCLE-HALT","market_id":"m",)"
         R"("price_dollars":"0.4000","delta_fp":"1.00","side":"no"}})");

    // With the bid quote now gone, PMM wants to re-Join at the new best_bid
    // (42) -- a genuinely NEW order, while still Halted. Must be rejected.
    PX_CHECK(wait_until([&] {
        for (auto v : rejects) {
            if (v == risk::Verdict::RejectMarketHalted) return true;
        }
        return false;
    }));

    stop.store(true, std::memory_order_relaxed);
    brain_thread.join();
    gateway_thread.join();
}

// ============================================================================
// ---- RESOLVED: stop trading permanently -- new orders AND cancels --------
// ============================================================================

void test_resolved_blocks_new_orders_and_cancels() {
    TickerRegistry registry;
    MarketId market;
    PX_CHECK(registry.add("LIFECYCLE-RESOLVED", market));
    KalshiConfig ws_cfg;
    Ingress ingress(ws_cfg, registry);

    risk::RiskConfig risk_cfg;
    auto brain = std::make_unique<Brain>(risk_cfg);
    brain->add_strategy(market, std::make_unique<PassiveMarketMaker>(market, /*trader=*/0));

    std::vector<risk::Verdict> rejects;
    brain->on_reject = [&](const OrderIntent&, risk::Verdict v) { rejects.push_back(v); };

    Gateway gateway(std::make_unique<SimulatedExchangeSink>(), registry);

    RingA ring_a;
    RingB ring_b;
    RingC ring_c;
    OracleRing oracle_ring;
    std::atomic<bool> stop{false};
    std::thread brain_thread([&] { brain->run(ring_a, oracle_ring, ring_c, ring_b, stop); });
    std::thread gateway_thread([&] { gateway.run(ring_b, ring_c, stop); });

    auto push = [&](const std::string& raw) {
        FeedMsg fm;
        std::string err;
        PX_CHECK(parse_feed_message(raw, fm, err));
        ingress.handle_message(fm, ring_a);
    };

    // Open by default -> both sides get quoted and acked normally, same
    // setup as the Halted scenario above (and for the identical reason: an
    // unacked order stays invisible to PMM via outstanding_orders, so
    // without acking it here, the "Cancel also blocked" assertion below
    // would actually be re-testing a rejected JOIN, not a rejected CANCEL).
    push(R"({"type":"orderbook_snapshot","sid":41,"seq":1,)"
         R"("msg":{"market_ticker":"LIFECYCLE-RESOLVED","market_id":"m",)"
         R"("yes_dollars_fp":[["0.4000","100.00"]],)"
         R"("no_dollars_fp":[["0.4000","100.00"]]}})");
    // Same "PMM needs an explicit next BookUpdate to quote the ask side"
    // nudge the Halted scenario needs — see its own comment for why.
    PX_CHECK(wait_until([&] { return brain->order_tracker().orders_for(market).size() == 1; }));
    push(R"({"type":"orderbook_delta","sid":41,"seq":2,)"
         R"("msg":{"market_ticker":"LIFECYCLE-RESOLVED","market_id":"m",)"
         R"("price_dollars":"0.4000","delta_fp":"1.00","side":"no"}})");
    PX_CHECK(wait_until([&] { return brain->order_tracker().orders_for(market).size() == 2; }));

    // determined -> Resolved. Both sides already correctly quoted, so
    // resolving alone triggers no action yet -- proven the same way the
    // Halted test proves it, by checking nothing new got rejected from the
    // lifecycle event itself before the book actually moves.
    push(R"({"type":"market_lifecycle_v2","sid":41,)"
         R"("msg":{"market_ticker":"LIFECYCLE-RESOLVED","event_type":"determined"}})");
    PX_CHECK(wait_until([&] { return brain->market_status(market) == MarketStatus::Resolved; }));

    // The book moves the resting bid off its quoted price -- PMM wants to
    // Cancel the stale bid quote (exactly the Halted test's trigger). Unlike
    // Halted, this Cancel must ALSO be rejected once Resolved: nothing is
    // left to trade, full stop, not even a risk-reducing cancel.
    const auto accepted_before = brain->intents_accepted();
    push(R"({"type":"orderbook_delta","sid":41,"seq":3,)"
         R"("msg":{"market_ticker":"LIFECYCLE-RESOLVED","market_id":"m",)"
         R"("price_dollars":"0.4000","delta_fp":"-100.00","side":"yes"}})");
    push(R"({"type":"orderbook_delta","sid":41,"seq":4,)"
         R"("msg":{"market_ticker":"LIFECYCLE-RESOLVED","market_id":"m",)"
         R"("price_dollars":"0.4200","delta_fp":"50.00","side":"yes"}})");
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // let PMM re-evaluate against the moved book
    PX_CHECK(brain->intents_accepted() == accepted_before); // the stale-quote Cancel was rejected, not sent
    PX_CHECK(brain->order_tracker().orders_for(market).size() == 2); // both quotes still resting -- cancel never went out
    bool saw_resolved_reject_for_cancel = false;
    for (auto v : rejects) {
        if (v == risk::Verdict::RejectMarketResolved) saw_resolved_reject_for_cancel = true;
    }
    PX_CHECK(saw_resolved_reject_for_cancel);

    stop.store(true, std::memory_order_relaxed);
    brain_thread.join();
    gateway_thread.join();
}

} // namespace

int main() {
    test_activated_maps_to_open();
    test_deactivated_maps_to_halted();
    test_determined_and_settled_map_to_resolved();
    test_informational_event_types_carry_no_status_change();
    test_lifecycle_missing_event_type_is_a_parse_error();

    test_ingress_maps_status_change_to_book_event();
    test_ingress_drops_informational_lifecycle_events();

    test_open_by_default_then_halted_blocks_new_orders_but_allows_cancel();
    test_resolved_blocks_new_orders_and_cancels();

    PX_TEST_REPORT_AND_RETURN();
}
