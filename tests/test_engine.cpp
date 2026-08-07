// tests/test_engine.cpp — MatchingEngine-level behavior: market lifecycle
// gating, the pre-trade risk check, cash/position conservation across trades
// involving more than two counterparties, and market-scoped cancel routing.
#include "px/matching_engine.hpp"
#include "test_harness.hpp"

using namespace px;

namespace {

void test_market_lifecycle_gates_orders() {
    MatchingEngine eng;
    const MarketId m = eng.create_market("KXTEST-A");
    const TraderId a = eng.create_trader(1'000'00);
    const TraderId b = eng.create_trader(1'000'00);

    // PreOpen: orders rejected.
    auto r0 = eng.submit(m, a, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 50, 1);
    PX_CHECK(r0.reject == RejectReason::MarketNotOpen);

    PX_CHECK(eng.open_market(m));
    PX_CHECK(!eng.open_market(m)); // already open — not a valid PreOpen->Open transition again

    auto r1 = eng.submit(m, a, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 50, 1);
    PX_CHECK(r1.reject == RejectReason::None && r1.rested_qty == 1);

    PX_CHECK(eng.halt_market(m));
    auto r2 = eng.submit(m, b, Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, 50, 1);
    PX_CHECK(r2.reject == RejectReason::MarketNotOpen);

    PX_CHECK(eng.resume_market(m));
    auto r3 = eng.submit(m, b, Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, 50, 1);
    PX_CHECK(r3.reject == RejectReason::None && r3.filled_qty == 1);

    PX_CHECK(eng.resolve_market(m, Settlement::Yes));
    PX_CHECK(!eng.resolve_market(m, Settlement::No)); // resolution is terminal
    auto r4 = eng.submit(m, a, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 50, 1);
    PX_CHECK(r4.reject == RejectReason::MarketNotOpen);
}

void test_insufficient_cash_rejected() {
    MatchingEngine eng;
    const MarketId m = eng.create_market("KXTEST-B");
    eng.open_market(m);
    const TraderId poor = eng.create_trader(/*cash*/ 100); // $1.00

    // Buying 10 @ 50 needs 500 cents of collateral; poor trader only has 100.
    auto r = eng.submit(m, poor, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 50, 10);
    PX_CHECK(r.reject == RejectReason::InsufficientCash);
    PX_CHECK(r.order_id == kInvalidOrderId); // rejected before an order was ever created
    PX_CHECK(eng.market(m)->book().find_order(1) == nullptr);

    // But an affordable size at the same price succeeds.
    auto ok = eng.submit(m, poor, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 50, 2);
    PX_CHECK(ok.reject == RejectReason::None);
}

void test_cash_and_position_conservation() {
    MatchingEngine eng;
    const MarketId m = eng.create_market("KXTEST-C");
    eng.open_market(m);

    constexpr int kNumTraders = 5;
    TraderId traders[kNumTraders];
    Cash starting = 10'000'00;
    for (auto& t : traders) t = eng.create_trader(starting);

    // A handful of resting orders at different prices, then aggressive orders
    // that sweep across several of them and several counterparties.
    (void)eng.submit(m, traders[0], Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, 40, 5);
    (void)eng.submit(m, traders[1], Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, 41, 5);
    (void)eng.submit(m, traders[2], Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, 42, 5);
    (void)eng.submit(m, traders[3], Contract::No,  Side::Buy,  OrderType::Limit, TimeInForce::GTC, 55, 5); // == SELL YES @45

    auto sweep = eng.submit(m, traders[4], Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 45, 20);
    PX_CHECK(sweep.filled_qty == 20);

    Cash total_cash = 0;
    Position total_position = 0;
    for (auto id : traders) {
        Trader* t = eng.trader(id);
        total_cash += t->cash();
        total_position += t->position(m);
    }
    PX_CHECK(total_cash == starting * kNumTraders); // pure transfer, nothing created/destroyed
    PX_CHECK(total_position == 0);                  // every long has an exactly offsetting short
}

void test_cancel_is_scoped_to_its_market() {
    MatchingEngine eng;
    const MarketId m1 = eng.create_market("KXTEST-D1");
    const MarketId m2 = eng.create_market("KXTEST-D2");
    eng.open_market(m1);
    eng.open_market(m2);
    const TraderId t = eng.create_trader(1'000'00);

    auto r = eng.submit(m1, t, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 50, 3);
    PX_CHECK(r.rested_qty == 3);

    // Same numeric order id has never existed in m2 — cancel there must fail cleanly.
    PX_CHECK(eng.cancel(m2, r.order_id) == RejectReason::UnknownOrder);
    PX_CHECK(eng.cancel(m1, r.order_id) == RejectReason::None);
}

void test_unknown_market_and_trader_reject_cleanly() {
    MatchingEngine eng;
    const MarketId m = eng.create_market("KXTEST-E");
    eng.open_market(m);
    const TraderId t = eng.create_trader(1'000'00);

    auto bad_market = eng.submit(/*market*/ 999, t, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 50, 1);
    PX_CHECK(bad_market.reject == RejectReason::MarketNotOpen);

    auto bad_trader = eng.submit(m, /*trader*/ 999, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 50, 1);
    PX_CHECK(bad_trader.reject == RejectReason::UnknownOrder);

    PX_CHECK(eng.cancel(999, 1) == RejectReason::MarketNotOpen);
}

} // namespace

int main() {
    test_market_lifecycle_gates_orders();
    test_insufficient_cash_rejected();
    test_cash_and_position_conservation();
    test_cancel_is_scoped_to_its_market();
    test_unknown_market_and_trader_reject_cleanly();
    PX_TEST_REPORT_AND_RETURN();
}
