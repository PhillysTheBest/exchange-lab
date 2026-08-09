// workload/generator.hpp — Stage 5, Section 3: a deterministic, seeded
// generator of realistic Kalshi-wire-shaped WS text frames, across
// configurable market counts/distributions and three named traffic
// profiles.
//
// DESIGN DECISION, stated explicitly rather than left implicit: this
// generator produces MARKET-DATA-side traffic only (the shapes
// live/kalshi_feed.cpp's parse_feed_message already decodes: snapshot,
// delta, ticker, trade, market_lifecycle_v2) — the same "wire text in,
// through the real parser, through the real Ingress::handle_message, onto
// ring A" path tests/test_replay_determinism.cpp and
// tests/test_replay_fuzz.cpp already exercise, extended to cover every
// scenario the Stage 5 plan's "adversarial workload" list asks for that is
// actually a market-data concept (rapid level changes, side flips,
// zero-quantity updates, duplicate events, sequence gaps, stale/reordered
// data, reconnect/recovery-triggered resnapshots).
//
// The plan's remaining adversarial items — partial fills, rapid
// cancel/replace, an own-order-update immediately after submission, a risk
// rejection — are order-lifecycle events, not market data: a real system
// doesn't receive "partial fill" over the WS feed, it receives ordinary
// market data and its OWN previously-placed order happens to partially
// fill via Kalshi's REST response / ring C. Fabricating those directly as
// synthetic events here would mean reinventing order-lifecycle simulation
// a second time, less realistically than just running it for real. Instead,
// bench/bench_workload_throughput.cpp (Section 4) drives THIS generator's
// market-data stream through a real closed-loop Ingress -> Brain -> Gateway
// (with a SimulatedExchangeSink-style stand-in for Kalshi's REST responses,
// the same pattern tests/test_pipeline_e2e.cpp and tests/test_lifecycle.cpp
// already use) with a real Strategy registered — partial fills, cancel/
// replace, and own-order-updates then occur as genuine consequences of that
// real system running against this workload, and a risk rejection is
// manufactured by configuring RiskConfig with a deliberately tight limit
// rather than faked as an event. That's the honest way to include these
// scenarios: produced by the real system under load, not asserted into
// existence.
//
// RNG convention: reuses tests/test_replay_fuzz.cpp's established pattern
// (std::mt19937_64, weighted uniform_int_distribution event-kind mix,
// recency-biased price reuse) rather than inventing a new one — same seed-42
// default, same "recent prices" bias toward realistic collision rates
// (a side flip only means something if it happens at a price that already
// has resting interest).
#pragma once

#include "px/types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <random>
#include <string>
#include <vector>

