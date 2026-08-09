// pipeline/brain.hpp — T2: the sole owner of live market/risk/order state.
//
// Drains three rings on one pinned thread and touches nothing else: ring A
// (book/ticker/trade updates from T1) mutates ShadowBook state (and, for
// Trade, the market's TradeWindow); the OracleRing (reused as-is from
// strategy/oracle_feed.hpp — the same one tools/strategy_demo.cpp drives
// locally) triggers strategy evaluation against that state; ring C (reports
// from T3) reconciles RiskEngine's notion of real inventory/rate budget and
// OrderTracker's notion of this account's own resting orders. Accepted
// intents (new orders AND cancels) go out on ring B. No crypto, no sockets,
// no std::string on the hot path — that's what T1 and T3 are for (see the
// plan doc's "why T3 exists" measurement: RSA-PSS signing is
// ~15,000-30,000x a book update and must never run on this thread).
//
// Stage 3: Signal -> Strategy. A book/trade mutation on a market now ALSO
// dispatches TriggerKind::BookUpdate to every strategy registered on that
// market (not just oracle events, as Stage 1 originally built it — see the
// plan doc's Context section for why that was the whole point of this
// stage). An Acked/Fill/Cancelled report dispatches TriggerKind::OwnOrderUpdate
// the same way, immediately, rather than waiting for the next incidental
// book tick — see strategy/strategy.hpp's TriggerKind doc comment for why
// that responsiveness needs its own trigger. Oracle events remain
// undispatched-by-market (every registered strategy sees every oracle event
// and filters for its own interest internally, same as Stage 1) since a
// single real-world event can legitimately matter to several differently-
// registered strategies at once.
//
// SIZE WARNING (found the hard way — an ad-hoc stack-local Brain in a test
// during Stage 3 development crashed with STATUS_STACK_OVERFLOW): Brain
// embeds one ShadowBook, one TradeWindow, and OrderTracker's per-market table
// by value, per kMaxMarkets — sizeof(Brain) is currently ~3.0 MB (measured;
// was ~780 KB before Stage 5 raised kMaxMarkets 256->1024 to reach its
// multi-market scaling sweep's 500/1000-market points — see px/types.hpp's
// kMaxMarkets comment and PERFORMANCE.md's "Multi-market scaling" section).
// That's exactly right as a heap-allocated object (every real call site uses
// std::make_unique<Brain> — see tools/kalshi_pipeline.cpp,
// tests/test_replay_determinism.cpp, tests/test_replay_fuzz.cpp) and exactly
// wrong as a stack local or by-value parameter — the same SIZE WARNING
// px/order_book.hpp already documents for OrderBook (~5.7 MB); this is now a
// comparably-sized instance of the identical hazard, not a new kind of
// problem.
#pragma once

#include "pipeline/events.hpp"
#include "pipeline/ingress.hpp" // RingA — declared alongside T1, which owns/produces it
#include "px/shadow_book.hpp"
#include "px/spsc_ring.hpp"
#include "px/types.hpp"
#include "risk_engine.hpp"
#include "strategy/market_state.hpp" // MarketState + extract_features — strategy.hpp only forward-declares it
#include "strategy/oracle_feed.hpp"
#include "strategy/order_tracker.hpp"
#include "strategy/strategy.hpp"
#include "strategy/trade_window.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace px::pipeline {

// RingB (OrderIntent) and RingC (ExecReport) are declared in events.hpp,
// alongside the payload types they carry — Brain is a consumer/producer of
// both, not their owner, and Gateway (T3) needs the same two ring types
// without pulling in Brain's heavier Strategy/RiskEngine dependencies.

class Brain {
public:
    explicit Brain(const risk::RiskConfig& risk_cfg) : risk_(risk_cfg) { market_status_.fill(MarketStatus::Open); }

    // `market` is which ShadowBook/MarketState this strategy reads when it
    // fires, for every trigger kind — see strategy/strategy.hpp's TriggerKind.
    //
    // Stage 5, Section 10: also records this entry's index into
    // strategy_index_by_market_[market] — see that member's own comment for
    // why. add_strategy is a one-time setup call (never on the hot path, same
    // tier as Brain's own constructor), so paying an extra vector push here
    // to make every future dispatch_market_triggered call O(this market's own
    // strategies) instead of O(all registered strategies) is free in
    // practice.
    void add_strategy(MarketId market, std::unique_ptr<strategy::Strategy> strat) {
        strategy_index_by_market_[market].push_back(strategies_.size());
        strategies_.emplace_back(market, std::move(strat));
    }

    // Runs until stop_flag is set, round-robin draining one event from each
    // ring per iteration and busy-spinning (never yielding — see
    // px/platform.hpp's cpu_relax) when all three are empty. Intended as a
    // std::thread entry point, pinned to a P-core (see
    // performance_core_ids/pin_current_thread_to) by the caller before
    // invoking this — pinning is this function's caller's job, not its own,
    // so it stays testable off a pinned thread too.
    void run(RingA& book_in, strategy::OracleRing& oracle_in, RingC& exec_in, RingB& intent_out,
             std::atomic<bool>& stop_flag);

    std::function<void(const OrderIntent&, risk::Verdict)> on_reject; // optional, for logging/metrics only

