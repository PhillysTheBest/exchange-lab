#include "pipeline/brain.hpp"
#include "px/platform.hpp" // cpu_relax()

#include <chrono>

namespace px::pipeline {

namespace {
[[nodiscard]] std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

void Brain::apply_book_event(const BookEvent& e, RingB& intent_out) noexcept {
    switch (e.kind) {
        case BookEventKind::SnapshotBegin:
            books_[e.market].clear(); // a fresh LevelSet run follows — see pipeline/ingress.cpp
            ++book_events_applied_;
            return; // a bare clear() isn't a meaningful trigger to dispatch on — the LevelSet run that
                    // follows will each trigger their own dispatch once real levels are set
        case BookEventKind::LevelSet:
            books_[e.market].set_level(e.side, e.price, e.qty);
            break;
        case BookEventKind::LevelDelta:
            books_[e.market].apply_delta(e.side, e.price, e.qty_delta);
            break;
        case BookEventKind::Ticker:
            // Informational only here — the shadow book itself is driven by
            // orderbook_delta, not by the ticker channel. Cross-checking the
            // two against each other is exactly what tools/kalshi_shadow.cpp
            // does at the single-market debugging level; not duplicated here.
            ++book_events_applied_;
            return;
        case BookEventKind::Trade:
            trade_windows_[e.market].record(e.side, e.price, e.qty, e.ts_ms);
            break;
        case BookEventKind::LifecycleStatus:
            // Dispatched like any other BookUpdate (falls through to the
            // shared tail below) so a strategy can react immediately — e.g.
            // cancel a resting quote the instant its market halts, the same
            // "don't wait for the next incidental tick" reasoning
            // OwnOrderUpdate already exists for. The actual "stop new
            // execution"/"stop trading permanently" enforcement is
            // handle_decision's job (see market_status_'s own comment in
            // brain.hpp), not this switch.
            market_status_[e.market] = e.lifecycle_status;
            break;
    }
    ++book_events_applied_;
    dispatch_market_triggered(e.market, strategy::TriggerKind::BookUpdate, intent_out);
}

void Brain::apply_exec_report(const ExecReport& e, RingB& intent_out) noexcept {
    switch (e.kind) {
        case ExecKind::Fill:
            risk_.record_fill(e.market, e.side, e.qty);
            order_tracker_.on_fill(e.market, e.id, e.qty);
            break;
        case ExecKind::RateLimited:
            risk_.on_rate_limited_response(now_ns());
            return; // not tied to a specific order's resting state — no OwnOrderUpdate to dispatch
        case ExecKind::Reject:
            order_tracker_.on_rejected(e.market, e.id); // drops the pending entry handle_decision added optimistically
            break;
        case ExecKind::Acked:
            order_tracker_.on_acked(e.market, e.id, e.qty, books_[e.market]);
            break;
        case ExecKind::Cancelled:
            order_tracker_.on_cancelled(e.market, e.id, e.qty);
            break;
    }
    dispatch_market_triggered(e.market, strategy::TriggerKind::OwnOrderUpdate, intent_out);
}

void Brain::dispatch_market_triggered(MarketId market, strategy::TriggerKind trigger, RingB& intent_out) noexcept {
    // Stage 5, Section 10: iterates only this market's own registered
    // strategies (via strategy_index_by_market_, populated once at
    // add_strategy time) instead of linearly scanning every strategy ever
    // registered on any market — see strategy_index_by_market_'s own comment
    // in pipeline/brain.hpp for the measured before/after this fixed.
    for (const std::size_t idx : strategy_index_by_market_[market]) {
        strategy::Strategy& strat = *strategies_[idx].second;
        const strategy::MarketState state = strategy::extract_features(
            market, books_[market], risk_.position(market), order_tracker_, trade_windows_[market], now_ns());
        const strategy::ExecutionDecision decision = strat.on_event(state, trigger, nullptr);
        handle_decision(market, decision, intent_out);
    }
}

void Brain::evaluate_oracle_event(const strategy::OracleEvent& e, RingB& intent_out) noexcept {
    // Deliberately NOT filtered by market — a real-world event can matter to
    // several differently-registered strategies at once, and it's each
    // strategy's own job (via its watch_source_/watch_key_ check — see e.g.
    // strategy/oracle_threshold_strategy.hpp) to decide relevance, exactly as
    // Stage 1's Signal::evaluate already worked.
    for (auto& [market, strat] : strategies_) {
        const strategy::MarketState state = strategy::extract_features(
            market, books_[market], risk_.position(market), order_tracker_, trade_windows_[market], now_ns());
        const strategy::ExecutionDecision decision = strat->on_event(state, strategy::TriggerKind::OracleEvent, &e);
        handle_decision(market, decision, intent_out);
    }
}

void Brain::handle_decision(MarketId market, const strategy::ExecutionDecision& decision,
                             RingB& intent_out) noexcept {
    if (decision.action == strategy::ExecutionAction::None || decision.action == strategy::ExecutionAction::Wait) {
        return;
    }

    OrderIntent intent;
    if (decision.action == strategy::ExecutionAction::Cancel) {
        intent.kind = IntentKind::CancelOrder;
        intent.id = decision.cancel_target;
        intent.market = market;
    } else {
        // Join / Improve / Cross all become a NewOrder intent, using
        // whatever side/price/qty the Strategy already decided — Brain's own
        // job here is only assigning the OrderId and running the gates
        // below, the same division of labor Stage 1 already established.
        intent = decision.intent;
        intent.kind = IntentKind::NewOrder;
        intent.id = next_order_id_++;
        intent.market = market;
    }

    // Market lifecycle gate (Stage 4), checked before RiskEngine and for the
    // same "cheapest, most-certain check first" reasoning RiskEngine's own
    // fat-finger check runs before its rate-limited one — this doesn't even
    // need now_ns(). RESOLVED blocks everything: nothing is left to trade.
    // HALTED blocks only new orders; a Cancel is risk-reducing (same
    // reasoning RiskEngine's own check_cancel already extends to fat-finger/
    // exposure — see risk_engine.hpp) and stays allowed, so a strategy can
    // still pull a resting quote while its market is halted.
    const MarketStatus status = market_status_[market];
    const bool blocked_by_lifecycle =
        status == MarketStatus::Resolved ||
        (status == MarketStatus::Halted && decision.action != strategy::ExecutionAction::Cancel);
    if (blocked_by_lifecycle) {
        ++intents_rejected_;
        if (on_reject) {
            on_reject(intent, status == MarketStatus::Resolved ? risk::Verdict::RejectMarketResolved
                                                                 : risk::Verdict::RejectMarketHalted);
        }
        return;
    }

    const risk::Verdict verdict = (decision.action == strategy::ExecutionAction::Cancel)
                                       ? risk_.check_cancel(now_ns())
                                       : risk_.check(market, intent.side, intent.price, intent.qty, now_ns());

    if (verdict != risk::Verdict::Accept) {
        ++intents_rejected_;
        if (on_reject) on_reject(intent, verdict);
        return;
    }

    ++intents_accepted_;
    if (intent.kind == IntentKind::NewOrder) {
        // Optimistic: recorded as pending before Gateway has confirmed
        // anything, so a same-tick re-evaluation doesn't re-decide to quote
        // the same side twice while the ack is still in flight (RSA signing
        // alone is ~1ms — see bench/bench_sign.cpp — an eternity relative to
        // how often BookUpdate can fire). Promoted to acked (and made
        // visible to strategies via MarketState::outstanding_orders) only
        // once ring C's Acked report arrives — see apply_exec_report. A false
        // return (this market's 32-order table is full — see
        // kMaxRestingOrdersPerMarket) is not specially handled: the intent
        // still goes out below, it just won't be trackable/cancellable
        // afterward, which at that order count already indicates a runaway
        // strategy rather than something worth adding recovery logic for.
        (void)order_tracker_.on_new(market, intent.id, intent.side, intent.price, intent.qty);
    }
    // An accepted intent must not be silently dropped — same backpressure
    // reasoning as ring A in pipeline/ingress.cpp, not a discard.
    while (!intent_out.try_push(intent)) cpu_relax();
}

void Brain::run(RingA& book_in, strategy::OracleRing& oracle_in, RingC& exec_in, RingB& intent_out,
                 std::atomic<bool>& stop_flag) {
    while (!stop_flag.load(std::memory_order_relaxed)) {
        bool did_work = false;

        BookEvent be;
        if (book_in.try_pop(be)) {
            apply_book_event(be, intent_out);
            did_work = true;
        }

        ExecReport er;
        if (exec_in.try_pop(er)) {
            apply_exec_report(er, intent_out);
            did_work = true;
        }

        strategy::OracleEvent oe;
        if (oracle_in.try_pop(oe)) {
            evaluate_oracle_event(oe, intent_out);
            did_work = true;
        }

        if (!did_work) cpu_relax();
    }
}

} // namespace px::pipeline
