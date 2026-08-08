// tests/test_strategies.cpp — each concrete Strategy's on_event() against
// hand-built MarketState, mirroring test_risk.cpp's injected-state style. No
// pipeline, no threads, no network.
#include "px/shadow_book.hpp"
#include "strategy/order_book_imbalance.hpp"
#include "strategy/oracle_threshold_strategy.hpp"
#include "strategy/passive_market_maker.hpp"
#include "strategy/trade_flow.hpp"
#include "test_harness.hpp"

using namespace px;
using namespace px::strategy;

namespace {

[[nodiscard]] MarketState make_state(ShadowBook& book, Position position, OrderTracker& tracker,
                                      TradeWindow& trades, std::int64_t now_ms = 0) {
    return extract_features(0, book, position, tracker, trades, now_ms);
}

// ---- OrderBookImbalance ------------------------------------------------------

void test_imbalance_heavy_bid_skew_crosses_buy() {
    OrderBookImbalance strat(0, 0);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 900);
    book.set_level(Side::Sell, 60, 100);
    OrderTracker tracker;
    TradeWindow trades;
    const auto d = strat.on_event(make_state(book, 0, tracker, trades), TriggerKind::BookUpdate, nullptr);
    PX_CHECK(d.action == ExecutionAction::Cross);
    PX_CHECK(d.intent.side == Side::Buy);
    PX_CHECK(d.intent.price == 60);
}

void test_imbalance_heavy_ask_skew_crosses_sell() {
    OrderBookImbalance strat(0, 0);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 100);
    book.set_level(Side::Sell, 60, 900);
    OrderTracker tracker;
    TradeWindow trades;
    const auto d = strat.on_event(make_state(book, 0, tracker, trades), TriggerKind::BookUpdate, nullptr);
    PX_CHECK(d.action == ExecutionAction::Cross);
    PX_CHECK(d.intent.side == Side::Sell);
    PX_CHECK(d.intent.price == 40);
}

void test_imbalance_moderate_skew_joins() {
    OrderBookImbalance strat(0, 0, /*aggressive=*/0.75, /*passive=*/0.60);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 650);
    book.set_level(Side::Sell, 60, 350); // imbalance = 0.65 -> between passive and aggressive
    OrderTracker tracker;
    TradeWindow trades;
    const auto d = strat.on_event(make_state(book, 0, tracker, trades), TriggerKind::BookUpdate, nullptr);
    PX_CHECK(d.action == ExecutionAction::Join);
    PX_CHECK(d.intent.side == Side::Buy);
    PX_CHECK(d.intent.price == 40); // joins at the current best, doesn't improve
}

void test_imbalance_balanced_waits() {
    OrderBookImbalance strat(0, 0);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 500);
    book.set_level(Side::Sell, 60, 500);
    OrderTracker tracker;
    TradeWindow trades;
    const auto d = strat.on_event(make_state(book, 0, tracker, trades), TriggerKind::BookUpdate, nullptr);
    PX_CHECK(d.action == ExecutionAction::Wait);
}

void test_imbalance_ignores_oracle_and_own_order_triggers() {
    OrderBookImbalance strat(0, 0);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 900);
    book.set_level(Side::Sell, 60, 100);
    OrderTracker tracker;
    TradeWindow trades;
    const auto s = make_state(book, 0, tracker, trades);
    PX_CHECK(strat.on_event(s, TriggerKind::OracleEvent, nullptr).action == ExecutionAction::None);
    PX_CHECK(strat.on_event(s, TriggerKind::OwnOrderUpdate, nullptr).action == ExecutionAction::None);
}

void test_imbalance_does_not_pile_on_existing_order() {
    OrderBookImbalance strat(0, 0);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 900);
    book.set_level(Side::Sell, 60, 100);
    OrderTracker tracker;
    TradeWindow trades;
    PX_CHECK(tracker.on_new(0, 1, Side::Buy, 40, 5));
    tracker.on_acked(0, 1, 5, book);
    const auto d = strat.on_event(make_state(book, 0, tracker, trades), TriggerKind::BookUpdate, nullptr);
    PX_CHECK(d.action == ExecutionAction::None); // already resting — leaves it alone
}

