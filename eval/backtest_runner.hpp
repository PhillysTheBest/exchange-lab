// eval/backtest_runner.hpp — Stage 6, Part B.3: the harness. Drives ONE real
// pipeline::Brain, with exactly ONE Strategy registered, through a caller-supplied
// sequence of BookEvents, single-threaded and fully deterministically — see
// docs/BACKTEST_ASSESSMENT.md for why single-threaded/synchronous is a deliberate
// choice (reproducibility), not a simplification of convenience.
//
// ONE STRATEGY PER RUNNER, DELIBERATELY: Brain supports multiple strategies sharing
// one market (see pipeline/brain.hpp's strategy_index_by_market_), and OrderIntent
// carries no strategy-identity field (strategy/strategy.hpp — by design, Brain's own
// job is only to route decisions, not track who authored them). Attributing fills
// back to "which strategy caused this" would need inventing that identity from
// nothing. Running each strategy against the identical event stream in its OWN
// Brain/market instead sidesteps the problem entirely AND is the fairer comparison
// for Part H's table: each strategy sees the same history with no cross-strategy
// interference (one strategy's resting order never eats the queue position another
// strategy would have gotten), matching how these would actually run in
// production — on separate accounts, not fighting over one book.
//
// EVENT PROCESSING ORDER, per step (one BookEvent in): if the event is a real Trade,
// FillSimulator::on_trade runs FIRST (modeling "our own fill notification arrives
// essentially concurrently with, and is processed just before, the public trade
// broadcast" — a deliberate, stated modeling choice, not the only defensible
// ordering), THEN the event itself is applied to Brain (BookUpdate dispatch). Any
// resulting ExecReports/OrderIntents are drained to a fixed point in a single
// worklist — an exec report can cause a new intent (OwnOrderUpdate), simulating an
// intent can cause a new exec report — with exec reports processed ahead of intents
// whenever both are pending, a fixed, deterministic tie-break.
#pragma once

#include "eval/eval_metrics.hpp"
#include "eval/fees.hpp"
#include "eval/fill_simulator.hpp"
#include "pipeline/brain.hpp"
#include "px/trader.hpp"
#include "risk/risk_engine.hpp"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace px::eval {

struct StrategyReport {
    std::string strategy_name;
    MarketId market = 0;
    EvalMetrics metrics{};
    Cash starting_cash = 0;
    Cash final_cash = 0;
    Position final_position = 0;
    Cash mark_price_cents = 0;   // settlement value if resolved, else last observed mid
    bool settled = false;
    Cash gross_pnl_cents = 0;    // (final_cash - starting_cash) + final_position * mark_price
    Cash net_pnl_cents = 0;      // gross_pnl - metrics.fees_cents
    bool diverged = false;       // see BacktestRunner's own DIVERGENCE GUARD comment
};

class BacktestRunner {
public:
    BacktestRunner(MarketId market, std::unique_ptr<strategy::Strategy> strategy, const risk::RiskConfig& risk_cfg,
                    Cash starting_cash = 1'000'000)
        : market_(market), strategy_name_(strategy->name()), brain_(std::make_unique<pipeline::Brain>(risk_cfg)),
          // pipeline::RingB embeds a Capacity=4096-element std::array<OrderIntent>
          // BY VALUE (~160 KB — see px/spsc_ring.hpp) — the exact same "never a
          // stack local, always heap-allocate one" hazard pipeline/brain.hpp's own
          // SIZE WARNING documents for Brain itself. Allocated ONCE here, reused
          // (fully drained) on every apply_book_event/apply_exec_report call below
          // — safe because this class is single-threaded end to end, never a real
          // producer/consumer pair, so "reuse an already-empty ring" is just
          // ordinary steady-state ring operation, not a hazard of its own.
          scratch_ring_(std::make_unique<pipeline::RingB>()), trader_(/*id=*/1, starting_cash),
          starting_cash_(starting_cash) {
        brain_->add_strategy(market, std::move(strategy));
        // Brain::handle_decision rejects an intent (RiskEngine's three checks, or
        // the market-lifecycle gate) BEFORE it ever reaches ring B — it never
        // becomes an OrderIntent this framework's worklist sees, and this
        // evaluator's FillSimulator never manufactures a post-accept
        // pipeline::ExecKind::Reject either (it stands in for a well-behaved,
        // always-responsive exchange, the same simplification
        // tests/test_pipeline_e2e.cpp's own SimulatedExchangeSink already makes —
        // see docs/BACKTEST_ASSESSMENT.md). Without this hook, every
        // RiskEngine/lifecycle rejection — the rejections that actually matter for
        // "how often did this strategy's own risk limits stop it" — would be
        // silently invisible to metrics_.orders_rejected. Brain exposes exactly
        // this callback for logging/metrics use (pipeline/brain.hpp) — not a new
        // seam, an existing one.
        brain_->on_reject = [this](const strategy::OrderIntent&, risk::Verdict) { metrics_.record_reject(); };
    }

