// tools/kalshi_pipeline.cpp — Stage 2 entry point: the full 3-thread/3-ring
// pipeline from the plan doc, wired together.
//
//   KALSHI --TLS/WS--> [T1 Ingress] --ring A--> [T2 Brain] --ring B--> [T3 Egress]
//                                                    ^                     |
//                                                    +-------ring C--------+
//
// T1 (pipeline::Ingress) owns the socket and does no crypto. T2 (Brain) owns
// all market/risk state on one thread and does no I/O. T3 (live::Gateway)
// does the RSA-PSS signing (measured at ~1ms p50 vs a ~75ns book update — see
// bench/bench_sign.cpp — the entire reason it isn't inline in T2) and REST
// send, gated by a Sink chosen here, once, at startup — never branched on
// per-order (see live/kalshi_gateway.hpp).
//
// SAFETY: defaults to DryRunSink — every order is built, signed, and logged,
// nothing is sent. Real sends require --demo AND KALSHI_API_KEY_ID/
// KALSHI_PRIVATE_KEY_PATH, and even then only ever reach Kalshi's demo host
// (KalshiRestConfig::host default) — production is a change only the caller
// makes explicitly by editing that config, not a flag this tool exposes.
//
// --replay <file> mode drives T1 from a file of raw WS-message JSON lines
// (the same shape golden-frame tests use — see tests/test_feed_parse.cpp)
// through Ingress::handle_message, the exact same decode/seq/BookEvent logic
// run() uses, with zero network — see tests/test_replay_determinism.cpp for
// the correctness property this makes checkable.
#include "kalshi_gateway.hpp"
#include "pipeline/brain.hpp"
#include "pipeline/ingress.hpp"
#include "px/platform.hpp"
#include "ticker_registry.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace px;
using namespace px::live;
using namespace px::pipeline;
using namespace px::strategy;

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s [--replay FILE] [--demo] TICKER [TICKER...]\n"
        "  --replay FILE   drive T1 from a file of raw WS-message JSON lines\n"
        "                  instead of a real connection (see tests/frames/)\n"
        "  --demo          allow T3 to actually send to Kalshi's DEMO host\n"
        "                  (needs KALSHI_API_KEY_ID / KALSHI_PRIVATE_KEY_PATH)\n"
        "                  -- omit this and every order stays dry-run only\n",
        argv0);
}
} // namespace

