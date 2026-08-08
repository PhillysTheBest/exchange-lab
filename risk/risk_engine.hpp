// risk/risk_engine.hpp — pre-trade risk gate for the live pipeline.
//
// Not a thread, not a service: a single check() call at the very end of
// strategy logic in T2 Brain, immediately before an OrderIntent is pushed to
// ring B — exactly the placement specified for it. Every check is
// integer-only. Kalshi contracts settle at 0 or 100 cents, so the maximum
// possible loss per contract is hard-capped and known at order-entry time;
// there is no margin model to compute beyond that bound, so this does exactly
// three things and nothing more: survive the rate limit, cap exposure, and
// reject fat-fingered prices. No escrow, no drawdown model, no kill switch —
// deliberately out of scope (see px/trader.hpp for the documented, separate
// escrow gap this does not attempt to close).
#pragma once

#include "px/types.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace px::risk {

enum class Verdict : std::uint8_t {
    Accept,
    RejectFatFinger,
    RejectExposure,
    RejectRateLimited,
};

[[nodiscard]] inline const char* to_string(Verdict v) noexcept {
    switch (v) {
        case Verdict::Accept:            return "accept";
        case Verdict::RejectFatFinger:   return "fat_finger";
        case Verdict::RejectExposure:    return "exposure_limit";
        case Verdict::RejectRateLimited: return "rate_limited";
    }
    return "unknown";
}

// Integer token bucket. Exact when `refill_per_sec` evenly divides
// 1'000'000'000 (true for Kalshi's documented Basic-tier write rate of
// 100/sec: 1e9/100 = 1e7 ns/token exactly). For tiers where it doesn't divide
// evenly, integer truncation in ns_per_token biases the refill rate very
// slightly *slow* — the safe direction for a self-throttle whose entire job
// is staying under the exchange's real limit, never over it.
class TokenBucket {
public:
    TokenBucket(std::int64_t capacity, std::int64_t refill_per_sec) noexcept
        : capacity_(capacity), tokens_(capacity),
          ns_per_token_(refill_per_sec > 0 ? 1'000'000'000LL / refill_per_sec : 1) {}

    // `now_ns` is any monotonic nanosecond clock the caller controls — real
    // wall time in production, an injected fake in tests (see
    // tests/test_risk.cpp). Returns true and deducts `cost` tokens iff enough
    // were available.
    [[nodiscard]] bool try_consume(std::int64_t cost, std::int64_t now_ns) noexcept {
        refill(now_ns);
        if (tokens_ < cost) return false;
        tokens_ -= cost;
        return true;
    }

    [[nodiscard]] std::int64_t tokens_available(std::int64_t now_ns) noexcept {
        refill(now_ns);
        return tokens_;
    }

    // Forces the bucket to empty — see RiskEngine::on_rate_limited_response.
    void drain(std::int64_t now_ns) noexcept {
        refill(now_ns);
        tokens_ = 0;
    }

private:
    void refill(std::int64_t now_ns) noexcept {
        if (!primed_) { last_ns_ = now_ns; primed_ = true; return; } // first call establishes the baseline
        const std::int64_t elapsed = now_ns - last_ns_;
        if (elapsed <= 0) return; // clock went backwards or didn't move — nothing to add
        last_ns_ = now_ns;
        carry_ns_ += elapsed;
        const std::int64_t gained = carry_ns_ / ns_per_token_;
        if (gained <= 0) return;
        carry_ns_ -= gained * ns_per_token_;
        tokens_ = std::min(capacity_, tokens_ + gained);
    }

    std::int64_t capacity_;
    std::int64_t tokens_;
    std::int64_t ns_per_token_;
    std::int64_t carry_ns_ = 0;
    std::int64_t last_ns_ = 0;
    bool primed_ = false;
};

struct RiskConfig {
    // Kalshi Basic tier: 100 write-tokens/sec, 10 tokens/request, a 1-second
    // bucket capacity for Basic specifically (larger tiers get a 2-second
    // capacity — see docs.kalshi.com/getting_started/rate_limits, the plan
    // doc's Sources). This defaults to Basic: ~10 orders/sec sustained, burst
    // up to 10 instantaneous from a full bucket. Tune per your actual tier.
    std::int64_t bucket_capacity_tokens = 100;
    std::int64_t bucket_refill_per_sec  = 100;
    std::int64_t tokens_per_order       = 10;
    // Kalshi's documented per-request cost (10 tokens) is stated for requests
    // generally, not broken out by endpoint — whether a cancel actually costs
    // the same as a create is an assumption, not a confirmed fact, same
    // epistemic status as tokens_per_order's own citation above. Kept as an
    // independently tunable field (default matching tokens_per_order) rather
    // than hard-reusing that field in code, so it can be corrected in one
    // place if real per-endpoint cost data ever surfaces.
    std::int64_t tokens_per_cancel      = 10;

