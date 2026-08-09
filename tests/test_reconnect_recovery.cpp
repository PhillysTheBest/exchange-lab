// tests/test_reconnect_recovery.cpp — Stage 5, Section 8: failure and
// recovery testing, covering exactly the gaps a prior exploration pass
// found NOT already covered by test_hostile_events.cpp/test_lifecycle.cpp/
// test_replay_determinism.cpp (all three already cover connect-retry
// backoff mechanics, seq-gap/duplicate dropping, and HALTED/RESOLVED
// gating — not repeated here).
//
// SCOPE, stated precisely (same discipline every other test file in this
// suite uses): a real socket-level "connection dies mid-stream, TCP resets,
// reconnects" scenario is NOT simulated here — that would need a mock TLS/
// WS server double this project doesn't have, and the functionally
// interesting question is what happens to Brain's state when the RESYNC
// mechanism reconnect already triggers (Ingress::request_resync — a fresh
// orderbook_snapshot) actually arrives, which IS fully testable through
// Ingress::handle_message, the same public synthetic-injection entry point
// tests/test_replay_determinism.cpp already relies on. So: this file starts
// from "a resync snapshot has arrived" and checks Brain's reaction to it,
// not the socket mechanics that would have produced one.
//
// Per explicit user direction (see docs/PERFORMANCE.md's Limitations section):
// this file does NOT build or test exchange-side order reconciliation — no
// such mechanism exists (confirmed: live/kalshi_rest.hpp has no GET-orders
// call), and none is added here. What IS tested and documented: the system
// never falsely reports an unresolved order as confirmed-live, and exactly
// what happens instead when local order-tracking capacity is exhausted
// (test_gateway_failure_path_orphans_pending_orders_without_lying, below) —
// a real, previously-undocumented mechanism found while writing this file,
// not a hypothetical.
#include "live/kalshi_gateway.hpp"
#include "pipeline/brain.hpp"
#include "pipeline/ingress.hpp"
#include "risk_engine.hpp"
#include "strategy/order_tracker.hpp"
#include "strategy/strategy.hpp"
#include "test_harness.hpp"
#include "ticker_registry.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>

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

// Records every dispatch it's asked to make (trigger + whether the book had
// both sides at the time) but never itself decides to trade — a pure
// observer, so its call count/observations are exactly Brain's dispatch
// behavior, uncontaminated by any decision logic of its own.
class ObserverStrategy final : public Strategy {
public:
    [[nodiscard]] ExecutionDecision on_event(const MarketState& state, TriggerKind, const OracleEvent*) noexcept override {
        ++dispatch_count;
        if (state.have_bid && state.have_ask) ++dispatch_count_both_sides;
        if (!state.have_bid && !state.have_ask) ++dispatch_count_empty_book;
        return {};
    }
    [[nodiscard]] const char* name() const noexcept override { return "ObserverStrategy(test)"; }

    std::atomic<int> dispatch_count{0};
    std::atomic<int> dispatch_count_both_sides{0};
    std::atomic<int> dispatch_count_empty_book{0};
};

// Joins the bid exactly once, the first time it sees a two-sided book with
// nothing already outstanding — the same "am I already exposed" gate
// strategy/order_book_imbalance.hpp and strategy/trade_flow.hpp use for
// real, simplified to a fixed price/qty for test predictability (same
// spirit as test_pipeline_e2e.cpp's JoinThenCancelStrategy).
class JoinOnceStrategy final : public Strategy {
public:
    JoinOnceStrategy(MarketId market, TraderId trader) noexcept : market_(market), trader_(trader) {}
    [[nodiscard]] ExecutionDecision on_event(const MarketState& state, TriggerKind trigger,
                                              const OracleEvent*) noexcept override {
        ExecutionDecision d;
        if (trigger != TriggerKind::BookUpdate || !state.have_bid || !state.have_ask) return d;
        if (!state.outstanding_orders.empty()) return d; // already exposed -- the whole point of this test
        d.action = ExecutionAction::Join;
        d.intent = OrderIntent{IntentKind::NewOrder, kInvalidOrderId,   market_,
                                trader_,              Contract::Yes,    Side::Buy,
                                OrderType::Limit,     TimeInForce::GTC, state.best_bid,
                                1};
        return d;
    }
    [[nodiscard]] const char* name() const noexcept override { return "JoinOnceStrategy(test)"; }

private:
    MarketId market_;
    TraderId trader_;
};

