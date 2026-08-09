// tests/test_alloc_audit.cpp — Stage 5, Section 9: proves, rather than
// assumes, the specific claim docs/CLAUDE.md/README already make — "heap-
// allocate-only at construction, not on the hot path" — for the exact four
// operations Brain::dispatch_market_triggered runs per event
// (bench/bench_pipeline_stages.cpp's stages 3-6): ShadowBook::apply_delta,
// strategy::extract_features, Strategy::on_event, and RiskEngine::check.
//
// SCOPE, stated precisely: this does NOT audit parse_feed_message or
// Ingress::handle_message (Section 2 already found those DO allocate a
// little — a market_ticker std::string per message — that's T1's job, a
// different claim than "Brain's own per-event decision path never
// allocates"). Conflating the two would either wrongly indict Brain for T1's
// allocation or wrongly clear T1 of one it actually has; keeping them
// separate is what makes each claim actually checkable.
//
// TECHNIQUE: global operator new/delete overrides, scoped to this one binary
// only (a test executable, never linked into anything else) — counting is
// gated by a flag this file flips on only around the four calls under
// audit, so Brain/ShadowBook/OrderTracker construction (which DOES legitimately
// allocate — arrays, strings, etc.) doesn't pollute the count.
#include "px/shadow_book.hpp"
#include "risk_engine.hpp"
#include "strategy/market_state.hpp"
#include "strategy/order_tracker.hpp"
#include "strategy/passive_market_maker.hpp"
#include "strategy/trade_window.hpp"
#include "test_harness.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
std::atomic<bool> g_counting{false};
std::atomic<long long> g_alloc_count{0};
std::atomic<long long> g_alloc_bytes{0};
} // namespace

void* operator new(std::size_t sz) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
        g_alloc_bytes.fetch_add(static_cast<long long>(sz), std::memory_order_relaxed);
    }
    void* p = std::malloc(sz);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t sz) { return ::operator new(sz); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace px;
using namespace px::strategy;

namespace {

void test_hot_path_allocates_nothing() {
    // Setup — untimed, uncounted: real construction, which legitimately
    // allocates (ShadowBook's Mask128 internals, OrderTracker's array, the
    // PassiveMarketMaker object itself, etc.).
    ShadowBook book;
    book.set_level(Side::Buy, 40, 100);
    book.set_level(Side::Sell, 60, 50);
    TradeWindow trades;
    OrderTracker tracker; // stack local — see its own SIZE NOTE, ~1.04MB, still fits
    risk::RiskConfig risk_cfg;
    risk_cfg.bucket_capacity_tokens = 10'000'000; // oversized -- see bench_pipeline_e2e.cpp's identical note;
                                                    // this test audits allocation, not rate-limit compliance
    risk::RiskEngine risk(risk_cfg);
    PassiveMarketMaker strat(/*market=*/0, /*trader=*/0);

    constexpr int kIters = 10'000;
    g_alloc_count.store(0);
    g_alloc_bytes.store(0);
    g_counting.store(true, std::memory_order_relaxed);

    for (int i = 0; i < kIters; ++i) {
        // Stage 3: shadow-book update (alternating delta, same technique
        // bench_pipeline_stages.cpp uses to keep quantities valid forever).
        const long long delta = (i % 2 == 0) ? 1 : -1;
        book.apply_delta(Side::Buy, 40, delta);

        // Stage 4: feature extraction.
        const MarketState state = extract_features(/*market=*/0, book, risk.position(0), tracker, trades, /*now_ms=*/i);

        // Stage 5: strategy decision.
        const ExecutionDecision decision = strat.on_event(state, TriggerKind::BookUpdate, nullptr);

        // Stage 6: risk check — only reachable when the strategy actually
        // decided to do something, same as production (pipeline/brain.cpp's
        // handle_decision).
        if (decision.action == ExecutionAction::Join || decision.action == ExecutionAction::Improve ||
            decision.action == ExecutionAction::Cross) {
            (void)risk.check(decision.intent.market, decision.intent.side, decision.intent.price,
                              decision.intent.qty, /*now_ns=*/static_cast<std::int64_t>(i) * 1000);
        } else if (decision.action == ExecutionAction::Cancel) {
            (void)risk.check_cancel(/*now_ns=*/static_cast<std::int64_t>(i) * 1000);
        }
    }

    g_counting.store(false, std::memory_order_relaxed);

    const long long allocs = g_alloc_count.load();
    const long long bytes = g_alloc_bytes.load();
    if (allocs != 0) {
        std::printf("  %lld heap allocation(s) totaling %lld bytes observed across %d iterations of "
                    "shadow-book-update -> feature-extraction -> strategy-decision -> risk-check "
                    "(expected 0)\n",
                    allocs, bytes, kIters);
    }
    PX_CHECK(allocs == 0);
}

} // namespace

int main() {
    test_hot_path_allocates_nothing();
    PX_TEST_REPORT_AND_RETURN();
}
