// tools/kalshi_shadow.cpp — Phase 3 entry point: connects to Kalshi's WS feed
// and maintains a live shadow OrderBook for one market, printing BBO changes.
//
// Requires real Kalshi API credentials, supplied only via environment
// variables (never as a CLI argument, which would be visible in process
// listings and shell history) — KALSHI_API_KEY_ID and KALSHI_PRIVATE_KEY_PATH.
// Defaults to the demo environment; KALSHI_ENV=prod switches to production
// market data explicitly. This tool only reads market data — it never places
// an order (see kalshi_rest.hpp for the separate, dry-run-by-default order path).
#include "kalshi_feed.hpp"
#include "px/order_book.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using namespace px;
using namespace px::live;

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <MARKET_TICKER>\n"
            "  env: KALSHI_API_KEY_ID, KALSHI_PRIVATE_KEY_PATH  (required)\n"
            "       KALSHI_ENV=demo|prod                        (default: demo)\n",
            argv[0]);
        return 2;
    }
    const std::string ticker = argv[1];

    const char* key_id = std::getenv("KALSHI_API_KEY_ID");
    const char* key_path = std::getenv("KALSHI_PRIVATE_KEY_PATH");
    if (!key_id || !key_path) {
        std::fprintf(stderr,
            "KALSHI_API_KEY_ID and KALSHI_PRIVATE_KEY_PATH must both be set.\n");
        return 2;
    }

    KalshiConfig cfg;
    cfg.api_key_id = key_id;
    cfg.private_key_path = key_path;
    const char* env = std::getenv("KALSHI_ENV");
    if (env && std::string(env) == "prod") {
        cfg.host = "external-api-ws.kalshi.com";
        std::fprintf(stderr, "*** KALSHI_ENV=prod: connecting to the PRODUCTION market data feed ***\n");
    }

    KalshiFeed feed;
    std::string error;
    if (!feed.connect(cfg, error)) {
        std::fprintf(stderr, "connect failed: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stderr, "connected to %s, subscribing to ticker+orderbook_delta for %s...\n",
                 cfg.host.c_str(), ticker.c_str());

    if (!feed.subscribe({"ticker", "orderbook_delta"}, {ticker}, error)) {
        std::fprintf(stderr, "subscribe failed: %s\n", error.c_str());
        return 1;
    }

    feed.on_ticker = [](std::string_view t, long long bid, long long ask) {
        std::printf("[ticker] %.*s yes_bid=%lld yes_ask=%lld\n", static_cast<int>(t.size()), t.data(), bid, ask);
        std::fflush(stdout);
    };

    // Heap-allocated: OrderBook is ~5.7 MB by design — see order_book.hpp SIZE WARNING.
    auto book = std::make_unique<OrderBook>(0);

    std::signal(SIGINT, on_sigint);
    Price last_bb = -1, last_ba = -1;
    while (!g_stop) {
        if (!feed.pump_once(ticker, *book, error)) {
            std::fprintf(stderr, "feed error: %s\n", error.c_str());
            break;
        }
        Price bb = 0, ba = 0;
        const bool have_bb = book->best_bid(bb);
        const bool have_ba = book->best_ask(ba);
        if ((have_bb && bb != last_bb) || (have_ba && ba != last_ba)) {
            std::printf("[shadow book] best_bid=%s best_ask=%s\n",
                        have_bb ? std::to_string(bb).c_str() : "-",
                        have_ba ? std::to_string(ba).c_str() : "-");
            std::fflush(stdout);
            last_bb = have_bb ? bb : -1;
            last_ba = have_ba ? ba : -1;
        }
    }
    std::fprintf(stderr, "stopped.\n");
    return 0;
}