    // Non-copyable, non-movable: brain_->on_reject above captures `this` by value
    // — a move would leave that callback pointing at the old (about-to-be-invalid)
    // address. Construct one per strategy and hold each by pointer/reference
    // (tools/run_backtest.cpp uses std::vector<std::unique_ptr<BacktestRunner>>
    // for exactly this reason) rather than storing BacktestRunner by value in a
    // container that could reallocate.
    BacktestRunner(const BacktestRunner&) = delete;
    BacktestRunner& operator=(const BacktestRunner&) = delete;
    BacktestRunner(BacktestRunner&&) = delete;
    BacktestRunner& operator=(BacktestRunner&&) = delete;

    // Feeds one real market-data event through the real Brain. Safe to call any
    // number of times, in any deterministic order the caller chooses (a fixed seed
    // -> a fixed event sequence -> a bit-identical report, same discipline
    // tests/test_replay_determinism.cpp already established for the book-update
    // path alone — this extends that guarantee through the fill/P&L path too,
    // which is exactly what threaded Brain::run() could not promise).
    //
    // NO EXECREPORT IS EVER DELIVERED IN THE SAME TICK AS ITS OWN CAUSE — found the
    // hard way, not designed in from the start: an earlier version of this class
    // fed FillSimulator's ExecReports back into Brain immediately, synchronously,
    // within the very same step() call that produced them. That IMMEDIATELY
    // deadlocked PassiveMarketMaker's exit logic: Brain dispatches OwnOrderUpdate on
    // an Acked report (see pipeline/brain.cpp's apply_exec_report — every ExecKind
    // except RateLimited triggers it) BEFORE a same-order Fill sitting right behind
    // it is processed, so a strategy reacting to "I'm still positioned" (true, but
    // stale — the fill just hasn't been applied yet) can re-submit before its own
    // first order even settles, cascading into an unbounded same-tick loop with
    // nothing to stop it. In production this exact race is structurally possible
    // too (Brain's real run() loop pops one ring-C item per iteration, same
    // ordering) but is masked by REAL signing/REST latency (~1ms, bench_sign.cpp) —
    // a second decision genuinely cannot round-trip through Gateway before the
    // first order's real Fill report is already sitting on ring C. A zero-latency
    // synchronous evaluator has no such gap, so this class supplies a minimal one
    // explicitly: everything FillSimulator produces (from submit/cancel/on_trade)
    // is queued and only delivered to Brain starting the NEXT tick (step() or
    // tick()), never the current one. This is simultaneously a bug fix (the
    // cascade can no longer revisit already-pending state within one tick, so it's
    // bounded by construction, not just by the guard below) and a more honest
    // latency model (a real order genuinely cannot be acked in zero elapsed time)
    // — see docs/BACKTEST_ASSESSMENT.md's existing "no network/signing latency
    // modeled" limitation, now with a concrete minimal treatment instead of none.
    //
    // DIVERGENCE GUARD, added after finding a real (if narrow) consequence of the
    // one-tick delay above: RiskEngine's exposure check only ever accounts for
    // FILLED position (via record_fill) — resting/in-flight exposure is invisible
    // to it, a gap risk/risk_engine.hpp's own header comment already documents
    // explicitly, naming PassiveMarketMaker by name as the strategy shape that can
    // trigger it. In production that gap's blast radius is bounded by real
    // signing/REST latency and Kalshi's real rate limit, both of which this
    // framework deliberately widens/disables (the one-tick delay above; the
    // generous backtest_risk_config() in tools/run_backtest.cpp) for OTHER good,
    // documented reasons — removing both real-world dampers at once let
    // PassiveMarketMaker's reactive exit logic submit many same-direction Cross
    // orders before any of their fills were known, each individually accepted
    // (RiskEngine's own projected-exposure check didn't yet see the others), on a
    // long enough adversarial synthetic run. Not a hang in the traditional sense —
    // each attempt DOES make real progress (whatever quantity is actually resting
    // at the current best gets consumed) — just potentially thousands of tiny
    // corrective attempts to unwind one large overshoot, without the real-world
    // throttles that would ordinarily prevent the overshoot in the first place.
    // Rather than let that either hang (the 10,000-iteration worklist guard would
    // eventually abandon a tick mid-drain, corrupting this strategy's tracked
    // state for the rest of the run) or silently truncate the replay window to
    // whatever happens not to trigger it, this class detects it explicitly: once
    // |position| exceeds kDivergenceThreshold (20x RiskConfig's own default
    // 100-contract-equivalent exposure cap — generous enough not to fire on the
    // gap's ordinary, bounded consequences, tight enough to catch a genuine
    // runaway before it corrupts anything), step()/tick() become no-ops and
    // report() says so via `diverged`. See docs/BACKTEST_ASSESSMENT.md and
    // docs/EXTERNAL_REVIEW.md item 1/6 — this is presented as a real finding
    // about PassiveMarketMaker's exit logic and RiskEngine's own documented gap,
    // not smoothed over as an implementation detail.
    static constexpr Position kDivergenceThreshold = 2000;

