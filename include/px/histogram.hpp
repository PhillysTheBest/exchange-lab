// px/histogram.hpp — latency sample storage and percentile reporting.
//
// The tail is the product: a mean hides exactly the outliers (a cache miss
// cascading into a TLB walk, an interrupt landing mid-order) that decide whether
// an exchange is predictable. So every benchmark here reports p50 through p99.99
// plus the max, computed by nearest rank over every sample, never a mean alone.
//
// Samples are raw TSC tick deltas stored as u32 in an array allocated (and
// pre-touched) up front: record() is one bounds check and one store, with no
// allocation and no floating point on the measured thread. Conversion to ns and
// sorting happen after the run.
#pragma once

#include "cycle_clock.hpp"
#include "types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace px {

struct Percentiles {
    std::size_t n = 0;
    double min = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0, mean = 0; // ns
};

class LatencyRecorder {
public:
    explicit LatencyRecorder(std::size_t capacity) : samples_(capacity, 0) {}

    void record(Tsc ticks) noexcept {
        if (n_ == samples_.size()) return;
        samples_[n_++] = ticks > kMax ? kMax : static_cast<std::uint32_t>(ticks);
    }

    [[nodiscard]] std::size_t count() const noexcept { return n_; }
    [[nodiscard]] std::uint32_t at(std::size_t i) const noexcept { return samples_[i]; }

    [[nodiscard]] std::size_t count_above(Tsc threshold_ticks) const noexcept {
        std::size_t c = 0;
        for (std::size_t i = 0; i < n_; ++i) c += samples_[i] > threshold_ticks;
        return c;
    }

    // Nearest-rank percentile (q in (0, 1]) of every recorded sample, in ns.
    [[nodiscard]] double percentile_ns(const TscClock& clock, double q) const {
        if (n_ == 0) return 0;
        std::vector<std::uint32_t> v(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(n_));
        std::size_t r = static_cast<std::size_t>(std::ceil(q * static_cast<double>(n_)));
        r = std::clamp<std::size_t>(r, 1, n_);
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(r - 1), v.end());
        return clock.to_ns(v[r - 1]);
    }

    // Nearest rank: the smallest sample with at least q*n samples at or below it.
    [[nodiscard]] Percentiles summarize(const TscClock& clock) const {
        return summarize_indices(clock, nullptr, 0);
    }

    // Same, restricted to the samples whose index appears in `idx`.
    [[nodiscard]] Percentiles summarize_indices(const TscClock& clock, const std::uint32_t* idx,
                                                std::size_t count) const {
        std::vector<std::uint32_t> v;
        if (idx == nullptr) {
            v.assign(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(n_));
        } else {
            v.reserve(count);
            for (std::size_t i = 0; i < count; ++i) v.push_back(samples_[idx[i]]);
        }
        Percentiles p;
        p.n = v.size();
        if (v.empty()) return p;
        std::sort(v.begin(), v.end());
        const auto rank = [&](double q) {
            std::size_t r = static_cast<std::size_t>(std::ceil(q * static_cast<double>(v.size())));
            r = std::clamp<std::size_t>(r, 1, v.size());
            return clock.to_ns(v[r - 1]);
        };
        double sum = 0;
        for (const std::uint32_t s : v) sum += s;
        p.min = clock.to_ns(v.front());
        p.p50 = rank(0.50);
        p.p90 = rank(0.90);
        p.p99 = rank(0.99);
        p.p999 = rank(0.999);
        p.p9999 = rank(0.9999);
        p.max = clock.to_ns(v.back());
        p.mean = clock.to_ns(static_cast<Tsc>(sum / static_cast<double>(v.size())));
        return p;
    }

private:
    static constexpr Tsc kMax = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> samples_;
    std::size_t n_ = 0;
};

inline void print_percentiles(const char* label, const Percentiles& p) {
    std::printf("%-30s n=%-8zu p50=%7.0f  p90=%7.0f  p99=%7.0f  p99.9=%7.0f  p99.99=%7.0f  max=%9.0f ns\n", label,
                p.n, p.p50, p.p90, p.p99, p.p999, p.p9999, p.max);
}

// Appends one CSV row per call; writes the header only into an empty file.
// Returns nullptr on open failure, and append_csv(nullptr, ...) is a no-op, so a
// missing CSV never fails a benchmark.
[[nodiscard]] inline std::FILE* open_csv_for_append(const char* path) {
    std::FILE* f = std::fopen(path, "a");
    if (!f) return nullptr;
    std::fseek(f, 0, SEEK_END);
    if (std::ftell(f) == 0) std::fprintf(f, "label,n,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns,mean_ns\n");
    return f;
}

inline void append_csv(std::FILE* f, const char* label, const Percentiles& p) {
    if (!f) return;
    std::fprintf(f, "%s,%zu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n", label, p.n, p.p50, p.p90, p.p99, p.p999, p.p9999,
                 p.max, p.mean);
}

} // namespace px