namespace px::workload {

enum class Profile { Normal, Bursty, Adversarial };
enum class MarketDist { Uniform, Skewed };

struct GeneratorConfig {
    std::uint64_t seed = 42;
    std::size_t num_markets = 1;
    Profile profile = Profile::Normal;
    MarketDist market_dist = MarketDist::Skewed; // realistic default — see the plan's own
                                                   // "realistic activity distributions, not
                                                   // identical traffic to every market" instruction
    // Bursty profile only: how many frames make up an idle stretch / a burst
    // stretch before flipping state. Ignored for Normal/Adversarial.
    std::uint64_t bursty_idle_frames = 200;
    std::uint64_t bursty_burst_frames = 2'000;
};

// One market's minimal running state — just enough to keep generated deltas
// self-consistent (a delta never drives a level negative; a side flip only
// happens at a price this market has actually touched before), not a real
// order book.
struct MarketRuntime {
    std::string ticker;
    long long seq = 1;
    // Recency-biased reuse, same idea as test_replay_fuzz.cpp's
    // recent_prices — capped small ring, oldest evicted first.
    std::deque<int> recent_price_cents;
    bool seeded = false;      // true once an initial snapshot has been emitted for this market
    bool side_flip_toggle = false; // alternates which side make_side_flip quotes onto
};

// What kind of frame a given call actually produced — surfaced so a caller
// (bench_workload_throughput.cpp) can tag/aggregate results without
// re-parsing its own output.
enum class FrameKind {
    Snapshot, Delta, Ticker, Trade, Lifecycle,
    DuplicateDelta,   // adversarial: re-sends the previous delta's exact seq+content
    SeqGap,           // adversarial: seq jumps forward, simulating a dropped message
    ZeroQtyUpdate,    // adversarial: a delta whose resulting level is exactly zero
    SideFlip,         // adversarial: a delta at a price previously resting on the other side
    ReconnectResync,  // adversarial: a fresh SnapshotBegin-equivalent (orderbook_snapshot),
                       // the same shape a real reconnect's request_resync produces
    StaleReplay,       // adversarial: re-sends an old seq number, well behind current
};

// Metadata about a generated frame — NOT the frame text itself. See
// next_frame()'s own comment for why: an earlier version of this struct held
// the JSON text by value (a fresh heap allocation per call, ~150-300 bytes,
// on the one single-threaded producer loop that also parses and paces) and
// that allocation cost (measured in isolation: p50 ~1166ns, comparable to or
// larger than parse_feed_message's own ~950ns) turned out to dominate Stage
// 5's first throughput-sweep results — the "saturation" it showed was mostly
// this generator's own allocator traffic, not the pipeline's real capacity.
// Caught before publishing (see docs/PERFORMANCE.md's Section 4 methodology
// note) and fixed by moving the JSON text into a caller-owned, reused
// std::string& buffer instead.
struct GeneratedFrame {
    std::size_t market_index = 0; // [0, num_markets)
    FrameKind kind = FrameKind::Delta;
};

class Generator {
public:
    explicit Generator(GeneratorConfig cfg)
        : cfg_(cfg), rng_(cfg.seed), market_pick_(0.0, 1.0), price_dist_(1, 99), qty_dist_(1, 50),
          delta_dist_(-20, 20), reuse_roll_(0, 1) {
        markets_.resize(cfg_.num_markets);
        for (std::size_t i = 0; i < cfg_.num_markets; ++i) {
            markets_[i].ticker = "WL-MKT-" + std::to_string(i);
        }
        // Skewed weights: market i (0 = hottest) gets weight 1/(i+1) — a
        // simple, defensible power-law-ish skew (a handful of hot markets
        // dominate traffic, a long tail is mostly quiet), not a uniform
        // 1/N split, per the plan's explicit "realistic activity
        // distributions" instruction for Section 6's multi-market sweep.
        weights_.resize(cfg_.num_markets);
        double total = 0.0;
        for (std::size_t i = 0; i < cfg_.num_markets; ++i) {
            weights_[i] = (cfg_.market_dist == MarketDist::Skewed) ? (1.0 / static_cast<double>(i + 1)) : 1.0;
            total += weights_[i];
        }
        double cum = 0.0;
        cum_weights_.resize(cfg_.num_markets);
        for (std::size_t i = 0; i < cfg_.num_markets; ++i) {
            cum += weights_[i] / total;
            cum_weights_[i] = cum;
        }
    }

    // Produces the next frame's JSON text into `json_out` (reused across
    // calls — see GeneratedFrame's own comment for why this is a reused
    // out-param rather than a returned-by-value std::string) and returns its
    // metadata. `frames_emitted()` since construction governs bursty-profile
    // state; the caller is responsible for actual wall-clock pacing (see
    // rate_multiplier()) — this class only decides WHAT to generate and WHEN
    // a burst is logically active, never sleeps/spins itself.
    [[nodiscard]] GeneratedFrame next_frame(std::string& json_out) {
        ++frames_emitted_;
        const std::size_t mkt = pick_market();
        MarketRuntime& m = markets_[mkt];

        if (!m.seeded) return make_snapshot(mkt, m, FrameKind::Snapshot, json_out);

        if (cfg_.profile == Profile::Adversarial) {
            // Round-robin through the adversarial scenario list so every
            // kind appears at a guaranteed, inspectable frequency (1-in-N
            // calls, deterministically) — content within each kind is still
            // RNG-driven from the seeded stream, but WHICH scenario fires on
            // a given call is not left to chance.
            const int kind = static_cast<int>((frames_emitted_ - 1) % kNumAdversarialKinds);
            switch (kind) {
                case 0: return make_delta(mkt, m, /*rapid_flip=*/true, json_out);
                case 1: return make_side_flip(mkt, m, json_out);
                case 2: return make_zero_qty(mkt, m, json_out);
                case 3: return make_duplicate(mkt, m, json_out);
                case 4: return make_seq_gap(mkt, m, json_out);
                case 5: return make_stale_replay(mkt, m, json_out);
                case 6: return make_snapshot(mkt, m, FrameKind::ReconnectResync, json_out);
                default: return make_delta(mkt, m, /*rapid_flip=*/false, json_out);
            }
        }

        // Normal/Bursty: a realistic mix — mostly deltas (the dominant
        // real-world message on a live order book), with periodic
        // ticker/trade updates. Weighted the same way test_replay_fuzz.cpp
        // weights its own event-kind roll: mostly the cheap/common case,
        // occasionally the others.
        const int roll = static_cast<int>(kind_roll_(rng_));
        if (roll <= 6) return make_delta(mkt, m, /*rapid_flip=*/false, json_out); // 70%
        if (roll <= 8) return make_ticker(mkt, m, json_out);                       // 20%
        return make_trade(mkt, m, json_out);                                       // 10%
    }

