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
//   spread           best ask minus best bid, in ticks, weighted by time, over the
//                    time the book has both sides and is not crossed: its median,
//                    and its mean in hundredths of a tick, which calibration uses
//                    because a median of whole ticks is too coarse to steer by
//   touch depth      the quantity at the best bid and the best ask, averaged,
//                    weighted by time: median and mean
//   print size       a print is the trades one aggressor made at one moment (T rows
//                    with the same time and side), summed: median and mean
//   size/depth       the mean print as a share of the mean touch depth, in
//                    thousandths. Means, because the medians of a few discrete
//                    sizes jump between them from one sample to the next
//   prints per move  how many prints it takes the mid to move one tick: 20 over
//                    the mean squared change of the mid, in ticks, across every
//                    run of 20 consecutive prints
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

// A product of a time weight and a value overflows 64 bits over a long tape (a
// day is 2.3e13 ns), so weighted sums are taken in 128. GCC and Clang have it;
// __extension__ keeps -Wpedantic quiet about it.
__extension__ typedef __int128 WideSum;
__extension__ typedef unsigned __int128 WideUSum;

struct TapeStats {
    Depth depth = Depth::Trades; // the depth the tape was read at
    std::uint64_t events = 0, adds = 0, cancels = 0, trades = 0, levels = 0, resets = 0;
    std::uint64_t prints = 0;
    std::uint64_t buy_prints = 0, sell_prints = 0; // the aggressor's side, when known
    std::int64_t span_ns = 0;

    // The book: time with both sides, one side or none, and crossed or locked.
    std::int64_t two_sided_ns = 0, one_sided_ns = 0, empty_ns = 0, crossed_ns = 0;
    std::int64_t spread_ticks = -1;     // the median; -1: not measurable at this depth
    std::int64_t spread_mean_x100 = -1; // the mean, in hundredths of a tick
    std::int64_t touch_depth = -1;      // the median
    std::int64_t touch_depth_mean_x100 = -1;

    std::int64_t print_qty = -1; // the median
    std::int64_t print_mean_x100 = -1;
    std::int64_t size_depth_pml = -1; // mean print * 1000 / mean touch depth

    std::uint64_t move_windows = 0;      // runs of kMoveWindow consecutive prints, overlapping
    std::int64_t move_sumsq = 0;         // over those runs, (change of the mid in half ticks)^2, summed
    std::int64_t prints_per_move_x100 = -1; // -1: too few prints, or the price never moved

    // How well the numbers are supported: a statistic from a handful of prints is noise.
    [[nodiscard]] bool enough_prints() const noexcept { return move_windows >= 10 * kMoveWindow; }
};

