// pipeline/ingress.hpp — T1: owns the Kalshi WebSocket connection.
//
// Blocking recv -> TLS de-framing (inside KalshiFeed/WebSocketClient) -> parse
// (parse_feed_message) -> resolve the ticker string to a MarketId (once, here
// — nothing downstream of this thread ever touches a std::string) -> push a
// fixed-size BookEvent onto ring A. Per-market seq-gap detection lives here,
// not in KalshiFeed (whose own SeqTracker only serves its single-ticker
// pump_once convenience path used by tools/kalshi_shadow.cpp) — same
// per-ticker-not-per-sid reasoning as the SEQ SCOPING note in kalshi_feed.cpp,
// applied per MarketId across however many tickers this connection watches.
#pragma once

#include "kalshi_feed.hpp"
#include "pipeline/events.hpp"
#include "px/spsc_ring.hpp"
#include "px/types.hpp"
#include "ticker_registry.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <string>
#include <string_view>

namespace px::pipeline {

inline constexpr std::size_t kRingACapacity = 1u << 14; // 16384
using RingA = SpscRing<BookEvent, kRingACapacity>;

class Ingress {
public:
    Ingress(live::KalshiConfig cfg, live::TickerRegistry registry)
        : cfg_(std::move(cfg)), registry_(std::move(registry)) {}

    // Connects, subscribes to ticker+orderbook_delta for every ticker in the
    // registry, and runs the receive loop until stop_flag is set or a
    // transport error occurs. Intended as a std::thread entry point (see
    // tools/kalshi_pipeline.cpp). A ring-A push failing (ring full) is not
    // treated as droppable the way risk/risk_engine.hpp's rate limiter treats
    // an over-budget order: dropping a book mutation silently desyncs the
    // shadow book exactly the way the original schema bug did, so a full ring
    // is backpressure (spin-wait), not a discard. Errors surface via on_error
    // rather than a return code, since by the time one happens there's no
    // caller left on the stack to hand it to synchronously.
    void run(RingA& out, std::atomic<bool>& stop_flag);

    // Applies one already-decoded FeedMsg exactly as run()'s receive loop
    // would (ticker resolution, seq tracking + resync-on-gap, BookEvent
    // emission) without touching the network. Public specifically so a
    // replay driver can push a deterministic, file-sourced sequence of
    // messages through the real production logic instead of a reimplemented
    // copy of it — see tools/kalshi_pipeline.cpp's --replay mode and
    // tests/test_replay_determinism.cpp, which is the actual point of
    // exposing this rather than keeping it private.
    void handle_message(const live::FeedMsg& fm, RingA& out);

    std::function<void(std::string_view message)> on_error;
    std::function<void(std::string_view message)> on_warning;

private:
    live::KalshiConfig cfg_;
    live::TickerRegistry registry_;
    live::KalshiFeed feed_;
    std::array<live::SeqTracker, kMaxMarkets> seq_trackers_{};
};

} // namespace px::pipeline
