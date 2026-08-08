// live/kalshi_gateway.hpp — T3 Egress: the dumb, fast pipe.
//
// Pops OrderIntent from ring B, does the crypto (RSA-PSS signing — measured
// at ~1ms p50, ~10,000x a book update, see bench/bench_sign.cpp and the plan
// doc's "why T3 exists" — which is the entire reason this isn't inline in
// T2), serializes + sends via kalshi_rest.hpp's already-built, already-tested
// place_order, and pushes what it can honestly determine back onto ring C.
//
// Zero trading logic, zero risk logic, zero market awareness: Gateway never
// decides *whether* to send — that was RiskEngine's job, already done before
// the intent reached ring B. What Gateway decides is *how* to send, and even
// that's fixed at construction via Sink (dry-run vs demo/live), not branched
// on per-order.
//
// FILL TRACKING GAP (flagged, not silently missing): Kalshi's create-order
// REST response does not carry fill information — confirmed against the
// published AsyncAPI spec, which documents fills as arriving asynchronously
// over the private, authenticated `fill` WebSocket channel, a completely
// separate connection from this REST path. Building that listener (reusing
// KalshiFeed's connect/subscribe machinery against channel "fill" instead of
// "orderbook_delta" would be the natural way — see kalshi_feed.hpp) is real,
// scoped-out work, not attempted here. What this file DOES parse from the
// REST response — the HTTP status line — is unambiguous and needs no schema
// guessing: 429 becomes ExecKind::RateLimited, any other non-2xx becomes
// ExecKind::Reject, and a 2xx becomes... nothing pushed to ring C, precisely
// because a 2xx here means "Kalshi accepted the order," not "Kalshi filled
// it." RiskEngine::record_fill (see risk/risk_engine.hpp) is consequently
// never invoked by this stage's live path — a real, documented limitation of
// Stage 2 as shipped, not a bug in RiskEngine itself (its own unit tests
// cover record_fill directly — see tests/test_risk.cpp).
#pragma once

#include "kalshi_rest.hpp"
#include "pipeline/events.hpp"
#include "ticker_registry.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace px::live {

// How Gateway actually places an order — swappable at construction, never
// branched on at runtime inside Gateway::run. That's what makes "this
// gateway cannot send real orders" a structural property instead of a
// convention: Gateway holds a Sink&, and only the Sink's concrete type
// decides whether a socket gets touched.
class Sink {
public:
    virtual ~Sink() = default;

    // `did_send` is true iff a real network request was attempted (always
    // false for DryRunSink, which structurally never constructs a socket —
    // see place_order's SAFETY POSTURE in kalshi_rest.hpp). Gateway only
    // parses `response` as an HTTP reply when did_send is true. Returns false
    // + `error` only when the request couldn't even be built/signed (e.g. a
    // bad key path) — an HTTP-level rejection (4xx/5xx) is a *successful*
    // attempt, reflected in `response`, not a false return here.
    [[nodiscard]] virtual bool place(const OrderRequest& req, bool& did_send, std::string& response,
                                      std::string& error) = 0;
};

class DryRunSink final : public Sink {
public:
    explicit DryRunSink(KalshiRestConfig cfg) : cfg_(std::move(cfg)) {}
    [[nodiscard]] bool place(const OrderRequest& req, bool& did_send, std::string& response,
                              std::string& error) override {
        did_send = false;
        return place_order(cfg_, req, /*dry_run=*/true, response, error);
    }

private:
    KalshiRestConfig cfg_;
};

// Sends for real, against whatever host `cfg` names (demo by default — see
// KalshiRestConfig::host; production is a deliberate, separate choice the
// caller of the constructor makes explicitly). Selected only by an explicit
// opt-in in tools/kalshi_pipeline.cpp, never the default.
class LiveSink final : public Sink {
public:
    explicit LiveSink(KalshiRestConfig cfg) : cfg_(std::move(cfg)) {}
    [[nodiscard]] bool place(const OrderRequest& req, bool& did_send, std::string& response,
                              std::string& error) override {
        did_send = true;
        return place_order(cfg_, req, /*dry_run=*/false, response, error);
    }

private:
    KalshiRestConfig cfg_;
};

// Parses an HTTP/1.x response's status line ("HTTP/1.1 429 ...") for the
// 3-digit status code. Unambiguous, schema-free — no guessing involved,
// unlike the fields this project has had to verify against Kalshi's docs
// elsewhere.
[[nodiscard]] bool parse_http_status(const std::string& response, int& status_out);

class Gateway {
public:
    // `registry` translates an intent's compact MarketId back to the ticker
    // string Kalshi's REST API needs — the one place in this pipeline a
    // string reappears after T1 deliberately stripped it away. Caller-owned;
    // must outlive the Gateway (same lifetime contract as TickerRegistry's
    // other consumers).
    Gateway(std::unique_ptr<Sink> sink, const TickerRegistry& registry)
        : sink_(std::move(sink)), registry_(registry) {}

    // Runs until stop_flag is set, popping intents from ring B and pushing
    // whatever ring C reports it can honestly produce (see the FILL TRACKING
    // GAP note above kalshi_gateway.hpp's top comment).
    void run(pipeline::RingB& intents_in, pipeline::RingC& exec_out, std::atomic<bool>& stop_flag);

    // Optional, for logging/metrics only — never on the decision path.
    std::function<void(const pipeline::OrderIntent&, const std::string& response, const std::string& error)>
        on_sent;

private:
    void handle_intent(const pipeline::OrderIntent& intent, pipeline::RingC& exec_out);

    std::unique_ptr<Sink> sink_;
    const TickerRegistry& registry_;
    std::uint64_t next_client_order_id_ = 1;
};

} // namespace px::live
