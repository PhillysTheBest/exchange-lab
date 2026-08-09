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
//
// RECONNECTION (Stage 4): run() no longer exits the instant connect/subscribe/
// receive fails once. It retries with capped exponential backoff, and after
// each successful reconnect explicitly re-requests a fresh orderbook_snapshot
// for every watched ticker (via request_resync — the same mechanism a seq gap
// already uses, not a new, separately-unverified assumption about what a
// bare subscribe delivers). That fresh Snapshot message is what actually
// resynchronizes each market's ShadowBook: SnapshotBegin already makes Brain
// clear() the book before replaying the new LevelSet run (see
// pipeline/brain.cpp), and a Snapshot already unconditionally resets
// SeqTracker's baseline (see kalshi_feed.hpp) — reconnection reuses both of
// those existing, already-tested mechanisms rather than inventing new ones.
// stop_flag remains the only way out of the retry loop; a sustained outage
// means indefinite retries, not giving up and going silently dark.
#pragma once

#include "kalshi_feed.hpp"
#include "pipeline/events.hpp"
#include "px/spsc_ring.hpp"
#include "px/types.hpp"
#include "ticker_registry.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace px::pipeline {

inline constexpr std::size_t kRingACapacity = 1u << 14; // 16384
using RingA = SpscRing<BookEvent, kRingACapacity>;

class Ingress {
public:
    Ingress(live::KalshiConfig cfg, live::TickerRegistry registry)
        : cfg_(std::move(cfg)), registry_(std::move(registry)), feed_(std::make_unique<live::KalshiFeed>()) {}

    // Connects, subscribes to ticker+orderbook_delta+trade for every ticker in
    // the registry, and runs the receive loop until stop_flag is set. A
    // connect/subscribe/receive failure no longer ends the thread — see the
    // RECONNECTION note above — it retries with backoff until stop_flag is
    // set. A ring-A push failing (ring full) is not treated as droppable the
    // way risk/risk_engine.hpp's rate limiter treats an over-budget order:
    // dropping a book mutation silently desyncs the shadow book exactly the
    // way the original schema bug did, so a full ring is backpressure
    // (spin-wait), not a discard. Failures surface via on_error/on_reconnecting
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
    // Fires once per reconnect attempt (both the first connect and every
    // retry after), before the attempt is made — `attempt` is 1-based,
    // `backoff` is how long run() waited before THIS attempt (zero for the
    // very first). Optional, logging/metrics only, same convention as
    // on_error/on_warning.
    std::function<void(int attempt, std::chrono::milliseconds backoff)> on_reconnecting;

private:
    live::KalshiConfig cfg_;
    live::TickerRegistry registry_;

    // A fresh live::KalshiFeed is constructed for every (re)connect attempt
    // inside run() — never reused or reset in place. KalshiFeed holds a
    // WebSocketClient by value, which holds a TlsSocket by value, and
    // TlsSocket explicitly deletes its copy constructor/assignment without
    // declaring a move constructor/assignment — which, per C++11 rules,
    // suppresses the implicitly-generated move operations too, not just
    // copy. That makes KalshiFeed neither copyable nor movable, so
    // "reset for a reconnect" can only mean "construct a new one" — a
    // unique_ptr, reassigned via std::make_unique, is the straightforward
    // way to do that without auditing every one of KalshiFeed's members
    // (pending_raw_, next_cmd_id_, orderbook_delta_sid_, ...) for correct
    // by-hand reset semantics. handle_message() also reads *feed_ (for
    // request_resync on a detected seq gap), which is why this stays a
    // member rather than a local inside run().
    std::unique_ptr<live::KalshiFeed> feed_;

    std::array<live::SeqTracker, kMaxMarkets> seq_trackers_{};
};

} // namespace px::pipeline
