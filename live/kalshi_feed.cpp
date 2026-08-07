#include "kalshi_feed.hpp"
#include "json_scan.hpp"

namespace px::live {

bool KalshiFeed::connect(const KalshiConfig& cfg, std::string& error) {
    key_ = load_private_key_pem(cfg.private_key_path, error);
    if (!key_) return false;
    api_key_id_ = cfg.api_key_id;

    const std::string ts = timestamp_ms_now();
    const std::string sig = sign_request(*key_, ts, "GET", cfg.ws_path, error);
    if (sig.empty()) return false;

    const std::vector<WebSocketClient::Header> headers = {
        {"KALSHI-ACCESS-KEY", cfg.api_key_id},
        {"KALSHI-ACCESS-SIGNATURE", sig},
        {"KALSHI-ACCESS-TIMESTAMP", ts},
    };
    return ws_.connect(cfg.host, cfg.port, cfg.ws_path, headers, error);
}

bool KalshiFeed::subscribe(const std::vector<std::string>& channels,
                            const std::vector<std::string>& market_tickers, std::string& error) {
    std::string j = "{\"id\":" + std::to_string(next_cmd_id_++) + ",\"cmd\":\"subscribe\",\"params\":{\"channels\":[";
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (i) j += ",";
        j += "\"" + channels[i] + "\"";
    }
    j += "]";
    if (!market_tickers.empty()) {
        j += ",\"market_tickers\":[";
        for (std::size_t i = 0; i < market_tickers.size(); ++i) {
            if (i) j += ",";
            j += "\"" + market_tickers[i] + "\"";
        }
        j += "]";
    }
    j += "}}";

    if (!ws_.send_text(j)) {
        error = "failed to send subscribe command";
        return false;
    }
    return true;
}

bool KalshiFeed::pump_once(std::string_view watch_ticker, OrderBook& book, std::string& error) {
    std::string msg;
    if (!ws_.receive_message(msg, error)) return false;

    std::string_view type;
    if (!json_extract_string(msg, "type", type)) return true; // unrecognized envelope; drained, not fatal

    std::string_view ticker;
    // json_extract_string is a flat substring search, not a structural parse, so
    // it finds "market_ticker" regardless of how deeply the docs' {"type":...,
    // "msg": {...}} envelope nests it.
    if (!json_extract_string(msg, "market_ticker", ticker) || ticker != watch_ticker) {
        return true; // not our market (or no ticker on this message type) — drained
    }

    if (type == "orderbook_snapshot") {
        // Documented REST orderbook shape, assumed mirrored by the WS snapshot —
        // see the SCHEMA CAVEAT in kalshi_feed.hpp.
        std::vector<std::pair<long long, long long>> yes_side, no_side;
        (void)json_extract_pair_array(msg, "yes", yes_side);
        (void)json_extract_pair_array(msg, "no", no_side);

        for (const auto& [p, q] : yes_side) {
            if (p < kMinPrice || p > kMaxPrice) continue;
            book.shadow_set_level(Side::Buy, static_cast<Price>(p), static_cast<Qty>(q));
        }
        for (const auto& [p, q] : no_side) {
            if (p < kMinPrice || p > kMaxPrice) continue;
            // A resting NO bid at price p is a Contract::No, Side::Buy order —
            // normalize() gives its YES-equivalent (side, price), exactly the
            // same transform used for live order entry, kept consistent here.
            const auto [nside, nprice] = normalize(Contract::No, Side::Buy, static_cast<Price>(p));
            book.shadow_set_level(nside, nprice, static_cast<Qty>(q));
        }
    } else if (type == "orderbook_delta") {
        // ASSUMED field names (price/delta/side), NOT independently confirmed —
        // see the SCHEMA CAVEAT in kalshi_feed.hpp. `delta` is a signed change;
        // applied against the level's current total_qty (readable directly off
        // the shadow book) rather than requiring a separately-tracked ledger.
        long long price = 0, delta = 0;
        std::string_view side_str;
        if (json_extract_int(msg, "price", price) && json_extract_int(msg, "delta", delta) &&
            json_extract_string(msg, "side", side_str) && price >= kMinPrice && price <= kMaxPrice) {
            const Contract c = (side_str == "no") ? Contract::No : Contract::Yes;
            const auto [nside, nprice] = normalize(c, Side::Buy, static_cast<Price>(price));
            const long long updated = static_cast<long long>(book.level(nprice).total_qty) + delta;
            book.shadow_set_level(nside, nprice, static_cast<Qty>(updated < 0 ? 0 : updated));
        }
    } else if (type == "ticker" && on_ticker) {
        long long yes_bid_i = 0, yes_ask_i = 0;
        if (json_extract_int(msg, "yes_bid", yes_bid_i) && json_extract_int(msg, "yes_ask", yes_ask_i)) {
            on_ticker(ticker, yes_bid_i, yes_ask_i);
        } else {
            double yes_bid = 0, yes_ask = 0;
            if (json_extract_number(msg, "yes_bid_dollars", yes_bid) &&
                json_extract_number(msg, "yes_ask_dollars", yes_ask)) {
                on_ticker(ticker, static_cast<long long>(yes_bid * 100 + 0.5),
                          static_cast<long long>(yes_ask * 100 + 0.5));
            }
        }
    }
    return true;
}

} // namespace px::live
