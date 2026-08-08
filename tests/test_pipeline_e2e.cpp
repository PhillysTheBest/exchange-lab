// tests/test_pipeline_e2e.cpp — proves the full live execution path end to
// end, dry-run, through REAL threads and REAL rings, not a reimplemented
// shortcut:
//
//   Kalshi (scripted frames) -> Ingress::handle_message -> ring A -> Brain
//   (T2 thread: ShadowBook, Strategy::on_event, RiskEngine) -> ring B ->
//   Gateway (T3 thread: request build/dispatch) -> (simulated) REST response
//   -> ring C -> Brain (OrderTracker, OwnOrderUpdate dispatch)
//
// covering BUY, SELL, CANCEL, ACK, FILL, and TriggerKind::OwnOrderUpdate —
// especially cancellation, which strategy/order_tracker.hpp's whole design
// depends on an order having actually been acked first.
//
// FINDING #1, documented rather than routed around: the real DryRunSink
// (live/kalshi_gateway.hpp — what kalshi_pipeline.exe uses by default)
// structurally never produces an ExecReport at all: Gateway::handle_new_order
// returns immediately after `if (!did_send) return;`, because dry-running
// against nothing means there is no exchange response to parse. That's
// correct and intentional (see kalshi_rest.hpp's SAFETY POSTURE), but it also
// means Acked/Fill/Cancelled/OwnOrderUpdate are UNEXERCISED by simply running
// kalshi_pipeline.exe in its default mode. SimulatedExchangeSink below closes
// that gap: a Sink standing in for a well-behaved exchange's REST responses
// (no network, no credentials — same spirit as
// tests/test_gateway_dry_run.cpp's FakeSink), so the state machine — not the
// wire-signing path, which is separately verified in bench_sign.cpp and the
// kalshi_auth tests — can be proven end to end.
//
// FINDING #2, found BY this test, not before it: the first version of
// OrderTracker::on_acked promoted a remaining_qty==0 order (a marketable/IOC
// order that fully resolves at entry — the common case for
// OrderBookImbalance/TradeFlow's IOC crosses) into the acked table anyway,
// and nothing ever removed it (on_fill didn't remove a zero-remaining entry
// either). That left a permanent zombie entry, so
// MarketState::outstanding_orders was never empty again after a strategy's
// first trade — silently disabling any strategy gating re-entry on
// `outstanding_orders.empty()` (both OrderBookImbalance and TradeFlow do).
// Fixed in strategy/order_tracker.hpp; test_cross_fill_then_trades_again_sell
// below is the regression test — it fails (times out waiting for a second
// Cross) against the pre-fix code.
#include "kalshi_gateway.hpp"
#include "pipeline/brain.hpp"
#include "pipeline/ingress.hpp"
#include "risk_engine.hpp"
#include "strategy/order_book_imbalance.hpp"
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
#include <vector>

using namespace px;
using namespace px::live;
using namespace px::pipeline;
using namespace px::strategy;

namespace {

// ---- SimulatedExchangeSink --------------------------------------------------
// Stands in for Kalshi's REST responses — never opens a socket, never signs
// anything (this is deliberately NOT DryRunSink; the wire/signing path is
// already proven elsewhere) — so Gateway's own response-dispatch logic
// (already unit-tested against canned responses in
// tests/test_gateway_dry_run.cpp's FakeSink) can be driven by a REAL Brain
// producing REAL intents instead of a test hand-assembling them.
class SimulatedExchangeSink final : public Sink {
public:
    // qty_filled_at_entry: how much of a NEW order's qty this "exchange"
    // fills synchronously — 0 for a resting GTC order (ack only), the full
    // requested qty for a marketable/IOC cross (ack + Fill, remaining 0) —
    // both realistic Kalshi V2 response shapes, same JSON fields already
    // schema-verified in tests/test_gateway_dry_run.cpp.
    explicit SimulatedExchangeSink(Qty qty_filled_at_entry) : qty_filled_at_entry_(qty_filled_at_entry) {}

    bool place(const OrderRequest& req, bool& did_send, std::string& response, std::string&) override {
        did_send = true;
        ++place_call_count;
        last_place_uuid = "sim-uuid-" + std::to_string(place_call_count);
        const Qty fill = (qty_filled_at_entry_ < req.qty) ? qty_filled_at_entry_ : req.qty;
        const Qty remaining = req.qty - fill;
        last_place_remaining_ = remaining;

        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "HTTP/1.1 201 Created\r\n\r\n"
                      "{\"order_id\":\"%s\",\"client_order_id\":\"px-sim\","
                      "\"fill_count\":\"%u.00\",\"remaining_count\":\"%u.00\","
                      "\"average_fill_price\":\"%.4f\",\"ts_ms\":1}",
                      last_place_uuid.c_str(), fill, remaining, static_cast<double>(req.price) / 100.0);
        response = buf;
        return true;
    }

