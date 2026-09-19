// px/spsc_ring.hpp — lock-free single-producer/single-consumer ring buffer.
//
// This is the LMAX Disruptor pattern in miniature: a fixed-capacity circular
// buffer with two monotonically-increasing cursors (head = next write position,
// tail = next read position; empty iff head==tail, full iff head-tail==Capacity),
// and — the actual point of the exercise — each side caches its *last-seen* view
// of the other side's cursor and only pays for the cross-core read (which bounces
// that cache line between cores) when its cache suggests the ring might be
// full/empty. A producer that's comfortably ahead of the consumer touches the
// consumer's cache line on essentially none of its pushes.
//
// The two cursors live in separate structs, one written only by the producer and
// one written only by the consumer. Each is aligned to 128 bytes, not 64: Intel's
// adjacent-line prefetcher pulls cache lines in 128-byte pairs, so two hot 64-byte
// lines that merely sit next to each other can still ping-pong between cores.
// Each struct's own two fields (its cursor + its cached copy of the other side's)
// share a line for free, since only one thread ever touches either of them.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace px {

template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing is for POD payloads only");

public:
    // Producer side only — never call from the consumer thread.
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::size_t head = prod_.head.load(std::memory_order_relaxed);
        if (head - prod_.cached_tail == Capacity) {
            prod_.cached_tail = cons_.tail.load(std::memory_order_acquire);
            if (head - prod_.cached_tail == Capacity) return false; // full
        }
        buffer_[head & kMask] = item;
        prod_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side only — never call from the producer thread.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t tail = cons_.tail.load(std::memory_order_relaxed);
        if (tail == cons_.cached_head) {
            cons_.cached_head = prod_.head.load(std::memory_order_acquire);
            if (tail == cons_.cached_head) return false; // empty
        }
        out = buffer_[tail & kMask];
        cons_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Racy by construction (reads both cursors without synchronizing with each
    // other) — for monitoring/diagnostics only, never for correctness decisions.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        return prod_.head.load(std::memory_order_relaxed) - cons_.tail.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    struct alignas(128) ProducerCursor {
        std::atomic<std::size_t> head{0};
        std::size_t cached_tail = 0; // producer's last-seen consumer tail_; avoids a
                                      // cross-core read on every push once comfortably ahead
    };
    struct alignas(128) ConsumerCursor {
        std::atomic<std::size_t> tail{0};
        std::size_t cached_head = 0; // consumer's last-seen producer head_, symmetrically
    };

    ProducerCursor prod_;
    ConsumerCursor cons_;
    alignas(128) std::array<T, Capacity> buffer_{};
};

} // namespace px
