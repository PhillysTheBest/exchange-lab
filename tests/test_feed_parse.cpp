// tests/test_feed_parse.cpp — golden-frame tests for live/kalshi_feed.{hpp,cpp}.
//
// Frames below are hand-built to match Kalshi's published AsyncAPI schema
// exactly (docs.kalshi.com/asyncapi.yaml) — quoted decimal strings for
// price_dollars/delta_fp/yes_dollars_fp/no_dollars_fp, not the unquoted
// integers an earlier version of this project's feed handler assumed. See
// kalshi_feed.hpp's SCHEMA STATUS comment. parse_feed_message and SeqTracker
// are pure functions/classes — no socket, no network — so these run instantly
// and deterministically.
#include "kalshi_feed.hpp"
#include "test_harness.hpp"

using namespace px;
using namespace px::live;

namespace {

void test_subscribed() {
    const auto raw = std::string_view(R"({"id":1,"type":"subscribed","msg":{"channel":"orderbook_delta","sid":2}})");
    FeedMsg fm;
    std::string err;
    PX_CHECK(parse_feed_message(raw, fm, err));
    PX_CHECK(fm.kind == FeedMsgKind::Subscribed);
    PX_CHECK(fm.subscribed_channel == "orderbook_delta");
    PX_CHECK(fm.subscribed_sid == 2);
}

void test_snapshot_basic() {
    // Matches the AsyncAPI example verbatim: price strings with 4 decimal
    // digits ("0.0800") for a value that's really a whole number of cents.
    const auto raw = std::string_view(R"({
        "type":"orderbook_snapshot","sid":2,"seq":1,
        "msg":{
            "market_ticker":"FED-23DEC-T3.00","market_id":"abc-123",
            "yes_dollars_fp":[["0.0800","300.00"],["0.2200","333.00"]],
            "no_dollars_fp":[["0.4000","50.00"]]
        }
    })");
    FeedMsg fm;
    std::string err;
    PX_CHECK(parse_feed_message(raw, fm, err));
    PX_CHECK(fm.kind == FeedMsgKind::Snapshot);
    PX_CHECK(fm.market_ticker == "FED-23DEC-T3.00");
    PX_CHECK(fm.has_seq && fm.seq == 1);
    PX_CHECK(fm.snapshot_levels.size() == 3);

    // yes_dollars_fp levels are already YES-relative bids: (Buy, 8, 300), (Buy, 22, 333).
    bool found_8 = false, found_22 = false, found_no_as_60_ask = false;
    for (const auto& lvl : fm.snapshot_levels) {
        if (lvl.side == Side::Buy && lvl.price == 8 && lvl.qty == 300) found_8 = true;
        if (lvl.side == Side::Buy && lvl.price == 22 && lvl.qty == 333) found_22 = true;
        // A NO bid at 40 cents normalizes to a YES ask (Sell) at 100-40=60.
        if (lvl.side == Side::Sell && lvl.price == 60 && lvl.qty == 50) found_no_as_60_ask = true;
    }
    PX_CHECK(found_8);
    PX_CHECK(found_22);
    PX_CHECK(found_no_as_60_ask);
}

void test_snapshot_empty_side_absent() {
    // A side with no resting interest is documented optional — absent, not an
    // empty array. Must not be treated as a parse failure.
    const auto raw = std::string_view(
        R"({"type":"orderbook_snapshot","sid":2,"seq":5,)"
        R"("msg":{"market_ticker":"X","market_id":"m","yes_dollars_fp":[["0.5000","10.00"]]}})");
    FeedMsg fm;
    std::string err;
    PX_CHECK(parse_feed_message(raw, fm, err));
    PX_CHECK(fm.kind == FeedMsgKind::Snapshot);
    PX_CHECK(fm.snapshot_levels.size() == 1);
}

void test_delta_yes_side() {
    const auto raw = std::string_view(
        R"({"type":"orderbook_delta","sid":2,"seq":42,)"
        R"("msg":{"market_ticker":"X","market_id":"m","price_dollars":"0.6200","delta_fp":"-5.00","side":"yes"}})");
    FeedMsg fm;
    std::string err;
    PX_CHECK(parse_feed_message(raw, fm, err));
    PX_CHECK(fm.kind == FeedMsgKind::Delta);
    PX_CHECK(fm.has_seq && fm.seq == 42);
    PX_CHECK(fm.delta_side == Side::Buy);   // yes side, unchanged by normalize
    PX_CHECK(fm.delta_price == 62);
    PX_CHECK(fm.delta_qty == -5);
}

