// px/pool.hpp — fixed-capacity slab allocator handing out Slot indices.
//
// The hot path never calls new/delete. All storage is one contiguous array
// allocated (and zero-filled, which also pre-faults every page) when the Pool is
// constructed; acquire()/release() are O(1) pushes and pops on a free list kept
// in a parallel index array. Slots are plain u32 indices, so references into the
// pool stay valid for the whole session and are half the width of a pointer.
#pragma once

#include "types.hpp"

#include <cassert>
#include <cstddef>
#include <memory>

namespace px {

template <typename T>
class Pool {
public:
    explicit Pool(std::size_t capacity)
        : capacity_(capacity), storage_(new T[capacity]()), free_link_(new Slot[capacity]) {
        assert(capacity > 0 && capacity < kNullSlot);
        for (std::size_t i = 0; i + 1 < capacity; ++i) free_link_[i] = static_cast<Slot>(i + 1);
        free_link_[capacity - 1] = kNullSlot;
#ifdef PX_DEBUG
        in_use_.reset(new bool[capacity]());
#endif
    }

    // kNullSlot when exhausted. The engine checks capacity before it commits to
    // an operation, so on the matching path this never actually returns null.
    [[nodiscard]] Slot acquire() noexcept {
        if (free_head_ == kNullSlot) return kNullSlot;
        const Slot s = free_head_;
        free_head_ = free_link_[s];
        ++live_;
#ifdef PX_DEBUG
        assert(!in_use_[s]);
        in_use_[s] = true;
#endif
        return s;
    }

    void release(Slot s) noexcept {
#ifdef PX_DEBUG
        assert(s < capacity_ && in_use_[s] && "double free or foreign slot");
        in_use_[s] = false;
#endif
        free_link_[s] = free_head_;
        free_head_ = s;
        --live_;
    }

    [[nodiscard]] T& operator[](Slot s) noexcept { return storage_[s]; }
    [[nodiscard]] const T& operator[](Slot s) const noexcept { return storage_[s]; }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t live() const noexcept { return live_; }
    [[nodiscard]] std::size_t available() const noexcept { return capacity_ - live_; }
    [[nodiscard]] bool full() const noexcept { return free_head_ == kNullSlot; }

private:
    std::size_t capacity_;
    std::unique_ptr<T[]> storage_;
    std::unique_ptr<Slot[]> free_link_;
    Slot free_head_ = 0;
    std::size_t live_ = 0;
#ifdef PX_DEBUG
    std::unique_ptr<bool[]> in_use_;
#endif
};

} // namespace px
