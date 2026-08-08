// tests/test_replay_determinism.cpp — the multithreaded pipeline's
// correctness net.
//
// SCOPE, stated precisely rather than overclaimed: this checks that a fixed,
// file-shaped sequence of WS frames, pushed through the real production path
// (parse_feed_message -> Ingress::handle_message -> ring A -> a real Brain
// running on its own thread), converges to an exact, predictable final
// ShadowBook state — regardless of exactly how the OS happened to schedule
// that thread. It does NOT claim byte-identical determinism for the
// oracle/signal/risk path, because that path genuinely depends on wall-clock
// time (RiskEngine's token bucket, see risk/risk_engine.hpp) and a live
// synthetic oracle feed's own sleep_for cadence — claiming determinism there
// would be exactly the kind of unverified claim this project tries not to
// make. No oracle feed thread runs in this test for that reason; only the
// book-update path is under test, same as tools/kalshi_pipeline.cpp's
// --replay mode (see its header comment for the same scoping).
#include "pipeline/brain.hpp"
#include "pipeline/ingress.hpp"
#include "risk_engine.hpp"
#include "test_harness.hpp"
#include "ticker_registry.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace px;
using namespace px::live;
using namespace px::pipeline;
using namespace px::strategy; // OracleRing

namespace {

// A fixed sequence of raw WS-message frames, schema-verified shape (same as
// tests/test_feed_parse.cpp's golden frames — quoted decimal strings, the
// real AsyncAPI field names). Deliberately gap-free: SeqTracker's gap
// detection is already covered in isolation by test_feed_parse.cpp; mixing it
// in here would conflate "does resync fire" with "is the converged state
// deterministic," two different claims.
const std::vector<std::string> kFrames = {
    R"({"type":"orderbook_snapshot","sid":2,"seq":1,)"
    R"("msg":{"market_ticker":"REPLAY-TEST","market_id":"m",)"
    R"("yes_dollars_fp":[["0.4000","100.00"],["0.6500","30.00"]],)"
    R"("no_dollars_fp":[["0.3000","20.00"]]}})",

    R"({"type":"orderbook_delta","sid":2,"seq":2,)"
    R"("msg":{"market_ticker":"REPLAY-TEST","market_id":"m",)"
    R"("price_dollars":"0.4000","delta_fp":"-25.00","side":"yes"}})",

    R"({"type":"orderbook_delta","sid":2,"seq":3,)"
    R"("msg":{"market_ticker":"REPLAY-TEST","market_id":"m",)"
    R"("price_dollars":"0.3000","delta_fp":"5.00","side":"no"}})", // NO bid @30c -> YES ask @70c

    R"({"type":"ticker","sid":3,)"
    R"("msg":{"market_ticker":"REPLAY-TEST","market_id":"m","price_dollars":"0.5000",)"
    R"("yes_bid_dollars":"0.4000","yes_ask_dollars":"0.6500","yes_bid_size_fp":"75.00",)"
    R"("yes_ask_size_fp":"30.00","last_trade_size_fp":"1.00","volume_fp":"1.00",)"
    R"("open_interest_fp":"1.00","dollar_volume":1,"dollar_open_interest":1,)"
    R"("ts_ms":1,"time":"2024-01-01T00:00:00Z"}})",
};

constexpr std::size_t kExpectedBookEvents = 4 /* SnapshotBegin + 3 levels */ + 1 /* delta */ + 1 /* delta */ +
                                             1 /* ticker */;

// Feeds kFrames through the real Ingress::handle_message -> ring A -> a real
// Brain thread, waits (wall-clock bounded, not iteration-count bounded — see
// tests/test_gateway_dry_run.cpp for why) for every event to be applied, and
// returns the converged ShadowBook so the caller can assert its exact state.
[[nodiscard]] ShadowBook run_once(MarketId market, TickerRegistry& registry) {
    KalshiConfig ws_cfg; // never connected — handle_message doesn't touch it
    Ingress ingress(ws_cfg, registry);

    RingA ring_a;
    RingC ring_c; // unused (no exec reports in this test) but required by Brain::run's signature
    RingB ring_b; // unused (no signals registered, so nothing is ever pushed to it)
    OracleRing oracle_ring; // left empty — no oracle thread, see file header

    risk::RiskConfig risk_cfg;
    Brain brain(risk_cfg);

    std::atomic<bool> stop{false};
    std::thread brain_thread([&] { brain.run(ring_a, oracle_ring, ring_c, ring_b, stop); });

    for (const auto& raw : kFrames) {
        FeedMsg fm;
        std::string err;
        if (parse_feed_message(raw, fm, err)) ingress.handle_message(fm, ring_a);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (brain.book_events_applied() < kExpectedBookEvents && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    stop.store(true, std::memory_order_relaxed);
    brain_thread.join();

    return brain.book(market); // by value — thread has already joined, no lifetime issue
}

void test_replay_converges_to_exact_expected_state() {
    TickerRegistry registry;
    MarketId market;
    PX_CHECK(registry.add("REPLAY-TEST", market));

    const ShadowBook book = run_once(market, registry);

    // Expected, worked through by hand against kFrames above:
    //   snapshot:  YES bid 40c qty 100, YES bid 65c qty 30,
    //              NO bid 30c qty 20 -> normalizes to YES ask 70c qty 20
    //   delta 1:   YES 40c qty -25  -> 100-25 = 75
    //   delta 2:   NO  30c qty +5   -> ask-side 70c: 20+5 = 25
    //   final:     Buy@40=75, Buy@65=30, Sell@70=25
    //   -> best_bid = 65 (higher of the two bids), best_ask = 70 (only ask)
    Price bb = 0, ba = 0;
    PX_CHECK(book.best_bid(bb) && bb == 65); // higher of the two YES-side bids (40c, 65c)
    PX_CHECK(book.best_ask(ba) && ba == 70); // the only ask-side (NO-derived) level

    PX_CHECK(book.level_qty(40) == 75);
    PX_CHECK(book.level_qty(65) == 30);
    PX_CHECK(book.level_qty(70) == 25);
}

void test_replay_is_repeatable_across_independent_runs() {
    // Two fully independent Brain/thread/ring instances, fed the identical
    // frame sequence, must converge to bit-identical book state — this is the
    // actual "determinism" claim: not that timing is identical (it isn't),
    // but that the *result* doesn't depend on it.
    TickerRegistry registry;
    MarketId market;
    PX_CHECK(registry.add("REPLAY-TEST", market));

    const ShadowBook a = run_once(market, registry);
    const ShadowBook b = run_once(market, registry);

    Price a_bb = 0, a_ba = 0, b_bb = 0, b_ba = 0;
    PX_CHECK(a.best_bid(a_bb) == b.best_bid(b_bb) && a_bb == b_bb);
    PX_CHECK(a.best_ask(a_ba) == b.best_ask(b_ba) && a_ba == b_ba);
    for (Price p = kMinPrice; p <= kMaxPrice; ++p) {
        PX_CHECK(a.level_qty(p) == b.level_qty(p));
    }
}

} // namespace

int main() {
    test_replay_converges_to_exact_expected_state();
    test_replay_is_repeatable_across_independent_runs();
    PX_TEST_REPORT_AND_RETURN();
}
