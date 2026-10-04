// arena/accounting.hpp — what each participant is actually worth, and how badly
// it is being picked off.
//
// P&L IS MARKED, NOT REALISED. Realised P&L alone is not a measure of anything
// in a session where a participant can end holding inventory: a market maker
// that is losing can hide it by not closing, and the bot that happens to be flat
// at the horn looks best. So every account is marked:
//
//     value = cash + position * mark - fees
//
// with `mark` the fundamental value V (arena/fair_value.hpp), which is the only
// mark nobody in the session can influence. Marking at the mid would let a
// participant improve its own reported P&L by moving the mid, which is not a
// measurement, it is a loophole.
//
// THE FREE INVARIANT. The session is closed: every fill moves cash between two
// accounts and quantity the other way, so with zero fees
//
//     sum of cash = 0,  sum of position = 0,  therefore sum of value = 0
//
// exactly, in integers, at every instant. Nothing else in the arena is checkable
// that cheaply, and the entire latency sweep is read off these numbers, so
// test_arena asserts it on a full session rather than trusting it.
//
// ADVERSE SELECTION, DEFINED — AND SEPARATED FROM THE SPREAD. "Adverse
// selection" gets quoted loosely often enough that a number labelled with it
// means little, so each fill is decomposed into two parts that add up exactly.
// With H the horizon, V the fundamental value, and q positive when the account
// bought:
//
//     edge   = (V(t+H) - fill_price) * q    what the fill was worth in the end
//     spread = (V(t)   - fill_price) * q    what it earned for providing liquidity
//     adverse= (V(t+H) - V(t))      * q     what the price did to it afterwards
//     edge   = spread + adverse              identically, in integers
//
// The decomposition is the whole point, and getting it wrong was the first
// version's mistake. A maker buys below fair value and sells above it, so its
// `edge` is positive almost by construction — that is the spread, not skill, and
// reporting it alone made a *slower* maker look better because a staler quote
// sits further from fair value and so captures more spread on every fill it does
// get. `adverse` contains no spread at all: it is only the price move after the
// trade, so it is negative exactly when the filler was on the wrong side of
// something it did not know. That is the number the latency experiment turns.
//
// Both are reported as a mean per fill in price units, with the share of fills
// whose `adverse` was negative beside them. The horizon is a session parameter
// and is printed with every result. Fills still inside their horizon when the
// session ends are resolved against the final V, and that count is reported too.
//
// Units: `cash` is in price units times quantity, where kPriceScale price units
// make one currency unit. Division into currency happens in the reporting code
// and nowhere else, because a session contains no floating point.
#pragma once

#include "arena/clock.hpp"
#include "px/types.hpp"

#include <cstdint>
#include <deque>
#include <vector>

namespace px::arena {

struct Account {
    std::int64_t position = 0; // signed quantity held
    std::int64_t cash = 0;     // price units * quantity, negative when long
    std::int64_t fees = 0;
    std::uint64_t fills = 0;
    std::uint64_t maker_fills = 0; // filled as the resting side
    std::uint64_t taker_fills = 0; // filled as the aggressor
    std::uint64_t filled_qty = 0;
    std::uint64_t orders_sent = 0;
    std::uint64_t rejects = 0;
    std::uint64_t self_trade_cancels = 0; // own quote killed by STP, not filled

    // The decomposition above, accumulated as fills reach their horizon.
    std::int64_t edge_sum = 0;    // (V(t+H) - price) * q
    std::int64_t spread_sum = 0;  // (V(t)   - price) * q
    std::int64_t adverse_sum = 0; // (V(t+H) - V(t))  * q, and edge == spread + adverse
    std::uint64_t markout_n = 0;
    std::uint64_t adverse_fills = 0;    // fills whose `adverse` was negative
    std::uint64_t unresolved_fills = 0; // resolved early, at the session's end

    [[nodiscard]] std::int64_t value(Price mark) const noexcept {
        return cash + position * static_cast<std::int64_t>(mark) - fees;
    }
    // Means per fill in price units, scaled by 1000 so that integer division
    // keeps three decimals without letting a double into a session.
    [[nodiscard]] std::int64_t edge_milli() const noexcept { return per_fill(edge_sum); }
    [[nodiscard]] std::int64_t spread_milli() const noexcept { return per_fill(spread_sum); }
    [[nodiscard]] std::int64_t adverse_milli() const noexcept { return per_fill(adverse_sum); }

private:
    [[nodiscard]] std::int64_t per_fill(std::int64_t sum) const noexcept {
        return markout_n == 0 ? 0 : sum * 1000 / static_cast<std::int64_t>(markout_n);
    }
};

class Ledger {
public:
    void reset(std::size_t accounts, SimTime horizon_ns, std::int64_t fee_per_qty) {
        accounts_.assign(accounts, Account{});
        pending_.clear();
        horizon_ = horizon_ns;
        fee_ = fee_per_qty;
        trades_ = 0;
        volume_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return accounts_.size(); }
    [[nodiscard]] Account& at(std::size_t i) noexcept { return accounts_[i]; }
    [[nodiscard]] const Account& at(std::size_t i) const noexcept { return accounts_[i]; }
    [[nodiscard]] std::uint64_t trades() const noexcept { return trades_; }
    [[nodiscard]] std::uint64_t volume() const noexcept { return volume_; }

