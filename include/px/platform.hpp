// px/platform.hpp — the few OS services the latency-sensitive threads need.
// Two implementations: src/platform_win32.cpp (what runs on the development
// machine) and src/platform_linux.cpp; the Makefile picks one by OS.
#pragma once

#include <immintrin.h>

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

// PAUSE: yields pipeline resources to the SMT sibling for one spin iteration
// without giving up the OS time slice (which std::this_thread::yield would).
inline void cpu_relax() noexcept {
    _mm_pause();
}

} // namespace px
