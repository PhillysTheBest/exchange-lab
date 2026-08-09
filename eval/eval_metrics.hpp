// eval/eval_metrics.hpp — Stage 6, Part B.3: the metric set the plan asks for,
// accumulated per strategy under evaluation. Pure data + pure accumulation
// functions, no engine/pipeline dependency — independently unit-testable the same
// way strategy/market_state.hpp's extract_features() is (see tests/test_eval_framework.cpp).
//
// Deliberately excludes "decision latency" — bench/bench_strategy.cpp already
// measures Strategy::on_event/extract_features latency correctly, in isolation, with
// real p50/p99 numbers already in the README. Re-deriving it here, inside a loop
// whose job is "iterate a scripted event sequence as fast as possible" rather than
// "measure one real call under realistic conditions," would produce a number that
// LOOKS like a latency measurement but isn't one — exactly the kind of unscrutinized
// number this project's own methodology-bug story (see README's "Measuring latency"
// section) exists to warn against. eval/backtest_runner.hpp's report instead
// cross-references bench_strategy's own numbers.
#pragma once

#include "px/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace px::eval {

// Per-fill classification — recorded once per ExecReport::Fill the backtest
// synthesizes (see fill_simulator.hpp).
enum class FillRole : std::uint8_t { Maker, Taker };

struct EvalMetrics {
    // ---- orders / fills / cancels ("at minimum" tier) ----------------------
    std::uint64_t orders_submitted = 0;
    std::uint64_t orders_acked = 0;
    std::uint64_t orders_with_any_fill = 0; // distinct orders that filled >=1 time
    std::uint64_t orders_cancelled = 0;
    std::uint64_t orders_rejected = 0;

    Qty qty_ordered = 0; // sum of every NewOrder intent's qty
    Qty qty_filled = 0;  // sum of every fill's qty

    std::uint64_t maker_fills = 0;
    std::uint64_t taker_fills = 0;

    Cash fees_cents = 0;

    // ---- P&L / drawdown ------------------------------------------------------
    // Equity here means "starting cash + realized cash flow + mark-to-market value of
    // any open position" — sampled by the caller (eval/backtest_runner.hpp, which
    // knows the current mid/settlement price) after every event, not computed here.
    Cash peak_equity_cents = 0;
    Cash max_drawdown_cents = 0;
    bool equity_seeded = false;
    Cash last_equity_cents = 0;

    // ---- risk-adjusted-return proxy (Stage 6, Part H — EXTERNAL_REVIEW.md item 7's
    // "Sharpe ratio... scorecard" BUILD decision) -------------------------------
    // Deliberately NOT called a Sharpe ratio, and not annualized: a textbook Sharpe
    // needs a return distribution over independent trials/periods (e.g. daily
    // returns across many days); this framework produces exactly ONE equity path
    // per (strategy, seed) — see docs/BACKTEST_ASSESSMENT.md's own limitations
    // section on single-path backtests. What's computed instead is honestly
    // narrower: mean(per-tick equity delta) / stdev(per-tick equity delta) — a
    // dimensionless "was the equity curve's drift large relative to its own
    // noise, over THIS one path" figure. Same "stated as a proxy, not a claimed
    // standard metric" treatment turnover() below already gets, for the same
    // reason (this project's own norm of not silently overclaiming a number's
    // meaning — see README's "Measuring latency" methodology-bug story).
    double equity_delta_sum_cents = 0.0;
    double equity_delta_sq_sum_cents2 = 0.0;
    std::uint64_t equity_delta_samples = 0;

    // ---- inventory -------------------------------------------------------------
    Cash max_abs_exposure_cents = 0; // peak |position| * 100c seen (matches
                                      // RiskEngine's own exposure definition)
    double abs_position_sum = 0.0;   // for turnover's average-|position| denominator
    std::uint64_t abs_position_samples = 0;
    Qty volume_traded = 0; // sum of every fill's qty (== qty_filled; named separately
                            // because turnover's numerator is conceptually "volume,"
                            // not "how much of our own orders got filled")

    // ---- time-to-fill ("where possible" tier) -----------------------------
    // Measured in LOGICAL EVENT TICKS (one per BookEvent the backtest driver
    // processes), not wall-clock nanoseconds — deliberately named _ticks, not _ns,
    // to avoid implying a precision this framework doesn't have. Only
    // BookEventKind::Trade carries a real exchange timestamp in this project's wire
    // schema (see pipeline/events.hpp's BookEvent) — every other event kind doesn't,
    // so no consistent wall-clock signal exists to measure real latency against
    // here. Real per-call decision latency is already measured correctly elsewhere
    // (bench/bench_strategy.cpp) — see this file's own header comment.
    std::uint64_t time_to_fill_samples = 0;
    double time_to_fill_sum_ticks = 0.0;
    std::uint64_t time_to_fill_max_ticks = 0;

    // ---- realized spread / adverse selection ("where possible" tier) -------
    // Per standard microstructure decomposition (Huang & Stoll-style): effective
    // spread = 2 * side_sign * (fill_price - mid_at_fill); realized spread = 2 *
    // side_sign * (fill_price - mid_at_horizon), where side_sign is +1 for a fill
    // WE bought (we're comparing our cost to the reference price) and -1 for a fill
    // we sold. A positive realized spread means our fills were, on average, on the
    // right side of where the market moved next; adverse_selection = effective -
    // realized is the piece of the spread that was given back to informed order flow.
    // "mid_at_horizon" here is the mid at the NEXT observed book event for this
    // market after the fill, an event-count horizon rather than a fixed wall-clock
    // one (e.g. Huang/Stoll's classic 5-minute convention) — flagged explicitly, not
    // silently substituted, since this project's synthetic replay doesn't carry a
    // meaningful wall-clock cadence to lag against (see docs/BACKTEST_ASSESSMENT.md).
    double effective_spread_sum_cents = 0.0;
    double realized_spread_sum_cents = 0.0;
    std::uint64_t spread_samples = 0;