    // One Trade event, booked to both sides. `taker`/`maker` are account indices,
    // `taker_side` is the aggressor's side so the maker takes the other, and
    // `v_now` is the fundamental value at the instant of the fill — needed for
    // the spread/adverse split, which is meaningless without it.
    void on_trade(std::size_t taker, std::size_t maker, Side taker_side, Price price, Qty qty, SimTime at,
                  Price v_now) {
        ++trades_;
        volume_ += qty;
        book_side(taker, taker_side, price, qty, at, v_now, false);
        book_side(maker, opposite(taker_side), price, qty, at, v_now, true);
    }

    // Resolves every pending markout whose horizon has passed. `v_now` must be
    // the fundamental value at `now`; the session calls this whenever V moves or
    // the clock advances past a horizon.
    void resolve_due(SimTime now, Price v_now) {
        while (!pending_.empty() && pending_.front().due <= now) {
            settle(pending_.front(), v_now, false);
            pending_.pop_front();
        }
    }

    // Resolves whatever is left against the final V, counting each one so a
    // reader can see how much of the markout was cut short.
    void resolve_all(Price v_final) {
        while (!pending_.empty()) {
            settle(pending_.front(), v_final, true);
            pending_.pop_front();
        }
    }

    [[nodiscard]] std::size_t pending_markouts() const noexcept { return pending_.size(); }

    // The invariants from the header comment, as one call. True in a closed
    // session with zero fees; with fees it is the fee total that leaks, so the
    // caller is told the sums and decides.
    [[nodiscard]] std::int64_t cash_sum() const noexcept {
        std::int64_t s = 0;
        for (const Account& a : accounts_) s += a.cash;
        return s;
    }
    [[nodiscard]] std::int64_t position_sum() const noexcept {
        std::int64_t s = 0;
        for (const Account& a : accounts_) s += a.position;
        return s;
    }
    [[nodiscard]] std::int64_t value_sum(Price mark) const noexcept {
        std::int64_t s = 0;
        for (const Account& a : accounts_) s += a.value(mark);
        return s;
    }

private:
    struct PendingMarkout {
        SimTime due = 0;
        std::size_t account = 0;
        Price price = 0;
        Price v_at_fill = 0;         // what fair value was when it traded
        std::int64_t signed_qty = 0; // positive when the account bought
    };

    void book_side(std::size_t who, Side side, Price price, Qty qty, SimTime at, Price v_now, bool as_maker) {
        if (who >= accounts_.size()) return;
        Account& a = accounts_[who];
        const std::int64_t q = static_cast<std::int64_t>(qty);
        const std::int64_t signed_qty = side == Side::Buy ? q : -q;
        a.position += signed_qty;
        a.cash -= signed_qty * static_cast<std::int64_t>(price);
        a.fees += fee_ * q;
        ++a.fills;
        if (as_maker) {
            ++a.maker_fills;
        } else {
            ++a.taker_fills;
        }
        a.filled_qty += qty;
        pending_.push_back(PendingMarkout{at + horizon_, who, price, v_now, signed_qty});
    }

    // edge == spread + adverse, in integers, with no rounding anywhere: the
    // identity is what makes the decomposition checkable rather than asserted.
    void settle(const PendingMarkout& p, Price v_horizon, bool early) {
        Account& a = accounts_[p.account];
        const auto v_h = static_cast<std::int64_t>(v_horizon);
        const auto v_0 = static_cast<std::int64_t>(p.v_at_fill);
        const auto px = static_cast<std::int64_t>(p.price);
        const std::int64_t adverse = (v_h - v_0) * p.signed_qty;
        a.edge_sum += (v_h - px) * p.signed_qty;
        a.spread_sum += (v_0 - px) * p.signed_qty;
        a.adverse_sum += adverse;
        ++a.markout_n;
        if (adverse < 0) ++a.adverse_fills;
        if (early) ++a.unresolved_fills;
    }

    std::vector<Account> accounts_;
    std::deque<PendingMarkout> pending_; // in due order, because horizon is fixed
    SimTime horizon_ = 1'000'000;
    std::int64_t fee_ = 0;
    std::uint64_t trades_ = 0;
    std::uint64_t volume_ = 0;
};

} // namespace px::arena
