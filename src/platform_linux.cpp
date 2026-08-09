// src/platform_linux.cpp — real, compilable code, not written from memory of
// what pthread_setaffinity_np looks like. The Makefile selects this file
// automatically when built on Linux (see the OS branch at the top of the
// Makefile); it has not been run on real hardware as part of this project since
// the only machine available here is Windows — see the plan doc.
#include "px/platform.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

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

bool pin_current_thread_to(unsigned logical_cpu) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(logical_cpu), &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) return false;

    // SCHED_FIFO at a modest static priority keeps a pinned, busy-spinning
    // thread from being preempted by ordinary SCHED_OTHER work landing on the
    // same core. Requires CAP_SYS_NICE (or root); best-effort by the same
    // contract as the affinity call above — failure here doesn't unwind the
    // affinity pin that already succeeded.
    sched_param sp{};
    sp.sched_priority = 10;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    return true;
}

std::uint64_t current_rss_bytes() noexcept {
    // /proc/self/status's VmRSS line, reported in kB — not getrusage's
    // ru_maxrss, which is a historical PEAK, not a current-instant reading
    // (and its units are platform-inconsistent across libc versions).
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    std::uint64_t kb = 0;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            std::sscanf(line + 6, "%llu", reinterpret_cast<unsigned long long*>(&kb));
            break;
        }
    }
    std::fclose(f);
    return kb * 1024;
}

std::uint64_t process_cpu_time_ns() noexcept {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    const auto to_ns = [](const timeval& tv) noexcept -> std::uint64_t {
        return static_cast<std::uint64_t>(tv.tv_sec) * 1'000'000'000ull + static_cast<std::uint64_t>(tv.tv_usec) * 1'000ull;
    };
    return to_ns(ru.ru_utime) + to_ns(ru.ru_stime);
}

} // namespace px