// ---- TradeFlow ----------------------------------------------------------------

void test_trade_flow_needs_minimum_sample() {
    TradeFlow strat(0, 0, /*min_trade_count=*/3);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 600);
    book.set_level(Side::Sell, 60, 400);
    OrderTracker tracker;
    TradeWindow trades;
    trades.record(Side::Buy, 45, 1, 100); // only 1 trade — below min_trade_count
    const auto d = strat.on_event(make_state(book, 0, tracker, trades, 100), TriggerKind::BookUpdate, nullptr);
    PX_CHECK(d.action == ExecutionAction::Wait);
}

void test_trade_flow_buy_dominated_and_bid_leaning_joins_buy() {
    TradeFlow strat(0, 0);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 600);
    book.set_level(Side::Sell, 60, 400);
    OrderTracker tracker;
    TradeWindow trades;
    trades.record(Side::Buy, 45, 1, 100);
    trades.record(Side::Buy, 45, 1, 200);
    trades.record(Side::Buy, 45, 1, 300);
    trades.record(Side::Sell, 45, 1, 400);
    const auto d = strat.on_event(make_state(book, 0, tracker, trades, 400), TriggerKind::BookUpdate, nullptr);
    PX_CHECK(d.action == ExecutionAction::Join);
    PX_CHECK(d.intent.side == Side::Buy);
    PX_CHECK(d.intent.price == 40);
}

void test_trade_flow_distrusts_tape_against_opposing_book() {
    // Buy-heavy tape, but the book itself is ask-heavy (sellers replenishing
    // as fast as they're hit) — the strategy is documented to require book
    // confirmation, not just the tape alone.
    TradeFlow strat(0, 0);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 100);
    book.set_level(Side::Sell, 60, 900); // imbalance well below 0.5
    OrderTracker tracker;
    TradeWindow trades;
    trades.record(Side::Buy, 45, 1, 100);
    trades.record(Side::Buy, 45, 1, 200);
    trades.record(Side::Buy, 45, 1, 300);
    const auto d = strat.on_event(make_state(book, 0, tracker, trades, 300), TriggerKind::BookUpdate, nullptr);
    PX_CHECK(d.action == ExecutionAction::Wait);
}

// ---- PassiveMarketMaker: quoting, cancel-on-drift, exit-on-position ------------

void test_pmm_quotes_missing_bid_first_when_flat() {
    PassiveMarketMaker strat(0, 0, /*min_spread=*/4, /*quote_qty=*/5, /*max_position=*/20);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 100);
    book.set_level(Side::Sell, 60, 100);
    OrderTracker tracker;
    TradeWindow trades;
    const auto d = strat.on_event(make_state(book, 0, tracker, trades), TriggerKind::BookUpdate, nullptr);
    PX_CHECK(d.action == ExecutionAction::Join);
    PX_CHECK(d.intent.side == Side::Buy);
    PX_CHECK(d.intent.price == 40);
    PX_CHECK(strat.state() == StrategyState::Quoting);
}

void test_pmm_quotes_ask_once_bid_is_resting() {
    PassiveMarketMaker strat(0, 0, 4, 5, 20);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 100);
    book.set_level(Side::Sell, 60, 100);
    OrderTracker tracker;
    TradeWindow trades;
    PX_CHECK(tracker.on_new(0, 1, Side::Buy, 40, 5));
    tracker.on_acked(0, 1, 5, book);
    const auto d = strat.on_event(make_state(book, 0, tracker, trades), TriggerKind::BookUpdate, nullptr);
    PX_CHECK(d.action == ExecutionAction::Join);
    PX_CHECK(d.intent.side == Side::Sell);
    PX_CHECK(d.intent.price == 60);
}