    [[nodiscard]] std::uint64_t book_events_applied() const noexcept { return book_events_applied_; }
    [[nodiscard]] std::uint64_t intents_accepted() const noexcept { return intents_accepted_; }
    [[nodiscard]] std::uint64_t intents_rejected() const noexcept { return intents_rejected_; }
    [[nodiscard]] const ShadowBook& book(MarketId m) const noexcept { return books_[m]; }
    [[nodiscard]] const risk::RiskEngine& risk() const noexcept { return risk_; }
    [[nodiscard]] const strategy::OrderTracker& order_tracker() const noexcept { return order_tracker_; }
    [[nodiscard]] MarketStatus market_status(MarketId m) const noexcept { return market_status_[m]; }

    // Stage 6 (eval/backtest_runner.hpp): promoted from private to public so a
    // single-threaded, fully deterministic backtest driver can call these directly —
    // exactly the same dispatch/risk/strategy code path run() uses internally when it
    // pops from a ring, just invoked synchronously instead of via SpscRing +
    // std::thread. run() below is unchanged and still the right entry point for the
    // live pipeline (real threads, real cross-core rings); this is NOT a second
    // implementation of Brain's logic, it's the same one made callable without a ring
    // in between. Why the eval framework needs synchronous, not threaded, dispatch:
    // see docs/BACKTEST_ASSESSMENT.md's "why not reuse run()" section — a backtest's
    // entire value proposition is reproducibility, and interleaving ring A/ring C
    // processing order via OS thread scheduling (what run() does) is exactly the kind
    // of nondeterminism this project's own test_replay_determinism.cpp already flags
    // as unsafe to claim outside the book-update-only path.
    void apply_book_event(const BookEvent& e, RingB& intent_out) noexcept;
    void apply_exec_report(const ExecReport& e, RingB& intent_out) noexcept;

private:
    void dispatch_market_triggered(MarketId market, strategy::TriggerKind trigger, RingB& intent_out) noexcept;
    void evaluate_oracle_event(const strategy::OracleEvent& e, RingB& intent_out) noexcept;
    void handle_decision(MarketId market, const strategy::ExecutionDecision& decision, RingB& intent_out) noexcept;

    // ~450 KB total (1024 * ~440 B; was ~110 KB at the pre-Stage-5
    // kMaxMarkets=256 — see px/types.hpp's kMaxMarkets comment) as declared,
    // but only the entries the registry actually assigned (a handful,
    // contiguous from index 0 — see ticker_registry.hpp) are ever touched,
    // so the real working set stays the few-KB, L1-friendly figure the plan
    // doc's ShadowBook design argues for — the unused tail of this array is
    // never faulted into cache.
    std::array<ShadowBook, kMaxMarkets> books_{};
    std::array<strategy::TradeWindow, kMaxMarkets> trade_windows_{};
    strategy::OrderTracker order_tracker_; // already indexed internally per-market — one instance, not an array

    // Stage 4: per-market lifecycle status, reused from px::MarketStatus
    // (px/types.hpp — already the offline Market's own PreOpen/Open/Halted/
    // Resolved vocabulary, not a parallel enum invented for the live path).
    // Defaults to Open, not PreOpen, deliberately: Kalshi's market_lifecycle_v2
    // channel reports STATE CHANGES from subscription time onward, not a
    // backfill of "here's the current status of everything you're watching"
    // — a market that was already open before this connection subscribed
    // will never get an explicit `activated` event. Defaulting to PreOpen
    // would misread "no lifecycle event has arrived yet" as "not tradeable,"
    // silently blocking every order this pipeline would otherwise have sent
    // — a strictly worse failure mode than the one line above fixes.
    // Explicit `deactivated`/`determined`/`settled` events (see
    // live/kalshi_feed.hpp) still correctly transition it away from Open.
    std::array<MarketStatus, kMaxMarkets> market_status_{};
    std::vector<std::pair<MarketId, std::unique_ptr<strategy::Strategy>>> strategies_;

    // Stage 5, Section 10: dispatch_market_triggered used to linearly scan
    // ALL of strategies_ on every single BookUpdate/OwnOrderUpdate, filtering
    // by MarketId — O(total registered strategies across every market), not
    // O(this market's own). Confirmed as a real, if modest, measurable cost
    // by bench/bench_dispatch_scaling.cpp (a targeted, low-noise, one-event-
    // at-a-time benchmark built specifically to isolate it from parse/
    // generator noise — see PERFORMANCE.md's Section 10 optimization log for
    // the before/after numbers). This index maps MarketId -> the indices
    // into strategies_ registered on that market, so dispatch_market_triggered
    // touches only this market's own strategies. evaluate_oracle_event's own
    // all-strategies scan is UNCHANGED and does NOT use this index — that
    // scan is intentional (a single oracle event can matter to several
    // differently-registered strategies at once), not the bug this fixes.
    std::array<std::vector<std::size_t>, kMaxMarkets> strategy_index_by_market_{};

    risk::RiskEngine risk_;
    OrderId next_order_id_ = 1;

    std::uint64_t book_events_applied_ = 0;
    std::uint64_t intents_accepted_ = 0;
    std::uint64_t intents_rejected_ = 0;
};

} // namespace px::pipeline
