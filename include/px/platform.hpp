// px/platform.hpp — the few OS services the latency-sensitive threads need.
// Three implementations, picked by the Makefile: src/platform_win32.cpp (what
// runs on the development machine), src/platform_linux.cpp, and
// src/platform_posix.cpp for macOS and any other POSIX system, which has no
// thread affinity to offer and says so.
#pragma once
#define PX_PLATFORM_HPP 1

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include <vector>

namespace px {

// One logical CPU id per physical performance core (SMT siblings and, on hybrid
// parts, efficiency cores excluded), or every logical CPU when the platform
// cannot say. Best-effort by design.
[[nodiscard]] std::vector<unsigned> performance_core_ids();

// What became of a thread's pinning. Benchmarks print it in their header, the
// way they print the power plan, because a number from an unpinned thread is a
// different measurement from a pinned one.
enum class Affinity : unsigned char {
    not_requested, // default scheduling, because nobody asked for pinning
    applied,       // pinned to the requested CPU
    unavailable,   // asked for, and the platform refused or cannot pin: default scheduling
};

[[nodiscard]] constexpr const char* affinity_name(Affinity a) noexcept {
    switch (a) {
    case Affinity::applied: return "applied";
    case Affinity::unavailable: return "requested but unavailable";
    case Affinity::not_requested: break;
    }
    return "default scheduling";
}

// Pins the calling thread to one logical CPU and raises its scheduling priority.
// Returns applied or unavailable and never throws: pinning improves latency, it
// is never required for correctness. On macOS it is always unavailable: its
// affinity tags are placement hints, not pinning, so none is attempted.
Affinity pin_current_thread_to(unsigned logical_cpu) noexcept;

// Process-wide latency hygiene, best-effort and silent on failure:
//   Windows — HIGH_PRIORITY_CLASS, opt out of EcoQoS power throttling, and a
//             large minimum working set so pre-faulted pages are not trimmed.
//   Linux   — mlockall(MCL_CURRENT | MCL_FUTURE) where permitted.
//   POSIX   — the same mlockall, where permitted; nothing else.
void harden_process() noexcept;

// One spin iteration's hint to the CPU, without giving up the OS time slice
// (which std::this_thread::yield would). x86-64: PAUSE, which yields pipeline
// resources to the SMT sibling. ARM64: YIELD, the same hint. Elsewhere nothing:
// it is only a hint, so a spin loop without it is still correct.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

} // namespace px
