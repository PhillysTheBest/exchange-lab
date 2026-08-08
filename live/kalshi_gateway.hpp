// live/kalshi_gateway.hpp — T3 Egress: the dumb, fast pipe.
//
// Pops an OrderIntent (new order or cancel) from ring B, does the crypto
// (RSA-PSS signing — measured at ~1ms p50, ~15,000-30,000x a book update,
// see bench/bench_sign.cpp and the plan doc's "why T3 exists" — the entire
// reason this isn't inline in T2), serializes + sends via kalshi_rest.hpp's
// already-tested place_order/cancel_order, and pushes what it can honestly
// determine back onto ring C.
//
// Zero trading logic, zero risk logic, zero market awareness: Gateway never
// decides *whether* to send — that was RiskEngine's job, already done before
// the intent reached ring B. What Gateway decides is *how* to send, and even
// that's fixed at construction via Sink (dry-run vs demo/live), not branched
// on per-order.
//
// ORDER-ID MAPPING (Stage 3): Gateway privately maps Brain's own OrderId to
// Kalshi's returned UUID order_id the instant a create-order response
// confirms one WITH SOMETHING LEFT RESTING (remaining_count > 0) — so a
// later CancelOrder intent (which carries the same OrderId, never Kalshi's
// UUID — see the plan doc's "Order-ID ring safety" section) can be resolved
// to the right DELETE path. A fully-resolved order (filled or IOC-killed at
// entry, remaining_count == 0) is never inserted at all: there's nothing left
// to ever legitimately cancel, and this map is only ever erased by a
// successful cancel (handle_cancel) — inserting anyway would leave a
// permanent, unbounded-over-a-session entry for every such order (found via
// tests/test_pipeline_e2e.cpp). This map is exactly the kind of thing that
// must never cross a ring (every ring payload in this codebase is a
// fixed-size POD — see pipeline/events.hpp's static_asserts); it lives here,
// privately, the same tier as TickerRegistry's strings.
//
// FILL TRACKING, narrowed in Stage 3, not closed: Kalshi's create-order
// response DOES carry `fill_count`/`average_fill_price` when a marketable
// order fills synchronously at entry — see live/kalshi_rest.hpp's
// OrderResponse, confirmed against the V2 schema — so THAT case is now
// caught here (pushed as ExecKind::Fill alongside Acked). What's still not
// caught: a fill arriving *after* an order already rests, which only the
// private, separately-authenticated `fill` WebSocket channel reports (see
// kalshi_feed.hpp) — building that listener (straightforward reuse of
// KalshiFeed's connect/subscribe machinery against channel "fill" instead of
// "orderbook_delta") is real, still-scoped-out work, not attempted here.
#pragma once

#include "kalshi_rest.hpp"
#include "pipeline/events.hpp"
#include "ticker_registry.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace px::live {

// How Gateway actually places/cancels an order — swappable at construction,
// never branched on at runtime inside Gateway::run. That's what makes "this
// gateway cannot send real orders" a structural property instead of a
// convention: Gateway holds a Sink&, and only the Sink's concrete type
// decides whether a socket gets touched.
class Sink {
public:
    virtual ~Sink() = default;

    // `did_send` is true iff a real network request was attempted (always
    // false for DryRunSink, which structurally never constructs a socket —
    // see place_order's/cancel_order's SAFETY POSTURE in kalshi_rest.hpp).
    // Gateway only parses `response` as an HTTP reply when did_send is true.
    // Returns false + `error` only when the request couldn't even be
    // built/signed (e.g. a bad key path) — an HTTP-level rejection (4xx/5xx)
    // is a *successful* attempt, reflected in `response`, not a false return.
    [[nodiscard]] virtual bool place(const OrderRequest& req, bool& did_send, std::string& response,
                                      std::string& error) = 0;

    // Same contract as place(), for a cancel — needs the same structural
    // dry-run-safety guarantee new orders already have, which is why this is
    // a Sink method and not a free function Gateway calls directly.
    [[nodiscard]] virtual bool cancel(const std::string& kalshi_order_id, bool& did_send, std::string& response,
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
    [[nodiscard]] bool cancel(const std::string& kalshi_order_id, bool& did_send, std::string& response,
                               std::string& error) override {
        did_send = false;
        return cancel_order(cfg_, kalshi_order_id, /*dry_run=*/true, response, error);
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
    [[nodiscard]] bool cancel(const std::string& kalshi_order_id, bool& did_send, std::string& response,
                               std::string& error) override {
        did_send = true;
        return cancel_order(cfg_, kalshi_order_id, /*dry_run=*/false, response, error);
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
    // note above kalshi_gateway.hpp's top comment).
    void run(pipeline::RingB& intents_in, pipeline::RingC& exec_out, std::atomic<bool>& stop_flag);

    // Optional, for logging/metrics only — never on the decision path.
    std::function<void(const pipeline::OrderIntent&, const std::string& response, const std::string& error)>
        on_sent;

private:
    void handle_intent(const pipeline::OrderIntent& intent, pipeline::RingC& exec_out);
    void handle_new_order(const pipeline::OrderIntent& intent, pipeline::RingC& exec_out);
    void handle_cancel(const pipeline::OrderIntent& intent, pipeline::RingC& exec_out);

    std::unique_ptr<Sink> sink_;
    const TickerRegistry& registry_;

    // Brain's OrderId -> Kalshi's UUID order_id, populated the instant a
    // create-order response confirms one, consulted (and erased) when a
    // matching CancelOrder intent arrives. Never crosses a ring — see the
    // file header comment.
    std::unordered_map<OrderId, std::string> kalshi_uuid_of_;
};

} // namespace px::live
