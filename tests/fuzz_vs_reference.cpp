// tests/fuzz_vs_reference.cpp — differential fuzzing of MatchingEngine against
// the naive ReferenceEngine. Seeded random command streams over a narrow price
// band (so levels are constantly created, emptied and re-created), several
// instruments, few owners (so self-trade prevention fires), duplicate and
// reused ids, invalid fields, foreign-owner cancels, and — on the small-capacity
// seeds — BookFull. After EVERY command the two engines must have emitted
// identical event streams (field by field) and show identical top-of-book and
// resting counts; the engine's full invariant check runs on every command of the
// first seed and periodically on the rest.
#include "px/matching_engine.hpp"
#include "reference_engine.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <memory>
#include <random>
#include <vector>

using namespace px;

namespace {

constexpr Price kTick = 100;
constexpr int kInstruments = 3;

bool same(const ExecEvent& a, const ExecEvent& b) {
    return a.seq == b.seq && a.order_id == b.order_id && a.maker_id == b.maker_id && a.price == b.price &&
           a.qty == b.qty && a.leaves == b.leaves && a.maker_leaves == b.maker_leaves && a.owner == b.owner &&
           a.maker_owner == b.maker_owner && a.instr == b.instr && a.type == b.type && a.reason == b.reason &&
           a.side == b.side;
}

void print_event(const char* who, const ExecEvent& e) {
    std::printf("    %s seq=%llu type=%d reason=%d id=%llu maker=%llu px=%lld qty=%u leaves=%u mleaves=%u owner=%u "
                "mowner=%u instr=%u side=%d\n",
                who, static_cast<unsigned long long>(e.seq), static_cast<int>(e.type), e.reason,
                static_cast<unsigned long long>(e.order_id), static_cast<unsigned long long>(e.maker_id),
                static_cast<long long>(e.price), e.qty, e.leaves, e.maker_leaves, e.owner, e.maker_owner, e.instr,
                static_cast<int>(e.side));
}

struct Stats {
    std::uint64_t events = 0, trades = 0, self_trades = 0, book_full = 0, duplicates = 0, unknown_cancels = 0;
};

OrderRequest random_request(std::mt19937_64& rng, OrderId& next_id, const std::vector<OrderId>& recent) {
    const auto roll = rng() % 100;
    const InstrumentId instr = static_cast<InstrumentId>(rng() % 100 == 0 ? kInstruments : rng() % kInstruments);
    const OwnerId owner = static_cast<OwnerId>(rng() % 6);
    const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
    if (roll < 30 && !recent.empty()) {
        // Cancel a recently used id — live, filled, cancelled or foreign-owned.
        return cancel_order(recent[rng() % recent.size()], instr, owner);
    }
    if (roll < 32) return cancel_order(1'000'000 + rng() % 1000, instr, owner); // never existed

    // New order. Ids are usually fresh; occasionally a recent one (duplicate if still live).
    OrderId id = next_id++;
    if (rng() % 50 == 0 && !recent.empty()) id = recent[rng() % recent.size()];
    Qty qty = static_cast<Qty>(1 + rng() % 40);
    if (rng() % 200 == 0) qty = 0;
    Price price = static_cast<Price>(90 + rng() % 21) * kTick; // 21-tick band around 1.00
    if (rng() % 200 == 0) price += kTick / 2;                    // off tick
    if (rng() % 400 == 0) price = -price;
    if (roll < 40) return market_order(id, instr, owner, side, qty);
    const Tif tif = roll < 55 ? Tif::IOC : Tif::GTC;
    return limit_order(id, instr, owner, side, price, qty, tif);
}

bool run_seed(std::uint64_t seed, int commands, const EngineConfig& cfg, int invariant_every, Stats& stats) {
    auto eng = std::make_unique<MatchingEngine>(cfg);
    test::ReferenceEngine ref(cfg);
    for (int i = 0; i < kInstruments; ++i) {
        char sym[8];
        std::snprintf(sym, sizeof sym, "F%d", i);
        eng->add_instrument(sym, kTick);
        ref.add_instrument(kTick);
    }

    std::mt19937_64 rng(seed);
    std::vector<OrderId> recent;
    OrderId next_id = 1;
    std::vector<ExecEvent> got;
    const EventSink sink(&got, [](void* c, const ExecEvent& e) { static_cast<std::vector<ExecEvent>*>(c)->push_back(e); });

    for (int n = 0; n < commands; ++n) {
        const OrderRequest req = random_request(rng, next_id, recent);
        if (req.type == ReqType::New) {
            if (recent.size() < 256) {
                recent.push_back(req.order_id);
            } else {
                recent[rng() % recent.size()] = req.order_id;
            }
        }

        got.clear();
        eng->process(req, sink);
        const std::vector<ExecEvent> want = ref.process(req);

        bool ok = got.size() == want.size();
        for (std::size_t i = 0; ok && i < got.size(); ++i) ok = same(got[i], want[i]);
        if (!ok) {
            std::printf("MISMATCH seed=%llu command=%d type=%d id=%llu\n", static_cast<unsigned long long>(seed), n,
                        static_cast<int>(req.type), static_cast<unsigned long long>(req.order_id));
            for (const auto& e : got) print_event("engine", e);
            for (const auto& e : want) print_event("ref   ", e);
            return false;
        }

        for (InstrumentId i = 0; i < kInstruments; ++i) {
            for (const Side s : {Side::Buy, Side::Sell}) {
                const auto d = eng->depth(i, s);
                if (d != ref.depth(i, s)) {
                    std::printf("DEPTH MISMATCH seed=%llu command=%d\n", static_cast<unsigned long long>(seed), n);
                    return false;
                }
                // Top of book every command; the whole ladder every 500 commands.
                const std::uint32_t ranks = (n % 500 == 0) ? d : (d > 0 ? 1u : 0u);
                for (std::uint32_t r = 0; r < ranks; ++r) {
                    const LevelView a = eng->level(i, s, r);
                    const auto b = ref.level(i, s, r);
                    if (!b.present || a.price != b.price || a.qty != b.qty || a.orders != b.orders) {
                        std::printf("LEVEL MISMATCH seed=%llu command=%d instr=%u side=%d rank=%u\n",
                                    static_cast<unsigned long long>(seed), n, i, static_cast<int>(s), r);
                        return false;
                    }
                }
            }
        }
        if (eng->resting_orders() != ref.resting_orders()) {
            std::printf("RESTING MISMATCH seed=%llu command=%d\n", static_cast<unsigned long long>(seed), n);
            return false;
        }
        if (invariant_every > 0 && n % invariant_every == 0) {
            const char* why = nullptr;
            if (!eng->check_invariants(&why)) {
                std::printf("INVARIANT seed=%llu command=%d: %s\n", static_cast<unsigned long long>(seed), n, why);
                return false;
            }
        }

        for (const auto& e : got) {
            ++stats.events;
            stats.trades += e.type == EventType::Trade;
            stats.self_trades += e.type == EventType::Cancelled && e.reason == static_cast<std::uint8_t>(CancelReason::SelfTrade);
            stats.book_full += e.type == EventType::Rejected && e.reason == static_cast<std::uint8_t>(RejectReason::BookFull);
            stats.duplicates += e.type == EventType::Rejected && e.reason == static_cast<std::uint8_t>(RejectReason::DuplicateOrderId);
            stats.unknown_cancels += e.type == EventType::Rejected && e.reason == static_cast<std::uint8_t>(RejectReason::UnknownOrder);
        }
    }
    const char* why = nullptr;
    return eng->check_invariants(&why);
}

} // namespace