    bool cancel(const std::string& kalshi_order_id, bool& did_send, std::string& response, std::string&) override {
        did_send = true;
        ++cancel_call_count;
        last_cancel_kalshi_id = kalshi_order_id;

        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "HTTP/1.1 200 OK\r\n\r\n"
                      "{\"order_id\":\"%s\",\"client_order_id\":\"px-sim\",\"reduced_by\":\"%u.00\",\"ts_ms\":2}",
                      kalshi_order_id.c_str(), last_place_remaining_);
        response = buf;
        return true;
    }

    int place_call_count = 0;
    int cancel_call_count = 0;
    std::string last_place_uuid;
    std::string last_cancel_kalshi_id;

private:
    Qty qty_filled_at_entry_;
    Qty last_place_remaining_ = 0;
};

// ---- JoinThenCancelStrategy --------------------------------------------------
// Scripted, deterministic, test-only: Joins the bid the first time it sees a
// real two-sided book with nothing outstanding, then cancels that exact
// order the moment OwnOrderUpdate confirms it acked, then confirms the
// cancel itself completed. Phase is progress observed from OUTSIDE — Brain
// owns the Strategy via unique_ptr, so an external atomic reference is the
// only way this test can watch it run on T2's thread.
class JoinThenCancelStrategy final : public Strategy {
public:
    JoinThenCancelStrategy(MarketId market, TraderId trader, std::atomic<int>& phase) noexcept
        : market_(market), trader_(trader), phase_(phase) {}

    [[nodiscard]] ExecutionDecision on_event(const MarketState& state, TriggerKind trigger,
                                              const OracleEvent*) noexcept override {
        ExecutionDecision d;
        if (trigger == TriggerKind::OracleEvent) return d;

        if (!state.outstanding_orders.empty()) {
            if (phase_.load(std::memory_order_acquire) == 1) {
                d.action = ExecutionAction::Cancel;
                d.cancel_target = state.outstanding_orders[0].id;
                phase_.store(2, std::memory_order_release);
            }
            return d;
        }

        if (phase_.load(std::memory_order_acquire) == 2) {
            phase_.store(3, std::memory_order_release); // cancel confirmed -- loop closed
            return d;
        }

        if (phase_.load(std::memory_order_acquire) == 0 && trigger == TriggerKind::BookUpdate && state.have_bid &&
            state.have_ask) {
            d.action = ExecutionAction::Join;
            d.intent = OrderIntent{IntentKind::NewOrder, kInvalidOrderId,   market_,
                                    trader_,              Contract::Yes,    Side::Buy,
                                    OrderType::Limit,     TimeInForce::GTC, state.best_bid,
                                    10};
            phase_.store(1, std::memory_order_release);
        }
        return d;
    }
    [[nodiscard]] const char* name() const noexcept override { return "JoinThenCancelStrategy(test)"; }

private:
    MarketId market_;
    TraderId trader_;
    std::atomic<int>& phase_;
};

[[nodiscard]] bool wait_until(const std::function<bool()>& pred, int seconds = 5) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::yield();
    }
    return false;
}

// ---- Scenario A: BUY, Join, Ack, Cancel, Cancelled, OwnOrderUpdate ---------
void test_join_then_cancel_buy() {
    TickerRegistry registry;
    MarketId market;
    PX_CHECK(registry.add("E2E-BUY", market));

    KalshiConfig ws_cfg; // never connected — handle_message doesn't touch it
    Ingress ingress(ws_cfg, registry);

    risk::RiskConfig risk_cfg;
    auto brain = std::make_unique<Brain>(risk_cfg); // SIZE WARNING — see pipeline/brain.hpp

    std::atomic<int> phase{0};
    brain->add_strategy(market, std::make_unique<JoinThenCancelStrategy>(market, /*trader=*/0, phase));

    auto sink = std::make_unique<SimulatedExchangeSink>(/*qty_filled_at_entry=*/0); // GTC join -> ack only
    SimulatedExchangeSink* sink_ptr = sink.get();
    Gateway gateway(std::move(sink), registry);

    RingA ring_a;
    RingB ring_b;
    RingC ring_c;
    OracleRing oracle_ring;
    std::atomic<bool> stop{false};

    std::thread brain_thread([&] { brain->run(ring_a, oracle_ring, ring_c, ring_b, stop); });
    std::thread gateway_thread([&] { gateway.run(ring_b, ring_c, stop); });

    // A real two-sided book, decoded via the real production parser — see
    // tests/test_replay_determinism.cpp for the identical frame-shape
    // convention (quoted-decimal Kalshi V2 schema).
    const std::vector<std::string> frames = {
        R"({"type":"orderbook_snapshot","sid":10,"seq":1,)"
        R"("msg":{"market_ticker":"E2E-BUY","market_id":"m",)"
        R"("yes_dollars_fp":[["0.4000","100.00"]],)"
        R"("no_dollars_fp":[["0.4000","50.00"]]}})", // YES bid@40 qty100; NO bid@40 -> YES ask@60 qty50
    };
    for (const auto& raw : frames) {
        FeedMsg fm;
        std::string err;
        if (parse_feed_message(raw, fm, err)) ingress.handle_message(fm, ring_a);
    }

    PX_CHECK(wait_until([&] { return phase.load(std::memory_order_acquire) == 3; }));

    PX_CHECK(brain->intents_accepted() == 2); // 1 NewOrder (Join) + 1 CancelOrder
    PX_CHECK(brain->intents_rejected() == 0);
    PX_CHECK(sink_ptr->place_call_count == 1);
    PX_CHECK(sink_ptr->cancel_call_count == 1);
    // Gateway resolved Brain's own OrderId to the SAME Kalshi UUID this exact
    // order was acked with — proof the id mapping (live/kalshi_gateway.hpp's
    // ORDER-ID MAPPING) round-tripped correctly through a real Brain/Gateway
    // pair, not just an isolated Gateway test hand-fed the UUID already.
    PX_CHECK(sink_ptr->last_cancel_kalshi_id == sink_ptr->last_place_uuid);
    PX_CHECK(brain->order_tracker().orders_for(market).empty()); // nothing left resting

    stop.store(true, std::memory_order_relaxed);
    brain_thread.join();
    gateway_thread.join();
}

