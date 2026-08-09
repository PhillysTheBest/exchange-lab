#include "kalshi_feed.hpp"
#include "json_scan.hpp"

namespace px::live {

bool parse_feed_message(std::string_view raw, FeedMsg& out, std::string& error) {
    out = FeedMsg{};

    std::string_view type;
    if (!json_extract_string(raw, "type", type)) {
        error = "message has no \"type\" field";
        return false;
    }

    long long seq_ll = 0;
    out.has_seq = json_extract_int(raw, "seq", seq_ll);
    if (out.has_seq) out.seq = static_cast<SeqNum>(seq_ll);

    if (type == "subscribed") {
        out.kind = FeedMsgKind::Subscribed;
        std::string_view channel;
        long long sid = -1;
        if (!json_extract_string(raw, "channel", channel) || !json_extract_int(raw, "sid", sid)) {
            error = "subscribed message missing channel/sid";
            return false;
        }
        out.subscribed_channel = std::string(channel);
        out.subscribed_sid = sid;
        return true;
    }

    if (type == "orderbook_snapshot") {
        out.kind = FeedMsgKind::Snapshot;
        std::string_view ticker;
        if (!json_extract_string(raw, "market_ticker", ticker)) {
            error = "orderbook_snapshot missing market_ticker";
            return false;
        }
        out.market_ticker = std::string(ticker);

        // Both arrays are documented optional (a side with no resting interest
        // is simply absent) — a missing array is not a parse failure.
        std::vector<std::pair<long long, long long>> yes_pairs, no_pairs;
        (void)json_extract_dollar_pair_array(raw, "yes_dollars_fp", yes_pairs);
        (void)json_extract_dollar_pair_array(raw, "no_dollars_fp", no_pairs);

        out.snapshot_levels.reserve(yes_pairs.size() + no_pairs.size());
        for (const auto& [cents, qty] : yes_pairs) {
            if (cents < kMinPrice || cents > kMaxPrice || qty < 0) continue;
            out.snapshot_levels.push_back({Side::Buy, static_cast<Price>(cents), static_cast<Qty>(qty)});
        }
        for (const auto& [cents, qty] : no_pairs) {
            if (cents < kMinPrice || cents > kMaxPrice || qty < 0) continue;
            // A resting NO bid at price p is a Contract::No, Side::Buy order —
            // normalize() gives its YES-equivalent, same transform as live order
            // entry (see normalize.hpp).
            const auto [nside, nprice] = normalize(Contract::No, Side::Buy, static_cast<Price>(cents));
            out.snapshot_levels.push_back({nside, nprice, static_cast<Qty>(qty)});
        }
        return true;
    }

    if (type == "orderbook_delta") {
        out.kind = FeedMsgKind::Delta;
        std::string_view ticker, side_str;
        long long price_cents = 0, delta_qty = 0;
        if (!json_extract_string(raw, "market_ticker", ticker) ||
            !json_extract_dollars_as_cents(raw, "price_dollars", price_cents) ||
            !json_extract_fixed_point_qty(raw, "delta_fp", delta_qty) ||
            !json_extract_string(raw, "side", side_str)) {
            error = "orderbook_delta missing a required field";
            return false;
        }
        if (price_cents < kMinPrice || price_cents > kMaxPrice) {
            error = "orderbook_delta price_dollars out of [1,99] cent range";
            return false;
        }
        out.market_ticker = std::string(ticker);
        const Contract c = (side_str == "no") ? Contract::No : Contract::Yes;
        const auto [nside, nprice] = normalize(c, Side::Buy, static_cast<Price>(price_cents));
        out.delta_side = nside;
        out.delta_price = nprice;
        out.delta_qty = delta_qty;
        return true;
    }

    if (type == "ticker") {
        out.kind = FeedMsgKind::Ticker;
        std::string_view ticker;
        long long bid_cents = 0, ask_cents = 0;
        if (!json_extract_string(raw, "market_ticker", ticker) ||
            !json_extract_dollars_as_cents(raw, "yes_bid_dollars", bid_cents) ||
            !json_extract_dollars_as_cents(raw, "yes_ask_dollars", ask_cents)) {
            error = "ticker message missing a required field";
            return false;
        }
        out.market_ticker = std::string(ticker);
        out.ticker_yes_bid = static_cast<Price>(bid_cents);
        out.ticker_yes_ask = static_cast<Price>(ask_cents);
        return true;
    }

    if (type == "trade") {
        out.kind = FeedMsgKind::Trade;
        std::string_view ticker, book_side;
        long long price_cents = 0, qty = 0, ts_ms = 0;
        if (!json_extract_string(raw, "market_ticker", ticker) ||
            !json_extract_dollars_as_cents(raw, "yes_price_dollars", price_cents) ||
            !json_extract_fixed_point_qty(raw, "count_fp", qty) ||
            !json_extract_string(raw, "taker_book_side", book_side) ||
            !json_extract_int(raw, "ts_ms", ts_ms)) {
            error = "trade message missing a required field";
            return false;
        }
        if (price_cents < kMinPrice || price_cents > kMaxPrice) {
            error = "trade yes_price_dollars out of [1,99] cent range";
            return false;
        }
        out.market_ticker = std::string(ticker);
        // "ask" means the taker crossed the ask (bought); "bid" means they
        // crossed the bid (sold) — see this file's SCHEMA STATUS note for why
        // this is the field this project reads, not taker_side/taker_outcome_side.
        out.trade_side = (book_side == "ask") ? Side::Buy : Side::Sell;
        out.trade_price = static_cast<Price>(price_cents);
        out.trade_qty = static_cast<Qty>(qty < 0 ? 0 : qty);
        out.trade_ts_ms = ts_ms;
        return true;
    }

    if (type == "market_lifecycle_v2") {
        out.kind = FeedMsgKind::Lifecycle;
        std::string_view ticker, event_type;
        if (!json_extract_string(raw, "market_ticker", ticker) || !json_extract_string(raw, "event_type", event_type)) {
            error = "market_lifecycle_v2 missing market_ticker/event_type";
            return false;
        }
        out.market_ticker = std::string(ticker);
        out.lifecycle_event_type = std::string(event_type);

        // See this file's SCHEMA STATUS note for the full event_type ->
        // MarketStatus mapping and why there's no "halted" value to map
        // from directly. Anything not listed here (created,
        // close_date_updated, price_level_structure_updated,
        // metadata_updated) leaves has_status_change false — decoded, not
        // dropped as Unknown, but implying no tradeability change.
        if (event_type == "activated") {
            out.has_status_change = true;
            out.lifecycle_status = MarketStatus::Open;
        } else if (event_type == "deactivated") {
            out.has_status_change = true;
            out.lifecycle_status = MarketStatus::Halted;
        } else if (event_type == "determined" || event_type == "settled") {
            out.has_status_change = true;
            out.lifecycle_status = MarketStatus::Resolved;
        }
        return true;
    }

    // Recognized-but-unhandled or genuinely unknown type (fill, error, ...)
    // — not this project's concern yet, and deliberately not treated as an
    // error: a type we don't act on arriving on the wire is routine, not
    // exceptional.
    out.kind = FeedMsgKind::Unknown;
    return true;
}

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

