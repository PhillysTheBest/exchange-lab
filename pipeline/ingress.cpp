#include "pipeline/ingress.hpp"
#include "px/platform.hpp" // cpu_relax()

namespace px::pipeline {

void Ingress::run(RingA& out, std::atomic<bool>& stop_flag) {
    feed_.on_warning = [this](std::string_view w) {
        if (on_warning) on_warning(w);
    };

    std::string error;
    if (!feed_.connect(cfg_, error)) {
        if (on_error) on_error("connect failed: " + error);
        return;
    }

    const std::vector<std::string>& tickers = registry_.all_tickers();
    if (!feed_.subscribe({"ticker", "orderbook_delta", "trade"}, tickers, error)) {
        if (on_error) on_error("subscribe failed: " + error);
        return;
    }

    while (!stop_flag.load(std::memory_order_relaxed)) {
        live::FeedMsg fm;
        if (!feed_.receive_next(fm, error)) {
            if (on_error) on_error("receive failed: " + error);
            return;
        }
        handle_message(fm, out);
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

    if (fm.kind != live::FeedMsgKind::Snapshot && fm.kind != live::FeedMsgKind::Delta) return;

    live::SeqTracker& tracker = seq_trackers_[market];
    const bool is_snapshot = (fm.kind == live::FeedMsgKind::Snapshot);
    if (!tracker.observe(is_snapshot, fm.has_seq, fm.seq)) {
        if (on_warning) on_warning("seq gap on " + fm.market_ticker + " — requesting resync");
        std::string resync_err;
        if (!feed_.request_resync(fm.market_ticker, resync_err) && on_warning) {
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