void test_pmm_waits_once_both_sides_quoted() {
    PassiveMarketMaker strat(0, 0, 4, 5, 20);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 100);
    book.set_level(Side::Sell, 60, 100);
    OrderTracker tracker;
    TradeWindow trades;
    PX_CHECK(tracker.on_new(0, 1, Side::Buy, 40, 5));
    tracker.on_acked(0, 1, 5, book);
    PX_CHECK(tracker.on_new(0, 2, Side::Sell, 60, 5));
    tracker.on_acked(0, 2, 5, book);
    const auto d = strat.on_event(make_state(book, 0, tracker, trades), TriggerKind::BookUpdate, nullptr);
    PX_CHECK(d.action == ExecutionAction::Wait);
}

void test_pmm_cancels_stale_quote_when_book_moves() {
    PassiveMarketMaker strat(0, 0, 4, 5, 20);
    ShadowBook book;
    book.set_level(Side::Buy, 42, 100); // best_bid moved from 40 to 42
    book.set_level(Side::Sell, 60, 100);
    OrderTracker tracker;
    TradeWindow trades;
    PX_CHECK(tracker.on_new(0, 1, Side::Buy, 40, 5)); // resting at stale 40
    tracker.on_acked(0, 1, 5, book);
    const auto d = strat.on_event(make_state(book, 0, tracker, trades), TriggerKind::BookUpdate, nullptr);
    PX_CHECK(d.action == ExecutionAction::Cancel);
    PX_CHECK(d.cancel_target == 1);
}

void test_pmm_wont_quote_a_tight_market() {
    PassiveMarketMaker strat(0, 0, /*min_spread=*/10, 5, 20);
    ShadowBook book;
    book.set_level(Side::Buy, 49, 100);
    book.set_level(Side::Sell, 51, 100); // spread = 2, below min_spread
    OrderTracker tracker;
    TradeWindow trades;
    const auto d = strat.on_event(make_state(book, 0, tracker, trades), TriggerKind::BookUpdate, nullptr);
    PX_CHECK(d.action == ExecutionAction::Wait);
}

void test_pmm_exits_long_position_on_book_update() {
    PassiveMarketMaker strat(0, 0, 4, 5, 20);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 100);
    book.set_level(Side::Sell, 60, 100);
    OrderTracker tracker;
    TradeWindow trades;
    const auto d = strat.on_event(make_state(book, /*position=*/10, tracker, trades), TriggerKind::BookUpdate,
                                   nullptr);
    PX_CHECK(d.action == ExecutionAction::Cross);
    PX_CHECK(d.intent.side == Side::Sell); // long -> sell to reduce
    PX_CHECK(d.intent.qty == 10);
    PX_CHECK(strat.state() == StrategyState::Exiting);
}

void test_pmm_exits_short_position_on_own_order_update_not_just_book_update() {
    // The whole reason TriggerKind::OwnOrderUpdate exists: a fill shouldn't
    // have to wait for the next incidental book tick to start being managed.
    PassiveMarketMaker strat(0, 0, 4, 5, 20);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 100);
    book.set_level(Side::Sell, 60, 100);
    OrderTracker tracker;
    TradeWindow trades;
    const auto d = strat.on_event(make_state(book, /*position=*/-7, tracker, trades),
                                   TriggerKind::OwnOrderUpdate, nullptr);
    PX_CHECK(d.action == ExecutionAction::Cross);
    PX_CHECK(d.intent.side == Side::Buy); // short -> buy to reduce
    PX_CHECK(d.intent.qty == 7);
}

void test_pmm_position_management_takes_priority_over_quoting() {
    PassiveMarketMaker strat(0, 0, 4, 5, 20);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 100);
    book.set_level(Side::Sell, 60, 100);
    OrderTracker tracker; // no resting orders at all
    TradeWindow trades;
    // Even with a perfectly quotable flat book and no resting orders, a
    // nonzero position must be dealt with before any fresh quoting starts.
    const auto d = strat.on_event(make_state(book, /*position=*/3, tracker, trades), TriggerKind::BookUpdate,
                                   nullptr);
    PX_CHECK(d.action == ExecutionAction::Cross);
}

