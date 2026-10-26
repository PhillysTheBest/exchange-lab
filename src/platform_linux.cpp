// src/platform_linux.cpp — Linux implementation of px/platform.hpp. The Makefile
// selects it automatically on a non-Windows build. It has been run under WSL2
// (results/wsl2/), not on bare-metal Linux.
#include "px/platform.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

namespace px {

std::vector<unsigned> performance_core_ids() {
    // Distinguishing P-cores from E-cores on Linux means reading
    // /sys/devices/cpu_core/cpus / /sys/devices/cpu_atom/cpus (Intel Thread
    // Director, kernel 5.16+) or comparing per-core capacity under
    // /sys/devices/system/cpu/cpuN/cpu_capacity — both genuinely kernel-version-
    // and vendor-specific in ways that are easy to get subtly wrong without
    // hardware to validate against. Rather than guess, this returns every online
    // logical CPU, which is exactly correct on the non-hybrid server/HFT boxes
    // this would actually run on, and lets the caller apply its own selection
    // (e.g. skip SMT siblings via /sys/.../topology/thread_siblings_list) on a
    // hybrid one.
    std::vector<unsigned> ids;
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    for (long i = 0; i < n; ++i) ids.push_back(static_cast<unsigned>(i));
    return ids;
}

Affinity pin_current_thread_to(unsigned logical_cpu) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(logical_cpu), &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) return Affinity::unavailable;

    // SCHED_FIFO at a modest static priority keeps a pinned, busy-spinning
    // thread from being preempted by ordinary SCHED_OTHER work landing on the
    // same core. Requires CAP_SYS_NICE (or root); best-effort by the same
    // contract as the affinity call above — failure here doesn't unwind the
    // affinity pin that already succeeded.
    sched_param sp{};
    sp.sched_priority = 10;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    return Affinity::applied;
}

void harden_process() noexcept {
    // Lock memory only when RLIMIT_MEMLOCK is unlimited (root, or ulimit -l
    // unlimited). With a finite limit, MCL_FUTURE turns every allocation past the
    // limit into a failure (std::bad_alloc), so there the pre-faulting done at
    // construction has to be enough.
    rlimit lim{};
    if (getrlimit(RLIMIT_MEMLOCK, &lim) == 0 && lim.rlim_cur == RLIM_INFINITY) {
        mlockall(MCL_CURRENT | MCL_FUTURE);
    }
}

} // namespace px