    // Block for a "subscribed" confirmation per requested channel. Anything
    // else received meanwhile (plausibly an orderbook_snapshot arriving before
    // its own channel's confirmation does) is queued in pending_raw_ for
    // receive_next rather than dropped — the first update after subscribing
    // matters. Capped generously: a healthy server confirms promptly, and a
    // cap that's never hit costs nothing.
    std::size_t confirmed = 0;
    const std::size_t max_attempts = channels.size() * 8 + 8;
    for (std::size_t attempt = 0; attempt < max_attempts && confirmed < channels.size(); ++attempt) {
        std::string raw;
        if (!ws_.receive_message(raw, error)) return false;

        FeedMsg fm;
        std::string parse_err;
        if (!parse_feed_message(raw, fm, parse_err)) {
            pending_raw_.push_back(std::move(raw)); // undecodable here — let the caller see it
            continue;
        }
        if (fm.kind == FeedMsgKind::Subscribed) {
            ++confirmed;
            if (fm.subscribed_channel == "orderbook_delta") orderbook_delta_sid_ = fm.subscribed_sid;
        } else {
            pending_raw_.push_back(std::move(raw));
        }
    }

    if (confirmed < channels.size()) {
        error = "did not receive \"subscribed\" confirmation for all " + std::to_string(channels.size()) +
                " channel(s) (" + std::to_string(confirmed) + " confirmed) — the server may have rejected " +
                "one of the requested channels or tickers";
        return false;
    }
    return true;
}