void test_pmm_tight_exposure_cap_blocks_both_sides_when_flat() {
    // With position==0, the bid-side and ask-side cap checks are mirror
    // images of each other (0+qty<=max vs 0-qty>=-max), so a cap tighter than
    // one quote's size blocks both sides simultaneously — the only reachable
    // form of this check, since a nonzero position always routes through the
    // exit branch before quoting logic runs at all (see the test above).
    PassiveMarketMaker strat(0, 0, /*min_spread=*/4, /*quote_qty=*/5, /*max_position=*/3);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 100);
    book.set_level(Side::Sell, 60, 100);
    OrderTracker tracker;
    TradeWindow trades;
    const auto d = strat.on_event(make_state(book, 0, tracker, trades), TriggerKind::BookUpdate, nullptr);
    PX_CHECK(d.action == ExecutionAction::Wait);
}

void test_pmm_full_state_machine_sequence() {
    PassiveMarketMaker strat(0, 0, 4, 5, 20);
    ShadowBook book;
    book.set_level(Side::Buy, 40, 100);
    book.set_level(Side::Sell, 60, 100);
    OrderTracker tracker;
    TradeWindow trades;

    PX_CHECK(strat.state() == StrategyState::Flat);

    // 1. Quote the bid.
    auto d1 = strat.on_event(make_state(book, 0, tracker, trades), TriggerKind::BookUpdate, nullptr);
    PX_CHECK(d1.action == ExecutionAction::Join && d1.intent.side == Side::Buy);
    PX_CHECK(tracker.on_new(0, 100, Side::Buy, 40, 5));
    tracker.on_acked(0, 100, 5, book);
    PX_CHECK(strat.state() == StrategyState::Quoting);

    // 2. Quote the ask.
    auto d2 = strat.on_event(make_state(book, 0, tracker, trades), TriggerKind::BookUpdate, nullptr);
    PX_CHECK(d2.action == ExecutionAction::Join && d2.intent.side == Side::Sell);
    PX_CHECK(tracker.on_new(0, 101, Side::Sell, 60, 5));
    tracker.on_acked(0, 101, 5, book);

    // 3. A fill on the bid moves us into Position/Exiting — simulate by
    // having the fill both change position (as RiskEngine/OrderTracker would
    // reflect) and firing OwnOrderUpdate immediately, per the design.
    tracker.on_fill(0, 100, 5);
    auto d3 = strat.on_event(make_state(book, /*position=*/5, tracker, trades), TriggerKind::OwnOrderUpdate,
                              nullptr);
    PX_CHECK(strat.state() == StrategyState::Exiting);
    PX_CHECK(d3.action == ExecutionAction::Cross && d3.intent.side == Side::Sell);

    // 4. The exit fills, position returns to 0 -> back to Flat.
    auto d4 = strat.on_event(make_state(book, /*position=*/0, tracker, trades), TriggerKind::OwnOrderUpdate,
                              nullptr);
    PX_CHECK(d4.action == ExecutionAction::None);
    // outstanding_orders still has the (stale, never-cancelled-in-this-test)
    // ask resting at 60 — state only fully resets to Flat once that's empty
    // too, matching the "flat means no position AND no resting quotes"
    // definition documented in strategy.hpp.
}

// ---- OracleThresholdStrategy: reference/demo-only, oracle-triggered only ------

