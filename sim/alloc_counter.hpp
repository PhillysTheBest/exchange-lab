// sim/alloc_counter.hpp — replaces the global allocation functions with versions
// that count every call, so a test or benchmark can assert that a region of code
// performed zero heap allocations.
//
// Include from EXACTLY ONE translation unit per executable: it defines the
// replaceable global operator new/delete family.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace px::sim {
inline std::atomic<std::uint64_t> g_allocations{0};
} // namespace px::sim

namespace px::sim::detail {

inline void* counted_alloc(std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}

inline void* counted_alloc_aligned(std::size_t n, std::align_val_t al) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    const auto a = static_cast<std::size_t>(al);
    const std::size_t size = ((n == 0 ? 1 : n) + a - 1) / a * a;
#if defined(_WIN32)
    if (void* p = _aligned_malloc(size, a)) return p;
#else
    if (void* p = std::aligned_alloc(a, size)) return p;
#endif
    throw std::bad_alloc();
}

inline void aligned_release(void* p) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

} // namespace px::sim::detail

void* operator new(std::size_t n) { return px::sim::detail::counted_alloc(n); }
void* operator new[](std::size_t n) { return px::sim::detail::counted_alloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return px::sim::detail::counted_alloc(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return px::sim::detail::counted_alloc(n); } catch (...) { return nullptr; }
}
void* operator new(std::size_t n, std::align_val_t a) { return px::sim::detail::counted_alloc_aligned(n, a); }
void* operator new[](std::size_t n, std::align_val_t a) { return px::sim::detail::counted_alloc_aligned(n, a); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { px::sim::detail::aligned_release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { px::sim::detail::aligned_release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { px::sim::detail::aligned_release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { px::sim::detail::aligned_release(p); }
