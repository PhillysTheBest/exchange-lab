// eval/fill_simulator.hpp — Stage 6, Part B.3: the one genuinely new piece of the
// evaluation framework (see docs/BACKTEST_ASSESSMENT.md). Turns the OrderIntents a
// real Strategy produces, plus the real market-data stream driving it, into the
// ExecReports a real Gateway would eventually produce — realistically enough that a
// resting order's fill isn't just asserted into existence.
//
// STANDS IN FOR GATEWAY, NOT FOR IT: the shape of what this returns (Acked always
// first with the real "remaining_count" semantics, THEN Fill if anything crossed at
// entry) deliberately mirrors live/kalshi_gateway.cpp's handle_new_order exactly,
// including the same "IOC never leaves anything resting, regardless of fill amount"
// rule and the same "always push Acked, even at remaining=0, so OrderTracker's
// already-pending entry gets correctly dropped rather than left a zombie" behavior —
// see strategy/order_tracker.hpp's own on_acked comment for why that matters.
//
// TWO MODELING CHOICES, stated explicitly rather than buried in the implementation:
//
// 1. MAKER FILLS ONLY CONSUME QUEUE POSITION FROM REAL BookEventKind::Trade EVENTS,
//    NEVER FROM A BARE LevelDelta DECREASE. A level's aggregate quantity can drop
//    for two different real-world reasons Kalshi's feed doesn't distinguish in the
//    delta itself: a trade (real execution, the mechanism that can reach OUR resting
//    order) or a cancellation (someone else pulling their own resting order, which
//    says nothing reliable about whether we're now closer to the front). Treating a
//    plain delta decrease as queue progress would systematically OVERSTATE how often
//    this strategy would have filled — the wrong direction for an honest evaluation
//    tool to fail in. Only the explicit `trade` channel (BookEventKind::Trade, an
//    unambiguous real execution) is trusted to consume `depth_ahead`.
// 2. THE REPLAYED BOOK IS NEVER MUTATED BY OUR OWN HYPOTHETICAL ORDERS. Real Kalshi
//    aggregate depth, once we're actually resting an order, would include our own
//    quantity — but this evaluator replays HISTORICAL/synthetic depth that was
//    captured (or generated) with no knowledge of our counterfactual participation.
//    This is the standard, unavoidable limitation of any backtester built on
//    aggregate (L2) data without a full order-by-order (L3) reconstruction — the same
//    limitation every L2-based backtester in docs/EXTERNAL_REVIEW.md's survey has —
//    not something a more clever implementation here would avoid.
#pragma once

#include "eval/eval_metrics.hpp"
#include "eval/fees.hpp"
#include "pipeline/events.hpp"
#include "px/shadow_book.hpp"
#include "px/types.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace px::eval {

struct SimulatedFill {
    OrderId id = kInvalidOrderId;
    MarketId market = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
    FillRole role = FillRole::Taker;
    std::int64_t ts_ns = 0;
};