namespace detail {

// The weighted mean, times 100; -1 with no weight.
[[nodiscard]] inline std::int64_t weighted_mean_x100(const std::map<std::int64_t, std::int64_t>& w) {
    WideSum total = 0, sum = 0;
    for (const auto& [v, n] : w) {
        total += n;
        sum += static_cast<WideSum>(v) * n;
    }
    if (total <= 0) return -1;
    return static_cast<std::int64_t>(sum * 100 / total);
}

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

// A window of tape time, [from_ns, to_ns): the book is built from every event,
// but only the moments inside are measured. The default is the whole tape, and
// measuring it is exactly what measuring with no window was.
inline constexpr std::int64_t kTapeStart = INT64_MIN;
inline constexpr std::int64_t kTapeEnd = INT64_MAX;

// The statistics of a tape read at `depth`: a tape can be read as shallower than
// it is (an L3 session read as trades only, to compare with a trades-only file),
// never deeper.
[[nodiscard]] inline TapeStats tape_stats(const Tape& tape, Depth depth, std::int64_t from_ns = kTapeStart,
                                          std::int64_t to_ns = kTapeEnd) {
    TapeStats st;
    st.depth = static_cast<int>(depth) < static_cast<int>(tape.h.depth) ? depth : tape.h.depth;
    const bool book = st.depth != Depth::Trades;
    TapeBook b;
    std::map<std::int64_t, std::int64_t> spread_w, depth_w;
    std::vector<std::int64_t> print_qty;
    std::vector<std::int64_t> mids; // the mid, in half ticks, at each print since the last reset
    auto close_windows = [&st, &mids] {
        for (std::size_t i = kMoveWindow; i < mids.size(); ++i) {
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
    std::int64_t first_t = 0, last_t = 0;
    bool any = false;
    std::size_t i = 0;
    while (i < ev.size()) {
        const std::int64_t t = ev[i].t;
        const bool in = t >= from_ns && t < to_ns;
        // One moment: every event with this time. A print is the trades one
        // aggressor made in it, consecutive T rows on one side.
        std::int64_t cur_qty = 0, cur_price = 0;
        char cur_side = 0;
        auto end_print = [&] {
            if (cur_side == 0) return;
            if (!in) {
                cur_side = 0;
                cur_qty = 0;
                return;
            }
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
            if (in) {
                ++st.events;
                if (!any) first_t = t;
                last_t = t;
                any = true;
                switch (e.kind) {
                    case 'A': ++st.adds; break;
                    case 'C': ++st.cancels; break;
                    case 'T': ++st.trades; break;
                    case 'L': ++st.levels; break;
                    case 'R': ++st.resets; break;
                    default: break;
                }
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
            // The book after this moment holds until the next one, or the window's
            // end. Outside the window it is read for its mid and holds for no time.
            const std::int64_t next = i < ev.size() ? ev[i].t : t;
            measure(in ? std::min(next, to_ns) - t : 0);
        }
    }
    close_windows();
    if (any) st.span_ns = last_t - first_t;

    if (book) {
        st.spread_ticks = detail::weighted_median(spread_w);
        st.spread_mean_x100 = detail::weighted_mean_x100(spread_w);
        st.touch_depth = detail::weighted_median(depth_w);
        st.touch_depth_mean_x100 = detail::weighted_mean_x100(depth_w);
    }
    if (!print_qty.empty()) {
        WideSum sum = 0;
        for (const std::int64_t q : print_qty) sum += q;
        st.print_mean_x100 = static_cast<std::int64_t>(sum * 100 / static_cast<WideSum>(print_qty.size()));
        auto mid = print_qty.begin() + static_cast<std::ptrdiff_t>((print_qty.size() - 1) / 2);
        std::nth_element(print_qty.begin(), mid, print_qty.end());
        st.print_qty = *mid;
    }
    if (st.print_mean_x100 > 0 && st.touch_depth_mean_x100 > 0)
        st.size_depth_pml = st.print_mean_x100 * 1000 / st.touch_depth_mean_x100;
    // Var of the mid over a window, in ticks^2, is sumsq / (4 * windows); a one-tick
    // move takes kMoveWindow / that many prints.
    if (st.move_windows > 0 && st.move_sumsq > 0)
        st.prints_per_move_x100 = 400 * kMoveWindow * static_cast<std::int64_t>(st.move_windows) / st.move_sumsq;
    return st;
}

[[nodiscard]] inline TapeStats tape_stats(const Tape& tape) { return tape_stats(tape, tape.h.depth); }

// ---- stylised facts: a diagnostic, never a gate ------------------------------
//
// What /facts prints beside a simulated market. Calibration matches the three
// statistics above and nothing else; these are the ones the literature uses to
// say whether a market model looks like a market (Cont 2001): returns that do
// not predict themselves, volatility that clusters, fat tails, volume that
// moves with volatility, and the shape of the spread and the book. None is
// fitted, none is a pass or a fail, and docs/DATA.md says what each one does
// and does not establish.
//
// SAMPLING. A real day and a 25 ms session have different clocks, so neither is
// read in seconds. Each tape is cut into intervals of kFactPrints of its own mean
// times between prints, so every tape is read at the same number of prints an
// interval. Intervals start afresh at each R row (a new session or day) and no
// return or lag crosses one. A return is the change of the mid, in half ticks,
// from the end of one interval to the end of the next, the mid carried forward
// while the book is one-sided; on a tape read as trades only, the print price.
//
// Integer throughout: sums are 128-bit, correlations divide by an integer
// square root, and every result is a count in thousandths (or ten-thousandths).
inline constexpr std::int64_t kFactPrints = 10;
inline constexpr std::int64_t kNotMeasured = INT64_MIN;

struct StylisedFacts {
    Depth depth = Depth::Trades;
    std::int64_t interval_ns = 0; // kFactPrints mean times between prints
    std::uint64_t prints = 0;
    std::uint64_t returns = 0;    // intervals with a return, over every segment

    std::int64_t ret_ac1_pml = kNotMeasured;  // autocorrelation of returns, lag 1
    std::int64_t abs_ac1_pml = kNotMeasured;  // of |return|, lag 1
    std::int64_t abs_ac10_pml = kNotMeasured; // of |return|, lag 10
    std::int64_t null_band_pml = kNotMeasured; // 2 / sqrt(returns): inside it, not told from 0
    std::int64_t tail3_x10k = kNotMeasured;   // returns beyond 3 s.d., per 10,000; a normal: 27
    std::int64_t zero_returns_pml = kNotMeasured; // intervals in which the mid did not move
    std::int64_t volvol_pml = kNotMeasured;   // correlation of an interval's volume with its |return|

    // The book, weighted by time while it is two-sided: needs L2 or L3.
    std::int64_t spread_p10 = kNotMeasured, spread_p50 = kNotMeasured, spread_p90 = kNotMeasured;
    std::int64_t one_tick_pml = kNotMeasured; // share of that time at a one-tick spread
    std::int64_t depth_p10 = kNotMeasured, depth_p50 = kNotMeasured, depth_p90 = kNotMeasured;

    // Order flow per print, so the clock never enters: needs L3.
    std::int64_t adds_per_print_x100 = kNotMeasured;
    std::int64_t cancels_per_print_x100 = kNotMeasured;
    std::int64_t cancels_per_add_pml = kNotMeasured;
};

namespace detail {

// floor(sqrt(v)), exactly, for any 128-bit unsigned value.
[[nodiscard]] inline WideUSum isqrt(WideUSum v) {
    if (v < 2) return v;
    WideUSum lo = 1, hi = static_cast<WideUSum>(1) << 64;
    if (hi > v) hi = v;
    while (lo < hi) { // the largest r with r * r <= v
        const WideUSum mid = lo + (hi - lo + 1) / 2;
        if (mid <= v / mid) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

// The value below which `num / den` of the weight lies.
[[nodiscard]] inline std::int64_t weighted_quantile(const std::map<std::int64_t, std::int64_t>& w, std::int64_t num,
                                                    std::int64_t den) {
    WideSum total = 0;
    for (const auto& [v, n] : w) total += n;
    if (total <= 0) return kNotMeasured;
    WideSum run = 0;
    for (const auto& [v, n] : w) {
        run += n;
        if (run * den >= total * num) return v;
    }
    return w.rbegin()->first;
}

// Sample autocorrelation at `lag`, in thousandths: the sum over pairs inside one
// segment of (x_i - mean)(x_i+lag - mean), over the sum of (x_i - mean)^2 over
// every x, the usual estimator. Each term is scaled by n so the mean stays an integer.
[[nodiscard]] inline std::int64_t autocorr_pml(const std::vector<std::vector<std::int64_t>>& seg, std::size_t lag) {
    WideSum n = 0, sum = 0;
    for (const auto& s : seg)
        for (const std::int64_t x : s) {
            ++n;
            sum += x;
        }
    if (n < 2) return kNotMeasured;
    WideSum den = 0, num = 0;
    std::size_t pairs = 0;
    for (const auto& s : seg) {
        for (std::size_t i = 0; i < s.size(); ++i) {
            const WideSum d = n * s[i] - sum;
            den += d * d;
            if (i + lag < s.size()) {
                num += d * (n * s[i + lag] - sum);
                ++pairs;
            }
        }
    }
    if (den == 0 || pairs == 0) return kNotMeasured;
    return static_cast<std::int64_t>(num * 1000 / den);
}

// Pearson correlation of two equal-length series, in thousandths.
[[nodiscard]] inline std::int64_t corr_pml(const std::vector<std::int64_t>& x, const std::vector<std::int64_t>& y) {
    const WideSum n = static_cast<WideSum>(x.size());
    if (n < 2 || y.size() != x.size()) return kNotMeasured;
    WideSum sx = 0, sy = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        sx += x[i];
        sy += y[i];
    }
    WideSum sxy = 0, sxx = 0, syy = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const WideSum dx = n * x[i] - sx, dy = n * y[i] - sy;
        sxy += dx * dy;
        sxx += dx * dx;
        syy += dy * dy;
    }
    if (sxx == 0 || syy == 0) return kNotMeasured;
    // sqrt(sxx * syy), with both halved alike until the product fits: sqrt of
    // (a / 4^k)(b / 4^k) is sqrt(ab) / 4^k, so the result is scaled back by 4^k.
    WideUSum a = static_cast<WideUSum>(sxx), b = static_cast<WideUSum>(syy);
    unsigned k = 0;
    while ((a >> 62) != 0 || (b >> 62) != 0) {
        a >>= 2;
        b >>= 2;
        ++k;
    }
    const WideSum den = static_cast<WideSum>(isqrt(a * b) << (2 * k));
    if (den == 0) return kNotMeasured;
    const WideSum r = sxy * 1000 / den;
    return static_cast<std::int64_t>(r > 1000 ? 1000 : r < -1000 ? -1000 : r);
}

} // namespace detail

// The stylised facts of a tape read at `depth`, over the window [from_ns, to_ns).
[[nodiscard]] inline StylisedFacts stylised_facts(const Tape& tape, Depth depth, std::int64_t from_ns = kTapeStart,
                                                  std::int64_t to_ns = kTapeEnd) {
    StylisedFacts f;
    f.depth = static_cast<int>(depth) < static_cast<int>(tape.h.depth) ? depth : tape.h.depth;
    const bool book = f.depth != Depth::Trades;
    const std::vector<TapeEvent>& ev = tape.ev;
    auto inside = [&](std::int64_t t) { return t >= from_ns && t < to_ns; };

    // Pass 1: the prints and the time the tape covers, segment by segment, to set
    // the interval. A print is the trades of one aggressor in one moment, as above.
    std::int64_t active_ns = 0, seg_first = 0, seg_last = 0;
    bool seg_open = false;
    std::uint64_t adds = 0, cancels = 0;
    for (std::size_t i = 0; i < ev.size();) {
        const std::int64_t t = ev[i].t;
        char side = 0;
        for (; i < ev.size() && ev[i].t == t; ++i) {
            const TapeEvent& e = ev[i];
            if (e.kind == 'R') {
                if (seg_open) active_ns += seg_last - seg_first;
                seg_open = false;
            }
            if (!inside(t)) continue;
            if (!seg_open) seg_first = t;
            seg_open = true;
            seg_last = t;
            if (e.kind == 'A') ++adds;
            if (e.kind == 'C') ++cancels;
            const char s = e.kind == 'T' ? (e.side == '-' ? '?' : e.side) : 0;
            if (s != side && side != 0) ++f.prints;
            side = s;
        }
        if (side != 0) ++f.prints;
    }
    if (seg_open) active_ns += seg_last - seg_first;
    if (f.depth == Depth::L3 && f.prints > 0) {
        f.adds_per_print_x100 = static_cast<std::int64_t>(adds * 100 / f.prints);
        f.cancels_per_print_x100 = static_cast<std::int64_t>(cancels * 100 / f.prints);
        if (adds > 0) f.cancels_per_add_pml = static_cast<std::int64_t>(cancels * 1000 / adds);
    }
    if (f.prints < 2 || active_ns <= 0) return f;
    f.interval_ns = std::max<std::int64_t>(1, static_cast<std::int64_t>(static_cast<WideSum>(active_ns) * kFactPrints /
                                                                         static_cast<WideSum>(f.prints)));

    // Pass 2: walk the tape again, closing an interval each time the clock passes
    // its end; each closed interval gives a return and a volume.
    TapeBook b;
    std::map<std::int64_t, std::int64_t> spread_w, depth_w;
    std::vector<std::vector<std::int64_t>> rets(1), absr(1);
    std::vector<std::int64_t> all_abs, vols;
    bool have_mid = false, have_prev = false;
    std::int64_t mid2 = 0, prev_mid2 = 0, seg_start = 0, bucket = 0, volume = 0;
    bool seg_started = false;
    auto close_to = [&](std::int64_t upto) { // close every interval ending at or before `upto`
        while (seg_started && seg_start + (bucket + 1) * f.interval_ns <= upto) {
            if (have_mid) {
                if (have_prev) {
                    const std::int64_t r = mid2 - prev_mid2;
                    rets.back().push_back(r);
                    absr.back().push_back(r < 0 ? -r : r);
                    all_abs.push_back(r < 0 ? -r : r);
                    vols.push_back(volume);
                }
                prev_mid2 = mid2;
                have_prev = true;
            }
            volume = 0;
            ++bucket;
        }
    };
    auto new_segment = [&] {
        if (!rets.back().empty()) {
            rets.emplace_back();
            absr.emplace_back();
        }
        seg_started = false;
        have_prev = false;
        have_mid = false;
        volume = 0;
    };
    for (std::size_t i = 0; i < ev.size();) {
        const std::int64_t t = ev[i].t;
        const bool in = inside(t);
        // A reset ends the segment where it stands: the time up to it is a gap
        // between sessions or days, not intervals in which nothing moved.
        bool resets = false;
        for (std::size_t j = i; j < ev.size() && ev[j].t == t; ++j) resets = resets || ev[j].kind == 'R';
        if (in && seg_started && !resets) close_to(t);
        if (in && !seg_started) {
            seg_started = true;
            seg_start = t;
            bucket = 0;
        }
        for (; i < ev.size() && ev[i].t == t; ++i) {
            const TapeEvent& e = ev[i];
            if (e.kind == 'R') new_segment();
            if (book) b.apply(e);
            if (!in || e.kind != 'T') continue;
            volume += e.qty;
            if (!book) {
                mid2 = 2 * e.price;
                have_mid = true;
            }
        }
        if (book) {
            std::int64_t bp = 0, bq = 0, ap = 0, aq = 0;
            if (b.best(0, bp, bq) && b.best(1, ap, aq) && ap > bp) {
                mid2 = ap + bp;
                have_mid = true;
                const std::int64_t next = i < ev.size() ? ev[i].t : t;
                const std::int64_t dt = in ? std::min(next, to_ns) - t : 0;
                if (dt > 0) {
                    spread_w[ap - bp] += dt;
                    depth_w[(bq + aq) / 2] += dt;
                }
            }
        }
    }

    for (const auto& s : rets) f.returns += s.size();
    if (f.returns >= 2) {
        f.ret_ac1_pml = detail::autocorr_pml(rets, 1);
        f.abs_ac1_pml = detail::autocorr_pml(absr, 1);
        f.abs_ac10_pml = detail::autocorr_pml(absr, 10);
        f.null_band_pml = static_cast<std::int64_t>(2000 / detail::isqrt(f.returns));
        f.volvol_pml = detail::corr_pml(vols, all_abs);
        // Beyond 3 s.d. of the mean: (n r - S)^2 * n > 9 * sum (n r - S)^2.
        const WideSum n = static_cast<WideSum>(f.returns);
        WideSum sum = 0, ss = 0;
        std::uint64_t zero = 0, beyond = 0;
        for (const auto& s : rets)
            for (const std::int64_t r : s) {
                sum += r;
                zero += r == 0 ? 1 : 0;
            }
        for (const auto& s : rets)
            for (const std::int64_t r : s) ss += (n * r - sum) * (n * r - sum);
        for (const auto& s : rets)
            for (const std::int64_t r : s) beyond += (n * r - sum) * (n * r - sum) * n > 9 * ss && ss > 0 ? 1 : 0;
        f.tail3_x10k = static_cast<std::int64_t>(beyond * 10'000 / f.returns);
        f.zero_returns_pml = static_cast<std::int64_t>(zero * 1000 / f.returns);
    }
    if (book && !spread_w.empty()) {
        f.spread_p10 = detail::weighted_quantile(spread_w, 1, 10);
        f.spread_p50 = detail::weighted_quantile(spread_w, 1, 2);
        f.spread_p90 = detail::weighted_quantile(spread_w, 9, 10);
        WideSum total = 0;
        for (const auto& [v, n] : spread_w) total += n;
        const auto one = spread_w.find(1);
        f.one_tick_pml = one == spread_w.end() ? 0 : static_cast<std::int64_t>(static_cast<WideSum>(one->second) * 1000 / total);
        f.depth_p10 = detail::weighted_quantile(depth_w, 1, 10);
        f.depth_p50 = detail::weighted_quantile(depth_w, 1, 2);
        f.depth_p90 = detail::weighted_quantile(depth_w, 9, 10);
    }
    return f;
}

} // namespace px::data