    void step(const pipeline::BookEvent& event) {
        if (diverged_) return;
        ++clock_;
        std::deque<pipeline::ExecReport> pending_exec;
        std::deque<strategy::OrderIntent> pending_intents;
        deliver_deferred(pending_exec);

        if (event.kind == pipeline::BookEventKind::Trade) {
            for (auto& r : fills_.on_trade(market_, event.side, event.price, event.qty, clock_)) {
                deferred_.push_back(r); // arrives next tick, not this one -- see above
            }
        }

        brain_->apply_book_event(event, *scratch_ring_);
        drain(*scratch_ring_, pending_intents);

        run_worklist(pending_exec, pending_intents);
        sample_state();
    }

    // Advances the logical clock and delivers whatever this simulator generated
    // during the previous tick, with no new market data attached — for callers
    // (tests, or a caller modeling a real gap in the replay) that need to force
    // delivery without inventing a throwaway market event. Real backtests
    // (tools/run_backtest.cpp) never need this — step() alone naturally delivers
    // everything as the replay proceeds.
    void tick() {
        if (diverged_) return;
        ++clock_;
        std::deque<pipeline::ExecReport> pending_exec;
        std::deque<strategy::OrderIntent> pending_intents;
        deliver_deferred(pending_exec);
        run_worklist(pending_exec, pending_intents);
        sample_state();
    }

    // Marks any open position to its settlement value (0c if the market resolved
    // No, 100c if Yes) instead of the last observed mid — call once, after the
    // last event, only if the replayed history actually carried the market through
    // to resolution. Safe to skip (report() falls back to last-mid marking) for a
    // replay window that ends before settlement — an honest "still open" position,
    // not a fabricated one.
    void settle(Settlement outcome) noexcept {
        settled_ = true;
        settlement_mark_cents_ = (outcome == Settlement::Yes) ? 100 : 0;
    }

    [[nodiscard]] StrategyReport report() const noexcept {
        StrategyReport r;
        r.strategy_name = strategy_name_;
        r.market = market_;
        r.metrics = metrics_;
        r.starting_cash = starting_cash_;
        r.final_cash = trader_.cash();
        r.final_position = trader_.position(market_);
        r.settled = settled_;
        r.mark_price_cents = settled_ ? settlement_mark_cents_ : last_mid_cents_;
        r.gross_pnl_cents = (r.final_cash - r.starting_cash) + r.final_position * r.mark_price_cents;
        r.net_pnl_cents = r.gross_pnl_cents - r.metrics.fees_cents;
        r.diverged = diverged_;
        return r;
    }

private:
    static void drain(pipeline::RingB& ring, std::deque<strategy::OrderIntent>& out) {
        strategy::OrderIntent intent;
        while (ring.try_pop(intent)) out.push_back(intent);
    }

