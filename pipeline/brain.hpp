// pipeline/brain.hpp — T2: the sole owner of live market/risk state.
//
// Drains three rings on one pinned thread and touches nothing else: ring A
// (book/ticker updates from T1) mutates ShadowBook state; the OracleRing
// (reused as-is from strategy/oracle_feed.hpp — the same one
// tools/strategy_demo.cpp drives locally) triggers signal evaluation against
// that state; ring C (execution reports from T3) reconciles RiskEngine's
// notion of real inventory and rate-limit budget. Accepted intents go out on
// ring B. No crypto, no sockets, no std::string on the hot path — that's what
// T1 and T3 are for (see the plan doc's "why T3 exists" measurement: RSA-PSS
// signing is ~10,000x a book update and must never run on this thread).
//
// IMPORTANT — this reuses px::strategy::Signal exactly as Phase 1 built it:
// Signal::evaluate takes an OracleEvent, not a book event (see
// strategy/signal.hpp). A book update alone never triggers evaluation; an
// oracle event does, using whatever the book currently looks like as context.
// Each registered signal is offered every oracle event and filters for its
// own (source, key) internally (see ThresholdSignal) — Brain doesn't
// pre-route events to signals.
#pragma once

#include "pipeline/events.hpp"
#include "pipeline/ingress.hpp" // RingA — declared alongside T1, which owns/produces it
#include "px/shadow_book.hpp"
#include "px/spsc_ring.hpp"
#include "px/types.hpp"
#include "risk_engine.hpp"
#include "strategy/oracle_feed.hpp"
#include "strategy/signal.hpp"

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
// without pulling in Brain's heavier Signal/RiskEngine dependencies.

class Brain {
public:
    explicit Brain(const risk::RiskConfig& risk_cfg) : risk_(risk_cfg) {}

    // `market` is which ShadowBook this signal reads BBO from when it fires.
    void add_signal(MarketId market, std::unique_ptr<strategy::Signal> signal) {
        signals_.emplace_back(market, std::move(signal));
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

private:
    void apply_book_event(const BookEvent& e) noexcept;
    void apply_exec_report(const ExecReport& e) noexcept;
    void evaluate_signals(const strategy::OracleEvent& e, RingB& intent_out);

    // ~110 KB total (256 * ~440 B) as declared, but only the entries the
    // registry actually assigned (a handful, contiguous from index 0 — see
    // ticker_registry.hpp) are ever touched, so the real working set stays
    // the few-KB, L1-friendly figure the plan doc's ShadowBook design argues
    // for — the unused tail of this array is never faulted into cache.
    std::array<ShadowBook, kMaxMarkets> books_{};
    std::vector<std::pair<MarketId, std::unique_ptr<strategy::Signal>>> signals_;
    risk::RiskEngine risk_;

    std::uint64_t book_events_applied_ = 0;
    std::uint64_t intents_accepted_ = 0;
    std::uint64_t intents_rejected_ = 0;
};

} // namespace px::pipeline
