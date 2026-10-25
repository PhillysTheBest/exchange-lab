// px/cycle_clock.hpp — the benchmarks' cycle counter, and one-shot calibration to ns.
//
// One counter per architecture, picked at compile time; everything else in the
// benchmarks sees only Tsc ticks, tsc_begin()/tsc_end() and TscClock:
//   x86-64  RDTSC. The bracket is the one Intel's "How to Benchmark Code
//           Execution Times" white paper prescribes, minus CPUID (which traps to
//           the hypervisor under Windows 11 VBS and costs microseconds):
//             begin: LFENCE; RDTSC   — the read cannot start before earlier work retires
//             end:   RDTSCP; LFENCE  — the read waits for the timed work, and nothing
//                                      after it can start before the read
//   ARM64   CNTVCT_EL0, the generic timer's virtual count. It ticks at a fixed
//           frequency (CNTFRQ_EL0), which makes it constant-rate by architecture
//           but far coarser than a TSC: resolution_ns() says how coarse. ISB on both sides orders the read against the
//           timed work, the role LFENCE plays on x86.
//   other   std::chrono::steady_clock, in nanoseconds. Correct, and the slowest.
// Every stamp is asm volatile with a "memory" clobber (or an opaque library
// call), so the compiler, LTO included, cannot move the timed work across it.
// kCounterName and TscClock::resolution_ns() let each benchmark print which
// counter produced its numbers. Nothing inside an arena session includes this
// header: simulated time never reads hardware (test_arena checks that).
#pragma once
#define PX_CYCLE_CLOCK_HPP 1

#include "types.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#define PX_COUNTER_X86 1
#include <cpuid.h>
#include <x86intrin.h>
#elif defined(__aarch64__)
#define PX_COUNTER_ARM64 1
#endif

#include <chrono>
#include <thread>

namespace px {

#if defined(PX_COUNTER_X86)

inline constexpr const char* kCounterName = "x86-64 TSC";

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

// An unfenced read, for spin loops that only need "has this much time passed".
[[nodiscard]] inline Tsc tsc_now() noexcept { return __rdtsc(); }

// CPUID leaf 0x80000007, EDX bit 8: the TSC ticks at a constant rate regardless
// of frequency scaling or C-states, so one calibration holds for the whole run.
// Checked once at startup, never assumed.
[[nodiscard]] inline bool has_invariant_tsc() noexcept {
    unsigned eax, ebx, ecx, edx;
    if (!__get_cpuid(0x80000007u, &eax, &ebx, &ecx, &edx)) return false;
    return (edx & (1u << 8)) != 0;
}

#elif defined(PX_COUNTER_ARM64)

inline constexpr const char* kCounterName = "ARM64 CNTVCT_EL0";

[[nodiscard]] inline Tsc tsc_now() noexcept {
    Tsc v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v) : : "memory");
    return v;
}

[[nodiscard]] inline Tsc tsc_begin() noexcept {
    Tsc v;
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory");
    return v;
}

[[nodiscard]] inline Tsc tsc_end() noexcept {
    Tsc v;
    asm volatile("isb\n\tmrs %0, cntvct_el0\n\tisb" : "=r"(v) : : "memory");
    return v;
}

// The generic timer is constant-rate by architecture: there is nothing to check.
[[nodiscard]] inline bool has_invariant_tsc() noexcept { return true; }

#else

inline constexpr const char* kCounterName = "std::chrono::steady_clock";

[[nodiscard]] inline Tsc tsc_now() noexcept {
    return static_cast<Tsc>(std::chrono::steady_clock::now().time_since_epoch().count());
}
[[nodiscard]] inline Tsc tsc_begin() noexcept { return tsc_now(); }
[[nodiscard]] inline Tsc tsc_end() noexcept { return tsc_now(); }
[[nodiscard]] inline bool has_invariant_tsc() noexcept { return std::chrono::steady_clock::is_steady; }

#endif

// Counter ticks per nanosecond, calibrated against std::chrono::steady_clock.
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
    // The length of one tick: no interval shorter than this can be told apart.
    [[nodiscard]] double resolution_ns() const noexcept { return 1.0 / ticks_per_ns_; }
    [[nodiscard]] double to_ns(Tsc ticks) const noexcept { return static_cast<double>(ticks) / ticks_per_ns_; }
    [[nodiscard]] Tsc from_ns(double ns) const noexcept { return static_cast<Tsc>(ns * ticks_per_ns_); }

private:
    double ticks_per_ns_ = 1.0;
    bool invariant_ = false;
};

} // namespace px
