// tests/test_replay_fuzz.cpp — adversarial-input fuzz test for the Brain ->
// ShadowBook path.
//
// SCOPE: unlike tests/fuzz_vs_reference.cpp (which differentially fuzzes the
// offline OrderBook against a naive std::map reference), there is no separate
// "obviously correct but slow" ShadowBook implementation to diff against —
// ShadowBook IS the simple implementation (see its own file header). What
// this test differentially checks instead is that Brain::apply_book_event
// (pipeline/brain.cpp) correctly *relays* a BookEvent's fields into
// ShadowBook's own documented set_level/apply_delta/clear semantics — a small
// reference model of that same spec, written independently here, plays the
// role fuzz_vs_reference.cpp's ReferenceBook plays there. A real bug this
// shape of test would catch: brain.cpp passing the wrong field (e.g.
// `e.qty_delta` where `e.qty` was meant), which a narrower unit test
// exercising only "nice" event sequences might never happen to trigger.
//
// Events are pushed directly onto ring A, bypassing Ingress/JSON parsing
// entirely — the wire schema is already covered by tests/test_feed_parse.cpp,
// and by the time a BookEvent exists, parse_feed_message has already
// range-checked every price into [kMinPrice, kMaxPrice] (see
// live/kalshi_feed.cpp). This test fuzzes the *full* [0, kNumLevels) index
// range instead, deliberately including the two values production code never
// sends (0 and 100, ShadowBook's own reserved sentinels — see
// px/shadow_book.hpp) and biases toward reusing recently-touched prices, to
// maximize how often the same price flips from a bid to an ask (or back)
// without an intervening zero — the exact scenario ShadowBook::set_level's
// own fix (see its comment) exists to handle. If that fix ever regresses,
// ShadowBook::check_invariants() aborts the process (assert, not PX_CHECK —
// same convention fuzz_vs_reference.cpp's own check_invariants() call uses),
// so a regression here fails loudly, not silently.
#include "pipeline/brain.hpp"
#include "px/platform.hpp" // cpu_relax()
#include "test_harness.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <thread>
#include <vector>

using namespace px;
using namespace px::pipeline;

namespace {

constexpr MarketId kMarketA = 0;
constexpr MarketId kMarketB = 1;
constexpr int kIterations = 4000;

// Independently-written mirror of ShadowBook's own documented semantics
// (px/shadow_book.hpp: set_level/apply_delta/clear) — see file header for why
// this plays the role a "boringly correct" reference implementation plays in
// tests/fuzz_vs_reference.cpp.
struct RefBook {
    struct Level {
        bool occupied = false;
        Side side = Side::Buy;
        Qty qty = 0;
    };
    std::array<Level, kNumLevels> levels{};

    void set_level(Side side, Price price, Qty qty) noexcept {
        Level& lv = levels[static_cast<std::size_t>(price)];
        lv.qty = qty;
        lv.occupied = qty > 0;
        if (lv.occupied) lv.side = side;
    }
    void apply_delta(Side side, Price price, long long delta) noexcept {
        Level& lv = levels[static_cast<std::size_t>(price)];
        const long long updated = static_cast<long long>(lv.qty) + delta;
        set_level(side, price, static_cast<Qty>(updated < 0 ? 0 : updated));
    }
    void clear() noexcept { levels.fill(Level{}); }

