// tests/test_engine.cpp — hand-checked matching scenarios. Every command is
// followed by a full check_invariants() pass and a sequence-continuity check.
#include "px/matching_engine.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <memory>
#include <vector>

using namespace px;

namespace {

constexpr Price kTick = 100; // $0.01
constexpr Price usd(double v) { return static_cast<Price>(v * static_cast<double>(kPriceScale) + 0.5); }

struct Harness {
    std::unique_ptr<MatchingEngine> eng;
    SeqNum expected_seq = 1;

    explicit Harness(const EngineConfig& cfg = {}, int instruments = 1) : eng(std::make_unique<MatchingEngine>(cfg)) {
        for (int i = 0; i < instruments; ++i) {
            char sym[16];
            std::snprintf(sym, sizeof sym, "SYM%d", i);
            eng->add_instrument(sym, kTick);
        }
    }

    std::vector<ExecEvent> run(const OrderRequest& r) {
        std::vector<ExecEvent> out;
        eng->process(r, EventSink(&out, [](void* c, const ExecEvent& e) {
                         static_cast<std::vector<ExecEvent>*>(c)->push_back(e);
                     }));
        const char* why = nullptr;
        const bool ok = eng->check_invariants(&why);
        PX_CHECK(ok);
        if (!ok) std::printf("  invariant violated: %s\n", why);
        for (const auto& e : out) {
            PX_CHECK(e.seq == expected_seq);
            ++expected_seq;
        }
        return out;
    }

