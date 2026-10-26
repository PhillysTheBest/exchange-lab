// arena/clock.hpp — the simulated clock and the scheduler that orders a session.
//
// This is the one genuinely new mechanism in the arena. The engine measures its
// own service time in real nanoseconds; a session instead *invents* time, so that
// a participant's reaction delay can be a controlled variable rather than a
// property of the machine the session happens to run on. Those two kinds of
// nanosecond must never be confused, which is why nothing here is called
// "latency": a strategy has a `reaction_ns`, the engine has a measured latency, and
// README.md keeps them apart in the same words.
//
// THE REACTION MODEL, stated once and relied on everywhere
//   A strategy with reaction delay d, woken by something that happened at time t:
//     * acts at time t + d — that is `now` in its MarketView, and the timestamp
//       its orders carry into the engine;
//     * sees the world as of t — its mirror is advanced only to `now - d`.
//   One knob therefore buys both halves of being slow: the view is stale, and
//   the order is late. A fast strategy sees fresher prices *and* lands sooner, which
//   is exactly the advantage the headline experiment prices.
//
// DETERMINISM. Two rules, and together they are the whole contract:
//   1. Every scheduled item carries a monotonic `seq` assigned when it is
//      pushed, and the ordering is (at, seq). That is a *total* order, so no tie
//      is ever broken by heap internals, insertion luck or pointer values.
//   2. No floating point and no wall clock anywhere inside a session. Intervals
//      are integer draws from the session's own RNG; a std::log or a
//      std::exponential_distribution would be free to differ between libm and
//      libstdc++ versions and would quietly break cross-OS replay — the same
//      reason sim/order_stream.hpp only ever uses raw mt19937_64 output.
//   Given both, a seed and a roster reproduce a session bit for bit, which
//   Session::hash() makes checkable and test_arena asserts.
#pragma once

#include <cassert>
#include <cstdint>
#include <queue>
#include <vector>

namespace px::arena {

// Nanoseconds since the start of the session. Signed, so that `now - reaction_ns`
// is still meaningful during the opening moments of a session.
using SimTime = std::int64_t;

inline constexpr SimTime kNever = INT64_MAX;

// The four things that can happen, plus the three flavours of a strategy looking at
// the market. They are separate kinds rather than one kind with a reason byte so
// that the scheduler stays a POD queue and the reason survives replay verbatim.
enum class WakeKind : std::uint8_t {
    OrderArrival = 0, // a request reaches the engine
    StrategyEvent,         // a strategy reacts to something it saw on the feed
    StrategyTimer,         // a strategy's own periodic wake elapsed
    StrategySignal,        // the one informed strategy reacts to its information moving
    FairValueStep,    // the fundamental value jumps
};

// What the scheduler holds. Deliberately small and trivially copyable: the queue
// is not a hot path, but keeping it a POD keeps replay easy to reason about.
struct Wake {
    SimTime at = 0;
    std::uint64_t seq = 0; // assigned on push; makes (at, seq) a total order
    WakeKind kind = WakeKind::StrategyEvent;
    std::uint16_t who = 0; // strategy index for StrategyWake, order slot for OrderArrival
};

// Earlier time first; equal times by sequence. Strict weak ordering with no ties,
// which is what makes a replay bit-identical rather than merely similar.
struct LaterWake {
    [[nodiscard]] bool operator()(const Wake& a, const Wake& b) const noexcept {
        if (a.at != b.at) return a.at > b.at;
        return a.seq > b.seq;
    }
};

class Scheduler {
public:
    void reset() {
        queue_ = Queue();
        next_seq_ = 0;
        now_ = 0;
    }

    // Returns the sequence number the item was given, which the session uses as
    // the key for an order arrival's payload.
    //
    // Pushing into the past would mean some component read a clock it does not
    // own, so it is clamped to now — a release build then stays deterministic
    // rather than reordering — and PX_DEBUG builds trip the assert.
    std::uint64_t push(SimTime at, WakeKind kind, std::uint16_t who = 0) {
        assert(at >= now_);
        if (at < now_) at = now_;
        const std::uint64_t seq = next_seq_++;
        queue_.push(Wake{at, seq, kind, who});
        return seq;
    }

    [[nodiscard]] bool empty() const noexcept { return queue_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return queue_.size(); }
    [[nodiscard]] SimTime now() const noexcept { return now_; }
    [[nodiscard]] SimTime next_time() const noexcept { return queue_.empty() ? kNever : queue_.top().at; }

    // Advances the clock to the next item and returns it. The clock only ever
    // moves forward, and only here: no other component may set the time.
    [[nodiscard]] Wake pop() {
        const Wake w = queue_.top();
        queue_.pop();
        now_ = w.at;
        return w;
    }

private:
    using Queue = std::priority_queue<Wake, std::vector<Wake>, LaterWake>;
    Queue queue_;
    std::uint64_t next_seq_ = 0;
    SimTime now_ = 0;
};

// A seeded integer RNG: raw mt19937_64 output only, exactly as the workload
// generator does it, so a seed means the same thing on every standard library.
// Nothing here returns a floating-point value.
class SimRng {
public:
    explicit SimRng(std::uint64_t seed = 42) noexcept { reset(seed); }

    void reset(std::uint64_t seed) noexcept {
        // SplitMix64, then used as the state of a xorshift generator. Written out
        // rather than taken from <random> so that the sequence is fixed by this
        // file and not by a library version.
        s_ = seed + 0x9E3779B97F4A7C15ULL;
    }

    [[nodiscard]] std::uint64_t next() noexcept {
        std::uint64_t z = (s_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    // Uniform in [0, n). Modulo bias is accepted and stated: every n here is tiny
    // next to 2^64, and rejection sampling would make the stream depend on how
    // many draws were rejected, which is a worse trade for replay clarity.
    [[nodiscard]] std::uint64_t below(std::uint64_t n) noexcept { return n == 0 ? 0 : next() % n; }

    [[nodiscard]] bool chance(std::uint64_t percent) noexcept { return below(100) < percent; }

    // An integer stand-in for an exponential waiting time: uniform over
    // [mean/4, 7*mean/4], so the mean is `mean` and the spread is wide enough
    // that arrivals do not fall into lockstep with anything else in the session.
    [[nodiscard]] SimTime interval(SimTime mean) noexcept {
        if (mean <= 1) return mean <= 0 ? 1 : mean;
        const auto span = static_cast<std::uint64_t>(mean) * 3 / 2;
        return static_cast<SimTime>(static_cast<std::uint64_t>(mean) / 4 + below(span) + 1);
    }

private:
    std::uint64_t s_ = 0;
};

} // namespace px::arena
