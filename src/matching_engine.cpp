#include "px/matching_engine.hpp"

namespace px {

MarketId MatchingEngine::create_market(std::string_view ticker) noexcept {
    const MarketId id = static_cast<MarketId>(markets_.size());
    markets_.push_back(std::make_unique<Market>(id, ticker));
    return id;
}

bool MatchingEngine::open_market(MarketId id) noexcept {
    Market* m = market(id);
    return m && m->open();
}
bool MatchingEngine::halt_market(MarketId id) noexcept {
    Market* m = market(id);
    return m && m->halt();
}
bool MatchingEngine::resume_market(MarketId id) noexcept {
    Market* m = market(id);
    return m && m->resume();
}
bool MatchingEngine::resolve_market(MarketId id, Settlement outcome) noexcept {
    Market* m = market(id);
    return m && m->resolve(outcome);
}

TraderId MatchingEngine::create_trader(Cash starting_cash) noexcept {
    const TraderId id = static_cast<TraderId>(traders_.size());
    traders_.emplace_back(id, starting_cash);
    return id;
}

Market* MatchingEngine::market(MarketId id) noexcept {
    if (id >= markets_.size()) return nullptr;
    return markets_[id].get();
}
const Market* MatchingEngine::market(MarketId id) const noexcept {
    if (id >= markets_.size()) return nullptr;
    return markets_[id].get();
}
Trader* MatchingEngine::trader(TraderId id) noexcept {
    if (id >= traders_.size()) return nullptr;
    return &traders_[id];
}
const Trader* MatchingEngine::trader(TraderId id) const noexcept {
    if (id >= traders_.size()) return nullptr;
    return &traders_[id];
}

void MatchingEngine::trade_sink_thunk(void* ctx, const Trade& t) noexcept {
    static_cast<MatchingEngine*>(ctx)->on_trade(t);
}

void MatchingEngine::on_trade(const Trade& raw) noexcept {
    Trade t = raw;
    t.seq = seq_++;

    if (trade_count_ < kMaxTradesPerSubmit) {
        trade_buf_[trade_count_++] = t;
    }

    // Zero-sum settlement: the taker's own side, then the maker's side, which is
    // always the exact opposite (they crossed) — cash/position move in equal and
    // opposite amounts between exactly these two accounts, never created or lost.
    if (Trader* taker = trader(t.taker_id)) {
        taker->apply_fill(t.market_id, t.taker_side, t.price, t.qty);
    }
    if (Trader* maker = trader(t.maker_id)) {
        maker->apply_fill(t.market_id, opposite(t.taker_side), t.price, t.qty);
    }
}

MatchingEngine::SubmitReport MatchingEngine::submit(MarketId market_id, TraderId trader_id,
                                                      Contract contract, Side side, OrderType type,
                                                      TimeInForce tif, Price price, Qty qty) noexcept {
    SubmitReport report;

    Market* mkt = market(market_id);
    if (!mkt || !mkt->is_open()) {
        report.reject = RejectReason::MarketNotOpen;
        return report;
    }

    Trader* tr = trader(trader_id);
    if (!tr) {
        report.reject = RejectReason::UnknownOrder; // no such trader account
        return report;
    }

    // Pre-trade risk check operates on the order's own (side, price) as
    // submitted — that formula (Buy: price, Sell: 100-price) is the max possible
    // loss on a contract bounded in [0,100] and holds regardless of contract
    // Yes/No, so no normalization is needed here; see Trader::can_afford.
    const Price risk_price = (type == OrderType::Market)
        ? (side == Side::Buy ? kMaxPrice : kMinPrice)
        : price;
    if (!tr->can_afford(side, risk_price, qty)) {
        report.reject = RejectReason::InsufficientCash;
        return report;
    }

    const OrderId id = next_order_id_++;
    trade_count_ = 0;

    const TradeSink sink(this, &MatchingEngine::trade_sink_thunk);
    const OrderBook::SubmitResult res = mkt->book().submit(
        id, trader_id, contract, side, type, tif, price, qty, read_tsc(), sink);

    report.order_id   = id;
    report.filled_qty = res.filled_qty;
    report.rested_qty = res.rested_qty;
    report.reject     = res.reject;
    return report;
}

RejectReason MatchingEngine::cancel(MarketId market_id, OrderId order_id) noexcept {
    Market* mkt = market(market_id);
    if (!mkt) return RejectReason::MarketNotOpen;
    return mkt->book().cancel(order_id);
}

} // namespace px
