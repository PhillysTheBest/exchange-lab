// tools/run_backtest.cpp — Stage 6, Part B.3/H: runs PassiveMarketMaker,
// OrderBookImbalance, and TradeFlow against the IDENTICAL seeded synthetic event
// sequence (eval::SyntheticMarket), each in its own eval::BacktestRunner (see that
// class's header comment for why one strategy per runner, not three sharing a
// book), and prints the Part H comparison table.
//
// No OpenSSL, no network — eval/ operates entirely on pipeline::BookEvent directly
// (see eval/synthetic_market.hpp's own header for why this tool doesn't route
// through the real JSON wire format). Deterministic: a fixed --seed always
// produces byte-identical metrics, the same guarantee
// tests/test_replay_determinism.cpp already established for the book-update path
// alone, now extended through the fill/P&L path (see docs/BACKTEST_ASSESSMENT.md).
#include "eval/backtest_runner.hpp"
#include "eval/synthetic_market.hpp"
#include "risk/risk_engine.hpp"
#include "strategy/order_book_imbalance.hpp"
#include "strategy/passive_market_maker.hpp"
#include "strategy/trade_flow.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace px;
using namespace px::eval;

namespace {

constexpr MarketId kMkt = 0;
constexpr TraderId kTrader = 1;

struct Args {
    std::uint64_t seed = 42;
    std::size_t events = 20'000;
    std::string settle; // "", "yes", "no"
};

[[nodiscard]] bool starts_with(const char* s, const char* prefix) noexcept {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

[[nodiscard]] bool parse_args(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (starts_with(a, "--seed=")) out.seed = static_cast<std::uint64_t>(std::atoll(a + 7));
        else if (starts_with(a, "--events=")) out.events = static_cast<std::size_t>(std::atoll(a + 9));
        else if (starts_with(a, "--settle=")) out.settle = a + 9;
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a);
            return false;
        }
    }
    return true;
}

// Runs one strategy against a fresh SyntheticMarket seeded identically to every
// other strategy's own run (a NEW SyntheticMarket per strategy, same seed, rather
// than one shared generator all three draw from — this is what makes "identical
// event sequence" true: a shared generator would have each strategy's own
// order/cancel activity interleaved with generator calls in the same call, and
// since strategies never mutate the generator, three FRESH same-seeded generators
// reproduce the exact same draws each strategy sees).
// RiskEngine's token bucket (risk/risk_engine.hpp) is deliberately driven by REAL
// wall-clock time even inside this backtest — Brain::handle_decision calls its own
// internal now_ns() (std::chrono::steady_clock), not a caller-injectable one (see
// docs/BACKTEST_ASSESSMENT.md's "rate limiting is a real-time artifact" note for why
// this wasn't changed). Found EMPIRICALLY by running this exact tool during Stage
// 6: replaying 20,000 events takes single-digit milliseconds of real wall time, so
// a bucket sized for Kalshi's real ~10-orders/sec budget drains almost instantly
// and then rejects nearly everything for the rest of the run — an artifact of
// compressed replay time, not a finding about any strategy's real fill behavior.
// Rate-limit behavior is already correctly tested, WITH real injected timing, at
// the RiskEngine layer directly (tests/test_risk.cpp) — this evaluation framework's
// job is strategy decision quality (spread capture, fill/P&L behavior), not
// re-proving the rate limiter, so it configures the bucket generously out of the
// way rather than let a replay-speed artifact dominate the results.
[[nodiscard]] risk::RiskConfig backtest_risk_config() noexcept {
    risk::RiskConfig cfg;
    cfg.bucket_capacity_tokens = 10'000'000;
    cfg.bucket_refill_per_sec = 10'000'000;
    // Fat-finger bounds and exposure checks are NOT wall-clock-dependent -- purely
    // price/position state -- so they stay at their real defaults and are
    // genuinely, honestly backtested as-is.
    return cfg;
}

[[nodiscard]] StrategyReport run_one(std::unique_ptr<strategy::Strategy> strat, std::uint64_t seed,
                                      std::size_t num_events, const std::string& settle) {
    BacktestRunner runner(kMkt, std::move(strat), backtest_risk_config());
    SyntheticMarket market(SyntheticMarketConfig{seed, /*initial_bid=*/40, /*initial_ask=*/55, /*initial_qty=*/100});

    for (const auto& e : market.initial_snapshot(kMkt)) runner.step(e);
    for (std::size_t i = 0; i < num_events; ++i) runner.step(market.next_event(kMkt));

    if (settle == "yes") runner.settle(Settlement::Yes);
    else if (settle == "no") runner.settle(Settlement::No);
    // else: leave unsettled -- report() marks the open position to the last
    // observed mid instead, an honestly-labeled "still open" position.

    return runner.report();
}

void print_row(const char* label, double v) { std::printf("| %-18s | %10.2f |\n", label, v); }
void print_row_i(const char* label, long long v) { std::printf("| %-18s | %10lld |\n", label, v); }

void print_report(const StrategyReport& r) {
    std::printf("\n=== %s ===\n", r.strategy_name.c_str());
    if (r.diverged) {
        std::printf(
            "*** DIVERGED: |position| exceeded this framework's safety threshold before the replay window "
            "ended -- see eval/backtest_runner.hpp's DIVERGENCE GUARD comment. Numbers below are frozen at the "
            "point this run was stopped, not a full-window result. ***\n");
    }
    std::printf("| %-18s | %10s |\n", "metric", "value");
    std::printf("|--------------------|------------|\n");
    print_row_i("orders", static_cast<long long>(r.metrics.orders_submitted));
    print_row_i("fills (qty)", static_cast<long long>(r.metrics.qty_filled));
    print_row("fill rate (qty)", 100.0 * r.metrics.fill_rate_by_qty());
    print_row("cancel rate", 100.0 * r.metrics.cancel_rate());
    print_row("maker ratio", 100.0 * r.metrics.maker_ratio());
    print_row("gross P&L (c)", static_cast<double>(r.gross_pnl_cents));
    print_row("fees (c)", static_cast<double>(r.metrics.fees_cents));
    print_row("net P&L (c)", static_cast<double>(r.net_pnl_cents));
    print_row("max drawdown (c)", static_cast<double>(r.metrics.max_drawdown_cents));
    print_row_i("final position", static_cast<long long>(r.final_position));
    print_row("max |exposure| (c)", static_cast<double>(r.metrics.max_abs_exposure_cents));
    print_row("turnover", r.metrics.turnover());
    print_row("avg time-to-fill (ticks)", r.metrics.avg_time_to_fill_ticks());
    print_row("avg adverse selection (c)", r.metrics.avg_adverse_selection_cents());
    print_row_i("rejected (risk/lifecycle)", static_cast<long long>(r.metrics.orders_rejected));
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return 1;

    std::printf("run_backtest: seed=%llu events=%zu settle=%s\n", static_cast<unsigned long long>(args.seed),
                args.events, args.settle.empty() ? "(none -- mark to last mid)" : args.settle.c_str());

    const auto pmm = run_one(std::make_unique<strategy::PassiveMarketMaker>(kMkt, kTrader), args.seed, args.events,
                              args.settle);
    const auto obi = run_one(std::make_unique<strategy::OrderBookImbalance>(kMkt, kTrader), args.seed, args.events,
                              args.settle);
    const auto tf =
        run_one(std::make_unique<strategy::TradeFlow>(kMkt, kTrader), args.seed, args.events, args.settle);

    print_report(pmm);
    print_report(obi);
    print_report(tf);
    return 0;
}
