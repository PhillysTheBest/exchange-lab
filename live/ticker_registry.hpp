// live/ticker_registry.hpp — maps Kalshi's string market tickers to this
// engine's compact MarketId, so nothing past T1 Ingress ever touches a
// std::string on a per-message basis. Populated from the markets an active
// strategy declares interest in — deliberately not a bulk/discovery registry
// (see the plan doc's "Markets — signal-driven, deliberately few": Kalshi
// charges the same connection either way, but every subscribed ticker is
// parse work T1 pays on every tick, whether or not any signal cares).
//
// Control-plane code (registration happens at startup/reconfiguration, not
// per market-data message), so std::unordered_map/string/vector are the right
// tools here — same "not on any hot path" carve-out live/ already documents
// (see tls_socket.hpp). A per-message ticker->MarketId lookup in T1 (find())
// is still negligible next to the network read and JSON scan it follows.
#pragma once

#include "px/types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace px::live {

class TickerRegistry {
public:
    // Registers `ticker` under the next available MarketId (capped at
    // kMaxMarkets, matching Trader's flat per-market position array — see
    // px/types.hpp). Idempotent: re-registering an already-known ticker just
    // returns its existing id. Returns false (leaving `out` untouched) once
    // the registry is full — a deliberately small, explicit ceiling rather
    // than silently growing to track markets nothing is watching.
    [[nodiscard]] bool add(const std::string& ticker, MarketId& out) {
        const auto it = ticker_to_id_.find(ticker);
        if (it != ticker_to_id_.end()) {
            out = it->second;
            return true;
        }
        if (id_to_ticker_.size() >= kMaxMarkets) return false;
        const MarketId id = static_cast<MarketId>(id_to_ticker_.size());
        ticker_to_id_.emplace(ticker, id);
        id_to_ticker_.push_back(ticker);
        out = id;
        return true;
    }

    [[nodiscard]] bool find(const std::string& ticker, MarketId& out) const {
        const auto it = ticker_to_id_.find(ticker);
        if (it == ticker_to_id_.end()) return false;
        out = it->second;
        return true;
    }

    [[nodiscard]] const std::string& ticker_of(MarketId id) const noexcept { return id_to_ticker_[id]; }
    [[nodiscard]] std::size_t size() const noexcept { return id_to_ticker_.size(); }
    [[nodiscard]] const std::vector<std::string>& all_tickers() const noexcept { return id_to_ticker_; }

private:
    std::unordered_map<std::string, MarketId> ticker_to_id_;
    std::vector<std::string> id_to_ticker_; // index == MarketId
};

} // namespace px::live
