// strategy/oracle_feed.hpp — pluggable out-of-band event sources.
//
// A prediction market doesn't move on order flow the way an equity does — it
// moves on the real-world event it's a bet on. "Will NYC hit 90°F today"
// resolves against NOAA's thermometer, not against anyone's order book. The
// fastest edge in this kind of market is converting an out-of-band observation
// into an order before the rest of the market's slower observers (or slower
// pipelines) catch up — see the plan doc's "we are not racing the wire, we are
// racing the decision."
//
// OracleFeed is the pluggable interface: each concrete feed runs on its own
// thread and pushes OracleEvent records into a shared SpscRing that the
// strategy thread drains alongside book updates. THIS PROJECT SHIPS ONLY
// SYNTHETIC/SIMULATED FEEDS (see below) — no code here calls out to NOAA, SEC
// EDGAR, or the FAA. Wiring a real one is exactly implementing OracleFeed::run
// against that source's actual API and pushing real events into the same ring;
// everything downstream (the ring, the strategy interface, the demo pipeline in
// tools/strategy_demo.cpp) is unchanged either way.
#pragma once

#include "px/spsc_ring.hpp"
#include "px/tsc_clock.hpp"
#include "px/types.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <utility>

namespace px::strategy {

enum class OracleSource : std::uint8_t { NOAA, SecEdgar, FAA, Synthetic };

// POD, ring-transportable. `key` is a short source-defined identifier (a
// weather station id, a ticker, an airport code — whatever the concrete feed's
// domain calls it), not a Kalshi market ticker; a Signal maps key -> market.
struct OracleEvent {
    OracleSource source = OracleSource::Synthetic;
    char key[24] = {};
    double value = 0.0;
    Tsc observed_ts = 0;

    void set_key(const char* s) noexcept {
        std::strncpy(key, s, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
    }
};

inline constexpr std::size_t kOracleRingCapacity = 1u << 12; // 4096
using OracleRing = SpscRing<OracleEvent, kOracleRingCapacity>;

class OracleFeed {
public:
    virtual ~OracleFeed() = default;

    // Runs until stop_flag is set, pushing events into `out`. Intended to be
    // the entry point of its own std::thread — see tools/strategy_demo.cpp.
    virtual void run(OracleRing& out, std::atomic<bool>& stop_flag) = 0;

    [[nodiscard]] virtual OracleSource source() const noexcept = 0;
};

// --- synthetic stand-ins -----------------------------------------------------
//
// Each mirrors the shape a real feed for that source would have (a `key` you'd
// recognize — a station id, a ticker, an airport code — and a plausible value
// range) but generates its numbers from a local PRNG on a fixed period. This
// demonstrates the threading/ring wiring end-to-end without depending on
// network access, an API key, or a third party's uptime.

class SyntheticNoaaFeed final : public OracleFeed {
public:
    explicit SyntheticNoaaFeed(std::string station = "KNYC", double mean_f = 75.0, unsigned seed = 1)
        : station_(std::move(station)), mean_f_(mean_f), rng_(seed) {}

    [[nodiscard]] OracleSource source() const noexcept override { return OracleSource::NOAA; }

    void run(OracleRing& out, std::atomic<bool>& stop_flag) override {
        std::normal_distribution<double> noise(0.0, 1.5);
        while (!stop_flag.load(std::memory_order_relaxed)) {
            OracleEvent e;
            e.source = OracleSource::NOAA;
            e.set_key(station_.c_str());
            e.value = mean_f_ + noise(rng_);
            e.observed_ts = read_tscp();
            while (!out.try_push(e) && !stop_flag.load(std::memory_order_relaxed)) cpu_relax();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

private:
    std::string station_;
    double mean_f_;
    std::mt19937_64 rng_;
};

class SyntheticSecEdgarFeed final : public OracleFeed {
public:
    explicit SyntheticSecEdgarFeed(std::string ticker = "ACME", unsigned seed = 2)
        : ticker_(std::move(ticker)), rng_(seed) {}

    [[nodiscard]] OracleSource source() const noexcept override { return OracleSource::SecEdgar; }

    void run(OracleRing& out, std::atomic<bool>& stop_flag) override {
        // value: 1.0 signals "a new filing just posted" (a real feed would emit
        // this on an actual RSS/API update, not a timer).
        std::uniform_int_distribution<int> gap_s(3, 8);
        while (!stop_flag.load(std::memory_order_relaxed)) {
            OracleEvent e;
            e.source = OracleSource::SecEdgar;
            e.set_key(ticker_.c_str());
            e.value = 1.0;
            e.observed_ts = read_tscp();
            while (!out.try_push(e) && !stop_flag.load(std::memory_order_relaxed)) cpu_relax();
            std::this_thread::sleep_for(std::chrono::seconds(gap_s(rng_)));
        }
    }

private:
    std::string ticker_;
    std::mt19937_64 rng_;
};

class SyntheticFaaFeed final : public OracleFeed {
public:
    explicit SyntheticFaaFeed(std::string airport = "JFK", unsigned seed = 3)
        : airport_(std::move(airport)), rng_(seed) {}

    [[nodiscard]] OracleSource source() const noexcept override { return OracleSource::FAA; }

    void run(OracleRing& out, std::atomic<bool>& stop_flag) override {
        std::poisson_distribution<int> delayed_flights(12.0);
        while (!stop_flag.load(std::memory_order_relaxed)) {
            OracleEvent e;
            e.source = OracleSource::FAA;
            e.set_key(airport_.c_str());
            e.value = static_cast<double>(delayed_flights(rng_));
            e.observed_ts = read_tscp();
            while (!out.try_push(e) && !stop_flag.load(std::memory_order_relaxed)) cpu_relax();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

private:
    std::string airport_;
    std::mt19937_64 rng_;
};

} // namespace px::strategy
