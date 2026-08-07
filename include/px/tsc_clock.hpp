// px/tsc_clock.hpp — raw TSC reads plus one-shot calibration to nanoseconds.
//
// Used two ways in this project: matching_engine.cpp stamps every Order with a
// raw read_tsc() (cheap, uncalibrated — only relative ordering matters there);
// bench/* calibrate a TscClock once at startup and convert tick deltas to ns for
// percentile reporting.
#pragma once

#include "types.hpp"

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#include <x86intrin.h>
#endif

#include <chrono>
#include <thread>

namespace px {

[[nodiscard]] inline Tsc read_tsc() noexcept {
    return __rdtsc();
}

// rdtscp additionally waits for prior instructions to retire (partial
// serialization) before reading, so it doesn't get reordered earlier by the
// out-of-order engine the way a bare rdtsc can — the right primitive for
// bracketing a narrow timed region in a benchmark. `core` receives the
// executing logical CPU (from the aux value) for free.
[[nodiscard]] inline Tsc read_tscp(unsigned& core) noexcept {
    unsigned aux = 0;
    const Tsc t = __rdtscp(&aux);
    core = aux & 0xFFF; // low 12 bits of IA32_TSC_AUX are the OS-programmed CPU id
    return t;
}
[[nodiscard]] inline Tsc read_tscp() noexcept {
    unsigned aux;
    return __rdtscp(&aux);
}

// CPUID leaf 0x80000007, EDX bit 8: "invariant TSC" — the counter ticks at a
// fixed rate independent of core frequency/power state, so a single calibration
// stays valid for the whole run. Checked, not assumed (it's been true on
// essentially everything since ~2010, but silently assuming it on a benchmark
// that reports nanosecond percentiles is exactly the kind of unverified claim
// this project is trying not to make).
[[nodiscard]] inline bool has_invariant_tsc() noexcept {
#if defined(_MSC_VER)
    int regs[4];
    __cpuid(regs, static_cast<int>(0x80000000));
    if (static_cast<unsigned>(regs[0]) < 0x80000007u) return false;
    __cpuid(regs, static_cast<int>(0x80000007));
    return (regs[3] & (1 << 8)) != 0;
#else
    unsigned eax, ebx, ecx, edx;
    if (!__get_cpuid(0x80000007u, &eax, &ebx, &ecx, &edx)) return false;
    return (edx & (1u << 8)) != 0;
#endif
}

// One-shot TSC-tick-per-nanosecond calibration against std::chrono::steady_clock.
class TscClock {
public:
    TscClock() noexcept { calibrate(); }

    [[nodiscard]] double ticks_per_ns() const noexcept { return ticks_per_ns_; }
    [[nodiscard]] bool invariant_tsc() const noexcept { return invariant_; }

    [[nodiscard]] double to_ns(Tsc delta_ticks) const noexcept {
        return static_cast<double>(delta_ticks) / ticks_per_ns_;
    }

private:
    void calibrate() noexcept {
        invariant_ = has_invariant_tsc();

        // Warm up (first rdtsc after a while can include extra frontend latency).
        (void)read_tscp();

        using clock = std::chrono::steady_clock;
        constexpr auto kCalibrationWindow = std::chrono::milliseconds(50);

        const auto wall_start = clock::now();
        const Tsc tsc_start = read_tscp();
        std::this_thread::sleep_for(kCalibrationWindow);
        const Tsc tsc_end = read_tscp();
        const auto wall_end = clock::now();

        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start).count();
        const Tsc ticks = tsc_end - tsc_start;
        if (ns > 0) {
            ticks_per_ns_ = static_cast<double>(ticks) / static_cast<double>(ns);
        }
    }

    double ticks_per_ns_ = 1.0;
    bool invariant_ = false;
};

} // namespace px