// Same shape as test_pipeline_e2e.cpp's SimulatedExchangeSink (ack every new
// order, remaining_count = qty - fill).
class WorkingSink final : public Sink {
public:
    bool place(const OrderRequest& req, bool& did_send, std::string& response, std::string&) override {
        did_send = true;
        ++place_call_count;
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "HTTP/1.1 201 Created\r\n\r\n"
                      "{\"order_id\":\"sim-%d\",\"client_order_id\":\"px-sim\","
                      "\"fill_count\":\"0.00\",\"remaining_count\":\"%u.00\","
                      "\"average_fill_price\":\"0.0000\",\"ts_ms\":1}",
                      place_call_count, req.qty);
        response = buf;
        return true;
    }
    bool cancel(const std::string&, bool& did_send, std::string& response, std::string&) override {
        did_send = true;
        response = "HTTP/1.1 200 OK\r\n\r\n{\"order_id\":\"sim\",\"client_order_id\":\"px-sim\",\"reduced_by\":\"1.00\",\"ts_ms\":2}";
        return true;
    }
    int place_call_count = 0;
};

// The Gateway failure-path double: place() "succeeds" (attempted=true) but
// deliberately produces one of the three response shapes
// live/kalshi_gateway.cpp's handle_new_order silently gives up on without
// ever pushing an ExecReport — see that file's handle_new_order for the
// exact three early-return branches this mirrors.
enum class FailureMode { SignFailure, UnparseableStatus, UnparseableBody };

class BrokenSink final : public Sink {
public:
    explicit BrokenSink(FailureMode mode) : mode_(mode) {}

    bool place(const OrderRequest&, bool& did_send, std::string& response, std::string& error) override {
        ++place_call_count;
        switch (mode_) {
            case FailureMode::SignFailure:
                did_send = false;
                error = "simulated: could not sign/build the request";
                return false; // handle_new_order's "if (!attempted) return;" branch
            case FailureMode::UnparseableStatus:
                did_send = true;
                response = "not an HTTP response at all"; // fails parse_http_status
                return true;
            case FailureMode::UnparseableBody:
                did_send = true;
                response = "HTTP/1.1 201 Created\r\n\r\nnot valid json either"; // 2xx but parse_order_response fails
                return true;
        }
        return true;
    }
    bool cancel(const std::string&, bool& did_send, std::string&, std::string&) override {
        did_send = false;
        return false;
    }
    int place_call_count = 0;

private:
    FailureMode mode_;
};

struct Harness {
    TickerRegistry registry;
    MarketId market{};
    std::unique_ptr<Ingress> ingress;
    std::unique_ptr<Brain> brain;
    RingA ring_a;
    RingB ring_b;
    RingC ring_c;
    OracleRing oracle_ring;
    std::atomic<bool> stop{false};
    std::thread brain_thread;
    std::thread gateway_thread;

    // Strategies below are constructed with a hardcoded MarketId of 0 before
    // start() ever runs — safe only because every Harness here adds exactly
    // one, brand-new ticker to a fresh TickerRegistry, and
    // TickerRegistry::add assigns sequential ids starting at 0 (verified in
    // live/ticker_registry.hpp), so the first (only) add() always yields 0.
    void start(const char* ticker, std::unique_ptr<Strategy> strat, std::unique_ptr<Sink> sink) {
        PX_CHECK(registry.add(ticker, market));
        KalshiConfig ws_cfg;
        ingress = std::make_unique<Ingress>(ws_cfg, registry);
        // Oversized on purpose, same reasoning as bench_pipeline_e2e.cpp's
        // identical override: these tests exercise resync/order-tracking
        // behavior, not Kalshi's real rate limit (already covered on its own
        // by tests/test_risk.cpp) -- left at the real ~10-orders/sec default,
        // test_gateway_failure_path_orphans_pending_orders_without_lying's
        // 40-decisions-in-a-tight-loop would mostly hit RejectRateLimited
        // instead of ever reaching Gateway, silently invalidating the test.
        risk::RiskConfig risk_cfg;
        risk_cfg.bucket_capacity_tokens = 10'000'000;
        brain = std::make_unique<Brain>(risk_cfg);
        if (strat) brain->add_strategy(market, std::move(strat));
        brain_thread = std::thread([&] { brain->run(ring_a, oracle_ring, ring_c, ring_b, stop); });
        gateway_ = std::make_unique<Gateway>(std::move(sink), registry);
        gateway_thread = std::thread([&] { gateway_->run(ring_b, ring_c, stop); });
    }

