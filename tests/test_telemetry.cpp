// tests/test_telemetry.cpp — Stage 6, Part G (extended Stage 6A):
// telemetry/position_account.hpp, telemetry/telemetry.hpp,
// telemetry/telemetry_publisher.hpp.
#include "telemetry/telemetry.hpp"
#include "telemetry/telemetry_publisher.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <string>

using namespace px;
using namespace px::telemetry;

namespace {

// Hand-verified scenario (see this file's own derivation in the Stage 6 session):
// open long 10@40, extend +5@44, partial close -5@50 (realizes some P&L), then a
// 15-lot sell that closes the remaining 10 AND flips to a 5-lot short, in one fill.
void test_position_account_open_extend_close_flip() {
    PositionAccount acc;

    acc.apply_fill(Side::Buy, 40, 10);
    PX_CHECK(acc.position() == 10);
    PX_CHECK(acc.avg_entry_cents() == 40);

    acc.apply_fill(Side::Buy, 44, 5);
    PX_CHECK(acc.position() == 15);
    PX_CHECK(acc.avg_entry_cents() == 41); // (400+220)/15 = 41.33 -> truncates to 41

    acc.apply_fill(Side::Sell, 50, 5); // partial close: realizes (50-41)*5 = 45
    PX_CHECK(acc.position() == 10);
    PX_CHECK(acc.realized_pnl_cents() == 45);

    acc.apply_fill(Side::Sell, 45, 15); // closes remaining 10 (+40 realized), flips to -5 @ 45
    PX_CHECK(acc.position() == -5);
    PX_CHECK(acc.realized_pnl_cents() == 85); // 45 + 40
    PX_CHECK(acc.avg_entry_cents() == 45);

    // Short 5 @ 45, mark at 40 (price fell -- good for the short): profit = (40-45)*(-5) = 25
    PX_CHECK(acc.unrealized_pnl_cents(40) == 25);
    // Mark at 50 (price rose -- bad for the short): loss = (50-45)*(-5) = -25
    PX_CHECK(acc.unrealized_pnl_cents(50) == -25);
}

void test_position_account_flat_has_no_pnl() {
    PositionAccount acc;
    PX_CHECK(acc.position() == 0);
    PX_CHECK(acc.avg_entry_cents() == 0);
    PX_CHECK(acc.unrealized_pnl_cents(50) == 0);
    acc.apply_fill(Side::Buy, 30, 4);
    acc.apply_fill(Side::Sell, 30, 4); // exact round-trip -- back to flat, zero P&L
    PX_CHECK(acc.position() == 0);
    PX_CHECK(acc.realized_pnl_cents() == 0);
}

void test_telemetry_state_counters_and_rings() {
    TelemetryState t(/*start_ts_ms=*/1000);
    PX_CHECK(t.events_received() == 0);

    t.set_connected(true);
    t.set_mode(Mode::DryRun);
    t.on_event_received();
    t.on_event_received();
    t.on_event_processed();
    PX_CHECK(t.connected());
    PX_CHECK(t.mode() == Mode::DryRun);
    PX_CHECK(t.events_received() == 2);
    PX_CHECK(t.events_processed() == 1);

    t.on_order_submitted(/*market=*/3, /*id=*/7, Side::Buy, 42, 10, /*ts_ms=*/1100);
    t.on_order_acked(3, 7, Side::Buy, 42, 10, 1101);
    PX_CHECK(t.orders_submitted() == 1);
    PX_CHECK(t.orders_acked() == 1);

    t.on_order_filled(3, 7, Side::Buy, 42, 10, /*fee_cents=*/5, 1102);
    PX_CHECK(t.orders_filled() == 1);
    PX_CHECK(t.fees_cents() == 5);
    PX_CHECK(t.markets()[3].in_use.load());
    PX_CHECK(t.markets()[3].position.load() == 10);
    PX_CHECK(t.markets()[3].avg_entry_cents.load() == 42);
    PX_CHECK(t.total_realized_pnl_cents() == 0); // opening a position realizes nothing yet

    t.on_book_update(3, /*have_bid=*/true, 44, /*have_ask=*/true, 46, /*ts_ms=*/1103);
    PX_CHECK(t.markets()[3].unrealized_pnl_cents.load() == (45 - 42) * 10); // mid=45
    PX_CHECK(t.total_unrealized_pnl_cents() == (45 - 42) * 10);
    PX_CHECK(t.net_pnl_cents() == (45 - 42) * 10 - 5); // unrealized - fees, realized still 0

    t.on_strategy_decision(3, "OrderBookImbalance", /*trigger=*/0, /*action=*/2, 46, 5, "imbalance=0.71", 1104);
    PX_CHECK(t.strategy_decisions() == 1);

    const auto orders = t.recent_orders();
    PX_CHECK(orders.size() == 3); // submit, ack, fill
    PX_CHECK(orders.back().kind == OrderEventKind::Fill);
    PX_CHECK(orders.back().id == 7); // Stage 6A: order id threaded through

    const auto decisions = t.recent_decisions();
    PX_CHECK(decisions.size() == 1);
    PX_CHECK(std::string(decisions[0].strategy_name) == "OrderBookImbalance");
    PX_CHECK(std::string(decisions[0].reason) == "imbalance=0.71");
}

// Regression test for a real dashboard-facing bug found while building Stage 6
// Part E/F: MarketSnapshot::status used to default to 0 (PreOpen), independent
// of pipeline::Brain's own market_status_ default of Open (see
// pipeline/brain.hpp's constructor) -- a market Brain considered tradeable from
// the start would show "PreOpen" on the dashboard until (if ever) an explicit
// LifecycleStatus event arrived for it. Caught by actually looking at the
// rendered dashboard against a real replay, not by inspection -- see
// telemetry.hpp's MarketSnapshot::status field comment for the fix.
void test_market_status_defaults_to_open_not_preopen() {
    TelemetryState t(0);
    // No on_lifecycle call anywhere in this test -- on_book_update alone must
    // still leave status reading Open(1), not PreOpen(0).
    t.on_book_update(/*market=*/5, /*have_bid=*/true, 40, /*have_ask=*/true, 60, /*ts_ms=*/10);
    PX_CHECK(t.markets()[5].in_use.load());
    PX_CHECK(t.markets()[5].status.load() == 1); // px::MarketStatus::Open
}

void test_recent_ring_wraps_correctly() {
    RecentRing<int, 4> ring;
    for (int i = 0; i < 10; ++i) ring.push(i);
    const auto snap = ring.snapshot();
    PX_CHECK(snap.size() == 4);
    PX_CHECK(snap[0] == 6);
    PX_CHECK(snap[3] == 9); // oldest-first, last 4 of 0..9 is 6,7,8,9
}

// Stage 6A: order-book depth publishing.
void test_depth_update() {
    TelemetryState t(0);
    const Price bid_prices[] = {60, 59, 57};
    const Qty bid_qtys[] = {10, 25, 5};
    const Price ask_prices[] = {62, 63};
    const Qty ask_qtys[] = {8, 40};
    t.on_depth_update(4, bid_prices, bid_qtys, 3, ask_prices, ask_qtys, 2);

    PX_CHECK(t.markets()[4].bid_depth_count.load() == 3);
    PX_CHECK(t.markets()[4].bid_depth_price[0].load() == 60);
    PX_CHECK(t.markets()[4].bid_depth_qty[0].load() == 10);
    PX_CHECK(t.markets()[4].bid_depth_price[2].load() == 57);
    PX_CHECK(t.markets()[4].ask_depth_count.load() == 2);
    PX_CHECK(t.markets()[4].ask_depth_price[1].load() == 63);
    PX_CHECK(t.markets()[4].ask_depth_qty[1].load() == 40);

    // Over-capacity input is truncated to kDepthLevels, not overflowed.
    Price many_prices[20];
    Qty many_qtys[20];
    for (int i = 0; i < 20; ++i) { many_prices[i] = static_cast<Price>(90 - i); many_qtys[i] = 1; }
    t.on_depth_update(4, many_prices, many_qtys, 20, nullptr, nullptr, 0);
    PX_CHECK(t.markets()[4].bid_depth_count.load() == kDepthLevels);
}

// Stage 6A: the public trade tape -- distinct from this account's own order fills.
void test_public_trade_tape() {
    TelemetryState t(0);
    t.on_public_trade(2, Side::Buy, 55, 10, 500);
    t.on_public_trade(2, Side::Sell, 54, 3, 501);
    const auto trades = t.recent_trades();
    PX_CHECK(trades.size() == 2);
    PX_CHECK(trades[0].price == 55);
    PX_CHECK(trades[1].side == Side::Sell);
}

// Stage 6A: session P&L history -- unthrottled on fills, throttled on book updates.
void test_pnl_history_sampling() {
    TelemetryState t(0);
    t.on_order_submitted(0, 1, Side::Buy, 40, 10, 1000);
    t.on_order_filled(0, 1, Side::Buy, 40, 10, /*fee_cents=*/0, 1000); // opens a position, no realized P&L yet
    PX_CHECK(t.pnl_history().size() == 1); // fills always sample, unthrottled

    // Two book updates less than kPnlSampleMinIntervalMs apart -- only the first
    // (or neither, if the very first call also seeds the throttle without
    // sampling -- see below) should add a new sample; a third far enough ahead
    // in time must.
    t.on_book_update(0, true, 41, true, 43, 1001); // 1ms after the fill's own sample
    const auto after_close_update = t.pnl_history().size();
    t.on_book_update(0, true, 41, true, 43, 1500); // still < kPnlSampleMinIntervalMs later
    PX_CHECK(t.pnl_history().size() == after_close_update); // throttled -- no new sample

    t.on_book_update(0, true, 41, true, 43, 1000 + kPnlSampleMinIntervalMs + 1);
    PX_CHECK(t.pnl_history().size() == after_close_update + 1); // throttle window elapsed

    const auto hist = t.pnl_history();
    PX_CHECK(hist.front().net_pnl_cents == 0); // opening a position alone realizes nothing
}

// Stage 6A: risk limits (static config, published once) and the live rate-limit
// token sample (from Brain's own T2 thread in production, called directly here).
void test_risk_limits_and_rate_limit() {
    TelemetryState t(0);
    PX_CHECK(t.max_exposure_cents() == 0); // never set yet
    t.set_risk_limits(/*max_exposure_cents=*/10'000, /*fat_finger_min=*/5, /*fat_finger_max=*/95,
                       /*rate_limit_capacity_tokens=*/100);
    PX_CHECK(t.max_exposure_cents() == 10'000);
    PX_CHECK(t.fat_finger_min() == 5);
    PX_CHECK(t.fat_finger_max() == 95);
    PX_CHECK(t.rate_limit_capacity() == 100);
    PX_CHECK(t.rate_limit_tokens() == -1); // sentinel: never sampled

    t.set_rate_limit_tokens(73);
    PX_CHECK(t.rate_limit_tokens() == 73);
}

// Stage 6A: Brain's OWN pre-send rejection (lifecycle gate / RiskEngine verdict) --
// distinct from on_order_rejected (Kalshi's post-send rejection of an order Brain
// already approved and sent).
void test_risk_rejected_distinct_from_order_rejected() {
    TelemetryState t(0);
    t.on_risk_rejected(1, 2000);
    PX_CHECK(t.risk_rejections() == 1);
    PX_CHECK(t.orders_rejected() == 0); // unaffected -- a different counter entirely

    t.on_order_rejected(1, /*id=*/9, Side::Sell, 60, 4, 2001);
    PX_CHECK(t.orders_rejected() == 1);
    PX_CHECK(t.risk_rejections() == 1); // unaffected the other way

    const auto orders = t.recent_orders();
    PX_CHECK(orders.size() == 2);
    PX_CHECK(orders[0].kind == OrderEventKind::RiskBlocked);
    PX_CHECK(orders[0].id == kInvalidOrderId); // never got far enough to be assigned one
    PX_CHECK(orders[1].kind == OrderEventKind::Reject);
    PX_CHECK(orders[1].id == 9);
}

void test_publish_snapshot_writes_readable_json() {
    TelemetryState t(2000);
    t.set_connected(true);
    t.set_risk_limits(10'000, 5, 95, 100);
    t.set_rate_limit_tokens(88);
    t.on_order_submitted(0, /*id=*/42, Side::Buy, 50, 1, 2001);
    t.on_order_filled(0, 42, Side::Buy, 50, 1, 2, 2002);
    t.on_book_update(0, true, 49, true, 51, 2003);
    const Price bid_prices[] = {49};
    const Qty bid_qtys[] = {12};
    t.on_depth_update(0, bid_prices, bid_qtys, 1, nullptr, nullptr, 0);
    t.on_public_trade(0, Side::Sell, 50, 2, 2004);

    const std::string path = "test_telemetry_snapshot.json";
    PX_CHECK(publish_snapshot(t, path));

    FILE* f = std::fopen(path.c_str(), "rb");
    PX_CHECK(f != nullptr);
    if (f) {
        std::string content;
        char buf[256];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
        std::fclose(f);
        PX_CHECK(content.find("\"connected\": true") != std::string::npos);
        PX_CHECK(content.find("\"orders_filled\": 1") != std::string::npos);
        PX_CHECK(content.find("\"market\": 0") != std::string::npos);
        PX_CHECK(content.find("\"id\": 42") != std::string::npos);
        PX_CHECK(content.find("\"max_exposure_cents\": 10000") != std::string::npos);
        PX_CHECK(content.find("\"rate_limit_tokens\": 88") != std::string::npos);
        PX_CHECK(content.find("\"bid_depth\": [{\"price\": 49, \"qty\": 12}]") != std::string::npos);
        PX_CHECK(content.find("\"recent_trades\"") != std::string::npos);
        PX_CHECK(content.find("\"pnl_history\"") != std::string::npos);
        std::remove(path.c_str());
    }
    std::remove((path + ".tmp").c_str());
}

} // namespace

int main() {
    test_position_account_open_extend_close_flip();
    test_position_account_flat_has_no_pnl();
    test_telemetry_state_counters_and_rings();
    test_market_status_defaults_to_open_not_preopen();
    test_recent_ring_wraps_correctly();
    test_depth_update();
    test_public_trade_tape();
    test_pnl_history_sampling();
    test_risk_limits_and_rate_limit();
    test_risk_rejected_distinct_from_order_rejected();
    test_publish_snapshot_writes_readable_json();
    PX_TEST_REPORT_AND_RETURN();
}
