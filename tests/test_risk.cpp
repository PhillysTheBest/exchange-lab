// tests/test_risk.cpp — unit tests for risk/risk_engine.hpp. Every check runs
// against an explicit injected `now_ns`, never the real clock, so refill
// timing is exact and deterministic.
#include "risk_engine.hpp"
#include "test_harness.hpp"

using namespace px;
using namespace px::risk;

namespace {

constexpr std::int64_t kSec = 1'000'000'000;

// ---- TokenBucket ------------------------------------------------------------

void test_bucket_starts_full_and_exhausts() {
    TokenBucket b(/*capacity=*/100, /*refill_per_sec=*/100);
    std::int64_t t = 0;
    for (int i = 0; i < 10; ++i) PX_CHECK(b.try_consume(10, t)); // 10 orders @ 10 tokens = exactly the full bucket
    PX_CHECK(!b.try_consume(10, t)); // exhausted, no time has passed
}

void test_bucket_refills_over_time() {
    TokenBucket b(100, 100); // 100 tokens/sec == 1 token per 10ms
    std::int64_t t = 0;
    PX_CHECK(b.tokens_available(t) == 100);
    for (int i = 0; i < 10; ++i) PX_CHECK(b.try_consume(10, t));
    PX_CHECK(b.tokens_available(t) == 0);

    t += kSec / 10; // 100ms later -> 10 tokens/sec-of-refill * 0.1s = 10 tokens
    PX_CHECK(b.tokens_available(t) == 10);
    PX_CHECK(b.try_consume(10, t));
    PX_CHECK(!b.try_consume(10, t));
}

void test_bucket_refill_capped_at_capacity() {
    TokenBucket b(100, 100);
    std::int64_t t = 0;
    t += 10 * kSec; // 10 seconds idle — would be 1000 tokens uncapped
    PX_CHECK(b.tokens_available(t) == 100); // capped at capacity
}

void test_bucket_no_fractional_loss_across_many_small_calls() {
    // At 100 tokens/sec, one token corresponds to exactly 10ms. Advance in 3ms
    // steps — deliberately NOT a divisor of 10ms — across 10 separate refill()
    // calls (30ms total, i.e. 3 tokens' worth). A naive implementation that
    // truncates elapsed-time-to-tokens independently on every call would see
    // 3ms < 10ms every single time and gain nothing; carry_ns_ exists so the
    // remainder survives across calls and the total still comes out exact.
    TokenBucket b(1000, 100);
    std::int64_t t = 0;
    PX_CHECK(b.try_consume(1000, t)); // drain the initial full bucket so refill is visible
    PX_CHECK(b.tokens_available(t) == 0);
    constexpr std::int64_t kStep = 3'000'000; // 3ms
    for (int i = 0; i < 10; ++i) {
        t += kStep;
        (void)b.tokens_available(t); // forces a refill() at this timestamp
    }
    PX_CHECK(b.tokens_available(t) == 3);
}

void test_bucket_drain_on_429() {
    TokenBucket b(100, 100);
    std::int64_t t = 0;
    PX_CHECK(b.tokens_available(t) == 100);
    b.drain(t);
    PX_CHECK(b.tokens_available(t) == 0);
}

// ---- RiskEngine::check -------------------------------------------------------

void test_fat_finger_bounds() {
    RiskConfig cfg;
    RiskEngine risk(cfg);
    std::int64_t t = 0;
    PX_CHECK(risk.check(0, Side::Buy, 4, 1, t) == Verdict::RejectFatFinger);
    PX_CHECK(risk.check(0, Side::Buy, 96, 1, t) == Verdict::RejectFatFinger);
    // Boundary values themselves are allowed.
    PX_CHECK(risk.check(1, Side::Buy, 5, 1, t) == Verdict::Accept);
    PX_CHECK(risk.check(2, Side::Buy, 95, 1, t) == Verdict::Accept);
}

void test_exposure_limit_boundary() {
    RiskConfig cfg;
    cfg.max_exposure_cents = 1000; // $10, i.e. 10 contracts at 100 cents max loss each
    cfg.bucket_capacity_tokens = 10'000; // rate limit not under test here
    cfg.bucket_refill_per_sec = 10'000;
    RiskEngine risk(cfg);
    std::int64_t t = 0;

    PX_CHECK(risk.check(0, Side::Buy, 50, 10, t) == Verdict::Accept); // exactly at the limit: 10*100=1000
    risk.record_fill(0, Side::Buy, 10);
    PX_CHECK(risk.position(0) == 10);
    PX_CHECK(risk.check(0, Side::Buy, 50, 1, t) == Verdict::RejectExposure); // 11 contracts would exceed it

    // Selling back down is fine even while at the limit.
    PX_CHECK(risk.check(0, Side::Sell, 50, 1, t) == Verdict::Accept);

    // A short position is bounded the same way as a long one (absolute value).
    RiskEngine risk2(cfg);
    PX_CHECK(risk2.check(1, Side::Sell, 50, 10, t) == Verdict::Accept);
    risk2.record_fill(1, Side::Sell, 10);
    PX_CHECK(risk2.position(1) == -10);
    PX_CHECK(risk2.check(1, Side::Sell, 50, 1, t) == Verdict::RejectExposure);
}

void test_rate_limit_rejects_after_bucket_exhausted() {
    RiskConfig cfg; // defaults: 100 cap, 100/sec refill, 10/order -> 10 orders burst
    cfg.max_exposure_cents = 1'000'000; // exposure not under test here
    RiskEngine risk(cfg);
    std::int64_t t = 0;
    for (int i = 0; i < 10; ++i) {
        PX_CHECK(risk.check(0, Side::Buy, 50, 1, t) == Verdict::Accept);
    }
    PX_CHECK(risk.check(0, Side::Buy, 50, 1, t) == Verdict::RejectRateLimited);

    t += kSec; // a full second later, the bucket should have refilled
    PX_CHECK(risk.check(0, Side::Buy, 50, 1, t) == Verdict::Accept);
}

void test_check_order_fat_finger_before_rate_limit() {
    // Exhaust the bucket, then confirm a fat-fingered price still reports as
    // RejectFatFinger, not RejectRateLimited — price sanity is checked first
    // and must not consume/depend on rate-limit state.
    RiskConfig cfg;
    cfg.max_exposure_cents = 1'000'000;
    RiskEngine risk(cfg);
    std::int64_t t = 0;
    for (int i = 0; i < 10; ++i) (void)risk.check(0, Side::Buy, 50, 1, t);
    PX_CHECK(risk.check(0, Side::Buy, 200, 1, t) == Verdict::RejectFatFinger);
}

void test_on_rate_limited_response_forces_reject() {
    RiskConfig cfg;
    cfg.max_exposure_cents = 1'000'000;
    RiskEngine risk(cfg);
    std::int64_t t = 0;
    PX_CHECK(risk.check(0, Side::Buy, 50, 1, t) == Verdict::Accept); // primes the bucket, spends 10
    risk.on_rate_limited_response(t); // simulate an unexpected 429 from Kalshi
    PX_CHECK(risk.check(0, Side::Buy, 50, 1, t) == Verdict::RejectRateLimited);
}

} // namespace

int main() {
    test_bucket_starts_full_and_exhausts();
    test_bucket_refills_over_time();
    test_bucket_refill_capped_at_capacity();
    test_bucket_no_fractional_loss_across_many_small_calls();
    test_bucket_drain_on_429();
    test_fat_finger_bounds();
    test_exposure_limit_boundary();
    test_rate_limit_rejects_after_bucket_exhausted();
    test_check_order_fat_finger_before_rate_limit();
    test_on_rate_limited_response_forces_reject();
    PX_TEST_REPORT_AND_RETURN();
}
