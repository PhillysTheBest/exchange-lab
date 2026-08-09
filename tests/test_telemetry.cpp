// tests/test_telemetry.cpp — Stage 6, Part G: telemetry/position_account.hpp,
// telemetry/telemetry.hpp, telemetry/telemetry_publisher.hpp.
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

    t.on_order_submitted(/*market=*/3, Side::Buy, 42, 10, /*ts_ms=*/1100);
    t.on_order_acked(3, Side::Buy, 42, 10, 1101);
    PX_CHECK(t.orders_submitted() == 1);
    PX_CHECK(t.orders_acked() == 1);

    t.on_order_filled(3, Side::Buy, 42, 10, /*fee_cents=*/5, 1102);
    PX_CHECK(t.orders_filled() == 1);
    PX_CHECK(t.fees_cents() == 5);
    PX_CHECK(t.markets()[3].in_use.load());
    PX_CHECK(t.markets()[3].position.load() == 10);
    PX_CHECK(t.markets()[3].avg_entry_cents.load() == 42);

    t.on_book_update(3, /*have_bid=*/true, 44, /*have_ask=*/true, 46);
    PX_CHECK(t.markets()[3].unrealized_pnl_cents.load() == (45 - 42) * 10); // mid=45

    t.on_strategy_decision(3, "OrderBookImbalance", /*trigger=*/0, /*action=*/2, 46, 5, "imbalance=0.71", 1103);
    PX_CHECK(t.strategy_decisions() == 1);

    const auto orders = t.recent_orders();
    PX_CHECK(orders.size() == 3); // submit, ack, fill
    PX_CHECK(orders.back().kind == OrderEventKind::Fill);

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
    t.on_book_update(/*market=*/5, /*have_bid=*/true, 40, /*have_ask=*/true, 60);
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

void test_publish_snapshot_writes_readable_json() {
    TelemetryState t(2000);
    t.set_connected(true);
    t.on_order_submitted(0, Side::Buy, 50, 1, 2001);
    t.on_order_filled(0, Side::Buy, 50, 1, 2, 2002);
    t.on_book_update(0, true, 49, true, 51);

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
    test_publish_snapshot_writes_readable_json();
    PX_TEST_REPORT_AND_RETURN();
}
