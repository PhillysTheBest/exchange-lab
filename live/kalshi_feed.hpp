// live/kalshi_feed.hpp — connects to Kalshi's WebSocket feed, decodes messages
// against the verified schema, and (via pump_once) applies market data into a
// px::OrderBook running in shadow mode (see order_book.hpp's shadow_set_level).
//
// SCHEMA STATUS: the outer envelope (`type`/`sid`/`seq`/`msg`) and every field
// this file reads (`market_ticker`, `price_dollars`, `delta_fp`,
// `yes_dollars_fp`/`no_dollars_fp`, `yes_bid_dollars`/`yes_ask_dollars`) are
// confirmed against Kalshi's published AsyncAPI spec (docs.kalshi.com/asyncapi.yaml)
// — this supersedes an earlier version of this file that guessed at unquoted
// integer fields (`price`/`delta`/`yes`/`no`) and was wrong. One thing remains
// genuinely unconfirmed rather than verified: whether `seq` is scoped per
// subscription (shared across every market_ticker on that sid) or per market
// within a subscription. See the SEQ SCOPING comment on request_resync below
// for how that uncertainty is handled without guessing wrong in the unsafe
// direction. Endpoint defaults to Kalshi's **demo** environment; pointing this
// at production is a change the caller of connect() makes explicitly (see
// KalshiConfig::host).
#pragma once

#include "kalshi_auth.hpp"
#include "px/order_book.hpp"
#include "px/types.hpp"
#include "ws_client.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace px::live {

struct KalshiConfig {
    // wss://external-api-ws.demo.kalshi.co/trade-api/ws/v2 by default (demo).
    // Production is wss://external-api-ws.kalshi.com/trade-api/ws/v2 — set this
    // explicitly to switch; there is no flag that flips it implicitly.
    std::string host = "external-api-ws.demo.kalshi.co";
    unsigned short port = 443;
    std::string ws_path = "/trade-api/ws/v2";

    std::string api_key_id;       // KALSHI-ACCESS-KEY
    std::string private_key_path; // PEM file — read at runtime, never embedded in source
};

enum class FeedMsgKind : std::uint8_t { Unknown, Subscribed, Snapshot, Delta, Ticker };

struct FeedLevel {
    Side  side;  // normalized YES-relative — see normalize.hpp
    Price price; // 1..99
    Qty   qty;   // absolute resting quantity at this level (snapshot only)
};

// Decoded result of one WS text message. Deliberately state-independent: a
// Delta carries a *signed change*, not a resolved absolute quantity, so
// parsing never needs to consult (and can't accidentally desync from) whatever
// book state the caller happens to be maintaining. Any consumer — the
// single-market OrderBook path below, or a future multi-market pipeline stage
// — applies the same message the same way.
struct FeedMsg {
    // market_ticker/subscribed_channel are owned strings, not string_views into
    // the raw message: parse_feed_message's caller typically holds the raw
    // buffer in a local that goes out of scope as soon as parsing returns
    // (receive_next, subscribe's confirmation loop), so a borrowed view would
    // dangle by the time the caller inspects it. A short ticker string is a
    // negligible copy next to the network read and JSON scan that produced it.
    FeedMsgKind kind = FeedMsgKind::Unknown;
    std::string market_ticker;
    bool has_seq = false;
    SeqNum seq = 0;

    // Subscribed only.
    std::string subscribed_channel;
    long long subscribed_sid = -1;

    // Snapshot only: full level set, normalized, any length.
    std::vector<FeedLevel> snapshot_levels;

    // Delta only: one level's signed change, normalized.
    Side  delta_side  = Side::Buy;
    Price delta_price = 0;
    long long delta_qty = 0; // signed; caller adds to its own running total

    // Ticker only.
    Price ticker_yes_bid = 0;
    Price ticker_yes_ask = 0;
};

