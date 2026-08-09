#include "pipeline/ingress.hpp"
#include "px/platform.hpp" // cpu_relax()

#include <algorithm>
#include <chrono>
#include <thread>

namespace px::pipeline {

namespace {

constexpr std::chrono::milliseconds kInitialBackoff{1000};
constexpr std::chrono::milliseconds kMaxBackoff{30000};

// Capped exponential backoff: 1s, 2s, 4s, 8s, 16s, 30s, 30s, ... A `current`
// of zero (the very first failure since the connection was last healthy)
// starts the schedule at kInitialBackoff rather than at zero — there's no
// reason to hammer a connection that just failed with an instant retry.
[[nodiscard]] std::chrono::milliseconds next_backoff(std::chrono::milliseconds current) noexcept {
    if (current.count() == 0) return kInitialBackoff;
    return std::min(current * 2, kMaxBackoff);
}

// Sleeps up to `dur` in short slices, checking stop_flag between each one, so
// a stop request arriving mid-backoff is honored within kPollSlice rather
// than waited out to the full (possibly 30s) backoff duration. Returns false
// iff stop_flag became true during the wait — the caller should give up, not
// retry.
[[nodiscard]] bool sleep_or_stop(std::chrono::milliseconds dur, std::atomic<bool>& stop_flag) noexcept {
    constexpr std::chrono::milliseconds kPollSlice{100};
    auto remaining = dur;
    while (remaining.count() > 0) {
        if (stop_flag.load(std::memory_order_relaxed)) return false;
        const auto slice = std::min(remaining, kPollSlice);
        std::this_thread::sleep_for(slice);
        remaining -= slice;
    }
    return !stop_flag.load(std::memory_order_relaxed);
}

} // namespace

void Ingress::run(RingA& out, std::atomic<bool>& stop_flag) {
    int attempt = 0;
    std::chrono::milliseconds backoff{0};

    while (!stop_flag.load(std::memory_order_relaxed)) {
        ++attempt;
        if (on_reconnecting) on_reconnecting(attempt, backoff);

        // A fresh KalshiFeed for every attempt — see ingress.hpp's feed_
        // comment for why this can't be a reused/reset-in-place object.
        feed_ = std::make_unique<live::KalshiFeed>();
        feed_->on_warning = [this](std::string_view w) {
            if (on_warning) on_warning(w);
        };

        std::string error;
        if (!feed_->connect(cfg_, error)) {
            if (on_error) on_error("connect failed (attempt " + std::to_string(attempt) + "): " + error);
            backoff = next_backoff(backoff);
            if (!sleep_or_stop(backoff, stop_flag)) return;
            continue;
        }

        const std::vector<std::string>& tickers = registry_.all_tickers();
        if (!feed_->subscribe({"ticker", "orderbook_delta", "trade"}, tickers, error)) {
            if (on_error) on_error("subscribe failed (attempt " + std::to_string(attempt) + "): " + error);
            backoff = next_backoff(backoff);
            if (!sleep_or_stop(backoff, stop_flag)) return;
            continue;
        }

        // Reconnected (attempt > 1, i.e. this isn't the very first connect of
        // this run): every watched market's ShadowBook and SeqTracker
        // baseline are stale relative to whatever happened on the wire during
        // the outage. Request a fresh snapshot per ticker — reusing
        // request_resync (the same mechanism a seq gap already triggers)
        // rather than assuming a bare subscribe() delivers one on its own,
        // which isn't independently verified against Kalshi's docs. The
        // resulting Snapshot message does the actual resync work through
        // already-tested paths: SnapshotBegin clears the ShadowBook
        // (pipeline/brain.cpp) and a Snapshot unconditionally re-baselines
        // SeqTracker (kalshi_feed.hpp) — nothing new to get wrong here.
        if (attempt > 1) {
            for (const auto& ticker : tickers) {
                std::string resync_err;
                if (!feed_->request_resync(ticker, resync_err) && on_warning) {
                    on_warning("post-reconnect resync request failed for " + ticker + ": " + resync_err);
                }
            }
        }

        backoff = std::chrono::milliseconds{0}; // healthy again — the next failure starts its own schedule fresh

        bool connection_alive = true;
        while (connection_alive && !stop_flag.load(std::memory_order_relaxed)) {
            live::FeedMsg fm;
            if (!feed_->receive_next(fm, error)) {
                if (on_error) on_error("receive failed: " + error + " — reconnecting");
                connection_alive = false;
                break;
            }
            handle_message(fm, out);
        }

        if (stop_flag.load(std::memory_order_relaxed)) return; // normal shutdown, not a failure — no backoff
        backoff = next_backoff(backoff);
        if (!sleep_or_stop(backoff, stop_flag)) return;
        // loop back to the top: fresh KalshiFeed, reconnect.
    }
}

void Ingress::handle_message(const live::FeedMsg& fm, RingA& out) {
    if (fm.kind == live::FeedMsgKind::Unknown || fm.kind == live::FeedMsgKind::Subscribed) return;

    MarketId market;
    if (!registry_.find(fm.market_ticker, market)) return; // arrived for a ticker we didn't ask to watch

    if (fm.kind == live::FeedMsgKind::Ticker) {
        BookEvent e;
        e.kind = BookEventKind::Ticker;
        e.market = market;
        e.price = fm.ticker_yes_bid;
        e.ask_price = fm.ticker_yes_ask;
        while (!out.try_push(e)) cpu_relax();
        return;
    }

    if (fm.kind == live::FeedMsgKind::Trade) {
        BookEvent e;
        e.kind = BookEventKind::Trade;
        e.market = market;
        e.side = fm.trade_side;
        e.price = fm.trade_price;
        e.qty = fm.trade_qty;
        e.ts_ms = fm.trade_ts_ms;
        while (!out.try_push(e)) cpu_relax();
        return;
    }

    if (fm.kind == live::FeedMsgKind::Lifecycle) {
        if (!fm.has_status_change) return; // created/close_date_updated/etc. -- informational, nothing to gate
        BookEvent e;
        e.kind = BookEventKind::LifecycleStatus;
        e.market = market;
        e.lifecycle_status = fm.lifecycle_status;
        while (!out.try_push(e)) cpu_relax();
        return;
    }

    if (fm.kind != live::FeedMsgKind::Snapshot && fm.kind != live::FeedMsgKind::Delta) return;

    live::SeqTracker& tracker = seq_trackers_[market];
    const bool is_snapshot = (fm.kind == live::FeedMsgKind::Snapshot);
    if (!tracker.observe(is_snapshot, fm.has_seq, fm.seq)) {
        if (on_warning) on_warning("seq gap on " + fm.market_ticker + " — requesting resync");
        std::string resync_err;
        if (!feed_->request_resync(fm.market_ticker, resync_err) && on_warning) {
            on_warning("resync request failed: " + resync_err);
        }
        return; // this message is against a book we now know is stale — drop it, a snapshot follows
    }

    if (is_snapshot) {
        // SnapshotBegin tells T2 to clear this market's ShadowBook before
        // applying the LevelSet run that follows — otherwise stale levels from
        // before a gap (or simply from before the initial snapshot arrived)
        // would linger. See pipeline/brain.cpp.
        BookEvent begin;
        begin.kind = BookEventKind::SnapshotBegin;
        begin.market = market;
        while (!out.try_push(begin)) cpu_relax();

        for (const auto& lvl : fm.snapshot_levels) {
            BookEvent e;
            e.kind = BookEventKind::LevelSet;
            e.market = market;
            e.side = lvl.side;
            e.price = lvl.price;
            e.qty = lvl.qty;
            while (!out.try_push(e)) cpu_relax();
        }
    } else {
        BookEvent e;
        e.kind = BookEventKind::LevelDelta;
        e.market = market;
        e.side = fm.delta_side;
        e.price = fm.delta_price;
        e.qty_delta = static_cast<std::int32_t>(fm.delta_qty);
        while (!out.try_push(e)) cpu_relax();
    }
}

} // namespace px::pipeline
