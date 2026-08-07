// tests/test_book.cpp — targeted unit tests for OrderBook mechanics that a
// random fuzz run might hit rarely (or not at all): specific priority orderings,
// boundary prices, and the exact post-condition of a cancel. fuzz_vs_reference.cpp
// covers breadth against an oracle; this covers named, deliberate scenarios.
#include "px/order_book.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <memory>
#include <vector>

using namespace px;

namespace {

// OrderBook is ~5.7 MB by design (see order_book.hpp SIZE WARNING) — always
// heap-allocate it, never declare one as a stack local.
std::unique_ptr<OrderBook> make_book(MarketId id = 0) {
    return std::make_unique<OrderBook>(id);
}

struct Collector {
    std::vector<Trade> trades;
    static void sink_fn(void* ctx, const Trade& t) { static_cast<Collector*>(ctx)->trades.push_back(t); }
    TradeSink sink() { return TradeSink(this, &Collector::sink_fn); }
};

void test_price_time_priority() {
    auto book = make_book();
    Collector c;

    // Two resting sells at the same price from different traders/times; the
    // earlier one (trader 1) must fill first, in full, before trader 2's does.
    auto r1 = book->submit(1, /*trader*/ 1, Contract::Yes, Side::Sell, OrderType::Limit,
                            TimeInForce::GTC, 60, 5, 100, c.sink());
    auto r2 = book->submit(2, /*trader*/ 2, Contract::Yes, Side::Sell, OrderType::Limit,
                            TimeInForce::GTC, 60, 5, 200, c.sink());
    PX_CHECK(r1.rested_qty == 5 && r2.rested_qty == 5);
    PX_CHECK(book->level(60).total_qty == 10);

    // A buyer takes 7 — must consume all 5 from trader 1 first, then 2 from trader 2.
    c.trades.clear();
    auto r3 = book->submit(3, /*trader*/ 9, Contract::Yes, Side::Buy, OrderType::Limit,
                            TimeInForce::GTC, 60, 7, 300, c.sink());
    PX_CHECK(r3.filled_qty == 7);
    PX_CHECK(c.trades.size() == 2);
    PX_CHECK(c.trades[0].maker_id == 1 && c.trades[0].qty == 5);
    PX_CHECK(c.trades[1].maker_id == 2 && c.trades[1].qty == 2);
    PX_CHECK(book->level(60).total_qty == 3); // 3 left resting from trader 2
}

void test_partial_fill_retains_queue_position() {
    auto book = make_book();
    Collector c;

    auto ra = book->submit(1, 1, Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, 50, 10, 100, c.sink());
    PX_CHECK(ra.rested_qty == 10);

    // Partially fill trader 1's order (take 4 of 10) — it stays resting with 6 left.
    c.trades.clear();
    auto rb = book->submit(2, 8, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 50, 4, 200, c.sink());
    PX_CHECK(rb.filled_qty == 4);
    PX_CHECK(book->level(50).total_qty == 6);
    const Order* resting = book->find_order(1);
    PX_CHECK(resting != nullptr && resting->qty == 6 && resting->status == OrderStatus::PartiallyFilled);

    // A new order from trader 3 now rests behind it at the same price.
    auto rc = book->submit(3, 3, Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, 50, 5, 300, c.sink());
    PX_CHECK(rc.rested_qty == 5);

    // A buyer taking 9 must drain the REMAINDER of order 1 (6) before touching
    // order 3's 5 — partial fill did not send it to the back of the queue.
    c.trades.clear();
    auto rd = book->submit(4, 9, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 50, 9, 400, c.sink());
    PX_CHECK(rd.filled_qty == 9);
    PX_CHECK(c.trades.size() == 2);
    PX_CHECK(c.trades[0].maker_id == 1 && c.trades[0].qty == 6);
    PX_CHECK(c.trades[1].maker_id == 3 && c.trades[1].qty == 3);
}

void test_market_sweep_and_ioc_remainder() {
    auto book = make_book();
    Collector c;

    book->submit(1, 1, Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, 50, 5, 100, c.sink());
    book->submit(2, 2, Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, 51, 5, 200, c.sink());

    // MARKET BUY for 12 — only 10 available across both levels; remainder (2)
    // must NOT rest (Market implies IOC), even though qty is a valid positive value.
    c.trades.clear();
    auto r = book->submit(3, 9, Contract::Yes, Side::Buy, OrderType::Market, TimeInForce::GTC, /*price ignored*/ 0, 12, 300, c.sink());
    PX_CHECK(r.filled_qty == 10);
    PX_CHECK(r.rested_qty == 0); // dropped, not rested, despite tif being requested as GTC
    PX_CHECK(c.trades.size() == 2);
    PX_CHECK(c.trades[0].price == 50 && c.trades[1].price == 51);

    Price bb, ba;
    PX_CHECK(!book->best_bid(bb));
    PX_CHECK(!book->best_ask(ba)); // book fully drained
    PX_CHECK(book->find_order(3) == nullptr); // the market order itself never rested
}

void test_cancel_of_partial_fill() {
    auto book = make_book();
    Collector c;

    book->submit(1, 1, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 40, 10, 100, c.sink());
    auto r = book->submit(2, 2, Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, 40, 3, 200, c.sink());
    PX_CHECK(r.filled_qty == 3);
    PX_CHECK(book->level(40).total_qty == 7);

    const RejectReason c1 = book->cancel(1);
    PX_CHECK(c1 == RejectReason::None);
    PX_CHECK(book->find_order(1) == nullptr);
    PX_CHECK(book->level(40).total_qty == 0);
    Price bb;
    PX_CHECK(!book->best_bid(bb));

    // Cancelling again (or a never-existed id) is a clean reject, not a crash.
    PX_CHECK(book->cancel(1) == RejectReason::UnknownOrder);
    PX_CHECK(book->cancel(9999) == RejectReason::UnknownOrder);
}

void test_self_trade_prevention() {
    auto book = make_book();
    Collector c;

    book->submit(1, 7, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 55, 4, 100, c.sink());
    // Same trader (7) submits an opposing, crossing order — the resting order
    // must be cancelled, not traded against, and the incoming order rests instead.
    c.trades.clear();
    auto r = book->submit(2, 7, Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, 55, 4, 200, c.sink());
    PX_CHECK(c.trades.empty());
    PX_CHECK(r.filled_qty == 0);
    PX_CHECK(r.rested_qty == 4);
    PX_CHECK(book->find_order(1) == nullptr); // cancelled, not filled
    PX_CHECK(book->find_order(2) != nullptr);
}

void test_tick_bounds() {
    auto book = make_book();
    Collector c;

    auto below = book->submit(1, 1, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 0, 1, 100, c.sink());
    PX_CHECK(below.reject == RejectReason::InvalidPrice);
    auto above = book->submit(2, 1, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 100, 1, 100, c.sink());
    PX_CHECK(above.reject == RejectReason::InvalidPrice);

    auto lo = book->submit(3, 1, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, kMinPrice, 1, 100, c.sink());
    PX_CHECK(lo.reject == RejectReason::None && lo.rested_qty == 1);
    auto hi = book->submit(4, 2, Contract::Yes, Side::Sell, OrderType::Limit, TimeInForce::GTC, kMaxPrice, 1, 100, c.sink());
    PX_CHECK(hi.reject == RejectReason::None && hi.rested_qty == 1);

    auto zero_qty = book->submit(5, 1, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 50, 0, 100, c.sink());
    PX_CHECK(zero_qty.reject == RejectReason::InvalidQty);
}

void test_normalize_round_trip() {
    for (Price p = kMinPrice; p <= kMaxPrice; ++p) {
        for (Side s : {Side::Buy, Side::Sell}) {
            for (Contract ctr : {Contract::Yes, Contract::No}) {
                const auto [nside, nprice] = normalize(ctr, s, p);
                const Side back = denormalize_side(ctr, nside);
                PX_CHECK(back == s);
                if (ctr == Contract::Yes) {
                    PX_CHECK(nprice == p);
                    PX_CHECK(nside == s);
                } else {
                    PX_CHECK(nprice == 100 - p);
                    PX_CHECK(nside == opposite(s));
                }
            }
        }
    }
    // The economically meaningful identity: BUY NO @ p behaves exactly like
    // SELL YES @ (100-p) all the way through the book, not just at the formula
    // level — checked end-to-end here.
    auto book = make_book();
    Collector c;
    auto resting = book->submit(1, 1, Contract::Yes, Side::Buy, OrderType::Limit, TimeInForce::GTC, 70, 5, 100, c.sink());
    PX_CHECK(resting.rested_qty == 5); // bid at 70

    // BUY NO @ 70 == SELL YES @ 30 -> should fully cross the resting bid at 70.
    c.trades.clear();
    auto crossing = book->submit(2, 2, Contract::No, Side::Buy, OrderType::Limit, TimeInForce::GTC, 70, 5, 200, c.sink());
    PX_CHECK(crossing.filled_qty == 5);
    PX_CHECK(c.trades.size() == 1 && c.trades[0].price == 70);
}

} // namespace

int main() {
    test_price_time_priority();
    test_partial_fill_retains_queue_position();
    test_market_sweep_and_ioc_remainder();
    test_cancel_of_partial_fill();
    test_self_trade_prevention();
    test_tick_bounds();
    test_normalize_round_trip();
    PX_TEST_REPORT_AND_RETURN();
}