// Pure, stateful gap detector for the `seq` field — isolated from the
// transport specifically so it's unit-testable without a live socket (see
// tests/test_feed_parse.cpp). See the SEQ SCOPING comment in kalshi_feed.cpp
// (next to its use in pump_once) for why this is tracked per watched ticker
// rather than per subscription.
class SeqTracker {
public:
    // Call with each Snapshot/Delta message's seq. Returns true if the message
    // continues an unbroken sequence (or establishes a new baseline, for a
    // Snapshot) and should be applied; false if a gap was detected, in which
    // case the tracker resets (won't trust the stream again until the next
    // Snapshot) and the caller should request a resync and drop this message.
    [[nodiscard]] bool observe(bool is_snapshot, bool has_seq, SeqNum seq) noexcept {
        if (!has_seq) return true; // nothing to check this message against
        if (is_snapshot) { has_last_ = true; last_ = seq; return true; }
        if (!has_last_) return true; // no baseline yet — nothing to compare against
        if (seq != last_ + 1) { has_last_ = false; return false; }
        last_ = seq;
        return true;
    }

private:
    bool has_last_ = false;
    SeqNum last_ = 0;
};

// Parses one raw WS text message per the schema above. Returns false only when
// the outer envelope itself can't be read (no `type` field) or a message whose
// `type` we recognize is missing a field that type requires. An unrecognized
// `type` value is not an error — it decodes to FeedMsgKind::Unknown with
// `error` left empty, since message types this project doesn't act on are
// routine, not exceptional.
[[nodiscard]] bool parse_feed_message(std::string_view raw, FeedMsg& out, std::string& error);

class KalshiFeed {
public:
    // Connects, TLS handshakes, and performs the signed WS upgrade (signs
    // timestamp+"GET"+ws_path per kalshi_auth.hpp). Returns false w/ `error` set
    // on any failure — including a bad/missing private key, which is checked
    // here rather than surfacing as an opaque handshake rejection later.
    [[nodiscard]] bool connect(const KalshiConfig& cfg, std::string& error);

    // Sends the subscribe command, then blocks reading confirmations until a
    // "subscribed" message has been seen for every requested channel (capped —
    // see kalshi_feed.cpp). Captures the orderbook_delta channel's `sid`,
    // needed later for request_resync. Any non-"subscribed" message seen while
    // waiting (e.g. an immediately-arriving snapshot) is buffered and replayed
    // to the caller by the next pump_once/receive_next call rather than dropped.
    [[nodiscard]] bool subscribe(const std::vector<std::string>& channels,
                                  const std::vector<std::string>& market_tickers, std::string& error);

    // Blocks for the next complete message and decodes it. Prefer this over
    // pump_once for anything beyond single-market/single-OrderBook use, since
    // it doesn't couple the caller to px::OrderBook. `error` is set only on a
    // transport failure or a peer-initiated close.
    [[nodiscard]] bool receive_next(FeedMsg& out, std::string& error);

    // Convenience wrapper for a single market: calls receive_next, and if the
    // message is for `watch_ticker`, applies it into `book` (tracking `seq` for
    // that ticker and requesting a resync on a detected gap) or invokes
    // on_ticker. Messages for other tickers or other kinds are decoded and
    // discarded. Returns false on transport error or a peer-initiated close —
    // NOT on a decode/schema issue, which is logged via on_warning instead so
    // one malformed message doesn't tear down an otherwise-healthy connection.
    [[nodiscard]] bool pump_once(std::string_view watch_ticker, OrderBook& book, std::string& error);

    // Requests a fresh orderbook_snapshot for `market_ticker` without altering
    // the subscription (`update_subscription` / `get_snapshot` — see
    // kalshi_feed.cpp). Callers normally don't need this directly; pump_once
    // calls it automatically on a detected seq gap.
    [[nodiscard]] bool request_resync(std::string_view market_ticker, std::string& error);

    std::function<void(std::string_view ticker, Price yes_bid, Price yes_ask)> on_ticker;
    // Invoked for a schema/decode problem that doesn't warrant tearing down the
    // connection (a gap-triggered resync, an unparseable field). Never invoked
    // for transport-level failures, which surface through the `error` out-param
    // instead. Optional — safe to leave unset.
    std::function<void(std::string_view message)> on_warning;

private:
    [[nodiscard]] bool raw_receive(std::string& msg, std::string& error);

    WebSocketClient ws_;
    std::unique_ptr<PrivateKey> key_;
    std::string api_key_id_;
    long long next_cmd_id_ = 1;
    long long orderbook_delta_sid_ = -1;

    // Messages read while waiting for "subscribed" confirmations that turned
    // out not to be confirmations — queued so receive_next still surfaces them
    // (e.g. a snapshot that arrives before the confirmation does) rather than
    // silently dropping the first update after subscribing.
    std::vector<std::string> pending_raw_;

    SeqTracker seq_tracker_; // for pump_once's single watched ticker
};

} // namespace px::live