    void send(const std::string& raw) {
        FeedMsg fm;
        std::string err;
        PX_CHECK(parse_feed_message(raw, fm, err));
        ingress->handle_message(fm, ring_a);
    }

    ~Harness() {
        stop.store(true, std::memory_order_relaxed);
        if (brain_thread.joinable()) brain_thread.join();
        if (gateway_thread.joinable()) gateway_thread.join();
    }

    std::unique_ptr<Gateway> gateway_;
};

std::string snapshot_json(const char* ticker, long long seq, const char* yes_levels, const char* no_levels) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  R"({"type":"orderbook_snapshot","sid":1,"seq":%lld,)"
                  R"("msg":{"market_ticker":"%s","market_id":"m",)"
                  R"("yes_dollars_fp":[%s],"no_dollars_fp":[%s]}})",
                  seq, ticker, yes_levels, no_levels);
    return buf;
}

std::string delta_json(const char* ticker, long long seq, const char* price, long long delta, const char* side) {
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  R"({"type":"orderbook_delta","sid":1,"seq":%lld,)"
                  R"("msg":{"market_ticker":"%s","market_id":"m",)"
                  R"("price_dollars":"%s","delta_fp":"%lld.00","side":"%s"}})",
                  seq, ticker, price, delta, side);
    return buf;
}

} // namespace

// ---- 1: resync rebuilds the book cleanly, no stale levels survive --------
void test_resync_rebuilds_book_cleanly() {
    Harness h;
    h.start("RECOVER-1", nullptr, std::make_unique<WorkingSink>());

    h.send(snapshot_json("RECOVER-1", 1, R"(["0.4000","100.00"])", R"(["0.3000","20.00"])"));
    PX_CHECK(wait_until([&] { return h.brain->book_events_applied() >= 3; })); // SnapshotBegin + 2 levels

    Price bb = 0, ba = 0;
    PX_CHECK(h.brain->book(h.market).best_bid(bb) && bb == 40);
    PX_CHECK(h.brain->book(h.market).best_ask(ba) && ba == 70); // NO bid 30c -> YES ask 70c

    // A real reconnect's request_resync produces exactly this shape: a fresh
    // orderbook_snapshot, completely different levels this time.
    h.send(snapshot_json("RECOVER-1", 2, R"(["0.6000","10.00"])", R"()"));
    PX_CHECK(wait_until([&] { return h.brain->book_events_applied() >= 4; })); // SnapshotBegin + 1 level

    PX_CHECK(h.brain->book(h.market).best_bid(bb) && bb == 60); // the ONLY level now
    PX_CHECK(!h.brain->book(h.market).best_ask(ba));            // old 70c ask must NOT survive the resync
    PX_CHECK(h.brain->book(h.market).level_qty(40) == 0);       // old 40c bid must NOT survive either
}

