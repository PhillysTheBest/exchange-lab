// tests/fuzz_vs_reference.cpp — differential fuzz test.
//
// Feeds an identical random stream of submit/cancel commands into the fast
// array+bitmask OrderBook and the deliberately naive ReferenceBook (see
// reference_book.hpp), and asserts every observable — fill/rest quantities,
// reject reasons, the exact trade sequence, and best bid/ask — matches after
// every single command. This is how an optimized matcher earns trust: not by
// inspection, but by disagreeing with a boringly-correct oracle zero times
// across tens of thousands of randomized orders.
#include "px/order_book.hpp"
#include "reference_book.hpp"

#include <cstdio>
#include <memory>
#include <random>
#include <vector>

using namespace px;

namespace {

struct Collector {
    std::vector<Trade> trades;
    static void sink_fn(void* ctx, const Trade& t) {
        static_cast<Collector*>(ctx)->trades.push_back(t);
    }
    [[nodiscard]] TradeSink sink() { return TradeSink(this, &Collector::sink_fn); }
};

bool trades_equal(const Trade& a, const Trade& b) {
    return a.market_id == b.market_id
        && a.taker_order_id == b.taker_order_id
        && a.maker_order_id == b.maker_order_id
        && a.taker_id == b.taker_id
        && a.maker_id == b.maker_id
        && a.price == b.price
        && a.qty == b.qty
        && a.taker_side == b.taker_side;
}

} // namespace

int main() {
    constexpr MarketId kMarket = 0;
    auto fast_ptr = std::make_unique<OrderBook>(kMarket); // see order_book.hpp SIZE WARNING
    OrderBook& fast = *fast_ptr;
    test::ReferenceBook ref(kMarket);

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> trader_dist(0, 5);
    std::uniform_int_distribution<int> price_dist(kMinPrice, kMaxPrice);
    std::uniform_int_distribution<int> qty_dist(1, 20);
    std::uniform_int_distribution<int> bit_dist(0, 1);
    std::uniform_int_distribution<int> type_roll(0, 9);   // 1/10: Market
    std::uniform_int_distribution<int> tif_roll(0, 9);    // 1/10: explicit IOC
    std::uniform_int_distribution<int> action_roll(0, 2); // 1/3: cancel (if any live)

    constexpr int kIterations = 20000;
    OrderId next_id = 1;
    std::vector<OrderId> live_candidates;
    int mismatches = 0;

    for (int i = 0; i < kIterations && mismatches <= 5; ++i) {
        const bool do_cancel = (action_roll(rng) == 0) && !live_candidates.empty();

        if (do_cancel) {
            std::uniform_int_distribution<std::size_t> pick(0, live_candidates.size() - 1);
            const std::size_t idx = pick(rng);
            const OrderId id = live_candidates[idx];
            live_candidates[idx] = live_candidates.back();
            live_candidates.pop_back();

            const RejectReason r1 = fast.cancel(id);
            const RejectReason r2 = ref.cancel(id);
            if (r1 != r2) {
                std::printf("MISMATCH cancel(%llu) at iter %d: fast=%d ref=%d\n",
                            static_cast<unsigned long long>(id), i, static_cast<int>(r1), static_cast<int>(r2));
                ++mismatches;
            }
            continue;
        }

        const OrderId id = next_id++;
        const TraderId trader = static_cast<TraderId>(trader_dist(rng));
        const Contract contract = bit_dist(rng) == 0 ? Contract::Yes : Contract::No;
        const Side side = bit_dist(rng) == 0 ? Side::Buy : Side::Sell;
        const OrderType type = (type_roll(rng) == 0) ? OrderType::Market : OrderType::Limit;
        const TimeInForce tif = (type == OrderType::Market)
            ? TimeInForce::IOC
            : ((tif_roll(rng) == 0) ? TimeInForce::IOC : TimeInForce::GTC);
        const Price price = static_cast<Price>(price_dist(rng));
        const Qty qty = static_cast<Qty>(qty_dist(rng));

        Collector c1, c2;
        const auto r1 = fast.submit(id, trader, contract, side, type, tif, price, qty,
                                     static_cast<Tsc>(i), c1.sink());
        const auto r2 = ref.submit(id, trader, contract, side, type, tif, price, qty, c2.sink());

        bool ok = r1.filled_qty == r2.filled_qty
               && r1.rested_qty == r2.rested_qty
               && r1.reject == r2.reject
               && c1.trades.size() == c2.trades.size();
        if (ok) {
            for (std::size_t k = 0; k < c1.trades.size(); ++k) {
                if (!trades_equal(c1.trades[k], c2.trades[k])) { ok = false; break; }
            }
        }
        if (!ok) {
            std::printf("MISMATCH submit at iter %d id=%llu: fast(filled=%u,rested=%u,reject=%d,trades=%zu) "
                        "ref(filled=%u,rested=%u,reject=%d,trades=%zu)\n",
                        i, static_cast<unsigned long long>(id),
                        r1.filled_qty, r1.rested_qty, static_cast<int>(r1.reject), c1.trades.size(),
                        r2.filled_qty, r2.rested_qty, static_cast<int>(r2.reject), c2.trades.size());
            ++mismatches;
        }

        Price fb, rb, fa, ra;
        const bool have_fb = fast.best_bid(fb), have_rb = ref.best_bid(rb);
        const bool have_fa = fast.best_ask(fa), have_ra = ref.best_ask(ra);
        if (have_fb != have_rb || (have_fb && fb != rb) || have_fa != have_ra || (have_fa && fa != ra)) {
            std::printf("MISMATCH bbo after iter %d\n", i);
            ++mismatches;
        }

#ifdef PX_DEBUG
        fast.check_invariants();
#endif

        if (r1.rested_qty > 0) live_candidates.push_back(id);
    }

    if (mismatches == 0) {
        std::printf("fuzz_vs_reference: %d iterations, 0 mismatches. PASS\n", kIterations);
        return 0;
    }
    std::printf("fuzz_vs_reference: %d mismatches. FAIL\n", mismatches);
    return 1;
}