int main(int argc, char** argv) {
    std::string replay_path;
    bool demo_send = false;
    std::vector<std::string> tickers;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--replay" && i + 1 < argc) {
            replay_path = argv[++i];
        } else if (arg == "--demo") {
            demo_send = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            tickers.push_back(arg);
        }
    }
    if (tickers.empty()) {
        print_usage(argv[0]);
        return 2;
    }

    TickerRegistry registry;
    for (const auto& t : tickers) {
        MarketId id;
        if (!registry.add(t, id)) {
            std::fprintf(stderr, "too many tickers (kMaxMarkets exceeded)\n");
            return 2;
        }
    }

    // ---- credentials (env vars only — never a CLI argument; see kalshi_shadow.cpp) ----
    const char* key_id = std::getenv("KALSHI_API_KEY_ID");
    const char* key_path = std::getenv("KALSHI_PRIVATE_KEY_PATH");
    KalshiConfig ws_cfg;
    KalshiRestConfig rest_cfg;
    if (key_id && key_path) {
        ws_cfg.api_key_id = key_id;
        ws_cfg.private_key_path = key_path;
        rest_cfg.api_key_id = key_id;
        rest_cfg.private_key_path = key_path;
    } else if (replay_path.empty()) {
        std::fprintf(stderr, "KALSHI_API_KEY_ID and KALSHI_PRIVATE_KEY_PATH must both be set "
                              "(unless running --replay, which needs no credentials).\n");
        return 2;
    }
    if (demo_send && !(key_id && key_path)) {
        std::fprintf(stderr, "--demo requires credentials to actually sign anything.\n");
        return 2;
    }

    // ---- the three rings -------------------------------------------------
    RingA ring_a;
    RingB ring_b;
    RingC ring_c;
    OracleRing oracle_ring;

    // ---- T2 Brain: risk config + a demonstration strategy -----------------
    // Real deployment: register whatever Signal your model actually is. This
    // one is the same template strategy_demo.cpp uses — a placeholder showing
    // the wiring works, not a claim that step-function weather modeling is a
    // good trading strategy (see strategy/signal.hpp's own disclaimer).
    risk::RiskConfig risk_cfg; // defaults to Kalshi's Basic tier — see risk_engine.hpp
    Brain brain(risk_cfg);
    brain.add_signal(0 /* first registered ticker */,
                      std::make_unique<ThresholdSignal>(OracleSource::NOAA, "KNYC", /*threshold=*/90.0,
                                                          /*market=*/0, /*trader=*/0, /*edge_cents=*/3, /*qty=*/1));
    brain.on_reject = [](const OrderIntent& intent, risk::Verdict v) {
        std::fprintf(stderr, "[risk] rejected market=%u side=%d price=%d qty=%u: %s\n", intent.market,
                     static_cast<int>(intent.side), intent.price, intent.qty, risk::to_string(v));
    };

    // ---- T3 Gateway: sink chosen once, here, never per-order --------------
    std::unique_ptr<Sink> sink;
    if (demo_send) {
        std::fprintf(stderr, "*** --demo: T3 WILL send real orders to Kalshi's demo host ***\n");
        sink = std::make_unique<LiveSink>(rest_cfg);
    } else {
        sink = std::make_unique<DryRunSink>(rest_cfg);
    }
    Gateway gateway(std::move(sink), registry);
    gateway.on_sent = [](const OrderIntent& intent, const std::string& response, const std::string& error) {
        if (!error.empty()) {
            std::fprintf(stderr, "[gateway] send failed for market=%u: %s\n", intent.market, error.c_str());
        } else {
            std::fprintf(stderr, "[gateway] sent market=%u side=%d price=%d qty=%u -> %.60s...\n", intent.market,
                         static_cast<int>(intent.side), intent.price, intent.qty, response.c_str());
        }
    };

    std::atomic<bool> stop_flag{false};
    const auto cores = performance_core_ids();
    std::fprintf(stderr, "%zu performance core id(s) available for pinning\n", cores.size());

    // ---- T2 thread ---------------------------------------------------------
    std::thread brain_thread([&] {
        if (cores.size() >= 1) pin_current_thread_to(cores[0]);
        brain.run(ring_a, oracle_ring, ring_c, ring_b, stop_flag);
    });

    // ---- T3 thread ---------------------------------------------------------
    std::thread gateway_thread([&] {
        if (cores.size() >= 2) pin_current_thread_to(cores[1]);
        gateway.run(ring_b, ring_c, stop_flag);
    });

    // ---- T1: either a real connection, or a file replayed with zero network
    Ingress ingress(ws_cfg, registry);
    ingress.on_error = [](std::string_view m) {
        std::fprintf(stderr, "[ingress] %.*s\n", static_cast<int>(m.size()), m.data());
    };
    ingress.on_warning = [](std::string_view m) {
        std::fprintf(stderr, "[ingress-warn] %.*s\n", static_cast<int>(m.size()), m.data());
    };

    std::thread oracle_thread;
    std::thread ingress_thread;

    if (!replay_path.empty()) {
        std::ifstream in(replay_path);
        if (!in) {
            std::fprintf(stderr, "cannot open replay file: %s\n", replay_path.c_str());
            g_stop = 1;
        } else {
            std::string line;
            std::size_t applied = 0, skipped = 0;
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                FeedMsg fm;
                std::string err;
                if (parse_feed_message(line, fm, err)) {
                    ingress.handle_message(fm, ring_a);
                    ++applied;
                } else {
                    ++skipped;
                }
            }
            std::fprintf(stderr, "[replay] %zu frame(s) applied, %zu unparseable, from %s\n", applied, skipped,
                         replay_path.c_str());
        }
        // No oracle thread in replay mode — see the file header comment on why
        // replay's determinism claim is scoped to the book-update path only.
    } else {
        ingress_thread = std::thread([&] { ingress.run(ring_a, stop_flag); });
        // Demonstration-only synthetic feed — see strategy/oracle_feed.hpp's
        // own header comment: this project ships no real NOAA/EDGAR/FAA calls.
        oracle_thread = std::thread([&] {
            SyntheticNoaaFeed noaa("KNYC", /*mean_f=*/88.0, /*seed=*/42);
            noaa.run(oracle_ring, stop_flag);
        });
    }

    std::signal(SIGINT, on_sigint);
    std::fprintf(stderr, "running (Ctrl+C to stop)...\n");
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::fprintf(stderr, "[stats] book_events=%llu intents_accepted=%llu intents_rejected=%llu\n",
                     static_cast<unsigned long long>(brain.book_events_applied()),
                     static_cast<unsigned long long>(brain.intents_accepted()),
                     static_cast<unsigned long long>(brain.intents_rejected()));
        if (!replay_path.empty()) break; // replay is a one-shot batch, not a long-running session
    }

    stop_flag.store(true, std::memory_order_relaxed);
    if (ingress_thread.joinable()) ingress_thread.join();
    if (oracle_thread.joinable()) oracle_thread.join();
    brain_thread.join();
    gateway_thread.join();

    std::fprintf(stderr, "stopped. final: book_events=%llu intents_accepted=%llu intents_rejected=%llu\n",
                 static_cast<unsigned long long>(brain.book_events_applied()),
                 static_cast<unsigned long long>(brain.intents_accepted()),
                 static_cast<unsigned long long>(brain.intents_rejected()));
    return 0;
}