// ---- 2: SnapshotBegin's clear() never dispatches on its own --------------
void test_snapshot_begin_does_not_dispatch_prematurely() {
    Harness h;
    auto obs = std::make_unique<ObserverStrategy>();
    ObserverStrategy* obs_ptr = obs.get();
    h.start("RECOVER-2", std::move(obs), std::make_unique<WorkingSink>());

    h.send(snapshot_json("RECOVER-2", 1, R"(["0.4000","100.00"])", R"(["0.3000","20.00"])"));
    PX_CHECK(wait_until([&] { return h.brain->book_events_applied() >= 3; }));
    const int dispatches_after_first_snapshot = obs_ptr->dispatch_count.load();
    PX_CHECK(obs_ptr->dispatch_count_empty_book.load() == 0); // never fired while the book was momentarily empty

    // A resync: SnapshotBegin clears the book (Brain::apply_book_event
    // returns immediately without dispatching for that specific event —
    // see pipeline/brain.cpp) before the fresh levels land.
    h.send(snapshot_json("RECOVER-2", 2, R"(["0.5000","5.00"])", R"()"));
    // Waits on dispatch_count itself, not book_events_applied() -- the two
    // increment a few instructions apart inside apply_book_event, so waiting
    // on the counter alone can observe it before the matching dispatch for
    // that same event has actually run, a real race this caught once.
    PX_CHECK(wait_until([&] { return obs_ptr->dispatch_count.load() >= dispatches_after_first_snapshot + 1; }));

    // Exactly one more dispatch than before per real level in the new
    // snapshot (1 level here) -- the SnapshotBegin itself contributed zero.
    PX_CHECK(obs_ptr->dispatch_count.load() == dispatches_after_first_snapshot + 1);
    PX_CHECK(obs_ptr->dispatch_count_empty_book.load() == 0); // still never observed an empty book mid-dispatch
}

// ---- 3: a resync never causes an already-exposed strategy to re-enter ----
void test_resync_does_not_duplicate_orders() {
    Harness h;
    h.start("RECOVER-3", std::make_unique<JoinOnceStrategy>(0, /*trader=*/0), std::make_unique<WorkingSink>());

    h.send(snapshot_json("RECOVER-3", 1, R"(["0.4000","100.00"])", R"(["0.3000","20.00"])"));
    PX_CHECK(wait_until([&] { return h.brain->intents_accepted() >= 1; }));
    PX_CHECK(wait_until([&] { return h.brain->order_tracker().orders_for(h.market).size() == 1; }));

    // Resync: book gets cleared and rebuilt with a fresh two-sided market —
    // JoinOnceStrategy will be dispatched again (both sides known again),
    // but its own outstanding_orders check must still see the earlier,
    // resync-unrelated order and decline to place a second one.
    h.send(snapshot_json("RECOVER-3", 2, R"(["0.4000","100.00"])", R"(["0.3000","20.00"])"));
    PX_CHECK(wait_until([&] { return h.brain->book_events_applied() >= 6; }, 3));
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let any (wrongly) duplicated order surface

    PX_CHECK(h.brain->intents_accepted() == 1); // still exactly one -- no duplicate
    PX_CHECK(h.brain->order_tracker().orders_for(h.market).size() == 1);
}

// ---- 4: risk state is untouched by a market-data-only resync -------------
void test_market_data_resync_leaves_risk_state_untouched() {
    Harness h;
    h.start("RECOVER-4", nullptr, std::make_unique<WorkingSink>());

    h.send(snapshot_json("RECOVER-4", 1, R"(["0.4000","100.00"])", R"(["0.3000","20.00"])"));
    PX_CHECK(wait_until([&] { return h.brain->book_events_applied() >= 3; }));
    const Position before = h.brain->risk().position(h.market);

    h.send(snapshot_json("RECOVER-4", 2, R"(["0.7000","5.00"])", R"()"));
    PX_CHECK(wait_until([&] { return h.brain->book_events_applied() >= 5; }));
    h.send(delta_json("RECOVER-4", 3, "0.7000", 3, "yes"));
    PX_CHECK(wait_until([&] { return h.brain->book_events_applied() >= 6; }));

    PX_CHECK(h.brain->risk().position(h.market) == before); // position only ever moves on a Fill (ring C), never
                                                               // on ring-A book/snapshot/delta traffic
}