void test_delta_no_side_normalizes() {
    // A NO-side delta at 40 cents is a YES ask (Sell) at 60 cents — same
    // transform live order entry uses (normalize.hpp).
    const auto raw = std::string_view(
        R"({"type":"orderbook_delta","sid":2,"seq":43,)"
        R"("msg":{"market_ticker":"X","market_id":"m","price_dollars":"0.4000","delta_fp":"12.00","side":"no"}})");
    FeedMsg fm;
    std::string err;
    PX_CHECK(parse_feed_message(raw, fm, err));
    PX_CHECK(fm.delta_side == Side::Sell);
    PX_CHECK(fm.delta_price == 60);
    PX_CHECK(fm.delta_qty == 12);
}

void test_ticker() {
    const auto raw = std::string_view(
        R"({"type":"ticker","sid":3,)"
        R"("msg":{"market_ticker":"X","market_id":"m","price_dollars":"0.5000",)"
        R"("yes_bid_dollars":"0.4900","yes_ask_dollars":"0.5100","yes_bid_size_fp":"10.00",)"
        R"("yes_ask_size_fp":"10.00","last_trade_size_fp":"1.00","volume_fp":"100.00",)"
        R"("open_interest_fp":"50.00","dollar_volume":5000,"dollar_open_interest":2500,)"
        R"("ts_ms":1712345678901,"time":"2024-04-05T12:34:38Z"}})");
    FeedMsg fm;
    std::string err;
    PX_CHECK(parse_feed_message(raw, fm, err));
    PX_CHECK(fm.kind == FeedMsgKind::Ticker);
    PX_CHECK(fm.ticker_yes_bid == 49);
    PX_CHECK(fm.ticker_yes_ask == 51);
}

void test_unknown_type_not_an_error() {
    // "trade" used to be this test's example of a genuinely-unhandled type —
    // Stage 3 decodes it now (see test_trade_* below), so this uses "fill"
    // instead, which remains unhandled (private channel, out of scope — see
    // kalshi_feed.cpp).
    const auto raw = std::string_view(R"({"type":"fill","sid":4,"msg":{"market_ticker":"X"}})");
    FeedMsg fm;
    std::string err;
    PX_CHECK(parse_feed_message(raw, fm, err)); // must still return true
    PX_CHECK(fm.kind == FeedMsgKind::Unknown);
}

void test_trade_ask_side_is_buy() {
    const auto raw = std::string_view(
        R"({"type":"trade","sid":5,)"
        R"("msg":{"trade_id":"550e8400-e29b-41d4-a716-446655440000","market_ticker":"X",)"
        R"("yes_price_dollars":"0.6200","no_price_dollars":"0.3800","count_fp":"5.00",)"
        R"("taker_side":"yes","taker_outcome_side":"yes","taker_book_side":"ask",)"
        R"("is_block_trade":false,"ts":1712345678,"ts_ms":1712345678901}})");
    FeedMsg fm;
    std::string err;
    PX_CHECK(parse_feed_message(raw, fm, err));
    PX_CHECK(fm.kind == FeedMsgKind::Trade);
    PX_CHECK(fm.market_ticker == "X");
    PX_CHECK(fm.trade_side == Side::Buy); // crossed the ask -> taker bought
    PX_CHECK(fm.trade_price == 62);
    PX_CHECK(fm.trade_qty == 5);
    PX_CHECK(fm.trade_ts_ms == 1712345678901);
}

void test_trade_bid_side_is_sell() {
    const auto raw = std::string_view(
        R"({"type":"trade","sid":5,)"
        R"("msg":{"trade_id":"uuid-2","market_ticker":"X",)"
        R"("yes_price_dollars":"0.4500","no_price_dollars":"0.5500","count_fp":"12.00",)"
        R"("taker_side":"yes","taker_outcome_side":"yes","taker_book_side":"bid",)"
        R"("is_block_trade":false,"ts":1712345679,"ts_ms":1712345679001}})");
    FeedMsg fm;
    std::string err;
    PX_CHECK(parse_feed_message(raw, fm, err));
    PX_CHECK(fm.trade_side == Side::Sell); // crossed the bid -> taker sold
    PX_CHECK(fm.trade_price == 45);
    PX_CHECK(fm.trade_qty == 12);
}

