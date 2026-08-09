// tests/test_workload_generator.cpp — Stage 5, Section 3: proves
// workload::Generator's output is actually usable, not just plausible-
// looking. Two claims, each checked directly rather than assumed:
//
//   1. Every frame it produces, across all three profiles and every
//      adversarial scenario kind, is valid enough to pass
//      live::parse_feed_message — a malformed generator would silently
//      invalidate every benchmark built on top of it (Sections 4/5/6), so
//      this is checked with the real parser, not a hand-rolled shortcut.
//   2. Driving a real Ingress -> Brain (multiple markets, a real thread,
//      the same production path tests/test_replay_determinism.cpp uses)
//      with the generator's output for all three profiles never hangs,
//      never crashes, and leaves every touched market's ShadowBook in a
//      structurally sane state (best_bid/best_ask, when present, are valid
//      prices) — proof the adversarial scenarios (seq gaps, duplicates,
//      stale replays, reconnect resyncs) interact correctly with Ingress's
//      already-tested SeqTracker/resync logic, not a new mechanism of
//      their own.
//
// Does NOT assert an exact final book state (test_replay_determinism.cpp's
// job, for a fixed non-random frame sequence) — this generator is
// deliberately randomized, so "converges to sane state without hanging or
// crashing" is the right claim to make, not "converges to THIS state."
#include "pipeline/brain.hpp"
#include "pipeline/ingress.hpp"
#include "risk_engine.hpp"
#include "test_harness.hpp"
#include "ticker_registry.hpp"
#include "workload/generator.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using namespace px;
using namespace px::live;
using namespace px::pipeline;
using namespace px::strategy; // OracleRing
using namespace px::workload;

namespace {

void test_every_frame_parses(Profile profile, const char* label) {
    GeneratorConfig cfg;
    cfg.seed = 42;
    cfg.num_markets = 8;
    cfg.profile = profile;
    cfg.market_dist = MarketDist::Skewed;
    Generator gen(cfg);

    int ok = 0, bad = 0;
    std::string raw; // reused across calls — see GeneratedFrame's header comment
    for (int i = 0; i < 5000; ++i) {
        (void)gen.next_frame(raw); // metadata (market/kind) not needed by this test
        FeedMsg fm;
        std::string err;
        if (parse_feed_message(raw, fm, err)) {
            ++ok;
        } else {
            ++bad;
            std::printf("  [%s] parse failed on frame %d: %s -- %s\n", label, i, raw.c_str(), err.c_str());
        }
    }
    PX_CHECK(bad == 0);
    PX_CHECK(ok == 5000);
}

// Drives `frame_count` generated frames through a real Ingress -> ring A ->
// real Brain thread (no strategies registered — this test is about the
// market-data path's robustness, not decisions), then waits briefly for
// whatever got applied to settle and returns the Brain for the caller to
// inspect. Same threading/synchronization shape as
// tests/test_replay_determinism.cpp's run_once, generalized to N markets and
// a generator instead of a fixed frame list.
[[nodiscard]] std::unique_ptr<Brain> drive_generator(Profile profile, std::size_t num_markets, int frame_count,
                                                       TickerRegistry& registry) {
    KalshiConfig ws_cfg;
    Ingress ingress(ws_cfg, registry);

    RingA ring_a;
    RingC ring_c;
    RingB ring_b;
    OracleRing oracle_ring;

    risk::RiskConfig risk_cfg;
    auto brain = std::make_unique<Brain>(risk_cfg); // heap — see pipeline/brain.hpp's SIZE WARNING

    std::atomic<bool> stop{false};
    std::thread brain_thread([&] { brain->run(ring_a, oracle_ring, ring_c, ring_b, stop); });

    GeneratorConfig cfg;
    cfg.seed = 42;
    cfg.num_markets = num_markets;
    cfg.profile = profile;
    cfg.market_dist = MarketDist::Skewed;
    Generator gen(cfg);

    int parsed = 0;
    std::string raw; // reused across calls — see GeneratedFrame's header comment
    for (int i = 0; i < frame_count; ++i) {
        (void)gen.next_frame(raw); // metadata (market/kind) not needed by this test
        FeedMsg fm;
        std::string err;
        if (parse_feed_message(raw, fm, err)) {
            ingress.handle_message(fm, ring_a);
            ++parsed;
        }
    }

    // Wall-clock bounded, not iteration-bounded — same reasoning as every
    // other multithreaded test in this suite (see test_gateway_dry_run.cpp):
    // we don't know the exact expected book_events_applied() count (seq
    // gaps/duplicates/stale replays are deliberately dropped by design), so
    // this waits for the ring to visibly drain rather than for a specific
    // count.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ring_a.size_approx() > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    // A little extra settling time for Brain to finish whatever it already
    // popped.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    stop.store(true, std::memory_order_relaxed);
    brain_thread.join();

    PX_CHECK(parsed > 0);
    PX_CHECK(brain->book_events_applied() > 0); // real progress was made, not a silent no-op
    return brain;
}

void test_generator_drives_real_pipeline_without_hanging_or_crashing(Profile profile, const char* label) {
    TickerRegistry registry;
    constexpr std::size_t kMarkets = 10;
    std::vector<MarketId> ids;
    for (std::size_t i = 0; i < kMarkets; ++i) {
        MarketId id = 0;
        PX_CHECK(registry.add("WL-MKT-" + std::to_string(i), id));
        ids.push_back(id);
    }

    const auto brain = drive_generator(profile, kMarkets, 3000, registry);

    int markets_with_state = 0;
    for (MarketId id : ids) {
        const ShadowBook& book = brain->book(id);
        Price bb = 0, ba = 0;
        const bool have_bid = book.best_bid(bb);
        const bool have_ask = book.best_ask(ba);
        if (have_bid) {
            PX_CHECK(bb >= kMinPrice && bb <= kMaxPrice);
            ++markets_with_state;
        }
        if (have_ask) PX_CHECK(ba >= kMinPrice && ba <= kMaxPrice);
    }
    // Skewed distribution means most traffic lands on a handful of markets —
    // at least one should have visible state; not every one of the 10 needs
    // to (the whole point of testing the skewed distribution is that most
    // are quiet).
    if (markets_with_state == 0) std::printf("  [%s] warning: no market ended up with a visible best_bid\n", label);
    PX_CHECK(markets_with_state > 0);
}

} // namespace

int main() {
    test_every_frame_parses(Profile::Normal, "normal");
    test_every_frame_parses(Profile::Bursty, "bursty");
    test_every_frame_parses(Profile::Adversarial, "adversarial");

    test_generator_drives_real_pipeline_without_hanging_or_crashing(Profile::Normal, "normal");
    test_generator_drives_real_pipeline_without_hanging_or_crashing(Profile::Bursty, "bursty");
    test_generator_drives_real_pipeline_without_hanging_or_crashing(Profile::Adversarial, "adversarial");

    PX_TEST_REPORT_AND_RETURN();
}