// ---- 5: the Gateway failure-path gap, demonstrated precisely -------------
// Confirms the property the user asked to verify: an unresolved order is
// NEVER reported as confirmed-live. Then demonstrates the real, broader
// mechanism found while writing this test (see pipeline/brain.cpp's
// handle_decision: "(void)order_tracker_.on_new(...)" — its return value is
// deliberately discarded, and the comment there states the intent is SENT
// to Gateway regardless of whether OrderTracker could track it): once a
// market's 32-slot pending-order table fills up (here, via a broken sink
// that never reports back — but the same table fills identically for ANY
// reason, including a healthy sink and enough real orders), every
// subsequent decision for that market still gets sent to the exchange, but
// becomes permanently untrackable locally (invisible to orders_for(),
// therefore never targetable by a future Cancel). No fix ships here — see
// this file's header comment and docs/PERFORMANCE.md's Limitations section.
void test_gateway_failure_path_orphans_pending_orders_without_lying() {
    for (FailureMode mode : {FailureMode::SignFailure, FailureMode::UnparseableStatus, FailureMode::UnparseableBody}) {
        Harness h;
        auto sink = std::make_unique<BrokenSink>(mode);
        BrokenSink* sink_ptr = sink.get();
        // AlwaysJoinStrategy-equivalent: fires on every BookUpdate once both
        // sides are known, with no outstanding-orders gate — deliberately,
        // so it keeps trying (exactly the runaway-strategy shape
        // handle_decision's own comment anticipates) rather than stopping
        // after its first (never-acked) attempt.
        class UngatedJoin final : public Strategy {
        public:
            explicit UngatedJoin(MarketId m) : m_(m) {}
            ExecutionDecision on_event(const MarketState& s, TriggerKind t, const OracleEvent*) noexcept override {
                ExecutionDecision d;
                if (t != TriggerKind::BookUpdate || !s.have_bid || !s.have_ask) return d;
                d.action = ExecutionAction::Join;
                d.intent = OrderIntent{IntentKind::NewOrder, kInvalidOrderId, m_,   0, Contract::Yes,
                                        Side::Buy,           OrderType::Limit, TimeInForce::GTC, s.best_bid, 1};
                return d;
            }
            const char* name() const noexcept override { return "UngatedJoin(test)"; }
            MarketId m_;
        };
        h.start("RECOVER-5", std::make_unique<UngatedJoin>(0), std::move(sink));

        h.send(snapshot_json("RECOVER-5", 1, R"(["0.4000","100.00"])", R"(["0.3000","20.00"])"));
        PX_CHECK(wait_until([&] { return h.brain->book_events_applied() >= 3; }));

        // Drive enough additional BookUpdates to exceed kMaxRestingOrdersPerMarket
        // (32) attempted new-order decisions -- each one fires UngatedJoin again.
        // seq continues from the snapshot's own seq=1 (the NEXT expected seq
        // is 2) -- starting anywhere else opens a seq gap, which SeqTracker
        // correctly desyncs on and drops everything after, silently
        // invalidating this test (caught by book_events_applied() staying
        // stuck at 3 with every delta below silently dropped).
        for (int i = 0; i < 40; ++i) {
            h.send(delta_json("RECOVER-5", 2 + i, "0.4000", (i % 2 == 0) ? 1 : -1, "yes"));
        }
        PX_CHECK(wait_until([&] { return h.brain->intents_accepted() >= 40; }, 5));
        std::this_thread::sleep_for(std::chrono::milliseconds(150)); // let Gateway attempt every one of them

        // Property the user asked to verify: never falsely confirmed-live.
        PX_CHECK(h.brain->order_tracker().orders_for(h.market).empty()); // acked-only view: nothing was ever
                                                                           // (falsely or otherwise) promoted

        // The broader mechanism: Brain kept SENDING every decision to
        // Gateway (real attempts, real sink calls) even long past the
        // 32-slot tracking capacity -- these later ones are accepted and
        // dispatched but can now never be tracked/cancelled locally.
        PX_CHECK(sink_ptr->place_call_count >= 40); // every single one really was sent, capacity or not
        PX_CHECK(h.brain->intents_accepted() >= 40); // Brain's own count agrees -- nothing was silently dropped
    }
}

int main() {
    test_resync_rebuilds_book_cleanly();
    test_snapshot_begin_does_not_dispatch_prematurely();
    test_resync_does_not_duplicate_orders();
    test_market_data_resync_leaves_risk_state_untouched();
    test_gateway_failure_path_orphans_pending_orders_without_lying();
    PX_TEST_REPORT_AND_RETURN();
}