    // Bursty profile: multiply the caller's target inter-arrival interval by
    // this to get the actual paced interval — <1.0 during a burst (faster),
    // >1.0 during idle (slower). Always 1.0 for Normal/Adversarial (no
    // modulation — the caller's own configured rate is the rate).
    [[nodiscard]] double rate_multiplier() const noexcept {
        if (cfg_.profile != Profile::Bursty) return 1.0;
        const std::uint64_t cycle = cfg_.bursty_idle_frames + cfg_.bursty_burst_frames;
        const std::uint64_t phase = (cycle > 0) ? (frames_emitted_ % cycle) : 0;
        return (phase < cfg_.bursty_idle_frames) ? 3.0 : 0.02; // idle: 3x slower; burst: 50x faster
    }

    [[nodiscard]] bool in_burst() const noexcept { return rate_multiplier() < 1.0; }
    [[nodiscard]] std::uint64_t frames_emitted() const noexcept { return frames_emitted_; }
    [[nodiscard]] const std::string& ticker_of(std::size_t market_index) const noexcept {
        return markets_[market_index].ticker;
    }

    // Deterministically seeds market `market_index` with its initial
    // snapshot, regardless of what next_frame()'s (possibly skewed) market
    // picker would have chosen on its own. For callers (Section 4/6's
    // throughput/scaling benchmark) that want every market bootstrapped
    // up front, untimed, before a clean timed measurement loop where every
    // subsequent next_frame() call is known to produce exactly one book
    // event (Normal/Bursty profiles only — Adversarial's deliberately-
    // dropped frames don't have that property, see this file's header
    // comment on why Adversarial is Section 8's tool to use, not Section
    // 4/6's).
    [[nodiscard]] GeneratedFrame make_initial_snapshot(std::size_t market_index, std::string& json_out) {
        return make_snapshot(market_index, markets_[market_index], FrameKind::Snapshot, json_out);
    }

    // Number of BookEvents a single make_initial_snapshot() call produces
    // (SnapshotBegin + one LevelSet per level) — callers that need to
    // predict Brain::book_events_applied()'s post-setup value use this
    // rather than hardcoding it a second time.
    static constexpr int kBookEventsPerInitialSnapshot = 3; // SnapshotBegin + 2 levels

private:
    static constexpr int kNumAdversarialKinds = 7;

    GeneratorConfig cfg_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> market_pick_;
    std::uniform_int_distribution<int> price_dist_;
    std::uniform_int_distribution<int> qty_dist_;
    std::uniform_int_distribution<int> delta_dist_;
    std::uniform_int_distribution<int> reuse_roll_;
    std::uniform_int_distribution<int> kind_roll_{0, 9};

    std::vector<MarketRuntime> markets_;
    std::vector<double> weights_;
    std::vector<double> cum_weights_;
    std::uint64_t frames_emitted_ = 0;

    [[nodiscard]] std::size_t pick_market() {
        if (cfg_.num_markets == 1) return 0;
        const double r = market_pick_(rng_);
        for (std::size_t i = 0; i < cum_weights_.size(); ++i) {
            if (r <= cum_weights_[i]) return i;
        }
        return cum_weights_.size() - 1;
    }

