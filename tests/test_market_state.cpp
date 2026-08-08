// tests/test_market_state.cpp — extract_features() against hand-built state.
// No pipeline, no threads, no network — ShadowBook/OrderTracker/TradeWindow
// are all plain, directly-constructible types (see strategy/market_state.hpp).
#include "px/shadow_book.hpp"
#include "strategy/market_state.hpp"
#include "test_harness.hpp"

using namespace px;
using namespace px::strategy;

namespace {

void test_empty_book() {
    ShadowBook book;
    OrderTracker tracker;
    TradeWindow trades;
    const MarketState s = extract_features(0, book, /*position=*/0, tracker, trades, /*now_ms=*/0);

    PX_CHECK(!s.have_bid);
    PX_CHECK(!s.have_ask);
    PX_CHECK(s.spread == 0);
    PX_CHECK(s.bid_depth == 0);
    PX_CHECK(s.ask_depth == 0);
    PX_CHECK(s.imbalance == 0.5); // documented convention for "no depth either side"
    PX_CHECK(s.outstanding_orders.empty());
    PX_CHECK(s.recent_trades.total == 0);
}

void test_bbo_and_spread() {
    ShadowBook book;
    book.set_level(Side::Buy, 40, 100);
    book.set_level(Side::Sell, 60, 50);
    OrderTracker tracker;
    TradeWindow trades;
    const MarketState s = extract_features(0, book, 0, tracker, trades, 0);

    PX_CHECK(s.have_bid && s.best_bid == 40);
    PX_CHECK(s.have_ask && s.best_ask == 60);
    PX_CHECK(s.spread == 20);
    PX_CHECK(s.bid_depth == 100);
    PX_CHECK(s.ask_depth == 50);
}

void test_one_sided_book_no_spread() {
    // Only a bid — spread is meaningless (documented: only valid when both
    // have_bid && have_ask) and must stay 0, not some garbage/sentinel value.
    ShadowBook book;
    book.set_level(Side::Buy, 40, 10);
    OrderTracker tracker;
    TradeWindow trades;
    const MarketState s = extract_features(0, book, 0, tracker, trades, 0);

    PX_CHECK(s.have_bid && !s.have_ask);
    PX_CHECK(s.spread == 0);
    PX_CHECK(s.bid_depth == 10);
    PX_CHECK(s.ask_depth == 0);
    PX_CHECK(s.imbalance == 1.0); // all depth on the bid side
}

void test_imbalance_calculation() {
    ShadowBook book;
    book.set_level(Side::Buy, 40, 900);
    book.set_level(Side::Sell, 60, 100);
    OrderTracker tracker;
    TradeWindow trades;
    const MarketState s = extract_features(0, book, 0, tracker, trades, 0);
    PX_CHECK(s.imbalance > 0.89 && s.imbalance < 0.91); // 900/1000 = 0.90
}

void test_position_passthrough() {
    ShadowBook book;
    OrderTracker tracker;
    TradeWindow trades;
    const MarketState s = extract_features(0, book, /*position=*/-25, tracker, trades, 0);
    PX_CHECK(s.position == -25);
}

void test_recent_trades_passthrough() {
    ShadowBook book;
    OrderTracker tracker;
    TradeWindow trades;
    trades.record(Side::Buy, 50, 1, 1000);
    trades.record(Side::Buy, 51, 1, 2000);
    trades.record(Side::Sell, 49, 1, 3000);
    const MarketState s = extract_features(0, book, 0, tracker, trades, /*now_ms=*/3000);
    PX_CHECK(s.recent_trades.total == 3);
    PX_CHECK(s.recent_trades.taker_buy == 2);
    PX_CHECK(s.recent_trades.taker_sell == 1);
}

void test_outstanding_orders_acked_only() {
    ShadowBook book;
    OrderTracker tracker;
    TradeWindow trades;

    PX_CHECK(tracker.on_new(0, 1, Side::Buy, 40, 10));
    PX_CHECK(tracker.on_new(0, 2, Side::Sell, 60, 5)); // left pending, deliberately not acked

    MarketState s1 = extract_features(0, book, 0, tracker, trades, 0);
    PX_CHECK(s1.outstanding_orders.empty()); // neither acked yet

    tracker.on_acked(0, 1, /*remaining_qty=*/10, book);
    MarketState s2 = extract_features(0, book, 0, tracker, trades, 0);
    PX_CHECK(s2.outstanding_orders.size() == 1);
    PX_CHECK(s2.outstanding_orders[0].id == 1);
}

void test_market_isolation() {
    // extract_features for market 0 must not see market 1's book/orders.
    ShadowBook book0, book1;
    book1.set_level(Side::Buy, 77, 999);
    OrderTracker tracker;
    TradeWindow trades0, trades1;
    PX_CHECK(tracker.on_new(1, 5, Side::Buy, 50, 1));
    tracker.on_acked(1, 5, 1, book1);

    const MarketState s0 = extract_features(0, book0, 0, tracker, trades0, 0);
    PX_CHECK(!s0.have_bid);
    PX_CHECK(s0.outstanding_orders.empty());

    const MarketState s1 = extract_features(1, book1, 0, tracker, trades1, 0);
    PX_CHECK(s1.have_bid && s1.best_bid == 77);
    PX_CHECK(s1.outstanding_orders.size() == 1);
}

} // namespace

int main() {
    test_empty_book();
    test_bbo_and_spread();
    test_one_sided_book_no_spread();
    test_imbalance_calculation();
    test_position_passthrough();
    test_recent_trades_passthrough();
    test_outstanding_orders_acked_only();
    test_market_isolation();
    PX_TEST_REPORT_AND_RETURN();
}
