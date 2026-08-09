#include "px/platform.hpp"

#include <windows.h>

#include <psapi.h>

namespace px {

namespace {
[[nodiscard]] std::uint64_t filetime_to_ns(const FILETIME& ft) noexcept {
    // FILETIME is 100ns ticks since 1601 — only the delta between two
    // readings is ever used by process_cpu_time_ns's callers, so the epoch
    // itself doesn't matter, only the tick width (100ns -> *100 for ns).
    const std::uint64_t ticks = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return ticks * 100;
}
} // namespace

std::vector<unsigned> performance_core_ids() {
    std::vector<unsigned> fallback_all;
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        for (DWORD i = 0; i < si.dwNumberOfProcessors; ++i) fallback_all.push_back(i);
    }

    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0) return fallback_all;

    std::vector<char> buf(len);
    auto* base = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, base, &len)) return fallback_all;

    struct CoreEntry {
        unsigned representative_logical_id;
        BYTE efficiency_class;
    };
    std::vector<CoreEntry> cores;
    BYTE max_efficiency = 0;

    char* ptr = buf.data();
    char* end = buf.data() + len;
    while (ptr < end) {
        auto* item = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
        if (item->Relationship == RelationProcessorCore && item->Processor.GroupCount > 0) {
            // Single processor-group assumption: fine up to 64 logical CPUs,
            // which covers every consumer/prosumer part this runs on.
            const KAFFINITY mask = item->Processor.GroupMask[0].Mask;
            unsigned representative = 0;
            for (unsigned bit = 0; bit < sizeof(KAFFINITY) * 8; ++bit) {
                if (mask & (KAFFINITY{1} << bit)) {
                    representative = bit;
                    break;
                }
            }
            const BYTE eff = item->Processor.EfficiencyClass;
            cores.push_back({representative, eff});
            if (eff > max_efficiency) max_efficiency = eff;
        }
        ptr += item->Size;
    }

    if (cores.empty()) return fallback_all;

    std::vector<unsigned> result;
    for (const auto& c : cores) {
        if (c.efficiency_class == max_efficiency) result.push_back(c.representative_logical_id);
    }
    return result.empty() ? fallback_all : result;
}

bool pin_current_thread_to(unsigned logical_cpu) noexcept {
    if (logical_cpu >= sizeof(DWORD_PTR) * 8) return false; // see single-group note above

    const HANDLE h = GetCurrentThread();
    const DWORD_PTR mask = DWORD_PTR{1} << logical_cpu;
    if (SetThreadAffinityMask(h, mask) == 0) return false;

    // Best-effort: a failure here doesn't unwind the affinity pin already applied.
    SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
    return true;
}

std::uint64_t current_rss_bytes() noexcept {
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return static_cast<std::uint64_t>(pmc.WorkingSetSize);
}

std::uint64_t process_cpu_time_ns() noexcept {
    FILETIME creation, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 0;
    return filetime_to_ns(kernel) + filetime_to_ns(user);
}

} // namespace px
