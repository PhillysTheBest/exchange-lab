// px/platform.hpp — the few OS services the latency-sensitive threads need.
// Two implementations: src/platform_win32.cpp (what runs on the development
// machine) and src/platform_linux.cpp; the Makefile picks one by OS.
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

// Pins the calling thread to one logical CPU and raises its scheduling priority.
// Returns false on failure and never throws: pinning improves latency, it is
// never required for correctness.
bool pin_current_thread_to(unsigned logical_cpu) noexcept;

// Process-wide latency hygiene, best-effort and silent on failure:
//   Windows — HIGH_PRIORITY_CLASS, opt out of EcoQoS power throttling, and a
//             large minimum working set so pre-faulted pages are not trimmed.
//   Linux   — mlockall(MCL_CURRENT | MCL_FUTURE) where permitted.
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
