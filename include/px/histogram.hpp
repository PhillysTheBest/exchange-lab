// px/histogram.hpp — latency percentile reporting.
//
// In this domain the tail is the product: a mean latency hides exactly the
// outliers (a GC-like pause, a mispredicted branch cascading into a TLB miss, an
// OS scheduler preemption) that determine whether an order beats the market or
// arrives after it's already moved. Every benchmark in bench/ reports
// p50/p90/p99/p99.9/p99.99/max from this, never an average.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace px {

class LatencyHistogram {
public:
    void record(double ns) noexcept { samples_.push_back(ns); }

    void reserve(std::size_t n) { samples_.reserve(n); }
    [[nodiscard]] std::size_t count() const noexcept { return samples_.size(); }

    struct Percentiles {
        double p50, p90, p99, p999, p9999, max, min, mean;
    };

    // Sorts the recorded samples in place (O(n log n), call once after collection).
    [[nodiscard]] Percentiles summarize() noexcept {
        Percentiles r{};
        if (samples_.empty()) return r;
        std::sort(samples_.begin(), samples_.end());
        const std::size_t n = samples_.size();
        auto at = [&](double q) {
            std::size_t idx = static_cast<std::size_t>(q * static_cast<double>(n - 1));
            return samples_[idx];
        };
        r.p50   = at(0.50);
        r.p90   = at(0.90);
        r.p99   = at(0.99);
        r.p999  = at(0.999);
        r.p9999 = at(0.9999);
        r.min   = samples_.front();
        r.max   = samples_.back();
        double sum = 0;
        for (double s : samples_) sum += s;
        r.mean = sum / static_cast<double>(n);
        return r;
    }

    static void print(const char* label, const Percentiles& p) {
        std::printf("%-28s  p50=%8.1f  p90=%8.1f  p99=%8.1f  p99.9=%8.1f  p99.99=%8.1f  max=%10.1f  (mean=%8.1f, min=%6.1f) ns\n",
                    label, p.p50, p.p90, p.p99, p.p999, p.p9999, p.max, p.mean, p.min);
    }

    // Stage 5, Section 7 (performance regression harness): opens `path` in
    // append mode and, iff the file is currently empty (a fresh file, or one
    // truncated/deleted before this run — perf/run_sweeps.py-style callers
    // typically remove the file first), writes the column header exactly
    // once. Returns nullptr (and leaves the file untouched) on open failure
    // — callers should treat that as "no CSV output this run," never fatal,
    // matching this project's platform.hpp "best-effort, absent-on-failure"
    // convention for measurement-only helpers.
    [[nodiscard]] static std::FILE* open_csv_for_append(const char* path) {
        std::FILE* f = std::fopen(path, "a");
        if (!f) return nullptr;
        std::fseek(f, 0, SEEK_END);
        if (std::ftell(f) == 0) {
            std::fprintf(f, "label,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns,mean_ns,min_ns\n");
        }
        return f;
    }

    // Appends one row for `label`. No-op if `f` is nullptr (see
    // open_csv_for_append) — every call site can pass the possibly-null
    // result straight through without its own null check.
    static void append_csv(std::FILE* f, const char* label, const Percentiles& p) {
        if (!f) return;
        std::fprintf(f, "%s,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n", label, p.p50, p.p90, p.p99, p.p999, p.p9999,
                     p.max, p.mean, p.min);
    }

private:
    std::vector<double> samples_;
};

} // namespace px