    [[nodiscard]] Qty level_qty(Price price) const noexcept { return levels[static_cast<std::size_t>(price)].qty; }
    [[nodiscard]] bool best_bid(Price& out) const noexcept {
        for (int p = kNumLevels - 1; p >= 0; --p) {
            if (levels[static_cast<std::size_t>(p)].occupied && levels[static_cast<std::size_t>(p)].side == Side::Buy) {
                out = static_cast<Price>(p);
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool best_ask(Price& out) const noexcept {
        for (int p = 0; p < kNumLevels; ++p) {
            if (levels[static_cast<std::size_t>(p)].occupied && levels[static_cast<std::size_t>(p)].side == Side::Sell) {
                out = static_cast<Price>(p);
                return true;
            }
        }
        return false;
    }
};

void assert_matches(const RefBook& ref, const ShadowBook& real, int iter) {
    for (int p = 0; p < kNumLevels; ++p) {
        const auto pp = static_cast<Price>(p);
        PX_CHECK(ref.level_qty(pp) == real.level_qty(pp));
        if (ref.level_qty(pp) != real.level_qty(pp)) {
            std::printf("  (mismatch at iter %d, price %d: ref=%u real=%u)\n", iter, p, ref.level_qty(pp),
                        real.level_qty(pp));
        }
    }
    Price rb = 0, sb = 0, ra = 0, sa = 0;
    const bool have_rb = ref.best_bid(rb), have_sb = real.best_bid(sb);
    const bool have_ra = ref.best_ask(ra), have_sa = real.best_ask(sa);
    PX_CHECK(have_rb == have_sb && (!have_rb || rb == sb));
    PX_CHECK(have_ra == have_sa && (!have_ra || ra == sa));
#ifdef PX_DEBUG
    real.check_invariants(); // aborts on violation — see file header
#endif
}

void test_adversarial_book_events_never_desync_or_violate_invariants() {
    risk::RiskConfig risk_cfg;
    // Heap-allocated, not a stack local — see pipeline/brain.hpp's SIZE WARNING.
    auto brain = std::make_unique<Brain>(risk_cfg);

    RingA ring_a;
    RingC ring_c;             // unused — no exec reports in this test
    RingB ring_b;             // unused — no strategies registered, so nothing is ever pushed to it
    strategy::OracleRing oracle_ring; // unused — no oracle thread in this test

    std::atomic<bool> stop{false};
    std::thread brain_thread([&] { brain->run(ring_a, oracle_ring, ring_c, ring_b, stop); });

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> price_dist(0, kNumLevels - 1); // full array range, incl. sentinels
    std::uniform_int_distribution<int> qty_dist(0, 1000);
    std::uniform_int_distribution<int> delta_dist(-2000, 2000);
    std::uniform_int_distribution<int> side_bit(0, 1);
    std::uniform_int_distribution<int> market_bit(0, 1);
    std::uniform_int_distribution<int> kind_roll(0, 9); // 0-3 LevelSet, 4-6 LevelDelta, 7 SnapshotBegin, 8 Ticker, 9 Trade
    std::uniform_int_distribution<int> reuse_roll(0, 1);
    std::uniform_int_distribution<std::size_t> pick_recent(0, 31); // clamped against actual size below

    RefBook ref_a, ref_b;
    std::vector<Price> recent_prices; // biases toward re-touching a price -> more side-flip collisions
    std::int64_t next_ts_ms = 0;

    for (int i = 0; i < kIterations; ++i) {
        const bool use_b = market_bit(rng) == 1;
        const MarketId market = use_b ? kMarketB : kMarketA;
        RefBook& ref = use_b ? ref_b : ref_a;

        Price price;
        if (!recent_prices.empty() && reuse_roll(rng) == 1) {
            const std::size_t idx = pick_recent(rng) % recent_prices.size();
            price = recent_prices[idx];
        } else {
            price = static_cast<Price>(price_dist(rng));
            recent_prices.push_back(price);
            if (recent_prices.size() > 64) recent_prices.erase(recent_prices.begin());
        }
        const Side side = side_bit(rng) == 0 ? Side::Buy : Side::Sell;

        BookEvent e;
        e.market = market;
        const int kind = kind_roll(rng);
        if (kind <= 3) {
            e.kind = BookEventKind::LevelSet;
            e.side = side;
            e.price = price;
            e.qty = static_cast<Qty>(qty_dist(rng));
            ref.set_level(e.side, e.price, e.qty);
        } else if (kind <= 6) {
            e.kind = BookEventKind::LevelDelta;
            e.side = side;
            e.price = price;
            e.qty_delta = delta_dist(rng);
            ref.apply_delta(e.side, e.price, e.qty_delta);
        } else if (kind == 7) {
            e.kind = BookEventKind::SnapshotBegin;
            ref.clear();
        } else if (kind == 8) {
            e.kind = BookEventKind::Ticker;
            e.price = price;                          // unused by ShadowBook — carried for realism only
            e.ask_price = static_cast<Price>(price_dist(rng));
        } else {
            e.kind = BookEventKind::Trade;
            e.side = side;
            e.price = price;
            e.qty = static_cast<Qty>(qty_dist(rng));
            e.ts_ms = next_ts_ms++;
            // Trade doesn't touch ShadowBook state (TradeWindow only) — no ref update.
        }

        while (!ring_a.try_push(e)) cpu_relax();

        // Deterministic sync: wait for this exact event to have been applied
        // before pushing the next one, so ref stays in lockstep with the
        // real book regardless of scheduling — same technique
        // tests/test_replay_determinism.cpp uses, just per-event here instead
        // of per-batch.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (brain->book_events_applied() < static_cast<std::uint64_t>(i + 1) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        PX_CHECK(brain->book_events_applied() == static_cast<std::uint64_t>(i + 1));

        assert_matches(ref_a, brain->book(kMarketA), i);
        assert_matches(ref_b, brain->book(kMarketB), i);
    }

    stop.store(true, std::memory_order_relaxed);
    brain_thread.join();
}

} // namespace

int main() {
    test_adversarial_book_events_never_desync_or_violate_invariants();
    PX_TEST_REPORT_AND_RETURN();
}
