// eval/synthetic_market.hpp — Stage 6, Part B.3/H: a deterministic, seeded generator
// of pipeline::BookEvents DIRECTLY (not Kalshi wire JSON text) for driving
// eval::BacktestRunner.
//
// Deliberately NOT workload::Generator reused as-is: that generator's whole job is
// producing realistic Kalshi WIRE TEXT to re-prove the JSON parsing path
// (live/kalshi_feed.cpp) end to end — already proven correct three separate ways
// (tests/test_feed_parse.cpp, tests/test_workload_generator.cpp,
// tests/test_replay_determinism.cpp). eval::BacktestRunner consumes
// pipeline::BookEvent directly with no JSON parsing step at all — generating JSON
// text only to immediately re-parse it back into the exact same BookEvent shape
// would be pure overhead, not re-verification of anything, and would needlessly
// pull an OpenSSL-linked dependency into a framework whose whole point is
// evaluating strategy decisions, not the wire format. This generator reuses
// workload::Generator's STATISTICAL CONVENTIONS in spirit (std::mt19937_64, seed 42
// default, recency-biased price reuse) at the BookEvent level directly.
//
// NEVER CROSSES THE BOOK, BY CONSTRUCTION — found the hard way, not designed in on
// the first attempt: an earlier version of this generator picked a delta's side and
// price independently and uniformly across the FULL [1,99] range, which meant it
// could (and, empirically, did — see docs/BACKTEST_ASSESSMENT.md) set a bid ABOVE
// an already-resting ask, producing a crossed book real Kalshi's own matching engine
// would never allow. ShadowBook itself trusts the feed and doesn't reject that
// (it's a shadow of whatever the exchange says, not a matching engine — see
// px/shadow_book.hpp), so a crossed synthetic book silently produces a negative
// MarketState::spread and, worse, breaks FillSimulator::sweep_levels' own documented
// invariant ("every occupied price >= best_ask is necessarily an ask level" — only
// true in a book that's never crossed). Fixed the same way real markets are never
// crossed in the first place: bid-side activity is confined to [kMinPrice, 49] and
// ask-side activity to [51, kMaxPrice], with 50 held out as a buffer — best_bid can
// never reach best_ask by construction, not by runtime validation.
//
// HONESTY NOTE, same one docs/BACKTEST_ASSESSMENT.md makes about
// workload::Generator itself: LevelDelta and Trade events are, by default, drawn
// independently — a real Kalshi trade and the orderbook_delta reflecting it are
// causally the SAME underlying match, but neither generator models that link
// unconditionally. This one goes one step further: when the immediately-preceding
// delta reduced (not increased) resting depth at some price, half the time the next
// Trade is deliberately paired against it (same price, taker side inferred from
// which side lost depth) — enough real signal for the fill simulator's
// queue-consumption logic to have something non-decorative to react to some of the
// time, not none of it. Still not a full causal model — flagged, not hidden.
#pragma once

#include "pipeline/events.hpp"
#include "px/types.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <random>

namespace px::eval {

struct SyntheticMarketConfig {
    std::uint64_t seed = 42;
    Price initial_bid = 40; // must stay in [kMinPrice, 49] -- see file header
    Price initial_ask = 55; // must stay in [51, kMaxPrice]
    Qty initial_qty = 100;
};

class SyntheticMarket {
public:
    explicit SyntheticMarket(SyntheticMarketConfig cfg)
        : cfg_(cfg), rng_(cfg.seed), bid_price_dist_(kMinPrice, 49), ask_price_dist_(51, kMaxPrice), qty_dist_(1, 30),
          delta_dist_(-15, 15), kind_roll_(0, 9) {}

    // The two-sided opening snapshot every replay needs before deltas mean
    // anything — mirrors workload::Generator::make_snapshot's own role.
    [[nodiscard]] std::array<pipeline::BookEvent, 2> initial_snapshot(MarketId market) {
        pipeline::BookEvent bid;
        bid.kind = pipeline::BookEventKind::LevelSet;
        bid.market = market;
        bid.side = Side::Buy;
        bid.price = cfg_.initial_bid;
        bid.qty = cfg_.initial_qty;
        pipeline::BookEvent ask;
        ask.kind = pipeline::BookEventKind::LevelSet;
        ask.market = market;
        ask.side = Side::Sell;
        ask.price = cfg_.initial_ask;
        ask.qty = cfg_.initial_qty;
        remember(Side::Buy, cfg_.initial_bid);
        remember(Side::Sell, cfg_.initial_ask);
        return {bid, ask};
    }

