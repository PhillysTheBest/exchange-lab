// px/pool.hpp — fixed-capacity slab allocator with an intrusive free list.
//
// The hot path never calls new/delete. All Order storage is a single contiguous
// array allocated once (when the Pool object itself is constructed); acquire()/
// release() are O(1) pointer-free operations that hand out/reclaim a `Slot`
// (a plain u32 index), not a pointer — that keeps every reference into the pool
// stable across the whole session and 4 bytes wide instead of 8.
#pragma once

#include "types.hpp"

#include <array>
#include <cstddef>

namespace px {

template <typename T, std::size_t Capacity>
class Pool {
public:
    Pool() noexcept {
        for (std::size_t i = 0; i + 1 < Capacity; ++i) {
            free_link_[i] = static_cast<Slot>(i + 1);
        }
        free_link_[Capacity - 1] = kNullSlot;
        free_head_ = 0;
    }

    // Returns kNullSlot if the pool is exhausted — callers on the matching path
    // must check this and reject the command (RejectReason::PoolExhausted) rather
    // than let it propagate as a dangling reference.
    [[nodiscard]] Slot acquire() noexcept {
        if (free_head_ == kNullSlot) return kNullSlot;
        const Slot s = free_head_;
        free_head_ = free_link_[s];
        ++live_;
        return s;
    }

    void release(Slot s) noexcept {
        free_link_[s] = free_head_;
        free_head_ = s;
        --live_;
    }

    [[nodiscard]] T& operator[](Slot s) noexcept { return storage_[s]; }
    [[nodiscard]] const T& operator[](Slot s) const noexcept { return storage_[s]; }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }
    [[nodiscard]] std::size_t live() const noexcept { return live_; }

private:
    alignas(64) std::array<T, Capacity> storage_{};
    std::array<Slot, Capacity> free_link_{};
    Slot free_head_ = 0;
    std::size_t live_ = 0;
};

} // namespace px
