// px/flat_map.hpp — fixed-capacity open-addressing map, OrderId -> Slot.
//
// Deliberately not std::unordered_map: that's a node-per-entry structure, it
// allocates on every insert, and a lookup costs at least one guaranteed cache
// miss chasing the bucket's node pointer. This is a single flat array pair,
// sized once by the caller for the worst case (no rehashing, no allocation
// after construction), linear-probed, with backward-shift deletion so heavy
// insert/cancel churn — exactly this table's workload — never degrades lookups
// via tombstone buildup. Algorithm: Knuth / Wikipedia "Open addressing", Deletion.
#pragma once

#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace px {

template <std::size_t Capacity>
class FlatOrderMap {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(Capacity >= 2, "Capacity too small");

public:
    FlatOrderMap() noexcept { keys_.fill(kInvalidOrderId); }

    // False if `key` already exists or the table is full (both are caller bugs
    // on this path — order IDs are engine-assigned and the table is sized for
    // the pool capacity — but checked rather than assumed).
    bool insert(OrderId key, Slot value) noexcept {
        if (size_ >= Capacity) return false;
        std::size_t i = index_of(key);
        for (;;) {
            if (keys_[i] == kInvalidOrderId) {
                keys_[i] = key;
                vals_[i] = value;
                ++size_;
                return true;
            }
            if (keys_[i] == key) return false; // duplicate key
            i = (i + 1) & kMask;
        }
    }

    [[nodiscard]] Slot find(OrderId key) const noexcept {
        std::size_t i = index_of(key);
        for (;;) {
            if (keys_[i] == kInvalidOrderId) return kNullSlot;
            if (keys_[i] == key) return vals_[i];
            i = (i + 1) & kMask;
        }
    }

    bool erase(OrderId key) noexcept {
        std::size_t i = index_of(key);
        for (;;) {
            if (keys_[i] == kInvalidOrderId) return false; // not present
            if (keys_[i] == key) break;
            i = (i + 1) & kMask;
        }

        keys_[i] = kInvalidOrderId;
        --size_;

        // Backward-shift: pull subsequent cluster entries back into the gap as
        // long as doing so doesn't violate their own probe sequence.
        std::size_t hole = i;
        std::size_t j = hole;
        for (;;) {
            j = (j + 1) & kMask;
            if (keys_[j] == kInvalidOrderId) return true;
            const std::size_t ideal = index_of(keys_[j]);
            bool blocked;
            if (hole <= j) {
                blocked = (ideal > hole) && (ideal <= j);
            } else {
                blocked = (ideal <= j) || (ideal > hole);
            }
            if (blocked) continue;
            keys_[hole] = keys_[j];
            vals_[hole] = vals_[j];
            keys_[j] = kInvalidOrderId;
            hole = j;
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    [[nodiscard]] static std::size_t index_of(OrderId key) noexcept {
        // splitmix64 finalizer: cheap, well-distributed avalanche for integer keys.
        std::uint64_t x = key;
        x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27; x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return static_cast<std::size_t>(x) & kMask;
    }

    std::array<OrderId, Capacity> keys_{};
    std::array<Slot, Capacity> vals_{};
    std::size_t size_ = 0;
};

} // namespace px
