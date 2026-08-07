// px/platform.hpp — CPU pinning interface. Two implementations exist:
// src/platform_win32.cpp (what actually runs on this machine) and
// src/platform_linux.cpp (real, compilable code — the Makefile picks it up
// automatically on a Linux build — kept alongside rather than left as a comment,
// since that's the file you'd actually run on a bypass-capable box; see the
// plan doc's "Reality check on latency" section for why kernel bypass itself
// isn't attempted here).
#pragma once

#include <immintrin.h>
#include <vector>

namespace px {

// Logical CPU indices of "performance" cores on a hybrid CPU (one representative
// logical id per physical core, SMT siblings excluded), or every logical CPU on
// a non-hybrid machine. Best-effort: falls back to "every logical CPU" when the
// platform can't report efficiency-class info, so pinning still does something
// sane rather than nothing.
[[nodiscard]] std::vector<unsigned> performance_core_ids();

// Pins the calling thread to a single logical CPU and, where supported, raises
// its scheduling priority to reduce preemption jitter. Returns false on failure
// and never throws — pinning is a latency optimization, not a correctness
// requirement; callers should treat failure as "proceed unpinned."
bool pin_current_thread_to(unsigned logical_cpu) noexcept;

// PAUSE (x86): relaxes the pipeline for one spin iteration without yielding the
// OS timeslice. Use inside a busy-spin loop on a pinned/isolated core instead of
// std::this_thread::yield(), which would hand the core back to the scheduler —
// exactly what a thread that just got pinned specifically to avoid that shouldn't do.
inline void cpu_relax() noexcept {
    _mm_pause();
}

} // namespace px
