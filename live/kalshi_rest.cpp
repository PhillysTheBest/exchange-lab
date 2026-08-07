#include "kalshi_rest.hpp"
#include "kalshi_auth.hpp"
#include "tls_socket.hpp"

namespace px::live {

bool place_order(const KalshiRestConfig& cfg, const OrderRequest& req, bool dry_run,
                  std::string& response_body, std::string& error) {
    static constexpr const char* kOrderPath = "/trade-api/v2/portfolio/orders"; // see header ENDPOINT CAVEAT

    std::string body = "{\"ticker\":\"" + req.market_ticker + "\""
                        ",\"action\":\"" + (req.side == Side::Buy ? std::string("buy") : std::string("sell")) + "\""
                        ",\"side\":\"yes\""
                        ",\"count\":" + std::to_string(req.qty) +
                        ",\"type\":\"limit\""
                        ",\"yes_price\":" + std::to_string(req.price);
    if (!req.client_order_id.empty()) body += ",\"client_order_id\":\"" + req.client_order_id + "\"";
    body += "}";

    auto key = load_private_key_pem(cfg.private_key_path, error);
    if (!key) return false;

    const std::string ts = timestamp_ms_now();
    const std::string sig = sign_request(*key, ts, "POST", kOrderPath, error);
    if (sig.empty()) return false;

    const std::string request_dump =
        "POST " + std::string(kOrderPath) + " HTTP/1.1\r\n"
        "Host: " + cfg.host + "\r\n"
        "KALSHI-ACCESS-KEY: " + cfg.api_key_id + "\r\n"
        "KALSHI-ACCESS-SIGNATURE: " + sig + "\r\n"
        "KALSHI-ACCESS-TIMESTAMP: " + ts + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;

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

} // namespace px::live