    // One more event: 70% LevelDelta, 30% Trade — same weighting
    // workload::Generator's own Normal profile uses for its dominant/secondary mix.
    [[nodiscard]] pipeline::BookEvent next_event(MarketId market) {
        return (kind_roll_(rng_) <= 6) ? make_delta(market) : make_trade(market);
    }

private:
    void remember(Side side, Price p) {
        auto& pool = (side == Side::Buy) ? recent_bid_ : recent_ask_;
        pool.push_back(p);
        if (pool.size() > 16) pool.pop_front();
    }

    // Picks a price VALID FOR RESTING ON `side` — bid-range for Buy, ask-range for
    // Sell, never the other, which is what keeps the book from ever crossing (see
    // file header).
    [[nodiscard]] Price pick_price(Side side) {
        auto& pool = (side == Side::Buy) ? recent_bid_ : recent_ask_;
        if (!pool.empty() && (rng_() % 2 == 0)) return pool[rng_() % pool.size()];
        const auto p = static_cast<Price>((side == Side::Buy) ? bid_price_dist_(rng_) : ask_price_dist_(rng_));
        remember(side, p);
        return p;
    }

    [[nodiscard]] pipeline::BookEvent make_delta(MarketId market) {
        pipeline::BookEvent e;
        e.kind = pipeline::BookEventKind::LevelDelta;
        e.market = market;
        e.side = (rng_() % 2 == 0) ? Side::Buy : Side::Sell;
        e.price = pick_price(e.side);
        e.qty_delta = delta_dist_(rng_);
        last_delta_ = e;
        return e;
    }

    [[nodiscard]] pipeline::BookEvent make_trade(MarketId market) {
        pipeline::BookEvent e;
        e.kind = pipeline::BookEventKind::Trade;
        e.market = market;
        if (last_delta_.qty_delta < 0 && (rng_() % 2 == 0)) {
            // Causally link to the most recent DEPTH-REDUCING delta — see file
            // header. trade.side is the TAKER's side (matches
            // live/kalshi_feed.cpp's own decode convention: Buy = lifted the ask
            // = consumed ask depth; Sell = hit the bid = consumed bid depth), so a
            // delta that reduced BID depth (side=Buy) pairs with a taker who SOLD.
            e.price = last_delta_.price;
            e.side = (last_delta_.side == Side::Buy) ? Side::Sell : Side::Buy;
        } else {
            // A taker buy consumes ASK-side resting depth, so its price must come
            // from the ask pool/range — and symmetrically for a taker sell — same
            // never-cross discipline as make_delta, applied to whichever side is
            // the one actually being consumed, not the taker's own side.
            const bool taker_buys = (rng_() % 2 == 0);
            e.side = taker_buys ? Side::Buy : Side::Sell;
            e.price = pick_price(taker_buys ? Side::Sell : Side::Buy);
        }
        e.qty = static_cast<Qty>(qty_dist_(rng_));
        // Real Kalshi wall-clock epoch-ms, not a synthetic counter — matches
        // Brain::dispatch_market_triggered's own now_ms_wall() (pipeline/brain.cpp),
        // which strategy/trade_window.hpp's TradeWindow compares this directly
        // against. Found the hard way (see docs/BACKTEST_ASSESSMENT.md): Brain
        // itself had a real, separate clock-unit bug on this exact path, fixed
        // alongside this generator once found — a Trade event with no ts_ms at all
        // (this project's BookEvent default, 0) would have been "infinitely old"
        // relative to any real wall-clock now_ms, silently zeroing out
        // TradeWindow::recent_counts() for every strategy that reads it (only
        // TradeFlow does) regardless of how realistic everything else here is. A
        // full backtest run completes in well under TradeWindow's own 5-second
        // recency window, so reading the real clock here (not a synthetic counter
        // scaled to match) is sufficient without needing to thread a shared virtual
        // clock through this generator and Brain separately.
        e.ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
        return e;
    }

    SyntheticMarketConfig cfg_;
    std::mt19937_64 rng_;
    std::uniform_int_distribution<int> bid_price_dist_;
    std::uniform_int_distribution<int> ask_price_dist_;
    std::uniform_int_distribution<int> qty_dist_;
    std::uniform_int_distribution<int> delta_dist_;
    std::uniform_int_distribution<int> kind_roll_;
    std::deque<Price> recent_bid_;
    std::deque<Price> recent_ask_;
    pipeline::BookEvent last_delta_{};
};

} // namespace px::eval