class FillSimulator {
public:
    // A new order Brain just accepted (Brain::handle_decision already ran
    // RiskEngine/lifecycle gating and assigned intent.id before this is ever called
    // — see pipeline/brain.cpp). `book` is the market's real ShadowBook, read-only,
    // reflecting whatever state it was in the instant this order was decided (the
    // same state the Strategy that produced it just saw). Returns the ExecReports
    // this generates, already in the order Brain must see them (Acked, then any
    // Fill) — feed each into a real `pipeline::Brain::apply_exec_report` in order.
    [[nodiscard]] std::vector<pipeline::ExecReport> submit(const strategy::OrderIntent& intent,
                                                            const px::ShadowBook& book, std::int64_t now_ns) {
        std::vector<pipeline::ExecReport> reports;

        bool crosses = false;
        Price touch = 0;
        if (intent.side == Side::Buy) {
            crosses = book.best_ask(touch) && intent.price >= touch;
        } else {
            crosses = book.best_bid(touch) && intent.price <= touch;
        }

        const auto levels = crosses ? sweep_levels(book, intent.side, intent.price, intent.qty)
                                     : std::vector<std::pair<Price, Qty>>{};
        Qty filled_now = 0;
        for (const auto& [p, q] : levels) filled_now += q;
        const Qty remaining_after_cross = intent.qty - filled_now;

        // IOC never leaves anything resting, regardless of how much filled at entry
        // — matches Kalshi's own create-order response semantics (remaining_count
        // reflects post-IOC-kill state), not a naive "requested minus filled."
        const Qty acked_remaining = (intent.tif == TimeInForce::GTC) ? remaining_after_cross : 0;

        pipeline::ExecReport acked;
        acked.kind = pipeline::ExecKind::Acked;
        acked.market = intent.market;
        acked.id = intent.id;
        acked.side = intent.side;
        acked.qty = acked_remaining;
        reports.push_back(acked);

        for (const auto& [p, q] : levels) {
            const Cash fee = taker_fee_cents(p, q);
            fills_.push_back(SimulatedFill{intent.id, intent.market, intent.side, p, q, FillRole::Taker, now_ns});
            fill_fees_[fills_.size() - 1] = fee;

            pipeline::ExecReport fill;
            fill.kind = pipeline::ExecKind::Fill;
            fill.market = intent.market;
            fill.id = intent.id;
            fill.side = intent.side;
            fill.price = p;
            fill.qty = q;
            reports.push_back(fill);
        }

        if (acked_remaining > 0) {
            TrackedOrder t;
            t.id = intent.id;
            t.market = intent.market;
            t.side = intent.side;
            t.price = intent.price;
            t.remaining_qty = acked_remaining;
            // Same UPPER-BOUND-ESTIMATE SPIRIT as strategy/order_tracker.hpp's own
            // on_acked (assume the worst about queue position, never the best),
            // but not literally the same formula: OrderTracker subtracts our own
            // remaining_qty from level_qty because in the LIVE system, by the time
            // Gateway's ack arrives, Kalshi's real aggregate depth already includes
            // our own just-added resting quantity, so that subtraction backs it
            // back out. Here, by construction (see file header, modeling choice 2),
            // the replayed book NEVER includes our hypothetical order at all — so
            // level_qty(price) alone, with no subtraction, already IS "everything
            // genuinely ahead of us."
            t.depth_ahead = book.level_qty(intent.price);
            t.acked_ns = now_ns;
            resting_[intent.market].push_back(t);
        }

        return reports;
    }

    // A cancel Brain accepted. Mirrors live/kalshi_gateway.cpp's handle_cancel: a
    // target this simulator never tracked (already filled, already cancelled) is
    // silently a no-op, not an error — MarketState::outstanding_orders is
    // acked-only by construction (strategy/order_tracker.hpp), so a well-behaved
    // Strategy structurally can't target anything else.
    [[nodiscard]] std::vector<pipeline::ExecReport> cancel(const strategy::OrderIntent& intent) {
        std::vector<pipeline::ExecReport> reports;
        auto& list = resting_[intent.market];
        const auto it = std::find_if(list.begin(), list.end(), [&](const TrackedOrder& o) { return o.id == intent.id; });
        if (it == list.end()) return reports;

        pipeline::ExecReport cancelled;
        cancelled.kind = pipeline::ExecKind::Cancelled;
        cancelled.market = intent.market;
        cancelled.id = intent.id;
        cancelled.side = it->side;
        cancelled.qty = it->remaining_qty; // reduced_by == everything that was left
        reports.push_back(cancelled);

        list.erase(it);
        return reports;
    }

