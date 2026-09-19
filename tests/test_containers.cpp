// tests/test_containers.cpp — Pool, FlatOrderMap and SortedLevels against
// obviously-correct standard-library models, under long random operation mixes.
#include "px/flat_map.hpp"
#include "px/order.hpp"
#include "px/pool.hpp"
#include "px/sorted_levels.hpp"
#include "test_harness.hpp"

#include <map>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

using namespace px;

namespace {

void test_pool() {
    Pool<Order> pool(4);
    PX_CHECK(pool.capacity() == 4 && pool.live() == 0 && !pool.full());
    std::set<Slot> got;
    for (int i = 0; i < 4; ++i) got.insert(pool.acquire());
    PX_CHECK(got.size() == 4);
    PX_CHECK(pool.full() && pool.acquire() == kNullSlot); // exhaustion is reported, not UB
    const Slot s = *got.begin();
    pool.release(s);
    PX_CHECK(!pool.full() && pool.live() == 3);
    PX_CHECK(pool.acquire() == s); // LIFO reuse: the most recently freed slot is hot in cache
    PX_CHECK(pool.full());
}

void test_flat_map() {
    FlatOrderMap map(1u << 12); // 4096 buckets; keep <= ~2000 live entries
    std::unordered_map<OrderId, Slot> model;
    std::vector<OrderId> live;
    std::mt19937_64 rng(7);
    std::size_t mismatches = 0;
    for (int i = 0; i < 2'000'000; ++i) {
        const auto op = rng() % 10;
        if (op < 5 && model.size() < 2000) {
            const OrderId k = rng() % 5000; // small key space: forces duplicates and long clusters
            const Slot v = static_cast<Slot>(rng());
            const bool ins = map.insert(k, v);
            const bool model_ins = model.emplace(k, v).second;
            if (ins != model_ins) ++mismatches;
            if (model_ins) live.push_back(k);
        } else if (op < 8 && !live.empty()) {
            const std::size_t j = rng() % live.size();
            const OrderId k = live[j];
            live[j] = live.back();
            live.pop_back();
            if (!map.erase(k)) ++mismatches;
            model.erase(k);
        } else {
            const OrderId k = rng() % 5000;
            const auto it = model.find(k);
            const Slot expect = it == model.end() ? kNullSlot : it->second;
            if (map.find(k) != expect) ++mismatches;
        }
        if (map.size() != model.size()) ++mismatches;
    }
    for (const auto& [k, v] : model) {
        if (map.find(k) != v) ++mismatches;
    }
    PX_CHECK(mismatches == 0);
    PX_CHECK(!map.insert(kInvalidOrderId, 1)); // reserved marker is refused
    PX_CHECK(map.find(kInvalidOrderId) == kNullSlot);
}

void test_sorted_levels_side(Side side) {
    constexpr std::uint32_t kCap = 64;
    SortedLevels idx(kCap);
    std::map<std::int64_t, Slot> model; // ascending by key, like idx
    std::mt19937_64 rng(side == Side::Buy ? 11 : 13);
    std::size_t mismatches = 0;
    Slot next_slot = 0;
    for (int i = 0; i < 200'000; ++i) {
        const Price price = static_cast<Price>(1 + rng() % 100) * 100;
        const std::int64_t key = SortedLevels::key_of(side, price);
        const auto pos = idx.locate(key);
        const bool in_model = model.count(key) != 0;
        if (pos.found != in_model) ++mismatches;
        if (rng() % 2 == 0) {
            if (!pos.found && !idx.full()) {
                idx.insert_at(pos.index, key, next_slot);
                model[key] = next_slot++;
            }
        } else if (pos.found) {
            if (rng() % 3 == 0 && pos.index == idx.size() - 1) {
                idx.pop_best();
            } else {
                idx.erase_at(pos.index);
            }
            model.erase(key);
        }
        if (idx.size() != model.size()) ++mismatches;
        // Strictly ascending keys, and the best (largest key) at rank 0.
        for (std::uint32_t j = 1; j < idx.size(); ++j) {
            if (!(idx.key_at_index(j - 1) < idx.key_at_index(j))) ++mismatches;
        }
        if (!model.empty()) {
            if (idx.key_at_rank(0) != model.rbegin()->first) ++mismatches;
            if (idx.best() != model.rbegin()->second) ++mismatches;
            // Rank 0 must be the best PRICE for this side.
            const Price best_price = side == Side::Buy ? idx.key_at_rank(0) : -idx.key_at_rank(0);
            const Price model_best = side == Side::Buy ? model.rbegin()->first : -model.rbegin()->first;
            if (best_price != model_best) ++mismatches;
        }
    }
    PX_CHECK(mismatches == 0);

    // Fill to capacity exactly.
    SortedLevels full(4);
    for (Price p = 1; p <= 4; ++p) {
        const auto k = SortedLevels::key_of(side, p * 100);
        const auto pos = full.locate(k);
        full.insert_at(pos.index, k, static_cast<Slot>(p));
    }
    PX_CHECK(full.full() && full.size() == 4);
    const Price expect_best = side == Side::Buy ? 400 : 100;
    PX_CHECK(full.best() == static_cast<Slot>(expect_best / 100));
}

} // namespace

int main() {
    test_pool();
    test_flat_map();
    test_sorted_levels_side(Side::Buy);
    test_sorted_levels_side(Side::Sell);
    PX_TEST_REPORT_AND_RETURN();
}
