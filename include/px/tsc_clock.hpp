// px/tsc_clock.hpp — time-stamp-counter reads and one-shot calibration to ns.
//
// tsc_begin()/tsc_end() bracket a timed region the way Intel's "How to Benchmark
// Code Execution Times" white paper prescribes, minus CPUID (which traps to the
// hypervisor under Windows 11 VBS and costs microseconds):
//   begin: LFENCE; RDTSC   — the read cannot start before earlier work retires
//   end:   RDTSCP; LFENCE  — the read waits for the timed work, and nothing
//                            after it can start before the read
// Both are asm volatile with a "memory" clobber, so the compiler (LTO included)
// cannot move loads or stores of the timed work across either stamp.
#pragma once

#include "types.hpp"

#include <cpuid.h>
#include <x86intrin.h>

#include <chrono>
#include <thread>

namespace px {

[[nodiscard]] inline Tsc tsc_begin() noexcept {
    unsigned lo, hi;
    asm volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return (static_cast<Tsc>(hi) << 32) | lo;
}

[[nodiscard]] inline Tsc tsc_end() noexcept {
    unsigned lo, hi, aux;
    asm volatile("rdtscp\n\tlfence" : "=a"(lo), "=d"(hi), "=c"(aux) : : "memory");
    return (static_cast<Tsc>(hi) << 32) | lo;
}

// CPUID leaf 0x80000007, EDX bit 8: the TSC ticks at a constant rate regardless
// of frequency scaling or C-states, so one calibration holds for the whole run.
// Checked once at startup, never assumed.
[[nodiscard]] inline bool has_invariant_tsc() noexcept {
    unsigned eax, ebx, ecx, edx;
    if (!__get_cpuid(0x80000007u, &eax, &ebx, &ecx, &edx)) return false;
    return (edx & (1u << 8)) != 0;
}

// TSC ticks per nanosecond, calibrated against std::chrono::steady_clock.
class TscClock {
public:
    explicit TscClock(std::chrono::milliseconds window = std::chrono::milliseconds(200)) noexcept {
        invariant_ = has_invariant_tsc();
        (void)tsc_end();
        using clock = std::chrono::steady_clock;
        const auto wall_start = clock::now();
        const Tsc t0 = tsc_end();
        std::this_thread::sleep_for(window);
        const Tsc t1 = tsc_end();
        const auto wall_end = clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start).count();
        if (ns > 0) ticks_per_ns_ = static_cast<double>(t1 - t0) / static_cast<double>(ns);
    }

    [[nodiscard]] double ticks_per_ns() const noexcept { return ticks_per_ns_; }
    [[nodiscard]] bool invariant_tsc() const noexcept { return invariant_; }
    [[nodiscard]] double to_ns(Tsc ticks) const noexcept { return static_cast<double>(ticks) / ticks_per_ns_; }
    [[nodiscard]] Tsc from_ns(double ns) const noexcept { return static_cast<Tsc>(ns * ticks_per_ns_); }

private:
    double ticks_per_ns_ = 1.0;
    bool invariant_ = false;
};

} // namespace px
