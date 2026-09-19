// px/sorted_levels.hpp — one side of one book: the price-ordered index of levels.
//
// A fixed-capacity sorted flat map from price to PriceLevel slot, kept as two
// parallel arrays (keys, slots) sorted ascending, with the BEST price at the
// back. The sort key is `price` for bids and `-price` for asks, so both sides use
// this one class: the highest bid and the lowest ask are both the largest key.
//
// Why a flat array and not std::map or a pooled tree: order-book activity is
// concentrated at the touch, and at the touch this is as cheap as it gets.
// Consuming the best level is pop_back(); a new best price is an append; a lookup
// is a binary search over one contiguous int64 array (16 KB at 2048 levels)
// instead of log2(L) dependent pointer chases. std::map would also allocate a
// node per level, which the zero-allocation rule forbids.
//
// Cost bounds, stated rather than implied: lookup O(log L); insert or erase at
// rank r from the best moves r entries (12 bytes each), so the worst cases —
// adding or removing a price at the far end of a full 2048-level side — shift
// about 24 KB. Bounded by the capacity; measured by bench_engine's far-end cases.
//
// The shifts are explicit loops, NOT memmove. Measured on Windows' UCRT: a 24 KB
// memmove one element DOWN (the erase direction: overlapping, destination below
// source) took ~7.9 us, apparently a slow byte-granular path, while the same
// shift UP took ~0.4 us. A plain loop that the compiler vectorises does either
// direction in ~0.6 us on any C runtime. The Makefile passes
// -fno-tree-loop-distribute-patterns so GCC cannot turn these loops back into
// memmove calls.
#pragma once

#include "types.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>

namespace px {

class SortedLevels {
public:
    explicit SortedLevels(std::uint32_t capacity)
        : capacity_(capacity), keys_(new std::int64_t[capacity]()), slots_(new Slot[capacity]()) {}

    [[nodiscard]] static constexpr std::int64_t key_of(Side side, Price price) noexcept {
        return side == Side::Buy ? price : -price;
    }

    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool full() const noexcept { return size_ == capacity_; }

    // rank 0 == best level. Precondition: rank < size().
    [[nodiscard]] Slot at_rank(std::uint32_t rank) const noexcept { return slots_[size_ - 1 - rank]; }
    [[nodiscard]] std::int64_t key_at_rank(std::uint32_t rank) const noexcept { return keys_[size_ - 1 - rank]; }
    [[nodiscard]] Slot best() const noexcept { return slots_[size_ - 1]; }

    struct Position {
        std::uint32_t index; // where `key` is, or where it would be inserted
        bool found;
    };

    // Checks the best level first (the common case: joining or improving the
    // touch), then falls back to a bounded binary search.
    [[nodiscard]] Position locate(std::int64_t key) const noexcept {
        if (size_ == 0 || key > keys_[size_ - 1]) return {size_, false};
        if (key == keys_[size_ - 1]) return {size_ - 1, true};
        const std::int64_t* first = keys_.get();
        const std::int64_t* it = std::lower_bound(first, first + size_ - 1, key);
        const auto idx = static_cast<std::uint32_t>(it - first);
        return {idx, idx < size_ && keys_[idx] == key};
    }

    [[nodiscard]] Slot find(std::int64_t key) const noexcept {
        const Position p = locate(key);
        return p.found ? slots_[p.index] : kNullSlot;
    }

    // Precondition: !full() and `index` came from locate(key) with found == false.
    // Both shifts copy through local pointers and a local size: Slot and size_ are
    // both u32, so a store through slots_ could otherwise alias size_, forcing a
    // reload every iteration and blocking vectorisation of the slot loop.
    void insert_at(std::uint32_t index, std::int64_t key, Slot slot) noexcept {
        assert(!full() && index <= size_);
        const std::uint32_t n = size_;
        std::int64_t* const k = keys_.get();
        Slot* const s = slots_.get();
        for (std::uint32_t i = n; i > index; --i) k[i] = k[i - 1]; // shift up: copy descending
        for (std::uint32_t i = n; i > index; --i) s[i] = s[i - 1];
        k[index] = key;
        s[index] = slot;
        size_ = n + 1;
    }

    void erase_at(std::uint32_t index) noexcept {
        assert(index < size_);
        const std::uint32_t last = size_ - 1;
        std::int64_t* const k = keys_.get();
        Slot* const s = slots_.get();
        for (std::uint32_t i = index; i < last; ++i) k[i] = k[i + 1]; // shift down: copy ascending
        for (std::uint32_t i = index; i < last; ++i) s[i] = s[i + 1];
        size_ = last;
    }

    void pop_best() noexcept {
        assert(size_ > 0);
        --size_;
    }

    // Raw index access for invariant checks (index 0 == worst level).
    [[nodiscard]] std::int64_t key_at_index(std::uint32_t i) const noexcept { return keys_[i]; }
    [[nodiscard]] Slot slot_at_index(std::uint32_t i) const noexcept { return slots_[i]; }

private:
    std::uint32_t capacity_;
    std::uint32_t size_ = 0;
    std::unique_ptr<std::int64_t[]> keys_;
    std::unique_ptr<Slot[]> slots_;
};

} // namespace px