// ---- Scenario B: SELL, Cross, Ack, Fill, OwnOrderUpdate, and the ----------
// ---- OrderTracker regression proof (Finding #2) ---------------------------
void test_cross_fill_then_trades_again_sell() {
    TickerRegistry registry;
    MarketId market;
    PX_CHECK(registry.add("E2E-SELL", market));

    KalshiConfig ws_cfg;
    Ingress ingress(ws_cfg, registry);

    risk::RiskConfig risk_cfg;
    auto brain = std::make_unique<Brain>(risk_cfg);

    // The real, shipped strategy — not a test stand-in — specifically
    // because Finding #2's bug only manifests through OrderBookImbalance's
    // own `!state.outstanding_orders.empty()` re-entry gate
    // (strategy/order_book_imbalance.hpp).
    brain->add_strategy(market, std::make_unique<OrderBookImbalance>(market, /*trader=*/0));

    // Every place() call fully fills at entry (qty_filled_at_entry huge, so
    // it's always clamped to the actual requested qty) — the IOC-cross
    // response shape OrderBookImbalance always produces (TimeInForce::IOC).
    auto sink = std::make_unique<SimulatedExchangeSink>(/*qty_filled_at_entry=*/999);
    SimulatedExchangeSink* sink_ptr = sink.get();
    Gateway gateway(std::move(sink), registry);

    RingA ring_a;
    RingB ring_b;
    RingC ring_c;
    OracleRing oracle_ring;
    std::atomic<bool> stop{false};

    std::thread brain_thread([&] { brain->run(ring_a, oracle_ring, ring_c, ring_b, stop); });
    std::thread gateway_thread([&] { gateway.run(ring_b, ring_c, stop); });

    // Heavily ask-skewed (imbalance = 10/(10+90) = 0.10 <= 1-0.75) ->
    // OrderBookImbalance::on_event issues Cross SELL @ best_bid=40.
    FeedMsg snap;
    std::string err;
    PX_CHECK(parse_feed_message(R"({"type":"orderbook_snapshot","sid":11,"seq":1,)"
                                 R"("msg":{"market_ticker":"E2E-SELL","market_id":"m",)"
                                 R"("yes_dollars_fp":[["0.4000","10.00"]],)"
                                 R"("no_dollars_fp":[["0.4000","90.00"]]}})",
                                 snap, err));
    ingress.handle_message(snap, ring_a);

    PX_CHECK(wait_until([&] { return sink_ptr->place_call_count >= 1; }));
    PX_CHECK(wait_until([&] { return brain->intents_accepted() >= 1; }));

    // Same skew, nudged further ask-heavy by a real delta -- a second,
    // independent BookUpdate. Pre-fix, the first Cross's fully-filled order
    // left a permanent zombie entry in OrderTracker and this would never
    // produce a second Cross; wait_until below times out and PX_CHECK fails
    // rather than hanging forever.
    FeedMsg delta;
    PX_CHECK(parse_feed_message(R"({"type":"orderbook_delta","sid":11,"seq":2,)"
                                 R"("msg":{"market_ticker":"E2E-SELL","market_id":"m",)"
                                 R"("price_dollars":"0.4000","delta_fp":"5.00","side":"no"}})",
                                 delta, err));
    ingress.handle_message(delta, ring_a);

    PX_CHECK(wait_until([&] { return sink_ptr->place_call_count >= 2; }));
    PX_CHECK(wait_until([&] { return brain->intents_accepted() >= 2; }));

    PX_CHECK(brain->intents_rejected() == 0);
    PX_CHECK(sink_ptr->place_call_count == 2); // exactly two crosses -- not zero, not a runaway loop
    // Every fully-resolved (IOC, remaining 0) cross leaves nothing resting --
    // the Finding #2 fix in one assertion.
    PX_CHECK(brain->order_tracker().orders_for(market).empty());

    stop.store(true, std::memory_order_relaxed);
    brain_thread.join();
    gateway_thread.join();
}

} // namespace

int main() {
    test_join_then_cancel_buy();
    test_cross_fill_then_trades_again_sell();
    PX_TEST_REPORT_AND_RETURN();
}
