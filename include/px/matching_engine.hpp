// px/matching_engine.hpp — sole owner of all mutable state, single-threaded.
//
// MatchingEngine ties Market/OrderBook/Trader together behind one command
// surface: create markets/traders (administrative, may allocate — never called
// from the trading hot path), then submit/cancel orders (the hot path — zero
// allocation, see order_book.hpp). Every Trade the book produces during a
// submit() call is applied to both counterparties' Trader accounts synchronously,
// in the same call, before submit() returns — there is no separate event loop
// to desync from, because everything happens on this one thread.
#pragma once

#include "market.hpp"
#include "trade.hpp"
#include "trader.hpp"
#include "tsc_clock.hpp"
#include "types.hpp"

#include <array>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace px {

class MatchingEngine {
public:
    MatchingEngine() noexcept { traders_.reserve(4096); }

    // ---- administrative (not hot path; may allocate) --------------------------
    [[nodiscard]] MarketId create_market(std::string_view ticker) noexcept;
    bool open_market(MarketId id) noexcept;
    bool halt_market(MarketId id) noexcept;
    bool resume_market(MarketId id) noexcept;
    bool resolve_market(MarketId id, Settlement outcome) noexcept;

    [[nodiscard]] TraderId create_trader(Cash starting_cash) noexcept;

    // ---- hot path: trading commands -------------------------------------------
    struct SubmitReport {
        OrderId order_id   = kInvalidOrderId;
        Qty     filled_qty = 0;
        Qty     rested_qty = 0;
        RejectReason reject = RejectReason::None;
    };

    // Engine assigns and returns the OrderId; the caller uses it for cancel().
    [[nodiscard]] SubmitReport submit(MarketId market_id, TraderId trader_id, Contract contract,
                                       Side side, OrderType type, TimeInForce tif,
                                       Price price, Qty qty) noexcept;

    [[nodiscard]] RejectReason cancel(MarketId market_id, OrderId order_id) noexcept;

    // ---- introspection ----------------------------------------------------------
    [[nodiscard]] Market* market(MarketId id) noexcept;
    [[nodiscard]] const Market* market(MarketId id) const noexcept;
    [[nodiscard]] Trader* trader(TraderId id) noexcept;
    [[nodiscard]] const Trader* trader(TraderId id) const noexcept;

    // Trades produced by the most recently completed submit() call, in the order
    // they occurred. Valid until the next submit() call.
    [[nodiscard]] std::span<const Trade> last_trades() const noexcept {
        return {trade_buf_.data(), trade_count_};
    }

    [[nodiscard]] SeqNum trade_sequence() const noexcept { return seq_; }

private:
    void on_trade(const Trade& raw) noexcept;
    static void trade_sink_thunk(void* ctx, const Trade& t) noexcept;

    std::vector<std::unique_ptr<Market>> markets_;
    std::vector<Trader> traders_;

    SeqNum seq_ = 0;
    OrderId next_order_id_ = 1;

    // Scratch buffer for the trades produced within a single in-flight submit()
    // call. Sized generously; a submit that would produce more trades than this
    // (i.e. it sweeps more than this many distinct resting orders in one call)
    // simply stops recording further ones here — book state and counterparties'
    // cash/position still update correctly for every trade via on_trade(), only
    // the introspection copy is capped.
    static constexpr std::size_t kMaxTradesPerSubmit = 4096;
    std::array<Trade, kMaxTradesPerSubmit> trade_buf_{};
    std::size_t trade_count_ = 0;
};

} // namespace px
