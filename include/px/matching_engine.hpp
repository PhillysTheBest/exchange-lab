// px/matching_engine.hpp — the single-threaded matching core.
//
// One MatchingEngine owns every book, order and price level for all of its
// instruments. It is not thread-safe by design: the whole point of the pipeline
// (sim/pipeline.hpp) is that exactly one thread ever calls process(), fed by a
// lock-free ring, so the core needs no locks, atomics or fences of its own.
//
// Memory: every container is sized by EngineConfig and allocated once in the
// constructor. process() performs no heap allocation, no system call and no
// unbounded loop other than the sweep itself (one iteration per maker filled).
//
// Matching rules: price-time priority. A new order first matches against the
// opposite side while it crosses (for a Market order, until it or the side is
// exhausted), filling each level's FIFO oldest-first at the resting price. Self
// trades are prevented by cancelling the resting order (cancel-resting). An IOC
// or Market remainder is cancelled; a GTC Limit remainder rests.
//
// Admission is decided BEFORE matching, so an order is never partly executed and
// then rejected: a GTC Limit needs a free order slot and, if its price is not
// already a level on its own side, a free level and room in that side's index.
// That can refuse an order which would have fully filled; the rule is
// conservative, deterministic, and mirrored exactly by the test oracle.
#pragma once

#include "flat_map.hpp"
#include "messages.hpp"
#include "order.hpp"
#include "pool.hpp"
#include "sorted_levels.hpp"
#include "types.hpp"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace px {

struct EngineConfig {
    std::uint32_t max_orders = 1u << 17;      // resting orders across all instruments
    std::uint32_t max_levels = 1u << 14;      // price levels across all instruments
    std::uint32_t levels_per_side = 2048;     // per instrument, per side
    std::uint16_t max_instruments = 64;
};

// Function pointer + context rather than std::function or a virtual interface:
// one predictable indirect call per event, no allocation, and the caller decides
// what an event means (push it onto a ring, append it to a vector, hash it).
class EventSink {
public:
    using Fn = void (*)(void* ctx, const ExecEvent&);
    constexpr EventSink(void* ctx, Fn fn) noexcept : ctx_(ctx), fn_(fn) {}
    void operator()(const ExecEvent& e) const noexcept { fn_(ctx_, e); }

private:
    void* ctx_;
    Fn fn_;
};

struct LevelView {
    Price price = 0;
    std::uint64_t qty = 0;
    std::uint32_t orders = 0;
};

class MatchingEngine {
public:
    explicit MatchingEngine(const EngineConfig& cfg = {});

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // Setup, not hot path (may allocate). Returns kInvalidInstrument if the
    // instrument table is full or the tick size is not positive.
    InstrumentId add_instrument(std::string_view symbol, Price tick_size);

    // The hot path. Emits every resulting event, in order, through `sink`.
    void process(const OrderRequest& req, EventSink sink) noexcept;

    // ---- read-only views (tests, tools, benchmarks) ----
    [[nodiscard]] std::size_t instrument_count() const noexcept { return books_.size(); }
    [[nodiscard]] std::string_view symbol(InstrumentId id) const noexcept;
    [[nodiscard]] Price tick_size(InstrumentId id) const noexcept { return books_[id].tick; }
    [[nodiscard]] std::uint32_t depth(InstrumentId id, Side side) const noexcept;
    // rank 0 == best price. Precondition: rank < depth(id, side).
    [[nodiscard]] LevelView level(InstrumentId id, Side side, std::uint32_t rank) const noexcept;
    [[nodiscard]] std::size_t resting_orders() const noexcept { return orders_.live(); }
    [[nodiscard]] bool is_live(OrderId id) const noexcept { return ids_.find(id) != kNullSlot; }
    [[nodiscard]] std::size_t live_levels() const noexcept { return levels_.live(); }
    [[nodiscard]] SeqNum next_seq() const noexcept { return next_seq_; }
    [[nodiscard]] const EngineConfig& config() const noexcept { return cfg_; }

    // Full structural check of every book (O(total orders)). Returns false and
    // stops at the first violation, whose description is left in `why`.
    [[nodiscard]] bool check_invariants(const char** why = nullptr) const;

private:
    struct Book {
        char symbol[16] = {};
        Price tick = 1;
        SortedLevels bids;
        SortedLevels asks;
        explicit Book(std::uint32_t levels_per_side) : bids(levels_per_side), asks(levels_per_side) {}
        [[nodiscard]] SortedLevels& side(Side s) noexcept { return s == Side::Buy ? bids : asks; }
        [[nodiscard]] const SortedLevels& side(Side s) const noexcept { return s == Side::Buy ? bids : asks; }
    };

    void process_new(const OrderRequest& req, EventSink sink) noexcept;
    void process_cancel(const OrderRequest& req, EventSink sink) noexcept;
    void reject(const OrderRequest& req, RejectReason why, EventSink sink) noexcept;

    // Unlinks a resting order from its level's FIFO and frees it; frees the
    // level too (and removes it from `idx`) when it becomes empty and
    // `drop_empty_level` is set.
    void remove_order(Slot os, bool drop_empty_level) noexcept;

    EngineConfig cfg_;
    Pool<Order> orders_;
    Pool<PriceLevel> levels_;
    FlatOrderMap ids_;
    std::vector<Book> books_;
    SeqNum next_seq_ = 1;
};

} // namespace px