    // A real market Trade this project's own feed reported (BookEventKind::Trade —
    // `taker_side` is the TAKER's side, exactly as live/kalshi_feed.cpp decodes it:
    // Buy means the taker lifted the ask, consuming ASK-side resting depth; Sell
    // means the taker hit the bid, consuming BID-side resting depth). Consumes
    // queue position (see file header, modeling choice 1) for any of our own
    // resting orders on the opposite (maker) side at this exact price, and fills
    // whatever's left once depth_ahead is exhausted. Processed in resting_'s own
    // insertion order (earliest-resting-first — the same price-time-priority
    // intuition the real exchange itself uses).
    [[nodiscard]] std::vector<pipeline::ExecReport> on_trade(MarketId market, Side taker_side, Price price, Qty qty,
                                                              std::int64_t now_ns) {
        std::vector<pipeline::ExecReport> reports;
        const Side maker_side = opposite(taker_side);
        Qty remaining_trade_qty = qty;

        auto& list = resting_[market];
        for (auto it = list.begin(); it != list.end() && remaining_trade_qty > 0;) {
            if (it->side != maker_side || it->price != price) {
                ++it;
                continue;
            }
            if (it->depth_ahead > 0) {
                const Qty consumed = std::min(it->depth_ahead, remaining_trade_qty);
                it->depth_ahead -= consumed;
                remaining_trade_qty -= consumed;
            }
            if (remaining_trade_qty > 0 && it->depth_ahead == 0 && it->remaining_qty > 0) {
                const Qty fill_qty = std::min(it->remaining_qty, remaining_trade_qty);
                const Cash fee = maker_fee_cents(price, fill_qty);
                fills_.push_back(SimulatedFill{it->id, market, it->side, price, fill_qty, FillRole::Maker, now_ns});
                fill_fees_[fills_.size() - 1] = fee;

                pipeline::ExecReport fill;
                fill.kind = pipeline::ExecKind::Fill;
                fill.market = market;
                fill.id = it->id;
                fill.side = it->side;
                fill.price = price;
                fill.qty = fill_qty;
                reports.push_back(fill);

                it->remaining_qty -= fill_qty;
                remaining_trade_qty -= fill_qty;
            }
            if (it->remaining_qty == 0) {
                it = list.erase(it); // fully filled -- nothing left to ever cancel or fill again
            } else {
                ++it;
            }
        }
        return reports;
    }

    [[nodiscard]] const std::vector<SimulatedFill>& fills() const noexcept { return fills_; }
    [[nodiscard]] Cash fee_for(std::size_t fill_index) const noexcept {
        const auto it = fill_fees_.find(fill_index);
        return it == fill_fees_.end() ? 0 : it->second;
    }

private:
    struct TrackedOrder {
        OrderId id = kInvalidOrderId;
        MarketId market = 0;
        Side side = Side::Buy;
        Price price = 0;
        Qty remaining_qty = 0;
        Qty depth_ahead = 0;
        std::int64_t acked_ns = 0;
    };

    // Walks price levels away from the touch, in the direction that WORSENS the
    // price for the aggressor — the same direction OrderBook::submit's own
    // MARKET-sweep discipline walks (px/order_book.hpp) — reading ShadowBook's
    // aggregate qty at each level. Safe to treat every occupied level from the
    // touch to `limit_price` as belonging to the correct side WITHOUT a separate
    // side query: ShadowBook only ever reflects an already-matched (non-crossed)
    // Kalshi book, so every occupied price >= best_ask is necessarily an ask level
    // (a bid resting at or above best_ask would itself be a cross, which a real
    // exchange's matching never leaves standing) — symmetric reasoning for
    // >= best_bid downward walks on the sell side.
    [[nodiscard]] static std::vector<std::pair<Price, Qty>> sweep_levels(const px::ShadowBook& book, Side side,
                                                                          Price limit_price, Qty want_qty) noexcept {
        std::vector<std::pair<Price, Qty>> out;
        Qty remaining = want_qty;
        Price touch = 0;
        if (side == Side::Buy) {
            if (!book.best_ask(touch)) return out;
            for (Price p = touch; p <= limit_price && p <= kMaxPrice && remaining > 0; ++p) {
                const Qty avail = book.level_qty(p);
                if (avail == 0) continue; // occupied levels aren't necessarily contiguous
                const Qty take = std::min(avail, remaining);
                out.emplace_back(p, take);
                remaining -= take;
            }
        } else {
            if (!book.best_bid(touch)) return out;
            for (Price p = touch; p >= limit_price && p >= kMinPrice && remaining > 0; --p) {
                const Qty avail = book.level_qty(p);
                if (avail == 0) continue;
                const Qty take = std::min(avail, remaining);
                out.emplace_back(p, take);
                remaining -= take;
            }
        }
        return out;
    }

    std::unordered_map<MarketId, std::vector<TrackedOrder>> resting_;
    std::vector<SimulatedFill> fills_;
    std::unordered_map<std::size_t, Cash> fill_fees_; // fills_ index -> fee, kept
                                                        // separate so SimulatedFill
                                                        // itself stays a plain,
                                                        // minimal record
};

} // namespace px::eval