void test_trade_missing_field_is_an_error() {
    // taker_book_side missing.
    const auto raw = std::string_view(
        R"({"type":"trade","sid":5,)"
        R"("msg":{"trade_id":"uuid-3","market_ticker":"X","yes_price_dollars":"0.5000",)"
        R"("count_fp":"1.00","ts_ms":1}})");
    FeedMsg fm;
    std::string err;
    PX_CHECK(!parse_feed_message(raw, fm, err));
    PX_CHECK(!err.empty());
}

void test_missing_type_is_an_error() {
    const auto raw = std::string_view(R"({"sid":4,"msg":{"market_ticker":"X"}})");
    FeedMsg fm;
    std::string err;
    PX_CHECK(!parse_feed_message(raw, fm, err));
    PX_CHECK(!err.empty());
}

void test_delta_missing_field_is_an_error() {
    // side is missing.
    const auto raw = std::string_view(
        R"({"type":"orderbook_delta","sid":2,"seq":1,)"
        R"("msg":{"market_ticker":"X","price_dollars":"0.5000","delta_fp":"1.00"}})");
    FeedMsg fm;
    std::string err;
    PX_CHECK(!parse_feed_message(raw, fm, err));
}

void test_price_high_precision_rounds_exactly() {
    // parse_decimal_scaled must not silently truncate a real (non-zero) digit
    // beyond the 2-cent scale — round instead. "0.0855" -> round to 9 cents,
    // not truncate to 8.
    const auto raw = std::string_view(
        R"({"type":"orderbook_delta","sid":2,"seq":1,)"
        R"("msg":{"market_ticker":"X","price_dollars":"0.0855","delta_fp":"1.00","side":"yes"}})");
    FeedMsg fm;
    std::string err;
    PX_CHECK(parse_feed_message(raw, fm, err));
    PX_CHECK(fm.delta_price == 9);
}

// ---- SeqTracker (pure, no I/O) ---------------------------------------------

void test_seq_tracker_normal_sequence() {
    SeqTracker t;
    PX_CHECK(t.observe(/*is_snapshot=*/true, /*has_seq=*/true, 10));
    PX_CHECK(t.observe(false, true, 11));
    PX_CHECK(t.observe(false, true, 12));
}

void test_seq_tracker_detects_gap() {
    SeqTracker t;
    PX_CHECK(t.observe(true, true, 10));
    PX_CHECK(t.observe(false, true, 11));
    PX_CHECK(!t.observe(false, true, 13)); // gap: expected 12, got 13
}

void test_seq_tracker_resets_after_gap_until_next_snapshot() {
    SeqTracker t;
    PX_CHECK(t.observe(true, true, 10));
    PX_CHECK(!t.observe(false, true, 13)); // gap detected, tracker resets
    // Next delta arrives with no established baseline — nothing to compare
    // against yet, so it's not flagged as a second gap.
    PX_CHECK(t.observe(false, true, 14));
    // A fresh snapshot re-establishes the baseline regardless of its own seq value.
    PX_CHECK(t.observe(true, true, 99));
    PX_CHECK(t.observe(false, true, 100));
    PX_CHECK(!t.observe(false, true, 102)); // gap again: expected 101
}

void test_seq_tracker_ignores_messages_without_seq() {
    SeqTracker t;
    PX_CHECK(t.observe(true, true, 10));
    PX_CHECK(t.observe(false, false, 0)); // has_seq=false — nothing to check
    PX_CHECK(t.observe(false, true, 11)); // baseline unaffected by the no-seq message
}

} // namespace

int main() {
    test_subscribed();
    test_snapshot_basic();
    test_snapshot_empty_side_absent();
    test_delta_yes_side();
    test_delta_no_side_normalizes();
    test_ticker();
    test_unknown_type_not_an_error();
    test_trade_ask_side_is_buy();
    test_trade_bid_side_is_sell();
    test_trade_missing_field_is_an_error();
    test_missing_type_is_an_error();
    test_delta_missing_field_is_an_error();
    test_price_high_precision_rounds_exactly();
    test_seq_tracker_normal_sequence();
    test_seq_tracker_detects_gap();
    test_seq_tracker_resets_after_gap_until_next_snapshot();
    test_seq_tracker_ignores_messages_without_seq();
    PX_TEST_REPORT_AND_RETURN();
}