    [[nodiscard]] bool has_best(Side s) const { return eng->depth(0, s) > 0; }
    [[nodiscard]] LevelView best(Side s, InstrumentId i = 0) const { return eng->level(i, s, 0); }
};

bool is(const ExecEvent& e, EventType t) { return e.type == t; }
bool is_trade(const ExecEvent& e, OrderId taker, OrderId maker, Price px, Qty q) {
    return e.type == EventType::Trade && e.order_id == taker && e.maker_id == maker && e.price == px && e.qty == q;
}
bool is_cancel(const ExecEvent& e, OrderId id, CancelReason why, Qty q) {
    return e.type == EventType::Cancelled && e.order_id == id && e.reason == static_cast<std::uint8_t>(why) && e.qty == q;
}
bool is_reject(const ExecEvent& e, OrderId id, RejectReason why) {
    return e.type == EventType::Rejected && e.order_id == id && e.reason == static_cast<std::uint8_t>(why);
}

void test_price_time_priority() {
    Harness h;
    h.run(limit_order(1, 0, 1, Side::Sell, usd(10.00), 100));
    h.run(limit_order(2, 0, 2, Side::Sell, usd(10.00), 50));
    h.run(limit_order(3, 0, 3, Side::Sell, usd(9.99), 70));
    PX_CHECK(h.best(Side::Sell).price == usd(9.99));

    const auto ev = h.run(limit_order(4, 0, 4, Side::Buy, usd(10.00), 200));
    PX_CHECK(ev.size() == 4);
    if (ev.size() == 4) {
        PX_CHECK(is(ev[0], EventType::Ack) && ev[0].leaves == 200);
        PX_CHECK(is_trade(ev[1], 4, 3, usd(9.99), 70));   // better price first
        PX_CHECK(is_trade(ev[2], 4, 1, usd(10.00), 100)); // then time priority at 10.00
        PX_CHECK(is_trade(ev[3], 4, 2, usd(10.00), 30));
        PX_CHECK(ev[3].leaves == 0 && ev[3].maker_leaves == 20);
    }
    PX_CHECK(h.eng->depth(0, Side::Sell) == 1 && h.best(Side::Sell).qty == 20);
    PX_CHECK(!h.has_best(Side::Buy)); // fully filled, nothing rested
    PX_CHECK(h.eng->resting_orders() == 1);
}

void test_partial_fill_keeps_queue_position() {
    Harness h;
    h.run(limit_order(1, 0, 1, Side::Sell, usd(5.00), 100));
    h.run(limit_order(2, 0, 2, Side::Sell, usd(5.00), 100));
    auto ev = h.run(limit_order(3, 0, 3, Side::Buy, usd(5.00), 30, Tif::IOC));
    PX_CHECK(ev.size() == 2 && is_trade(ev[1], 3, 1, usd(5.00), 30) && ev[1].maker_leaves == 70);
    ev = h.run(limit_order(4, 0, 4, Side::Buy, usd(5.00), 80, Tif::IOC));
    PX_CHECK(ev.size() == 3);
    if (ev.size() == 3) {
        PX_CHECK(is_trade(ev[1], 4, 1, usd(5.00), 70)); // order 1 is still first in line
        PX_CHECK(is_trade(ev[2], 4, 2, usd(5.00), 10));
    }
    PX_CHECK(h.best(Side::Sell).qty == 90 && h.best(Side::Sell).orders == 1);
}

void test_market_orders() {
    Harness h;
    auto ev = h.run(market_order(1, 0, 1, Side::Buy, 10));
    PX_CHECK(ev.size() == 2 && is(ev[0], EventType::Ack) && ev[0].price == 0);
    PX_CHECK(ev.size() == 2 && is_cancel(ev[1], 1, CancelReason::IocRemainder, 10)); // empty book

    h.run(limit_order(2, 0, 2, Side::Buy, usd(1.00), 10));
    h.run(limit_order(3, 0, 3, Side::Buy, usd(0.50), 10));
    h.run(limit_order(4, 0, 4, Side::Buy, usd(0.01), 10));
    ev = h.run(market_order(5, 0, 5, Side::Sell, 25)); // ignores price: sweeps best-first
    PX_CHECK(ev.size() == 4);
    if (ev.size() == 4) {
        PX_CHECK(is_trade(ev[1], 5, 2, usd(1.00), 10));
        PX_CHECK(is_trade(ev[2], 5, 3, usd(0.50), 10));
        PX_CHECK(is_trade(ev[3], 5, 4, usd(0.01), 5) && ev[3].leaves == 0);
    }
    PX_CHECK(h.eng->depth(0, Side::Buy) == 1 && h.best(Side::Buy).qty == 5);
    PX_CHECK(!h.has_best(Side::Sell)); // a market remainder never rests
}

void test_ioc_and_gtc_remainders() {
    Harness h;
    h.run(limit_order(1, 0, 1, Side::Sell, usd(20.00), 10));
    auto ev = h.run(limit_order(2, 0, 2, Side::Buy, usd(20.00), 25, Tif::IOC));
    PX_CHECK(ev.size() == 3 && is_cancel(ev[2], 2, CancelReason::IocRemainder, 15));
    PX_CHECK(!h.has_best(Side::Buy));

    h.run(limit_order(3, 0, 3, Side::Sell, usd(20.00), 10));
    ev = h.run(limit_order(4, 0, 4, Side::Buy, usd(20.00), 25)); // GTC: remainder rests silently
    PX_CHECK(ev.size() == 2 && ev.back().leaves == 15);
    PX_CHECK(h.has_best(Side::Buy) && h.best(Side::Buy).price == usd(20.00) && h.best(Side::Buy).qty == 15);

    ev = h.run(limit_order(5, 0, 5, Side::Buy, usd(19.00), 5, Tif::IOC)); // nothing to hit
    PX_CHECK(ev.size() == 2 && is_cancel(ev[1], 5, CancelReason::IocRemainder, 5));
}

void test_cancel() {
    Harness h;
    h.run(limit_order(1, 0, 7, Side::Buy, usd(3.00), 40));
    h.run(limit_order(2, 0, 8, Side::Buy, usd(3.00), 60));
    PX_CHECK(h.best(Side::Buy).orders == 2);

    auto ev = h.run(cancel_order(1, 0, 99)); // wrong owner
    PX_CHECK(ev.size() == 1 && is_reject(ev[0], 1, RejectReason::UnknownOrder));
    ev = h.run(cancel_order(1, 0, 7));
    PX_CHECK(ev.size() == 1 && is_cancel(ev[0], 1, CancelReason::User, 40) && ev[0].price == usd(3.00));
    PX_CHECK(h.best(Side::Buy).orders == 1 && h.best(Side::Buy).qty == 60);
    ev = h.run(cancel_order(1, 0, 7)); // already gone
    PX_CHECK(ev.size() == 1 && is_reject(ev[0], 1, RejectReason::UnknownOrder));
    ev = h.run(cancel_order(12345, 0, 7)); // never existed
    PX_CHECK(ev.size() == 1 && is_reject(ev[0], 12345, RejectReason::UnknownOrder));

    h.run(cancel_order(2, 0, 8)); // last order: the level must disappear
    PX_CHECK(!h.has_best(Side::Buy) && h.eng->live_levels() == 0 && h.eng->resting_orders() == 0);

    // A partially-filled order cancels its remaining quantity only.
    h.run(limit_order(3, 0, 1, Side::Sell, usd(3.00), 50));
    h.run(limit_order(4, 0, 2, Side::Buy, usd(3.00), 20, Tif::IOC));
    ev = h.run(cancel_order(3, 0, 1));
    PX_CHECK(ev.size() == 1 && is_cancel(ev[0], 3, CancelReason::User, 30));
}

void test_duplicate_id_rejected_before_matching() {
    Harness h;
    h.run(limit_order(1, 0, 1, Side::Sell, usd(1.00), 10));
    // Same id, marketable: must be rejected WITHOUT trading.
    const auto ev = h.run(limit_order(1, 0, 2, Side::Buy, usd(1.00), 10, Tif::IOC));
    PX_CHECK(ev.size() == 1 && is_reject(ev[0], 1, RejectReason::DuplicateOrderId));
    PX_CHECK(h.best(Side::Sell).qty == 10);
    // Once the id is no longer live it may be reused.
    h.run(cancel_order(1, 0, 1));
    const auto ev2 = h.run(limit_order(1, 0, 2, Side::Buy, usd(1.00), 10));
    PX_CHECK(ev2.size() == 1 && is(ev2[0], EventType::Ack));
}

void test_validation() {
    Harness h;
    const auto one = [&](const OrderRequest& r, RejectReason why) {
        const auto ev = h.run(r);
        PX_CHECK(ev.size() == 1 && is_reject(ev[0], r.order_id, why));
    };
    one(limit_order(1, 5, 1, Side::Buy, usd(1.00), 10), RejectReason::UnknownInstrument);
    one(limit_order(2, 0, 1, Side::Buy, usd(1.00), 0), RejectReason::InvalidQty);
    one(limit_order(3, 0, 1, Side::Buy, 0, 10), RejectReason::InvalidPrice);
    one(limit_order(4, 0, 1, Side::Buy, -kTick, 10), RejectReason::InvalidPrice);
    one(limit_order(5, 0, 1, Side::Buy, usd(1.00) + 50, 10), RejectReason::InvalidPrice); // off tick
    one(limit_order(6, 0, 1, Side::Buy, kMaxPrice + kTick, 10), RejectReason::InvalidPrice);
    one(limit_order(kInvalidOrderId, 0, 1, Side::Buy, usd(1.00), 10), RejectReason::InvalidOrderId);
    PX_CHECK(h.eng->resting_orders() == 0);
    // A market order's price field is ignored, whatever it holds.
    OrderRequest m = market_order(7, 0, 1, Side::Buy, 10);
    m.price = -12345;
    const auto ev = h.run(m);
    PX_CHECK(ev.size() == 2 && is(ev[0], EventType::Ack));
}

void test_book_full() {
    EngineConfig cfg;
    cfg.max_orders = 3;
    cfg.max_levels = 3;
    cfg.levels_per_side = 2;
    Harness h(cfg);
    h.run(limit_order(1, 0, 1, Side::Buy, usd(1.00), 10));
    h.run(limit_order(2, 0, 1, Side::Buy, usd(1.01), 10));
    // Third distinct bid price: the side is full.
    auto ev = h.run(limit_order(3, 0, 1, Side::Buy, usd(1.02), 10));
    PX_CHECK(ev.size() == 1 && is_reject(ev[0], 3, RejectReason::BookFull));
    // Same price as an existing level needs no new level: accepted.
    ev = h.run(limit_order(4, 0, 1, Side::Buy, usd(1.01), 10));
    PX_CHECK(ev.size() == 1 && is(ev[0], EventType::Ack));
    // Order pool is now full (3 resting): any GTC is refused up front, even one
    // that would have filled completely — admission is conservative by design.
    ev = h.run(limit_order(5, 0, 2, Side::Sell, usd(1.01), 5));
    PX_CHECK(ev.size() == 1 && is_reject(ev[0], 5, RejectReason::BookFull));
    // ...while an IOC never needs a slot and still trades.
    ev = h.run(limit_order(6, 0, 2, Side::Sell, usd(1.01), 5, Tif::IOC));
    PX_CHECK(ev.size() == 2 && is_trade(ev[1], 6, 2, usd(1.01), 5));
}

void test_self_trade_prevention() {
    Harness h;
    h.run(limit_order(1, 0, 1, Side::Sell, usd(10.00), 50)); // owner 1
    h.run(limit_order(2, 0, 2, Side::Sell, usd(10.00), 50)); // owner 2, behind it
    const auto ev = h.run(limit_order(3, 0, 1, Side::Buy, usd(10.00), 80)); // owner 1 buys
    PX_CHECK(ev.size() == 3);
    if (ev.size() == 3) {
        PX_CHECK(is_cancel(ev[1], 1, CancelReason::SelfTrade, 50) && ev[1].side == Side::Sell);
        PX_CHECK(is_trade(ev[2], 3, 2, usd(10.00), 50) && ev[2].leaves == 30);
    }
    PX_CHECK(!h.has_best(Side::Sell));
    PX_CHECK(h.best(Side::Buy).price == usd(10.00) && h.best(Side::Buy).qty == 30);
}

void test_instrument_isolation() {
    Harness h(EngineConfig{}, 2);
    h.run(limit_order(1, 0, 1, Side::Sell, usd(1.00), 10));
    const auto ev = h.run(limit_order(2, 1, 2, Side::Buy, usd(1.00), 10)); // other instrument
    PX_CHECK(ev.size() == 1 && is(ev[0], EventType::Ack));
    PX_CHECK(h.eng->depth(0, Side::Sell) == 1 && h.eng->depth(1, Side::Buy) == 1);
    PX_CHECK(h.eng->depth(0, Side::Buy) == 0 && h.eng->depth(1, Side::Sell) == 0);
    // Order ids are exchange-wide: a cancel routes to the right book by id alone.
    const auto c = h.run(cancel_order(2, 0, 2));
    PX_CHECK(c.size() == 1 && is_cancel(c[0], 2, CancelReason::User, 10) && c[0].instr == 1);
}

void test_deep_book_ordering() {
    Harness h;
    // Insert bids in scrambled price order; they must come out best-first.
    const int prices[] = {50, 10, 90, 30, 70, 20, 80, 40, 60, 100};
    OrderId id = 1;
    for (const int p : prices) h.run(limit_order(id++, 0, 1, Side::Buy, p * kTick, 1));
    PX_CHECK(h.eng->depth(0, Side::Buy) == 10);
    bool descending = true;
    for (std::uint32_t r = 0; r + 1 < 10; ++r) {
        descending &= h.eng->level(0, Side::Buy, r).price > h.eng->level(0, Side::Buy, r + 1).price;
    }
    PX_CHECK(descending && h.best(Side::Buy).price == 100 * kTick);
    // Cancel from the middle, the worst and the best.
    h.run(cancel_order(4, 0, 1)); // 30
    h.run(cancel_order(2, 0, 1)); // 10 (worst)
    h.run(cancel_order(10, 0, 1)); // 100 (best)
    PX_CHECK(h.eng->depth(0, Side::Buy) == 7 && h.best(Side::Buy).price == 90 * kTick);
}

} // namespace

int main() {
    test_price_time_priority();
    test_partial_fill_keeps_queue_position();
    test_market_orders();
    test_ioc_and_gtc_remainders();
    test_cancel();
    test_duplicate_id_rejected_before_matching();
    test_validation();
    test_book_full();
    test_self_trade_prevention();
    test_instrument_isolation();
    test_deep_book_ordering();
    PX_TEST_REPORT_AND_RETURN();
}