void test_oracle_threshold_ignores_book_and_own_order_triggers() {
    OracleThresholdStrategy strat(OracleSource::NOAA, "KNYC", 90.0, 0, 0, 3, 1);
    ShadowBook book;
    book.set_level(Side::Sell, 92, 50);
    book.set_level(Side::Buy, 8, 50);
    OrderTracker tracker;
    TradeWindow trades;
    const auto s = make_state(book, 0, tracker, trades);
    PX_CHECK(strat.on_event(s, TriggerKind::BookUpdate, nullptr).action == ExecutionAction::None);
    PX_CHECK(strat.on_event(s, TriggerKind::OwnOrderUpdate, nullptr).action == ExecutionAction::None);
}

void test_oracle_threshold_crosses_on_matching_event() {
    OracleThresholdStrategy strat(OracleSource::NOAA, "KNYC", 90.0, 0, 0, 3, 1);
    ShadowBook book;
    book.set_level(Side::Sell, 92, 50);
    book.set_level(Side::Buy, 8, 50);
    OrderTracker tracker;
    TradeWindow trades;
    const auto s = make_state(book, 0, tracker, trades);

    OracleEvent e;
    e.source = OracleSource::NOAA;
    e.set_key("KNYC");
    e.value = 95.0; // comfortably above threshold+1
    const auto d = strat.on_event(s, TriggerKind::OracleEvent, &e);
    PX_CHECK(d.action == ExecutionAction::Cross);
    PX_CHECK(d.intent.side == Side::Buy);
    PX_CHECK(d.intent.price == 92);
}

void test_oracle_threshold_ignores_wrong_key() {
    OracleThresholdStrategy strat(OracleSource::NOAA, "KNYC", 90.0, 0, 0, 3, 1);
    ShadowBook book;
    book.set_level(Side::Sell, 92, 50);
    OrderTracker tracker;
    TradeWindow trades;
    const auto s = make_state(book, 0, tracker, trades);

    OracleEvent e;
    e.source = OracleSource::NOAA;
    e.set_key("KBOS"); // different station
    e.value = 95.0;
    PX_CHECK(strat.on_event(s, TriggerKind::OracleEvent, &e).action == ExecutionAction::None);
}

void test_oracle_threshold_null_oracle_pointer_is_safe() {
    OracleThresholdStrategy strat(OracleSource::NOAA, "KNYC", 90.0, 0, 0, 3, 1);
    ShadowBook book;
    OrderTracker tracker;
    TradeWindow trades;
    const auto s = make_state(book, 0, tracker, trades);
    PX_CHECK(strat.on_event(s, TriggerKind::OracleEvent, nullptr).action == ExecutionAction::None);
}

} // namespace

int main() {
    test_imbalance_heavy_bid_skew_crosses_buy();
    test_imbalance_heavy_ask_skew_crosses_sell();
    test_imbalance_moderate_skew_joins();
    test_imbalance_balanced_waits();
    test_imbalance_ignores_oracle_and_own_order_triggers();
    test_imbalance_does_not_pile_on_existing_order();

    test_trade_flow_needs_minimum_sample();
    test_trade_flow_buy_dominated_and_bid_leaning_joins_buy();
    test_trade_flow_distrusts_tape_against_opposing_book();

    test_pmm_quotes_missing_bid_first_when_flat();
    test_pmm_quotes_ask_once_bid_is_resting();
    test_pmm_waits_once_both_sides_quoted();
    test_pmm_cancels_stale_quote_when_book_moves();
    test_pmm_wont_quote_a_tight_market();
    test_pmm_exits_long_position_on_book_update();
    test_pmm_exits_short_position_on_own_order_update_not_just_book_update();
    test_pmm_position_management_takes_priority_over_quoting();
    test_pmm_tight_exposure_cap_blocks_both_sides_when_flat();
    test_pmm_full_state_machine_sequence();

    test_oracle_threshold_ignores_book_and_own_order_triggers();
    test_oracle_threshold_crosses_on_matching_event();
    test_oracle_threshold_ignores_wrong_key();
    test_oracle_threshold_null_oracle_pointer_is_safe();

    PX_TEST_REPORT_AND_RETURN();
}
