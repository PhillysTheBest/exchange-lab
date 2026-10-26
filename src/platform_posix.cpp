// src/platform_posix.cpp — px/platform.hpp for macOS and any other POSIX system
// that is neither Linux nor Windows. The Makefile selects it by `uname -s`.
//
// It pins nothing. macOS has no thread affinity in the Linux sense: its
// thread_policy_set(THREAD_AFFINITY_POLICY) tags are hints that threads with
// the same tag share a cache, not a placement, and Apple silicon ignores them.
// Pretending otherwise would print "pinned" above numbers from threads the
// scheduler moved freely, so pin_current_thread_to reports unavailable and the
// benchmarks print that. Benchmark numbers from such a machine are not
// comparable with pinned ones (docs/ENGINE.md).
#include "px/platform.hpp"

#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

namespace px {

std::vector<unsigned> performance_core_ids() {
    // No portable way to tell performance from efficiency cores: every online
    // logical CPU, as on Linux.
    std::vector<unsigned> ids;
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    for (long i = 0; i < n; ++i) ids.push_back(static_cast<unsigned>(i));
    return ids;
}

Affinity pin_current_thread_to(unsigned /*logical_cpu*/) noexcept { return Affinity::unavailable; }

void harden_process() noexcept {
    // The same rule as Linux: lock memory only when RLIMIT_MEMLOCK is unlimited,
    // because with a finite limit MCL_FUTURE turns later allocations into failures.
    rlimit lim{};
    if (getrlimit(RLIMIT_MEMLOCK, &lim) == 0 && lim.rlim_cur == RLIM_INFINITY) {
        mlockall(MCL_CURRENT | MCL_FUTURE);
    }
}

} // namespace px
