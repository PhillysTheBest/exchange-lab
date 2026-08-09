#include "pipeline/brain.hpp"
#include "px/platform.hpp" // cpu_relax()

#include <chrono>

namespace px::pipeline {

namespace {
// steady_clock, nanoseconds -- for RiskEngine's token bucket (risk/risk_engine.hpp),
// which only ever needs ELAPSED time between calls, never wall-clock alignment, so a
// monotonic-but-arbitrary-epoch clock (immune to NTP step adjustments) is exactly
// right here.
[[nodiscard]] std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// system_clock, milliseconds -- for strategy::extract_features's `now_ms` parameter,
// which strategy/trade_window.hpp's TradeWindow::recent_counts compares DIRECTLY
// against TradeRecord::ts_ms -- itself Kalshi's own real wall-clock,
// milliseconds-since-Unix-epoch trade timestamp (live/kalshi_feed.cpp's trade
// decoder). Found the hard way (Stage 6, building eval/backtest_runner.hpp against a
// real Brain with a real TradeFlow strategy): this call site originally passed
// now_ns() here too -- steady_clock's epoch is arbitrary (commonly time-since-boot,
// never Unix time) and its units are nanoseconds, not milliseconds, so
// `t.ts_ms < (now_ms - kTradeWindowMs)` was comparing two numbers with no defined
// relationship to each other. Silent, not a crash: TradeWindow's ring never actually
// throws for out-of-range values, it just (depending on how the two unrelated scales
// happened to compare) undercounts or fully zeroes out "recent" trades -- and did,
// unconditionally, in Stage 6's own evaluation run: TradeFlow (the one strategy that
// reads MarketState::recent_trades) placed exactly zero orders across 20,000
// synthetic events, traced back to this, not to TradeFlow's own (already
// extensively tested — tests/test_strategies.cpp) threshold logic. Existing tests
// never caught it because tests/test_strategies.cpp exercises TradeWindow/
// extract_features directly with explicit, controlled now_ms values, never through
// this specific Brain call site with a real elapsed-wall-clock gap.
[[nodiscard]] std::int64_t now_ms_wall() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
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
            if (telemetry_) telemetry_->on_lifecycle(e.market, static_cast<std::uint8_t>(e.lifecycle_status));
            break;
    }
    ++book_events_applied_;
    if (telemetry_ && (e.kind == BookEventKind::LevelSet || e.kind == BookEventKind::LevelDelta)) {
        Price bid = 0, ask = 0;
        const bool have_bid = books_[e.market].best_bid(bid);
        const bool have_ask = books_[e.market].best_ask(ask);
        telemetry_->on_book_update(e.market, have_bid, bid, have_ask, ask);
    }
    dispatch_market_triggered(e.market, strategy::TriggerKind::BookUpdate, intent_out);
}

void Brain::apply_exec_report(const ExecReport& e, RingB& intent_out) noexcept {
    switch (e.kind) {
        case ExecKind::Fill:
            risk_.record_fill(e.market, e.side, e.qty);
            order_tracker_.on_fill(e.market, e.id, e.qty);
            // fee_cents hardcoded 0: the live pipeline doesn't compute per-fill fees
            // anywhere today (eval/fees.hpp's Kalshi fee formula is backtest-only —
            // see docs/BACKTEST_ASSESSMENT.md); telemetry reports what Brain actually
            // knows rather than guessing. Revisit once/if T3 starts reporting a real
            // fee alongside a fill.
            if (telemetry_) telemetry_->on_order_filled(e.market, e.side, e.price, e.qty, /*fee_cents=*/0, now_ms_wall());
            break;
        case ExecKind::RateLimited:
            risk_.on_rate_limited_response(now_ns());
            return; // not tied to a specific order's resting state — no OwnOrderUpdate to dispatch
        case ExecKind::Reject:
            // Looked up BEFORE the mutating call below: on_rejected() removes the
            // pending entry outright, so this is the last point side/price (which
            // ExecReport itself doesn't carry for Reject — see events.hpp) are
            // available at all. Same reasoning for Acked/Cancelled below.
            if (telemetry_) {
                if (const auto* r = order_tracker_.find(e.market, e.id))
                    telemetry_->on_order_rejected(e.market, r->side, r->price, e.qty, now_ms_wall());
            }
            order_tracker_.on_rejected(e.market, e.id); // drops the pending entry handle_decision added optimistically
            break;
        case ExecKind::Acked:
            if (telemetry_) {
                if (const auto* r = order_tracker_.find(e.market, e.id))
                    telemetry_->on_order_acked(e.market, r->side, r->price, e.qty, now_ms_wall());
            }
            order_tracker_.on_acked(e.market, e.id, e.qty, books_[e.market]);
            break;
        case ExecKind::Cancelled:
            if (telemetry_) {
                if (const auto* r = order_tracker_.find(e.market, e.id))
                    telemetry_->on_order_cancelled(e.market, r->side, r->price, e.qty, now_ms_wall());
            }
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
            market, books_[market], risk_.position(market), order_tracker_, trade_windows_[market], now_ms_wall());
        const strategy::ExecutionDecision decision = strat.on_event(state, trigger, nullptr);
        handle_decision(market, decision, intent_out, strat.name(), trigger);
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
            market, books_[market], risk_.position(market), order_tracker_, trade_windows_[market], now_ms_wall());
        const strategy::ExecutionDecision decision = strat->on_event(state, strategy::TriggerKind::OracleEvent, &e);
        handle_decision(market, decision, intent_out, strat->name(), strategy::TriggerKind::OracleEvent);
    }
}

void Brain::handle_decision(MarketId market, const strategy::ExecutionDecision& decision, RingB& intent_out,
                             const char* strategy_name, strategy::TriggerKind trigger) noexcept {
    if (decision.action == strategy::ExecutionAction::None || decision.action == strategy::ExecutionAction::Wait) {
        return;
    }

    // Recorded here rather than at the call site: this is the first point
    // every caller (dispatch_market_triggered AND evaluate_oracle_event)
    // agrees an actionable decision (not None/Wait — see the early return
    // above) actually happened, so the counter/ring means the same thing
    // regardless of which trigger produced it.
    if (telemetry_) {
        const Price reason_price = decision.action == strategy::ExecutionAction::Cancel ? 0 : decision.intent.price;
        const Qty reason_qty = decision.action == strategy::ExecutionAction::Cancel ? 0 : decision.intent.qty;
        telemetry_->on_strategy_decision(market, strategy_name, static_cast<std::uint8_t>(trigger),
                                          static_cast<std::uint8_t>(decision.action), reason_price, reason_qty,
                                          /*reason=*/nullptr, now_ms_wall());
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
        if (telemetry_) telemetry_->on_order_submitted(market, intent.side, intent.price, intent.qty, now_ms_wall());
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
            if (telemetry_) telemetry_->on_event_received();
            apply_book_event(be, intent_out);
            if (telemetry_) telemetry_->on_event_processed();
            did_work = true;
        }

        ExecReport er;
        if (exec_in.try_pop(er)) {
            if (telemetry_) telemetry_->on_event_received();
            apply_exec_report(er, intent_out);
            if (telemetry_) telemetry_->on_event_processed();
            did_work = true;
        }

        strategy::OracleEvent oe;
        if (oracle_in.try_pop(oe)) {
            if (telemetry_) telemetry_->on_event_received();
            evaluate_oracle_event(oe, intent_out);
            if (telemetry_) telemetry_->on_event_processed();
            did_work = true;
        }

        if (!did_work) cpu_relax();
    }
}

} // namespace px::pipeline