bool KalshiFeed::raw_receive(std::string& msg, std::string& error) {
    if (!pending_raw_.empty()) {
        msg = std::move(pending_raw_.front());
        pending_raw_.erase(pending_raw_.begin()); // small, rare queue — O(n) here is fine
        return true;
    }
    return ws_.receive_message(msg, error);
}

bool KalshiFeed::receive_next(FeedMsg& out, std::string& error) {
    std::string raw;
    if (!raw_receive(raw, error)) return false;

    std::string parse_err;
    if (!parse_feed_message(raw, out, parse_err)) {
        if (on_warning) on_warning("decode: " + parse_err);
        out = FeedMsg{}; // Unknown/empty — transport is fine, this one message just didn't decode
    }
    return true;
}

bool KalshiFeed::request_resync(std::string_view market_ticker, std::string& error) {
    if (orderbook_delta_sid_ < 0) {
        error = "no orderbook_delta subscription id yet — call subscribe() first";
        return false;
    }
    const std::string j = "{\"id\":" + std::to_string(next_cmd_id_++) +
                           ",\"cmd\":\"update_subscription\",\"params\":{\"sids\":[" +
                           std::to_string(orderbook_delta_sid_) +
                           "],\"action\":\"get_snapshot\",\"market_tickers\":[\"" + std::string(market_ticker) +
                           "\"]}}";
    if (!ws_.send_text(j)) {
        error = "failed to send update_subscription/get_snapshot command";
        return false;
    }
    return true;
}

bool KalshiFeed::pump_once(std::string_view watch_ticker, OrderBook& book, std::string& error) {
    FeedMsg fm;
    if (!receive_next(fm, error)) return false;

    if (fm.kind == FeedMsgKind::Ticker) {
        if (fm.market_ticker == watch_ticker && on_ticker) on_ticker(watch_ticker, fm.ticker_yes_bid, fm.ticker_yes_ask);
        return true;
    }
    if (fm.kind != FeedMsgKind::Snapshot && fm.kind != FeedMsgKind::Delta) return true; // not ours to apply
    if (fm.market_ticker != watch_ticker) return true;

    // SEQ SCOPING (genuinely unconfirmed — flagged rather than guessed silently):
    // the spec documents `seq` as "used for snapshot/delta consistency" but does
    // not state whether it's a single counter per subscription (shared across
    // every market_ticker on that sid) or scoped per market within a shared
    // subscription. This project subscribes one ticker at a time (see
    // tools/kalshi_shadow.cpp), so it can't observe the difference directly.
    // Tracked here (SeqTracker, per KalshiFeed instance = per watched ticker)
    // rather than per sid: if the true scope turns out to be per-sid and this
    // connection ever subscribes to more than one ticker, the failure mode of
    // this choice is an occasional *unnecessary* resync (harmless — get_snapshot
    // is idempotent) rather than a *missed* gap (which would silently corrupt
    // the book). Fails safe either way.
    const bool is_snapshot = (fm.kind == FeedMsgKind::Snapshot);
    if (!seq_tracker_.observe(is_snapshot, fm.has_seq, fm.seq)) {
        if (on_warning) {
            on_warning("seq gap on " + std::string(watch_ticker) + " (got " + std::to_string(fm.seq) +
                       ") — requesting resync");
        }
        std::string resync_err;
        if (!request_resync(watch_ticker, resync_err) && on_warning) {
            on_warning("resync request failed: " + resync_err);
        }
        return true; // this delta is against a book we now know is stale — drop it
    }

    if (fm.kind == FeedMsgKind::Snapshot) {
        // A snapshot (initial or resync-triggered) replaces state rather than
        // merging with it — otherwise stale levels from before a gap would
        // linger forever. Cheap and rare (once at connect, occasionally on
        // resync), not a hot-path cost.
        for (Price p = kMinPrice; p <= kMaxPrice; ++p) {
            book.shadow_set_level(Side::Buy, p, 0);
            book.shadow_set_level(Side::Sell, p, 0);
        }
        for (const auto& lvl : fm.snapshot_levels) {
            book.shadow_set_level(lvl.side, lvl.price, lvl.qty);
        }
    } else { // Delta — apply the signed change against the book's own current total
        const long long current = static_cast<long long>(book.level(fm.delta_price).total_qty);
        const long long updated = current + fm.delta_qty;
        book.shadow_set_level(fm.delta_side, fm.delta_price, static_cast<Qty>(updated < 0 ? 0 : updated));
    }
    return true;
}

} // namespace px::live
