// px/market.hpp — a single market's metadata, lifecycle, and its OrderBook.
#pragma once

#include "order_book.hpp"
#include "types.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace px {

class Market {
public:
    static constexpr std::size_t kTickerCap = 32; // e.g. "KXHIGHNY-25AUG08-B89.5"

    Market(MarketId id, std::string_view ticker) noexcept : id_(id), book_(id) {
        const std::size_t n = std::min(ticker.size(), kTickerCap - 1);
        std::memcpy(ticker_, ticker.data(), n);
        ticker_[n] = '\0';
    }

    [[nodiscard]] MarketId id() const noexcept { return id_; }
    [[nodiscard]] std::string_view ticker() const noexcept { return ticker_; }
    [[nodiscard]] MarketStatus status() const noexcept { return status_; }
    [[nodiscard]] Settlement settlement() const noexcept { return settlement_; }
    [[nodiscard]] bool is_open() const noexcept { return status_ == MarketStatus::Open; }

    [[nodiscard]] OrderBook& book() noexcept { return book_; }
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }

    // Lifecycle: PreOpen -> Open <-> Halted -> Resolved. Resolved is terminal.
    // Each transition returns false (and does nothing) if called from a status
    // that doesn't permit it — callers should surface that as a reject, not a
    // crash; markets moving through their lifecycle out of order is routine.
    bool open() noexcept {
        if (status_ != MarketStatus::PreOpen) return false;
        status_ = MarketStatus::Open;
        return true;
    }
    bool halt() noexcept {
        if (status_ != MarketStatus::Open) return false;
        status_ = MarketStatus::Halted;
        return true;
    }
    bool resume() noexcept {
        if (status_ != MarketStatus::Halted) return false;
        status_ = MarketStatus::Open;
        return true;
    }
    bool resolve(Settlement outcome) noexcept {
        if (status_ == MarketStatus::Resolved) return false;
        if (outcome != Settlement::Yes && outcome != Settlement::No) return false;
        status_ = MarketStatus::Resolved;
        settlement_ = outcome;
        return true;
    }

private:
    MarketId id_;
    char ticker_[kTickerCap]{};
    MarketStatus status_ = MarketStatus::PreOpen;
    Settlement settlement_ = Settlement::Unresolved;
    OrderBook book_;
};

} // namespace px