    void record_order_submitted(Qty qty) noexcept {
        ++orders_submitted;
        qty_ordered += qty;
    }
    void record_ack() noexcept { ++orders_acked; }
    void record_cancel() noexcept { ++orders_cancelled; }
    void record_reject() noexcept { ++orders_rejected; }

    void record_fill(FillRole role, Qty qty, Cash fee_cents, bool first_fill_for_this_order) noexcept {
        if (first_fill_for_this_order) ++orders_with_any_fill;
        qty_filled += qty;
        volume_traded += qty;
        fees_cents += fee_cents;
        if (role == FillRole::Maker) ++maker_fills;
        else ++taker_fills;
    }

    void sample_equity(Cash equity_cents) noexcept {
        if (!equity_seeded) {
            peak_equity_cents = equity_cents;
            equity_seeded = true;
            last_equity_cents = equity_cents;
            return;
        }
        peak_equity_cents = std::max(peak_equity_cents, equity_cents);
        const Cash drawdown = peak_equity_cents - equity_cents;
        max_drawdown_cents = std::max(max_drawdown_cents, drawdown);

        const double delta = static_cast<double>(equity_cents - last_equity_cents);
        equity_delta_sum_cents += delta;
        equity_delta_sq_sum_cents2 += delta * delta;
        ++equity_delta_samples;
        last_equity_cents = equity_cents;
    }

    void sample_exposure(Position position) noexcept {
        const Cash exposure = (position < 0 ? -position : position) * 100;
        max_abs_exposure_cents = std::max(max_abs_exposure_cents, exposure);
        abs_position_sum += static_cast<double>(position < 0 ? -position : position);
        ++abs_position_samples;
    }

    void record_time_to_fill(std::int64_t ticks) noexcept {
        ++time_to_fill_samples;
        time_to_fill_sum_ticks += static_cast<double>(ticks);
        time_to_fill_max_ticks = std::max(time_to_fill_max_ticks, static_cast<std::uint64_t>(ticks));
    }

    void record_spread(double effective_cents, double realized_cents) noexcept {
        ++spread_samples;
        effective_spread_sum_cents += effective_cents;
        realized_spread_sum_cents += realized_cents;
    }

    // ---- derived ---------------------------------------------------------------
    [[nodiscard]] double fill_rate_by_qty() const noexcept {
        return qty_ordered == 0 ? 0.0 : static_cast<double>(qty_filled) / static_cast<double>(qty_ordered);
    }
    [[nodiscard]] double cancel_rate() const noexcept {
        return orders_submitted == 0 ? 0.0 : static_cast<double>(orders_cancelled) / static_cast<double>(orders_submitted);
    }
    [[nodiscard]] double maker_ratio() const noexcept {
        const auto total = maker_fills + taker_fills;
        return total == 0 ? 0.0 : static_cast<double>(maker_fills) / static_cast<double>(total);
    }
    [[nodiscard]] double avg_time_to_fill_ticks() const noexcept {
        return time_to_fill_samples == 0 ? 0.0 : time_to_fill_sum_ticks / static_cast<double>(time_to_fill_samples);
    }
    [[nodiscard]] double avg_effective_spread_cents() const noexcept {
        return spread_samples == 0 ? 0.0 : effective_spread_sum_cents / static_cast<double>(spread_samples);
    }
    [[nodiscard]] double avg_realized_spread_cents() const noexcept {
        return spread_samples == 0 ? 0.0 : realized_spread_sum_cents / static_cast<double>(spread_samples);
    }
    [[nodiscard]] double avg_adverse_selection_cents() const noexcept {
        return avg_effective_spread_cents() - avg_realized_spread_cents();
    }
    // Simplified proxy, stated explicitly as one rather than a textbook/GAAP
    // definition: total contract volume traded, divided by this strategy's own
    // time-averaged absolute position. A strategy holding a small, stable position
    // while trading a lot (turning it over repeatedly) scores high; one that
    // accumulates a large, static position scores low — the intuition "turnover"
    // is meant to capture, without claiming to match any specific external
    // convention.
    // See equity_delta_sum_cents's own comment above for what this is/isn't.
    // Population stdev (divide by N, not N-1) -- this is a description of the one
    // path actually observed, not an estimate of a wider population, so the small
    // Bessel's-correction distinction doesn't apply. Returns 0.0 for a
    // (near-)constant equity curve rather than dividing by ~0.
    [[nodiscard]] double risk_adjusted_return_proxy() const noexcept {
        if (equity_delta_samples < 2) return 0.0;
        const double n = static_cast<double>(equity_delta_samples);
        const double mean = equity_delta_sum_cents / n;
        const double variance = std::max(0.0, equity_delta_sq_sum_cents2 / n - mean * mean);
        const double stdev = std::sqrt(variance);
        return stdev < 1e-9 ? 0.0 : mean / stdev;
    }
    [[nodiscard]] double turnover() const noexcept {
        if (abs_position_samples == 0) return 0.0;
        const double avg_abs_position = abs_position_sum / static_cast<double>(abs_position_samples);
        if (avg_abs_position < 1.0) return static_cast<double>(volume_traded); // avoid div-by-~0
        return static_cast<double>(volume_traded) / avg_abs_position;
    }
};

} // namespace px::eval