int main() {
    constexpr int kCommands = 100'000;
    EngineConfig roomy;                 // defaults: never full
    EngineConfig tiny;                  // small enough that BookFull fires regularly
    tiny.max_orders = 48;
    tiny.max_levels = 20;
    tiny.levels_per_side = 6;

    Stats stats;
    int seeds = 0;
    bool all_ok = true;
    const struct {
        std::uint64_t seed;
        const EngineConfig* cfg;
        int invariant_every;
    } runs[] = {{1, &roomy, 1}, {2, &roomy, 64}, {3, &tiny, 1}, {4, &tiny, 64}};
    for (const auto& r : runs) {
        const bool ok = run_seed(r.seed, kCommands, *r.cfg, r.invariant_every, stats);
        PX_CHECK(ok);
        all_ok &= ok;
        ++seeds;
    }
    std::printf("fuzz_vs_reference: %d seeds x %d commands, %llu events (%llu trades, %llu self-trade cancels, "
                "%llu BookFull, %llu duplicate ids, %llu unknown cancels): %s\n",
                seeds, kCommands, static_cast<unsigned long long>(stats.events),
                static_cast<unsigned long long>(stats.trades), static_cast<unsigned long long>(stats.self_trades),
                static_cast<unsigned long long>(stats.book_full), static_cast<unsigned long long>(stats.duplicates),
                static_cast<unsigned long long>(stats.unknown_cancels), all_ok ? "0 mismatches" : "MISMATCH");
    // The fuzz must actually reach the interesting paths, or a pass means little.
    PX_CHECK(stats.trades > 10'000 && stats.self_trades > 100 && stats.book_full > 100 && stats.duplicates > 10);
    PX_TEST_REPORT_AND_RETURN();
}
