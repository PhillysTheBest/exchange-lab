#include "kalshi_rest.hpp"
#include "json_scan.hpp"
#include "kalshi_auth.hpp"
#include "tls_socket.hpp"

namespace px::live {

namespace {

// Shared by place_order/cancel_order: both are "sign this method+path(+body),
// dump it if dry_run, otherwise open a fresh TLS connection and send it" —
// the only things that differ are the method, path, and body. A fresh
// connection per call (rather than a pooled/persistent one) is deliberately
// fine here: Kalshi's own rate limit caps this project to roughly 10
// requests/sec (see risk/risk_engine.hpp), nowhere near where connection
// setup cost would matter.
[[nodiscard]] bool send_signed_request(const KalshiRestConfig& cfg, const std::string& method,
                                        const std::string& path, const std::string& body, bool dry_run,
                                        std::string& response_body, std::string& error) {
    auto key = load_private_key_pem(cfg.private_key_path, error);
    if (!key) return false;

    const std::string ts = timestamp_ms_now();
    const std::string sig = sign_request(*key, ts, method, path, error);
    if (sig.empty()) return false;

    std::string request_dump = method + " " + path +
                                " HTTP/1.1\r\n"
                                "Host: " +
                                cfg.host +
                                "\r\n"
                                "KALSHI-ACCESS-KEY: " +
                                cfg.api_key_id +
                                "\r\n"
                                "KALSHI-ACCESS-SIGNATURE: " +
                                sig +
                                "\r\n"
                                "KALSHI-ACCESS-TIMESTAMP: " +
                                ts + "\r\n";
    if (!body.empty()) {
        request_dump += "Content-Type: application/json\r\n";
        request_dump += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request_dump += "Connection: close\r\n\r\n";
    request_dump += body;

    if (dry_run) {
        response_body = "[DRY RUN -- request built and signed, nothing sent over the network]\n" + request_dump;
        return true;
    }

    TlsSocket sock;
    if (!sock.connect(cfg.host, cfg.port, error)) return false;
    if (!sock.write_all(request_dump.data(), request_dump.size())) {
        error = "failed to send request";
        return false;
    }

    std::string resp;
    char buf[4096];
    for (;;) {
        const int n = sock.read(buf, sizeof(buf));
        if (n <= 0) break; // 0 = clean EOF (server closed after "Connection: close"), <0 = error
        resp.append(buf, static_cast<std::size_t>(n));
    }
    response_body = resp;
    return true;
}

} // namespace

bool place_order(const KalshiRestConfig& cfg, const OrderRequest& req, bool dry_run, std::string& response_body,
                  std::string& error) {
    static constexpr const char* kOrderPath = "/trade-api/v2/portfolio/events/orders";

    // V2's `side` is "bid"/"ask" — a single unified book, not the "yes"/"no"
    // token-selection field V1 used. This is the same normalization insight
    // this project's own normalize.hpp already implements: post-normalization,
    // Side::Buy always means "wants YES", so it always maps to "bid" and
    // Side::Sell always maps to "ask", regardless of which contract the
    // caller originally traded in.
    std::string body = "{\"ticker\":\"" + req.market_ticker + "\"";
    if (!req.client_order_id.empty()) body += ",\"client_order_id\":\"" + req.client_order_id + "\"";
    body += ",\"side\":\"" + std::string(req.side == Side::Buy ? "bid" : "ask") + "\"";
    body += ",\"count\":\"" + format_qty_fixed_point(req.qty) + "\"";
    body += ",\"price\":\"" + format_cents_as_dollars(req.price) + "\"";
    body += ",\"time_in_force\":\"" +
            std::string(req.tif == TimeInForce::GTC ? "good_till_canceled" : "immediate_or_cancel") + "\"";
    // Required by the schema; exact behavioral difference between the two
    // allowed values not independently confirmed beyond their names — see
    // kalshi_rest.hpp's SCHEMA STATUS note. "taker_at_cross" read as
    // "cancel/adjust the crossing (taker) side on a self-cross," which
    // protects a resting quote of ours rather than silently pulling it.
    body += ",\"self_trade_prevention_type\":\"taker_at_cross\"";
    body += "}";

    return send_signed_request(cfg, "POST", kOrderPath, body, dry_run, response_body, error);
}

bool cancel_order(const KalshiRestConfig& cfg, const std::string& kalshi_order_id, bool dry_run,
                   std::string& response_body, std::string& error) {
    const std::string path = "/trade-api/v2/portfolio/events/orders/" + kalshi_order_id;
    return send_signed_request(cfg, "DELETE", path, /*body=*/"", dry_run, response_body, error);
}

bool parse_order_response(const std::string& response_body, OrderResponse& out) {
    const std::string_view sv(response_body);

    std::string_view order_id_sv;
    if (!json_extract_string(sv, "order_id", order_id_sv)) return false;
    out.order_id = std::string(order_id_sv);

    std::string_view client_id_sv;
    if (json_extract_string(sv, "client_order_id", client_id_sv)) out.client_order_id = std::string(client_id_sv);

    long long fill_count = 0, remaining_count = 0;
    if (!json_extract_fixed_point_qty(sv, "fill_count", fill_count)) return false;
    if (!json_extract_fixed_point_qty(sv, "remaining_count", remaining_count)) return false;
    out.fill_count = static_cast<Qty>(fill_count < 0 ? 0 : fill_count);
    out.remaining_count = static_cast<Qty>(remaining_count < 0 ? 0 : remaining_count);

    long long avg_price_cents = 0;
    out.has_average_fill_price = json_extract_dollars_as_cents(sv, "average_fill_price", avg_price_cents);
    if (out.has_average_fill_price) out.average_fill_price_cents = static_cast<Price>(avg_price_cents);

    return true;
}

bool parse_cancel_response(const std::string& response_body, CancelResponse& out) {
    const std::string_view sv(response_body);

    std::string_view order_id_sv;
    if (!json_extract_string(sv, "order_id", order_id_sv)) return false;
    out.order_id = std::string(order_id_sv);

    std::string_view client_id_sv;
    if (json_extract_string(sv, "client_order_id", client_id_sv)) out.client_order_id = std::string(client_id_sv);

    long long reduced_by = 0;
    if (!json_extract_fixed_point_qty(sv, "reduced_by", reduced_by)) return false;
    out.reduced_by = static_cast<Qty>(reduced_by < 0 ? 0 : reduced_by);

    return true;
}

} // namespace px::live
