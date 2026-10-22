// marketdata/stats.hpp — what a tape says about its market, in integers.
//
// These are the statistics /data show prints and calibration matches. They are
// computed by one function from a tape, whatever made it: an importer reading a
// real file, or an arena session written out as a tape. Calibration compares
// the two through this same function, so a statistic is never measured one way
// on the data and another way on the simulation.
//
// Every statistic is a ratio or a count of ticks, never a rate in real seconds:
// a suite session lasts 25 ms of simulated time and a real market moves over
// seconds, so calibration matches the shape of the market, not its clock
// (docs/DATA.md, "Ratios, not the clock").
//
//   spread           the time-weighted median of best ask minus best bid, in ticks,
//                    over the time the book has both sides and is not crossed
//   touch depth      the time-weighted median of the quantity at the best bid and
//                    the best ask, averaged
//   print size       the median print: the trades one aggressor made at one moment
//                    (T rows with the same time and side), summed
//   size/depth       print size as a share of touch depth, in thousandths
//   prints per move  how many prints it takes the mid to move one tick, from the
//                    squared change of the mid over windows of 20 prints
//
// The mid is measured in half ticks (bid + ask), so it stays an integer. On a
// trades-only tape, or when a tape is read as one, the print price stands in
// for the mid; bid-ask bounce makes that noisier, which is why calibration
// reads the simulation at the same depth as the data.
#pragma once

#include "marketdata/tape.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace px::data {

// The book a tape describes. Orders with an id are tracked one by one; size
// from L rows, and size an order without an id left, is anonymous at its
// level. A cancel or trade naming an order the tape never added (it rested
// before the file began) takes its size from the anonymous size at its price.
class TapeBook {
public:
    void apply(const TapeEvent& e) {
        switch (e.kind) {
            case 'A': {
                const int s = side_index(e.side);
                if (s < 0) return;
                if (e.id != 0) {
                    remove_order(e.id);
                    orders_[e.id] = Order{s, e.price, e.qty};
                } else {
                    anon_[s][e.price] += e.qty;
                }
                total_[s][e.price] += e.qty;
                return;
            }
            case 'C': take(e.id, side_index(e.side), e.price, e.qty); return;
            case 'T':
                // A trade with no resting order named is a print: a trades-only
                // tape's, or a hidden execution's, and it leaves the book alone.
                if (e.id != 0) take(e.id, side_index(opposite(e.side)), e.price, e.qty);
                return;
            case 'L': {
                const int s = side_index(e.side);
                if (s < 0) return;
                const std::int64_t known = level(total_[s], e.price) - level(anon_[s], e.price);
                const std::int64_t anon = e.qty > known ? e.qty - known : 0;
                set(anon_[s], e.price, anon);
                set(total_[s], e.price, known + anon);
                return;
            }
            case 'R':
                orders_.clear();
                for (int s = 0; s < 2; ++s) {
                    anon_[s].clear();
                    total_[s].clear();
                }
                return;
            default: return;
        }
    }

    // The best price on a side and the quantity there; false when the side is empty.
    [[nodiscard]] bool best(int side, std::int64_t& price, std::int64_t& qty) const {
        const auto& m = total_[side];
        if (m.empty()) return false;
        const auto it = side == 0 ? std::prev(m.end()) : m.begin();
        price = it->first;
        qty = it->second;
        return true;
    }

