// live/kalshi_rest.hpp — minimal signed REST client for order create/cancel.
//
// SAFETY POSTURE: `place_order`/`cancel_order` with dry_run=true (the default
// everywhere either is called in this project — see live/kalshi_gateway.hpp's
// DryRunSink) build and sign the full request but never construct a
// TlsSocket or perform any network I/O — a structural guarantee in the code
// below, not just a documented convention. Only an explicit dry_run=false
// sends anything.
//
// SCHEMA STATUS (Stage 3): rewritten against Kalshi's V2 order-write
// endpoints, fetched directly from docs.kalshi.com — the V1 path this file
// used before (`/trade-api/v2/portfolio/orders`) is being deprecated (rate-
// limit cost already increased May 14 2026, deprecation eligible May 21
// 2026), and the V1 request body this file built (`action`/`type`/`yes_price`
// fields, `side` hardcoded to `"yes"`) does not match either schema — it was
// never independently confirmed in the first place (the original API-
// overview fetch 404'd). Same "the schema was wrong" story as Stage 2's
// kalshi_feed.cpp, now for the order-write path instead of market data.
#pragma once

#include "px/types.hpp"

#include <string>

namespace px::live {

struct KalshiRestConfig {
    // Demo, recommended alias (the older "demo-api.kalshi.co" still works —
    // see docs.kalshi.com/getting_started/api_environments — but this
    // matches the "external-api" family live/kalshi_feed.hpp's WS host
    // already uses). Production is a deliberate, separate choice the caller
    // makes explicitly by setting this field, never a flag this file exposes.
    std::string host = "external-api.demo.kalshi.co";
    unsigned short port = 443;
    std::string api_key_id;
    std::string private_key_path;
};

struct OrderRequest {
    std::string market_ticker;
    Side side;         // Buy/Sell of the YES token (post-normalization — see normalize.hpp);
                        // mapped to Kalshi's V2 "bid"/"ask" — see kalshi_rest.cpp
    Price price;
    Qty qty;
    TimeInForce tif = TimeInForce::GTC; // GTC -> "good_till_canceled", IOC -> "immediate_or_cancel"
    std::string client_order_id;        // required in practice: this is how Gateway later maps
                                         // Kalshi's returned order_id back to Brain's OrderId
};

// Parsed fields from a successful (2xx) create-order response. `order_id` is
// Kalshi's own UUID — kept here as an ordinary std::string (this file isn't
// ring-constrained; it's Gateway's private boundary code, the same tier as
// TickerRegistry's strings) but never forwarded past
// live/kalshi_gateway.cpp onto any ring — see the plan doc's "Order-ID ring
// safety" section for why.
struct OrderResponse {
    std::string order_id;
    std::string client_order_id;
    Qty fill_count = 0;
    Qty remaining_count = 0;
    bool has_average_fill_price = false;
    Price average_fill_price_cents = 0; // valid iff has_average_fill_price
};

struct CancelResponse {
    std::string order_id;
    std::string client_order_id;
    Qty reduced_by = 0; // contracts canceled — the remaining count at time of cancellation
};

// Returns false with `error` set on failure (including in dry-run mode, if
// signing itself fails — e.g. a bad key path). `response_body` receives
// either the "would send" request dump (dry run) or Kalshi's real raw HTTP
// response text (status line + headers + body) — parse the 2xx case with
// parse_order_response, the status line with live/kalshi_gateway.cpp's
// parse_http_status.
[[nodiscard]] bool place_order(const KalshiRestConfig& cfg, const OrderRequest& req, bool dry_run,
                                std::string& response_body, std::string& error);

// Same dry-run/live split and safety guarantee as place_order, for
// DELETE /trade-api/v2/portfolio/events/orders/{kalshi_order_id}.
// `kalshi_order_id` is Kalshi's UUID (from a prior OrderResponse::order_id),
// never Brain's own OrderId.
[[nodiscard]] bool cancel_order(const KalshiRestConfig& cfg, const std::string& kalshi_order_id, bool dry_run,
                                 std::string& response_body, std::string& error);

// Parses a successful (2xx) create-order response body into OrderResponse.
// Returns false only on a genuine decode failure (a required field missing
// or unparseable) — an order-level rejection is a non-2xx HTTP status, a
// different, already-handled case (see kalshi_gateway.cpp's parse_http_status),
// not something this function reports.
[[nodiscard]] bool parse_order_response(const std::string& response_body, OrderResponse& out);

[[nodiscard]] bool parse_cancel_response(const std::string& response_body, CancelResponse& out);

} // namespace px::live
