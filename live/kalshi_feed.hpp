// live/kalshi_feed.hpp — connects to Kalshi's WebSocket feed and applies market
// data into a px::OrderBook running in shadow mode (see order_book.hpp's
// shadow_set_level).
//
// SCHEMA CAVEAT, stated plainly rather than papered over: this project's source
// material (see the plan doc's Sources) confirmed the outer envelope
// (`type`, `market_ticker`, subscribe command shape) and the well-documented
// REST orderbook shape (`{"yes": [[price,qty],...], "no": [[price,qty],...]}`),
// but not a complete field-by-field spec of the WS orderbook_delta message
// beyond the `market_ticker` field. This implementation assumes
// orderbook_snapshot mirrors the REST shape and that orderbook_delta carries
// flat `price`/`delta`/`side` fields, both clearly marked below — verify both
// against a live capture before relying on this for real trading. Endpoint
// defaults to Kalshi's **demo** environment; pointing this at production is a
// change the caller of connect() makes explicitly (see KalshiConfig::host).
#pragma once

#include "kalshi_auth.hpp"
#include "px/order_book.hpp"
#include "ws_client.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace px::live {

struct KalshiConfig {
    // wss://external-api-ws.demo.kalshi.co/trade-api/ws/v2 by default (demo).
    // Production is wss://external-api-ws.kalshi.com/trade-api/ws/v2 — set this
    // explicitly to switch; there is no flag that flips it implicitly.
    std::string host = "external-api-ws.demo.kalshi.co";
    unsigned short port = 443;
    std::string ws_path = "/trade-api/ws/v2";

    std::string api_key_id;       // KALSHI-ACCESS-KEY
    std::string private_key_path; // PEM file — read at runtime, never embedded in source
};

class KalshiFeed {
public:
    // Connects, TLS handshakes, and performs the signed WS upgrade (signs
    // timestamp+"GET"+ws_path per kalshi_auth.hpp). Returns false w/ `error` set
    // on any failure — including a bad/missing private key, which is checked
    // here rather than surfacing as an opaque handshake rejection later.
    [[nodiscard]] bool connect(const KalshiConfig& cfg, std::string& error);

    [[nodiscard]] bool subscribe(const std::vector<std::string>& channels,
                                  const std::vector<std::string>& market_tickers, std::string& error);

    // Blocks for the next message. If it's for `watch_ticker`, applies it into
    // `book` (orderbook_snapshot/delta) or invokes on_ticker (ticker messages).
    // Messages for other tickers are read and discarded. Returns false on
    // transport error or a peer-initiated close.
    [[nodiscard]] bool pump_once(std::string_view watch_ticker, OrderBook& book, std::string& error);

    std::function<void(std::string_view ticker, long long yes_bid_cents, long long yes_ask_cents)> on_ticker;

private:
    WebSocketClient ws_;
    std::unique_ptr<PrivateKey> key_;
    std::string api_key_id_;
    long long next_cmd_id_ = 1;
};

} // namespace px::live
