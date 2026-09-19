// px/flat_map.hpp — fixed-capacity open-addressing hash map, OrderId -> Slot.
//
// Deliberately not std::unordered_map, which allocates a node per insert and
// costs a pointer chase per lookup. This is one flat array of 16-byte
// {key, slot} entries, sized once by the caller (no rehash, no allocation after
// construction), linear-probed so a probe sequence walks adjacent cache lines,
// with backward-shift deletion so heavy insert/erase churn never accumulates
// tombstones. The caller keeps the load factor at or below 50%; a completely full
// table would make find() spin forever, so insert() refuses to fill it.
// Algorithm: Knuth / Wikipedia "Open addressing", deletion without tombstones.
#pragma once

#include "types.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace px {

class FlatOrderMap {
public:
    explicit FlatOrderMap(std::size_t capacity_pow2)
        : mask_(capacity_pow2 - 1), capacity_(capacity_pow2), entries_(new Entry[capacity_pow2]) {
        assert(capacity_pow2 >= 2 && (capacity_pow2 & (capacity_pow2 - 1)) == 0);
        for (std::size_t i = 0; i < capacity_; ++i) entries_[i] = Entry{};
    }

    // False if `key` is already present, is the reserved empty marker, or the
    // table would become completely full.
    bool insert(OrderId key, Slot value) noexcept {
        if (key == kInvalidOrderId || size_ + 1 >= capacity_) return false;
        std::size_t i = index_of(key);
        for (;;) {
            Entry& e = entries_[i];
            if (e.key == kInvalidOrderId) {
                e.key = key;
                e.slot = value;
                ++size_;
                return true;
            }
            if (e.key == key) return false;
            i = (i + 1) & mask_;
        }
    }

    [[nodiscard]] Slot find(OrderId key) const noexcept {
        if (key == kInvalidOrderId) return kNullSlot;
        std::size_t i = index_of(key);
        for (;;) {
            const Entry& e = entries_[i];
            if (e.key == key) return e.slot;
            if (e.key == kInvalidOrderId) return kNullSlot;
            i = (i + 1) & mask_;
        }
    }

    bool erase(OrderId key) noexcept {
        if (key == kInvalidOrderId) return false;
        std::size_t i = index_of(key);
        for (;;) {
            if (entries_[i].key == key) break;
            if (entries_[i].key == kInvalidOrderId) return false;
            i = (i + 1) & mask_;
        }
        entries_[i].key = kInvalidOrderId;
        --size_;

        // Backward shift: pull later members of the probe cluster into the hole
        // whenever that doesn't move them in front of their own home bucket.
        std::size_t hole = i;
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (entries_[j].key == kInvalidOrderId) return true;
            const std::size_t home = index_of(entries_[j].key);
            const bool stays = (hole <= j) ? (home > hole && home <= j) : (home > hole || home <= j);
            if (stays) continue;
            entries_[hole] = entries_[j];
            entries_[j].key = kInvalidOrderId;
            hole = j;
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    struct Entry {
        OrderId key = kInvalidOrderId;
        Slot slot = kNullSlot;
        std::uint32_t pad = 0;
    };
    static_assert(sizeof(Entry) == 16);

    [[nodiscard]] std::size_t index_of(OrderId key) const noexcept {
        // splitmix64 finalizer: cheap, full-avalanche mixing for sequential ids.
        std::uint64_t x = key;
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return static_cast<std::size_t>(x) & mask_;
    }

    std::size_t mask_;
    std::size_t capacity_;
    std::unique_ptr<Entry[]> entries_;
    std::size_t size_ = 0;
};

} // namespace px