    // The best n levels on a side, best first: (price, quantity).
    [[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>> top(int side, std::size_t n) const {
        std::vector<std::pair<std::int64_t, std::int64_t>> out;
        if (side == 0) {
            for (auto it = total_[0].rbegin(); it != total_[0].rend() && out.size() < n; ++it) out.push_back(*it);
        } else {
            for (auto it = total_[1].begin(); it != total_[1].end() && out.size() < n; ++it) out.push_back(*it);
        }
        return out;
    }

    [[nodiscard]] std::size_t resting_orders() const noexcept { return orders_.size(); }

    [[nodiscard]] static char opposite(char side) noexcept { return side == 'B' ? 'S' : side == 'S' ? 'B' : '-'; }

private:
    struct Order {
        int side = 0;
        std::int64_t price = 0;
        std::int64_t qty = 0;
    };
    using Levels = std::map<std::int64_t, std::int64_t>;

    [[nodiscard]] static int side_index(char side) noexcept { return side == 'B' ? 0 : side == 'S' ? 1 : -1; }
    [[nodiscard]] static std::int64_t level(const Levels& m, std::int64_t price) {
        const auto it = m.find(price);
        return it == m.end() ? 0 : it->second;
    }
    static void set(Levels& m, std::int64_t price, std::int64_t qty) {
        if (qty > 0) m[price] = qty;
        else m.erase(price);
    }

    void remove_order(std::uint64_t id) {
        const auto it = orders_.find(id);
        if (it == orders_.end()) return;
        const Order o = it->second;
        orders_.erase(it);
        set(total_[o.side], o.price, level(total_[o.side], o.price) - o.qty);
    }

    // qty leaves order `id`, or, when the tape never added it, the anonymous size at its price.
    void take(std::uint64_t id, int side, std::int64_t price, std::int64_t qty) {
        const auto it = id != 0 ? orders_.find(id) : orders_.end();
        if (it != orders_.end()) {
            Order& o = it->second;
            const std::int64_t d = std::min(qty, o.qty);
            o.qty -= d;
            set(total_[o.side], o.price, level(total_[o.side], o.price) - d);
            if (o.qty == 0) orders_.erase(it);
            return;
        }
        if (side < 0) return;
        const std::int64_t d = std::min(qty, level(anon_[side], price));
        set(anon_[side], price, level(anon_[side], price) - d);
        set(total_[side], price, level(total_[side], price) - d);
    }

    std::unordered_map<std::uint64_t, Order> orders_;
    Levels anon_[2];
    Levels total_[2];
};

inline constexpr int kMoveWindow = 20; // prints per window for prints-per-move

struct TapeStats {
    Depth depth = Depth::Trades; // the depth the tape was read at
    std::uint64_t events = 0, adds = 0, cancels = 0, trades = 0, levels = 0, resets = 0;
    std::uint64_t prints = 0;
    std::uint64_t buy_prints = 0, sell_prints = 0; // the aggressor's side, when known
    std::int64_t span_ns = 0;

    // The book: time with both sides, one side or none, and crossed or locked.
    std::int64_t two_sided_ns = 0, one_sided_ns = 0, empty_ns = 0, crossed_ns = 0;
    std::int64_t spread_ticks = -1; // -1: not measurable at this depth
    std::int64_t touch_depth = -1;

    std::int64_t print_qty = -1;       // median
    std::int64_t size_depth_pml = -1;  // print_qty * 1000 / touch_depth

    std::uint64_t move_windows = 0;
    std::int64_t move_sumsq = 0;         // sum over windows of (change of the mid in half ticks)^2
    std::int64_t prints_per_move_x100 = -1; // -1: too few prints, or the price never moved

    // How well the numbers are supported: a statistic from a handful of prints is noise.
    [[nodiscard]] bool enough_prints() const noexcept { return move_windows >= 10; }
};

namespace detail {

// The value below which half the weight lies.
[[nodiscard]] inline std::int64_t weighted_median(const std::map<std::int64_t, std::int64_t>& w) {
    std::int64_t total = 0;
    for (const auto& [v, n] : w) total += n;
    if (total <= 0) return -1;
    std::int64_t run = 0;
    for (const auto& [v, n] : w) {
        run += n;
        if (2 * run >= total) return v;
    }
    return w.rbegin()->first;
}

} // namespace detail

// The statistics of a tape read at `depth`: a tape can be read as shallower than
// it is (an L3 session read as trades only, to compare with a trades-only file),
// never deeper.
[[nodiscard]] inline TapeStats tape_stats(const Tape& tape, Depth depth) {
    TapeStats st;
    st.depth = static_cast<int>(depth) < static_cast<int>(tape.h.depth) ? depth : tape.h.depth;
    const bool book = st.depth != Depth::Trades;
    TapeBook b;
    std::map<std::int64_t, std::int64_t> spread_w, depth_w;
    std::vector<std::int64_t> print_qty;
    std::vector<std::int64_t> mids; // the mid, in half ticks, at each print since the last reset
    auto close_windows = [&st, &mids] {
        for (std::size_t i = kMoveWindow; i < mids.size(); i += kMoveWindow) {
            const std::int64_t d = mids[i] - mids[i - kMoveWindow];
            st.move_sumsq += d * d;
            ++st.move_windows;
        }
        mids.clear();
    };

    // Measures the book as it stands, for the dt it holds. Its mid is then the
    // mid the next moment's prints trade against.
    bool have_mid = false;
    std::int64_t mid2 = 0;
    auto measure = [&](std::int64_t dt) {
        std::int64_t bp = 0, bq = 0, ap = 0, aq = 0;
        const bool hb = b.best(0, bp, bq), ha = b.best(1, ap, aq);
        have_mid = false;
        if (hb && ha) {
            if (ap <= bp) {
                st.crossed_ns += dt;
                return;
            }
            st.two_sided_ns += dt;
            if (dt > 0) {
                spread_w[ap - bp] += dt;
                depth_w[(bq + aq) / 2] += dt;
            }
            have_mid = true;
            mid2 = ap + bp;
        } else if (hb || ha) {
            st.one_sided_ns += dt;
        } else {
            st.empty_ns += dt;
        }
    };

    const std::vector<TapeEvent>& ev = tape.ev;
    st.events = ev.size();
    if (!ev.empty()) st.span_ns = ev.back().t - ev.front().t;
    std::size_t i = 0;
    while (i < ev.size()) {
        const std::int64_t t = ev[i].t;
        // One moment: every event with this time. A print is the trades one
        // aggressor made in it, consecutive T rows on one side.
        std::int64_t cur_qty = 0, cur_price = 0;
        char cur_side = 0;
        auto end_print = [&] {
            if (cur_side == 0) return;
            ++st.prints;
            if (cur_side == 'B') ++st.buy_prints;
            if (cur_side == 'S') ++st.sell_prints;
            print_qty.push_back(cur_qty);
            if (!book) mids.push_back(2 * cur_price);
            else if (have_mid) mids.push_back(mid2);
            cur_side = 0;
            cur_qty = 0;
        };
        for (; i < ev.size() && ev[i].t == t; ++i) {
            const TapeEvent& e = ev[i];
            switch (e.kind) {
                case 'A': ++st.adds; break;
                case 'C': ++st.cancels; break;
                case 'T': ++st.trades; break;
                case 'L': ++st.levels; break;
                case 'R': ++st.resets; break;
                default: break;
            }
            if (e.kind == 'T') {
                const char side = e.side == '-' ? '?' : e.side;
                if (side != cur_side) end_print();
                cur_side = side;
                cur_qty += e.qty;
                cur_price = e.price;
            } else {
                end_print();
                if (e.kind == 'R') close_windows();
            }
            if (book) b.apply(e);
        }
        end_print();
        if (book) {
            // The book after this moment holds until the next one.
            const std::int64_t next = i < ev.size() ? ev[i].t : t;
            measure(next - t);
        }
    }
    close_windows();

    if (book) {
        st.spread_ticks = detail::weighted_median(spread_w);
        st.touch_depth = detail::weighted_median(depth_w);
    }
    if (!print_qty.empty()) {
        auto mid = print_qty.begin() + static_cast<std::ptrdiff_t>((print_qty.size() - 1) / 2);
        std::nth_element(print_qty.begin(), mid, print_qty.end());
        st.print_qty = *mid;
    }
    if (st.print_qty > 0 && st.touch_depth > 0) st.size_depth_pml = st.print_qty * 1000 / st.touch_depth;
    // Var of the mid over a window, in ticks^2, is sumsq / (4 * windows); a one-tick
    // move takes kMoveWindow / that many prints.
    if (st.move_windows > 0 && st.move_sumsq > 0)
        st.prints_per_move_x100 = 400 * kMoveWindow * static_cast<std::int64_t>(st.move_windows) / st.move_sumsq;
    return st;
}

[[nodiscard]] inline TapeStats tape_stats(const Tape& tape) { return tape_stats(tape, tape.h.depth); }

} // namespace px::data