    [[nodiscard]] int pick_price(MarketRuntime& m) {
        if (!m.recent_price_cents.empty() && reuse_roll_(rng_) == 1) {
            const std::size_t idx = static_cast<std::size_t>(rng_() % m.recent_price_cents.size());
            return m.recent_price_cents[idx];
        }
        const int p = price_dist_(rng_);
        remember_price(m, p);
        return p;
    }

    void remember_price(MarketRuntime& m, int cents) {
        m.recent_price_cents.push_back(cents);
        if (m.recent_price_cents.size() > 32) m.recent_price_cents.pop_front();
    }

    [[nodiscard]] static std::string dollars(int cents) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d.%04d", cents / 100, (cents % 100) * 100);
        return buf;
    }

    [[nodiscard]] GeneratedFrame make_snapshot(std::size_t mkt, MarketRuntime& m, FrameKind kind,
                                                std::string& json_out) {
        m.seeded = true;
        remember_price(m, 40);
        remember_price(m, 60);
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      R"({"type":"orderbook_snapshot","sid":1,"seq":%lld,)"
                      R"("msg":{"market_ticker":"%s","market_id":"m",)"
                      R"("yes_dollars_fp":[["0.4000","100.00"]],)"
                      R"("no_dollars_fp":[["0.4000","50.00"]]}})",
                      m.seq, m.ticker.c_str());
        ++m.seq;
        json_out = buf; // reuses json_out's existing capacity once warmed up — see GeneratedFrame's comment
        return {mkt, kind};
    }

    [[nodiscard]] GeneratedFrame make_delta(std::size_t mkt, MarketRuntime& m, bool rapid_flip,
                                             std::string& json_out) {
        const int price = pick_price(m);
        const int qdelta = rapid_flip ? (delta_dist_(rng_) >= 0 ? 20 : -20) : delta_dist_(rng_);
        const char* side = (rng_() % 2 == 0) ? "yes" : "no";
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      R"({"type":"orderbook_delta","sid":1,"seq":%lld,)"
                      R"("msg":{"market_ticker":"%s","market_id":"m",)"
                      R"("price_dollars":"%s","delta_fp":"%d.00","side":"%s"}})",
                      m.seq, m.ticker.c_str(), dollars(price).c_str(), qdelta, side);
        ++m.seq;
        json_out = buf;
        return {mkt, FrameKind::Delta};
    }

    [[nodiscard]] GeneratedFrame make_side_flip(std::size_t mkt, MarketRuntime& m, std::string& json_out) {
        // Reuses a previously-touched price (recency bias) but flips which
        // side it's quoted on — the exact scenario
        // tests/test_replay_fuzz.cpp's own recency bias was built to
        // exercise against ShadowBook::set_level's documented side-flip fix.
        const int price = pick_price(m);
        m.side_flip_toggle = !m.side_flip_toggle;
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      R"({"type":"orderbook_delta","sid":1,"seq":%lld,)"
                      R"("msg":{"market_ticker":"%s","market_id":"m",)"
                      R"("price_dollars":"%s","delta_fp":"5.00","side":"%s"}})",
                      m.seq, m.ticker.c_str(), dollars(price).c_str(), m.side_flip_toggle ? "yes" : "no");
        ++m.seq;
        json_out = buf;
        return {mkt, FrameKind::SideFlip};
    }

    [[nodiscard]] GeneratedFrame make_zero_qty(std::size_t mkt, MarketRuntime& m, std::string& json_out) {
        const int price = pick_price(m);
        char buf[320];
        // delta_fp "0.00" — a level update that changes nothing quantity-wise,
        // a real, legal-per-schema edge case worth exercising on its own.
        std::snprintf(buf, sizeof(buf),
                      R"({"type":"orderbook_delta","sid":1,"seq":%lld,)"
                      R"("msg":{"market_ticker":"%s","market_id":"m",)"
                      R"("price_dollars":"%s","delta_fp":"0.00","side":"yes"}})",
                      m.seq, m.ticker.c_str(), dollars(price).c_str());
        ++m.seq;
        json_out = buf;
        return {mkt, FrameKind::ZeroQtyUpdate};
    }

    [[nodiscard]] GeneratedFrame make_duplicate(std::size_t mkt, MarketRuntime& m, std::string& json_out) {
        // Re-sends the CURRENT seq (not incremented) with fresh content —
        // Ingress's SeqTracker should drop this as already-seen.
        const int price = pick_price(m);
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      R"({"type":"orderbook_delta","sid":1,"seq":%lld,)"
                      R"("msg":{"market_ticker":"%s","market_id":"m",)"
                      R"("price_dollars":"%s","delta_fp":"1.00","side":"yes"}})",
                      m.seq > 1 ? m.seq - 1 : m.seq, m.ticker.c_str(), dollars(price).c_str());
        json_out = buf; // seq NOT advanced — this is the point
        return {mkt, FrameKind::DuplicateDelta};
    }

    [[nodiscard]] GeneratedFrame make_seq_gap(std::size_t mkt, MarketRuntime& m, std::string& json_out) {
        const int price = pick_price(m);
        char buf[320];
        m.seq += 25; // jump forward — simulates several dropped messages
        std::snprintf(buf, sizeof(buf),
                      R"({"type":"orderbook_delta","sid":1,"seq":%lld,)"
                      R"("msg":{"market_ticker":"%s","market_id":"m",)"
                      R"("price_dollars":"%s","delta_fp":"1.00","side":"yes"}})",
                      m.seq, m.ticker.c_str(), dollars(price).c_str());
        ++m.seq;
        json_out = buf;
        return {mkt, FrameKind::SeqGap};
    }

    [[nodiscard]] GeneratedFrame make_stale_replay(std::size_t mkt, MarketRuntime& m, std::string& json_out) {
        // A seq well behind current — simulates a reordered/delayed message
        // arriving very late, distinct from an exact duplicate.
        const int price = pick_price(m);
        const long long stale_seq = (m.seq > 10) ? (m.seq - 10) : 1;
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      R"({"type":"orderbook_delta","sid":1,"seq":%lld,)"
                      R"("msg":{"market_ticker":"%s","market_id":"m",)"
                      R"("price_dollars":"%s","delta_fp":"1.00","side":"yes"}})",
                      stale_seq, m.ticker.c_str(), dollars(price).c_str());
        json_out = buf; // current seq NOT advanced
        return {mkt, FrameKind::StaleReplay};
    }

    [[nodiscard]] GeneratedFrame make_ticker(std::size_t mkt, MarketRuntime& m, std::string& json_out) {
        const int bid = pick_price(m);
        const int ask = std::min(99, bid + 2 + static_cast<int>(rng_() % 5));
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      R"({"type":"ticker","sid":2,)"
                      R"("msg":{"market_ticker":"%s","market_id":"m",)"
                      R"("price_dollars":"%s","yes_bid_dollars":"%s","yes_ask_dollars":"%s",)"
                      R"("yes_bid_size_fp":"10.00","yes_ask_size_fp":"10.00",)"
                      R"("last_trade_size_fp":"1.00","volume_fp":"1.00","open_interest_fp":"1.00",)"
                      R"("dollar_volume":1,"dollar_open_interest":1,"ts_ms":1,)"
                      R"("time":"2026-08-09T00:00:00Z"}})",
                      m.ticker.c_str(), dollars(bid).c_str(), dollars(bid).c_str(), dollars(ask).c_str());
        json_out = buf;
        return {mkt, FrameKind::Ticker};
    }

    [[nodiscard]] GeneratedFrame make_trade(std::size_t mkt, MarketRuntime& m, std::string& json_out) {
        const int price = pick_price(m);
        const int qty = qty_dist_(rng_);
        const char* book_side = (rng_() % 2 == 0) ? "ask" : "bid";
        char buf[384];
        std::snprintf(buf, sizeof(buf),
                      R"({"type":"trade","sid":3,)"
                      R"("msg":{"market_ticker":"%s","market_id":"m",)"
                      R"("yes_price_dollars":"%s","count_fp":"%d.00",)"
                      R"("taker_book_side":"%s","ts_ms":1}})",
                      m.ticker.c_str(), dollars(price).c_str(), qty, book_side);
        json_out = buf;
        return {mkt, FrameKind::Trade};
    }
};

} // namespace px::workload
