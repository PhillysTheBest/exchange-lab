// tests/test_eval_framework.cpp — Stage 6, Part B.3: unit tests for eval/.
//
// Drives eval::BacktestRunner directly with hand-constructed pipeline::BookEvents
// (same "bypass Ingress/JSON parsing, push events directly" convention
// tests/test_replay_fuzz.cpp already established) and a small test-only
// ScriptedStrategy that returns exactly the decisions each test needs, in order —
// full determinism, no reliance on any real Strategy's actual threshold logic,
// which keeps these tests isolated to eval/'s own correctness (fee formula, fill
// simulation, P&L/drawdown/settlement accounting), not strategy/*.hpp's.
#include "eval/backtest_runner.hpp"
#include "eval/eval_metrics.hpp"
#include "eval/fees.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

using namespace px;
using namespace px::eval;

namespace {

constexpr MarketId kMkt = 0;

using ScriptStep = std::function<strategy::ExecutionDecision(const strategy::MarketState&, strategy::TriggerKind)>;

// Returns each script step's decision on successive on_event calls (regardless of
// trigger kind — these tests construct event sequences where exactly which call
// index corresponds to which real-world trigger is worked out by hand, documented
// inline at each call site below), then defaults to None (Wait) once the script is
// exhausted — so a test only needs to script the calls it actually cares about.
class ScriptedStrategy final : public strategy::Strategy {
public:
    explicit ScriptedStrategy(std::vector<ScriptStep> script) : script_(std::move(script)) {}
    [[nodiscard]] strategy::ExecutionDecision on_event(const strategy::MarketState& s, strategy::TriggerKind t,
                                                        const strategy::OracleEvent*) noexcept override {
        if (call_index_ < script_.size()) return script_[call_index_++](s, t);
        ++call_index_;
        return strategy::ExecutionDecision{};
    }
    [[nodiscard]] const char* name() const noexcept override { return "ScriptedStrategy"; }

private:
    std::vector<ScriptStep> script_;
    std::size_t call_index_ = 0;
};

[[nodiscard]] pipeline::BookEvent level_set(Side side, Price price, Qty qty) noexcept {
    pipeline::BookEvent e;
    e.kind = pipeline::BookEventKind::LevelSet;
    e.market = kMkt;
    e.side = side;
    e.price = price;
    e.qty = qty;
    return e;
}

[[nodiscard]] pipeline::BookEvent trade(Side taker_side, Price price, Qty qty) noexcept {
    pipeline::BookEvent e;
    e.kind = pipeline::BookEventKind::Trade;
    e.market = kMkt;
    e.side = taker_side;
    e.price = price;
    e.qty = qty;
    return e;
}

strategy::ExecutionDecision join(Side side, Price price, Qty qty) {
    strategy::ExecutionDecision d;
    d.action = strategy::ExecutionAction::Join;
    d.intent = strategy::OrderIntent{strategy::IntentKind::NewOrder,
                                      kInvalidOrderId,
                                      kMkt,
                                      /*trader=*/1,
                                      Contract::Yes,
                                      side,
                                      OrderType::Limit,
                                      TimeInForce::GTC,
                                      price,
                                      qty};
    return d;
}

strategy::ExecutionDecision cross(Side side, Price price, Qty qty) {
    strategy::ExecutionDecision d;
    d.action = strategy::ExecutionAction::Cross;
    d.intent = strategy::OrderIntent{strategy::IntentKind::NewOrder,
                                      kInvalidOrderId,
                                      kMkt,
                                      /*trader=*/1,
                                      Contract::Yes,
                                      side,
                                      OrderType::Limit,
                                      TimeInForce::IOC,
                                      price,
                                      qty};
    return d;
}

void test_fees() {
    // 50c is the fee-formula's peak (P*(1-P) maximized at P=0.5) — hand-verified
    // against docs/EXTERNAL_REVIEW.md item 3's cited formula:
    // taker = round_up(0.07 * C * P * (1-P)), maker = round_up(0.0175 * ...).
    PX_CHECK(taker_fee_cents(50, 1) == 2);  // 0.07*1*0.5*0.5=0.0175 -> round up to 2c
    PX_CHECK(maker_fee_cents(50, 1) == 1);  // 0.0175*1*0.5*0.5=0.004375 -> round up to 1c
    PX_CHECK(taker_fee_cents(90, 100) == 63); // 0.07*100*0.9*0.1=0.63 -> exact, no rounding needed
    PX_CHECK(maker_fee_cents(90, 100) == 16); // 0.0175*100*0.9*0.1=0.1575 -> round up to 16c... wait, dollars->cents: 0.1575 dollars = 15.75c -> round up to 16c
    PX_CHECK(taker_fee_cents(50, 0) == 0);  // zero quantity, zero fee
    // maker always cheaper than taker, same price/qty -- qty=100 (not 1) so the
    // comparison isn't accidentally masked by both sides rounding up to the same
    // 1-cent floor at a tiny notional.
    PX_CHECK(maker_fee_cents(50, 100) == 44);  // 0.0175*100*0.5*0.5=0.4375 -> round up to 44c
    PX_CHECK(taker_fee_cents(50, 100) == 175); // 0.07*100*0.5*0.5=1.75 -> exact
    PX_CHECK(maker_fee_cents(50, 100) < taker_fee_cents(50, 100));
}

void test_maker_fill_via_trade() {
    auto strat = std::make_unique<ScriptedStrategy>(std::vector<ScriptStep>{
        // call #1: BookUpdate from the first LevelSet -- rest a GTC buy at 40.
        [](const strategy::MarketState&, strategy::TriggerKind) { return join(Side::Buy, 40, 10); },
    });
    BacktestRunner runner(kMkt, std::move(strat), risk::RiskConfig{});

    runner.step(level_set(Side::Buy, 40, 100)); // call #1 fires here; Acked COMPUTED (depth_ahead=100), not yet delivered
    {
        const auto r = runner.report();
        PX_CHECK(r.metrics.orders_submitted == 1);
        PX_CHECK(r.metrics.orders_acked == 0); // computed but not yet DELIVERED -- see step()'s own comment
                                                // on why every ExecReport lags its cause by one tick
    }
    runner.step(level_set(Side::Sell, 42, 50)); // unrelated level -- also delivers the deferred Acked
    {
        const auto r = runner.report();
        PX_CHECK(r.metrics.orders_acked == 1);
        PX_CHECK(r.metrics.orders_with_any_fill == 0);
    }

    runner.step(trade(Side::Sell, 40, 60)); // taker sold (hit the bid) -- consumes 60 of our 100 depth_ahead
    {
        const auto r = runner.report();
        PX_CHECK(r.metrics.orders_with_any_fill == 0); // depth_ahead (now 40) not exhausted yet -- no fill
    }

    runner.step(trade(Side::Sell, 40, 50)); // consumes remaining 40 depth_ahead, computes our 10-qty fill
    {
        const auto r = runner.report();
        PX_CHECK(r.metrics.orders_with_any_fill == 0); // computed this tick, not delivered until the next one
    }
    runner.tick(); // delivers the fill computed above -- no new market event needed
    {
        const auto r = runner.report();
        PX_CHECK(r.metrics.orders_with_any_fill == 1);
        PX_CHECK(r.metrics.maker_fills == 1);
        PX_CHECK(r.metrics.taker_fills == 0);
        PX_CHECK(r.metrics.qty_filled == 10);
        PX_CHECK(r.metrics.fees_cents == maker_fee_cents(40, 10));
        PX_CHECK(r.final_position == 10);
        PX_CHECK(r.final_cash == r.starting_cash - 40 * 10);
    }
}

void test_taker_sweep_across_levels() {
    auto strat = std::make_unique<ScriptedStrategy>(std::vector<ScriptStep>{
        [](const strategy::MarketState&, strategy::TriggerKind) { return strategy::ExecutionDecision{}; }, // call #1: wait
        [](const strategy::MarketState&, strategy::TriggerKind) { return cross(Side::Buy, 43, 30); },       // call #2: cross
    });
    BacktestRunner runner(kMkt, std::move(strat), risk::RiskConfig{});

    runner.step(level_set(Side::Sell, 42, 20)); // call #1 -- wait
    runner.step(level_set(Side::Sell, 43, 50)); // call #2 -- cross IOC buy @43 x30, sweeps both levels (computed,
                                                 // not yet delivered)
    runner.tick(); // delivers the sweep's Acked + two Fills

    const auto r = runner.report();
    PX_CHECK(r.metrics.taker_fills == 2);
    PX_CHECK(r.metrics.maker_fills == 0);
    PX_CHECK(r.metrics.qty_filled == 30);
    PX_CHECK(r.metrics.orders_with_any_fill == 1); // one order, two fill events
    PX_CHECK(r.metrics.fees_cents == taker_fee_cents(42, 20) + taker_fee_cents(43, 10));
    PX_CHECK(r.final_position == 30);
    PX_CHECK(r.final_cash == r.starting_cash - (42 * 20 + 43 * 10));
}

void test_cancel_prevents_later_fill() {
    auto strat = std::make_unique<ScriptedStrategy>(std::vector<ScriptStep>{
        // call #1: BookUpdate from the LevelSet -- rest a GTC sell at 60.
        [](const strategy::MarketState&, strategy::TriggerKind) { return join(Side::Sell, 60, 5); },
        // call #2: OwnOrderUpdate from the (now-delivered) Acked report -- cancel it immediately.
        [](const strategy::MarketState& s, strategy::TriggerKind) {
            strategy::ExecutionDecision d;
            if (!s.outstanding_orders.empty()) {
                d.action = strategy::ExecutionAction::Cancel;
                d.cancel_target = s.outstanding_orders[0].id;
            }
            return d;
        },
    });
    BacktestRunner runner(kMkt, std::move(strat), risk::RiskConfig{});

    runner.step(level_set(Side::Buy, 55, 20)); // call #1 rests the sell (Acked computed, not yet delivered)
    runner.tick(); // delivers Acked -- call #2 fires (OwnOrderUpdate), cancels; Cancelled computed, not yet delivered
    {
        const auto r = runner.report();
        PX_CHECK(r.metrics.orders_cancelled == 0); // computed, not delivered yet
    }
    runner.tick(); // delivers Cancelled
    {
        const auto r = runner.report();
        PX_CHECK(r.metrics.orders_cancelled == 1);
    }

    // FillSimulator's OWN bookkeeping (resting_) was updated the instant cancel()
    // was CALLED (synchronous), independent of when the Cancelled report is
    // delivered to Brain -- so this later trade correctly finds nothing resting
    // to fill, regardless of the delivery-timing model above.
    runner.step(trade(Side::Buy, 60, 100)); // a taker buy at exactly our (now-cancelled) price -- must NOT fill us
    runner.tick();
    const auto r = runner.report();
    PX_CHECK(r.metrics.orders_with_any_fill == 0);
    PX_CHECK(r.metrics.maker_fills == 0);
    PX_CHECK(r.metrics.fees_cents == 0);
    PX_CHECK(r.final_position == 0);
    PX_CHECK(r.final_cash == r.starting_cash);
}

void test_max_drawdown() {
    EvalMetrics m;
    m.sample_equity(1000);
    m.sample_equity(1200);
    m.sample_equity(900);
    m.sample_equity(1100);
    m.sample_equity(700);
    PX_CHECK(m.peak_equity_cents == 1200);
    PX_CHECK(m.max_drawdown_cents == 500); // 1200 -> 700
}

void test_risk_adjusted_return_proxy() {
    // Steady +100c/tick drift, zero noise -- deltas are all identical, so stdev is
    // 0 and the proxy falls back to 0.0 (the "don't divide by ~0" guard), not a
    // spuriously infinite value.
    {
        EvalMetrics m;
        m.sample_equity(1000);
        m.sample_equity(1100);
        m.sample_equity(1200);
        m.sample_equity(1300);
        PX_CHECK(m.risk_adjusted_return_proxy() == 0.0);
    }
    // Two samples aren't enough for a delta-of-deltas stdev at all (only one delta
    // exists) -- guarded the same way, not a crash/NaN.
    {
        EvalMetrics m;
        m.sample_equity(1000);
        m.sample_equity(1100);
        PX_CHECK(m.risk_adjusted_return_proxy() == 0.0);
    }
    // Positive drift with real noise around it -- mean/stdev should come out
    // positive (upward drift dominates the noise), hand-computed: deltas are
    // +200, -50, +150 -- mean=100, population variance = ((100)^2+(150)^2+(50)^2)/3
    // = (10000+22500+2500)/3 = 11666.67, stdev ~= 108.01 -- proxy ~= 100/108.01 ~= 0.926.
    {
        EvalMetrics m;
        m.sample_equity(1000);
        m.sample_equity(1200); // delta +200
        m.sample_equity(1150); // delta -50
        m.sample_equity(1300); // delta +150
        const double proxy = m.risk_adjusted_return_proxy();
        PX_CHECK(proxy > 0.9 && proxy < 0.95);
    }
}

void test_settlement_marks_open_position() {
    auto strat = std::make_unique<ScriptedStrategy>(std::vector<ScriptStep>{
        [](const strategy::MarketState&, strategy::TriggerKind) { return cross(Side::Buy, 50, 10); },
    });
    BacktestRunner runner(kMkt, std::move(strat), risk::RiskConfig{});
    runner.step(level_set(Side::Sell, 50, 20)); // crosses immediately, computes a 10 @ 50 fill (not yet delivered)
    runner.tick();                              // delivers it

    runner.settle(Settlement::Yes);
    const auto r = runner.report();
    PX_CHECK(r.settled);
    PX_CHECK(r.mark_price_cents == 100);
    PX_CHECK(r.final_position == 10);
    const Cash expected_fee = taker_fee_cents(50, 10);
    PX_CHECK(r.metrics.fees_cents == expected_fee);
    // gross = (final_cash - starting_cash) + position*mark = -500 + 10*100 = 500
    PX_CHECK(r.gross_pnl_cents == 500);
    PX_CHECK(r.net_pnl_cents == 500 - expected_fee);
}

} // namespace

int main() {
    test_fees();
    test_maker_fill_via_trade();
    test_taker_sweep_across_levels();
    test_cancel_prevents_later_fill();
    test_max_drawdown();
    test_risk_adjusted_return_proxy();
    test_settlement_marks_open_position();
    PX_TEST_REPORT_AND_RETURN();
}