    void deliver_deferred(std::deque<pipeline::ExecReport>& pending_exec) {
        while (!deferred_.empty()) {
            pending_exec.push_back(deferred_.front());
            deferred_.pop_front();
        }
    }

    // Fixed-point worklist over what's pending THIS tick only. Bounded by
    // construction now (see step()'s own comment: nothing produced while draining
    // this worklist feeds back into it — nEW ExecReports always go to deferred_,
    // next tick's problem, not this one) — every real path either consumes finite
    // requested quantity (fills) or removes exactly one resting order (cancels),
    // and can spawn at most one new intent per exec report via Brain's own
    // single-decision-per-dispatch discipline. The iteration cap below is
    // consequently not expected to ever fire; kept as a guard against a future
    // regression turning into a silent hang rather than a loud, debuggable one.
    void run_worklist(std::deque<pipeline::ExecReport>& pending_exec, std::deque<strategy::OrderIntent>& pending_intents) {
        std::size_t guard = 0;
        while (!pending_exec.empty() || !pending_intents.empty()) {
            if (++guard > 10'000) {
                std::fprintf(stderr, "eval::BacktestRunner::run_worklist: did not converge (>10000 iterations) "
                                      "for strategy '%s' -- this is a bug, not expected backtest behavior\n",
                              strategy_name_.c_str());
                diverged_ = true;
                break;
            }
            // Checked mid-cascade, not just once per tick (sample_state(), called
            // after this whole worklist drains, would be too late to catch a
            // single tick's backlog blowing through the threshold in one go) --
            // see the DIVERGENCE GUARD comment on step()/tick() for what this is
            // and why.
            const Position pos = trader_.position(market_);
            if ((pos < 0 ? -pos : pos) > kDivergenceThreshold) {
                diverged_ = true;
                break;
            }
            if (!pending_exec.empty()) {
                const pipeline::ExecReport er = pending_exec.front();
                pending_exec.pop_front();
                record_exec(er);
                brain_->apply_exec_report(er, *scratch_ring_);
                drain(*scratch_ring_, pending_intents);
            } else {
                const strategy::OrderIntent intent = pending_intents.front();
                pending_intents.pop_front();
                if (intent.kind == strategy::IntentKind::NewOrder) {
                    metrics_.record_order_submitted(intent.qty);
                    for (auto& r : fills_.submit(intent, brain_->book(market_), clock_)) deferred_.push_back(r);
                } else {
                    for (auto& r : fills_.cancel(intent)) deferred_.push_back(r);
                }
            }
        }
    }

    void record_exec(const pipeline::ExecReport& er) {
        switch (er.kind) {
            case pipeline::ExecKind::Acked:
                metrics_.record_ack();
                order_acked_tick_[er.id] = clock_;
                break;
            case pipeline::ExecKind::Cancelled:
                metrics_.record_cancel();
                break;
            case pipeline::ExecKind::Reject:
                metrics_.record_reject();
                break;
            case pipeline::ExecKind::RateLimited:
                break; // not tied to a specific order — nothing to attribute a fill/cancel/reject metric to
            case pipeline::ExecKind::Fill: {
                const bool first_fill = filled_orders_.insert(er.id).second;
                const FillRole role = last_fill_role(er.id);
                const Cash fee = fee_cents_for(role == FillRole::Maker, er.price, er.qty);
                metrics_.record_fill(role, er.qty, fee, first_fill);
                trader_.apply_fill(market_, er.side, er.price, er.qty);

                if (first_fill) {
                    const auto ack_it = order_acked_tick_.find(er.id);
                    if (ack_it != order_acked_tick_.end()) {
                        metrics_.record_time_to_fill(clock_ - ack_it->second);
                    }
                }

                const double mid = current_mid_cents();
                if (mid >= 0.0) {
                    const double side_sign = (er.side == Side::Buy) ? 1.0 : -1.0;
                    const double effective = 2.0 * side_sign * (static_cast<double>(er.price) - mid);
                    pending_spread_.push_back({clock_, side_sign, static_cast<double>(er.price), effective});
                }
                break;
            }
        }
    }