    Cash  max_exposure_cents = 10'000; // e.g. $100 max |position| notional per market
    Price fat_finger_min     = 5;
    Price fat_finger_max     = 95;
};

class RiskEngine {
public:
    explicit RiskEngine(const RiskConfig& cfg) noexcept
        : cfg_(cfg), bucket_(cfg.bucket_capacity_tokens, cfg.bucket_refill_per_sec) {}

    // Checked in the order a broken strategy would actually fail them: price
    // sanity first (cheapest, and catches garbage regardless of market/rate
    // state), then exposure (still pure local state), then the rate limit —
    // the one check with an external, time-varying side effect (token
    // consumption), so it's only ever spent on an intent that already passed
    // the other two.
    [[nodiscard]] Verdict check(MarketId market, Side side, Price price, Qty qty, std::int64_t now_ns) noexcept {
        if (price < cfg_.fat_finger_min || price > cfg_.fat_finger_max) return Verdict::RejectFatFinger;

        const Position signed_qty = (side == Side::Buy) ? static_cast<Position>(qty) : -static_cast<Position>(qty);
        const Position projected = position_[market] + signed_qty;
        const Cash projected_notional = (projected < 0 ? -projected : projected) * 100;
        if (projected_notional > cfg_.max_exposure_cents) return Verdict::RejectExposure;

        if (!bucket_.try_consume(cfg_.tokens_per_order, now_ns)) return Verdict::RejectRateLimited;
        return Verdict::Accept;
    }

    // A cancel only ever reduces exposure, so fat-finger/exposure checks
    // don't apply — this exists purely to make cancels pay the rate limiter
    // too (see tokens_per_cancel's caveat above), so a strategy that
    // cancels-and-replaces in a loop can't bypass the same budget a stream of
    // new orders would be subject to.
    //
    // Known gap, not fixed here: this engine's exposure check (above) only
    // ever accounts for *filled* position (via record_fill) — resting
    // exposure from GTC orders that haven't traded yet is invisible to it. A
    // market-making strategy resting quotes on both sides at once
    // (strategy/passive_market_maker.hpp) therefore has real worst-case
    // exposure this check never saw at any single order's entry time. Same
    // "advisory, not escrowed" simplification px/trader.hpp already documents
    // for the offline engine; staying out of an escrow model is a deliberate
    // scope boundary, not an oversight.
    [[nodiscard]] Verdict check_cancel(std::int64_t now_ns) noexcept {
        if (!bucket_.try_consume(cfg_.tokens_per_cancel, now_ns)) return Verdict::RejectRateLimited;
        return Verdict::Accept;
    }

    // Called by T2 when ring C delivers a fill for one of our own orders, so
    // the exposure check tracks real inventory rather than only our own sent
    // intents (an intent might partially fill, or another session on the same
    // key might trade).
    void record_fill(MarketId market, Side side, Qty qty) noexcept {
        position_[market] += (side == Side::Buy) ? static_cast<Position>(qty) : -static_cast<Position>(qty);
    }

    // A 429 from Kalshi means our local bucket over-estimated remaining
    // budget (e.g. another session on the same API key spent tokens we didn't
    // know about). Draining to empty resyncs us to the exchange's real view
    // instead of continuing to optimistically spend against a bucket that's
    // actually already empty — this is the "ring C closes the loop" part of
    // the rate-limit story, not just self-throttling in a vacuum.
    void on_rate_limited_response(std::int64_t now_ns) noexcept { bucket_.drain(now_ns); }

    [[nodiscard]] Position position(MarketId market) const noexcept { return position_[market]; }
    [[nodiscard]] std::int64_t tokens_available(std::int64_t now_ns) noexcept {
        return bucket_.tokens_available(now_ns);
    }

private:
    RiskConfig cfg_;
    TokenBucket bucket_;
    std::array<Position, kMaxMarkets> position_{};
};

} // namespace px::risk
