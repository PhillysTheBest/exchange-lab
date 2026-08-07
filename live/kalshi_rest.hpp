// live/kalshi_rest.hpp — minimal signed REST client for order placement.
//
// SAFETY POSTURE: `place_order` with dry_run=true (the default everywhere this
// is called in this project — see tools/kalshi_shadow.cpp and the plan doc's
// "Credentials and live trading" section) builds and signs the full request but
// never constructs a TlsSocket or performs any network I/O — that's a structural
// guarantee in the code below, not just a documented convention. Only an
// explicit dry_run=false sends anything.
//
// ENDPOINT CAVEAT: the REST order-creation path below
// (`/trade-api/v2/portfolio/orders`) reflects Kalshi's commonly-documented API
// shape but was not independently re-confirmed against current docs during this
// session (the API-overview fetch 404'd — see the plan doc's Sources). Verify
// it against current Kalshi docs before sending a real (non-dry-run) order.
#pragma once

#include "px/types.hpp"

#include <string>

namespace px::live {

struct KalshiRestConfig {
    std::string host = "demo-api.kalshi.co"; // demo by default; production is a deliberate, separate choice
    unsigned short port = 443;
    std::string api_key_id;
    std::string private_key_path;
};

struct OrderRequest {
    std::string market_ticker;
    Side side; // Buy/Sell of the YES token (post-normalization — see normalize.hpp)
    Price price;
    Qty qty;
    std::string client_order_id; // optional, for idempotency on the caller's side
};

// Returns false with `error` set on failure (including in dry-run mode, if
// signing itself fails — e.g. a bad key path). `response_body` receives either
// the "would send" request dump (dry run) or Kalshi's real HTTP response.
[[nodiscard]] bool place_order(const KalshiRestConfig& cfg, const OrderRequest& req, bool dry_run,
                                std::string& response_body, std::string& error);

} // namespace px::live