    // A fill's maker/taker role isn't itself on ExecReport — FillSimulator knows it
    // (it's the caller that decided submit() vs on_trade() produced it) but that
    // information doesn't travel through Brain (ExecReport, like every ring
    // payload, stays POD/minimal — see pipeline/events.hpp). Recovered here from
    // FillSimulator's own fill log, which IS ordered consistently with what was
    // just applied (both this class and FillSimulator process one worklist entry
    // at a time, synchronously, in the same thread) — matched by scanning from the
    // most recent fill for this order id, since a taker fill and a later maker
    // fill for the same id (crossed at entry, then rested a remainder that later
    // gets hit) are both legitimate and distinguishable by position in the log.
    [[nodiscard]] FillRole last_fill_role(OrderId id) const noexcept {
        const auto& log = fills_.fills();
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            if (it->id == id) return it->role;
        }
        return FillRole::Taker; // unreachable in practice: this is only called
                                 // immediately after FillSimulator recorded the
                                 // fill this exact ExecReport describes
    }

    [[nodiscard]] double current_mid_cents() const noexcept {
        Price bid = 0, ask = 0;
        const bool hb = brain_->book(market_).best_bid(bid);
        const bool ha = brain_->book(market_).best_ask(ask);
        if (hb && ha) return (static_cast<double>(bid) + static_cast<double>(ask)) / 2.0;
        if (hb) return static_cast<double>(bid);
        if (ha) return static_cast<double>(ask);
        return -1.0; // no observable market yet
    }

    void sample_state() {
        const Position pos = trader_.position(market_);
        if ((pos < 0 ? -pos : pos) > kDivergenceThreshold) {
            diverged_ = true;
            return; // see the DIVERGENCE GUARD comment on step()/tick() -- stop
                     // touching this strategy's state any further, don't let a
                     // runaway scenario keep corrupting metrics past this point
        }

        Price bid = 0, ask = 0;
        const bool hb = brain_->book(market_).best_bid(bid);
        const bool ha = brain_->book(market_).best_ask(ask);
        if (hb && ha) last_mid_cents_ = (bid + ask) / 2;
        else if (hb) last_mid_cents_ = bid;
        else if (ha) last_mid_cents_ = ask;

        const double mid = current_mid_cents();
        if (mid >= 0.0) {
            // Equity = cash-on-hand (already reflects every realized fill via
            // Trader::apply_fill) + the open position's current mark-to-market
            // value. Fees are tracked separately in metrics_.fees_cents, not
            // netted into this running curve, so max-drawdown reflects trading
            // P&L specifically — the report()'s net_pnl_cents is where fees apply.
            const Cash equity = trader_.cash() + trader_.position(market_) * static_cast<Cash>(mid);
            metrics_.sample_equity(equity);
        }
        metrics_.sample_exposure(trader_.position(market_));

        // Resolve realized-spread/adverse-selection records at least one full
        // event-tick after the fill that produced them — see
        // eval/eval_metrics.hpp's own comment on why this is an event-tick
        // horizon, not a wall-clock one.
        while (!pending_spread_.empty() && pending_spread_.front().fill_tick < clock_) {
            const auto rec = pending_spread_.front();
            pending_spread_.pop_front();
            if (mid >= 0.0) {
                const double realized = 2.0 * rec.side_sign * (rec.fill_price - mid);
                metrics_.record_spread(rec.effective_spread, realized);
            }
        }
    }

    struct PendingSpreadRecord {
        std::int64_t fill_tick;
        double side_sign;
        double fill_price;
        double effective_spread;
    };

    MarketId market_;
    std::string strategy_name_;
    std::unique_ptr<pipeline::Brain> brain_;
    std::unique_ptr<pipeline::RingB> scratch_ring_;
    FillSimulator fills_;
    px::Trader trader_;
    Cash starting_cash_;

    std::int64_t clock_ = 0;
    EvalMetrics metrics_;
    std::deque<pipeline::ExecReport> deferred_; // generated this tick, delivered next -- see step()'s own comment
    std::unordered_map<OrderId, std::int64_t> order_acked_tick_;
    std::unordered_set<OrderId> filled_orders_; // which order ids have recorded a "first fill" already
    std::deque<PendingSpreadRecord> pending_spread_;
    Cash last_mid_cents_ = 50; // sane default before any book state exists
    bool settled_ = false;
    Cash settlement_mark_cents_ = 0;
    bool diverged_ = false; // see the DIVERGENCE GUARD comment on step()/tick()
};

} // namespace px::eval
