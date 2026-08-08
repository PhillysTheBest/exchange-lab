// strategy/trade_window.hpp — a fixed-capacity, time-bounded rolling window
// over a market's recent public trades (Kalshi's `trade` WS channel — see
// live/kalshi_feed.cpp, pipeline/events.hpp's BookEventKind::Trade).
//
// "Recent" is defined by exchange time (each trade's own ts_ms), not
// processing time — a burst of trades that all happened within the same
// millisecond on Kalshi's side counts as simultaneous even if T2 processes
// them a few microseconds apart.
//
// A ring buffer, not a std::deque or std::vector: fixed capacity, no
// allocation — consistent with the rest of this project's hot-path
// discipline. Brain calls record() on every BookEventKind::Trade
// (pipeline/brain.cpp) and recent_counts() on every BookUpdate dispatch via
// extract_features (market_state.hpp), which can be hundreds of times per
// second, so all three recency counts are computed in one pass rather than
// three redundant scans.
#pragma once

#include "px/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace px::strategy {

// 64, not a rounder/larger number: at kTradeWindowMs=5000, this comfortably
// covers a brisk 10 trades/sec sustained for the full window (already fast
// for prediction-market trade volume, which tends to be sparse compared to
// e.g. equities) with headroom, while keeping sizeof(TradeWindow) — and by
// extension sizeof(Brain), which holds one per market (see
// pipeline/brain.hpp's SIZE WARNING) — from growing unnecessarily. Widen
// this only if a real deployment's trade volume actually needs it.
inline constexpr std::size_t kTradeWindowCapacity = 64; // ring buffer of recent trades
inline constexpr std::int64_t kTradeWindowMs = 5000;     // "recent" = last 5s of exchange time

struct TradeRecord {
    Side side = Side::Buy; // already YES-normalized — see kalshi_feed.cpp's trade decoder
    Price price = 0;
    Qty qty = 0;
    std::int64_t ts_ms = 0;
};

struct TradeCounts {
    std::size_t total = 0;
    std::size_t taker_buy = 0;
    std::size_t taker_sell = 0;
};

class TradeWindow {
public:
    void record(Side side, Price price, Qty qty, std::int64_t ts_ms) noexcept {
        trades_[head_] = TradeRecord{side, price, qty, ts_ms};
        head_ = (head_ + 1) % kTradeWindowCapacity;
        if (count_ < kTradeWindowCapacity) ++count_;
    }

    // Trades with ts_ms >= (now_ms - kTradeWindowMs). If a burst exhausts the
    // ring's capacity before now_ms advances past the window, this
    // undercounts (older trades fell off the ring entirely) — an explicit,
    // documented limitation of a fixed-size ring rather than an unbounded
    // log, consistent with every other bounded structure in this project.
    [[nodiscard]] TradeCounts recent_counts(std::int64_t now_ms) const noexcept {
        TradeCounts c;
        const std::int64_t cutoff = now_ms - kTradeWindowMs;

        // Walk backward from the most recently written entry; stop at the
        // first trade older than the cutoff. Trades are recorded in
        // non-decreasing ts_ms order in practice (Kalshi delivers them in
        // real time) — an out-of-order trade older than its neighbors simply
        // stops being counted past that point: a conservative undercount,
        // never a crash or an infinite scan.
        std::size_t seen = 0;
        std::size_t idx = (head_ + kTradeWindowCapacity - 1) % kTradeWindowCapacity;
        while (seen < count_) {
            const TradeRecord& t = trades_[idx];
            if (t.ts_ms < cutoff) break;
            ++c.total;
            if (t.side == Side::Buy) ++c.taker_buy;
            else                     ++c.taker_sell;
            idx = (idx + kTradeWindowCapacity - 1) % kTradeWindowCapacity;
            ++seen;
        }
        return c;
    }

private:
    std::array<TradeRecord, kTradeWindowCapacity> trades_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

} // namespace px::strategy
