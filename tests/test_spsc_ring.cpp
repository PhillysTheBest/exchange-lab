// tests/test_spsc_ring.cpp — the ring under real cross-thread traffic: 10M
// sequenced 64-byte messages through a small ring (so it is constantly full and
// constantly empty), checking that every message arrives, in order, intact.
#include "px/messages.hpp"
#include "px/platform.hpp"
#include "px/spsc_ring.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <memory>
#include <thread>

using namespace px;

int main() {
    constexpr std::uint64_t kMessages = 10'000'000;
    auto ring = std::make_unique<SpscRing<OrderRequest, 1024>>();

    const auto cores = performance_core_ids();
    std::uint64_t producer_sum = 0;
    std::thread producer([&] {
        if (cores.size() >= 2) pin_current_thread_to(cores[0]);
        for (std::uint64_t i = 0; i < kMessages; ++i) {
            OrderRequest r;
            r.order_id = i;
            r.price = static_cast<Price>(i * 7);
            r.qty = static_cast<Qty>(i);
            producer_sum += i;
            while (!ring->try_push(r)) cpu_relax();
        }
    });

    if (cores.size() >= 2) pin_current_thread_to(cores[1]);
    std::uint64_t expected = 0;
    std::uint64_t order_errors = 0;
    std::uint64_t payload_errors = 0;
    std::uint64_t consumer_sum = 0;
    while (expected < kMessages) {
        OrderRequest r;
        if (!ring->try_pop(r)) {
            cpu_relax();
            continue;
        }
        order_errors += r.order_id != expected;
        payload_errors += (r.price != static_cast<Price>(r.order_id * 7)) || (r.qty != static_cast<Qty>(r.order_id));
        consumer_sum += r.order_id;
        ++expected;
    }
    producer.join();

    OrderRequest extra;
    PX_CHECK(order_errors == 0);
    PX_CHECK(payload_errors == 0);
    PX_CHECK(consumer_sum == producer_sum);
    PX_CHECK(!ring->try_pop(extra)); // nothing left over
    PX_CHECK(ring->size_approx() == 0);
    PX_TEST_REPORT_AND_RETURN();
}
