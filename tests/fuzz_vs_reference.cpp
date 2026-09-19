// tests/fuzz_vs_reference.cpp — differential fuzzing of MatchingEngine against
// the naive ReferenceEngine. After EVERY command the two engines must have
// emitted identical event streams (field by field) and show identical top of
// book and resting counts; whole ladders are compared periodically and the
// engine's full invariant check runs every command on some seeds.
//
// Three workload profiles, because each stresses a different part of the engine:
//   shallow  a 21-tick band: levels constantly created, emptied and re-created at
//            the touch; few owners so self-trade prevention fires; duplicate and
//            reused ids, invalid fields, foreign-owner cancels. On a tiny-capacity
//            config it also drives BookFull.
//   deep     buys spread up to 1500 ticks below the mid and sells up to 1500
//            above (5% out to 1900), so each side grows past 1000 levels; cancels
//            deep in the book and new prices behind every level exercise the level
//            index's longest shifts (the far-end erase and insert).
//   churn64  side-biased prices over 150 ticks on 64-level sides: the sides sit at
//            capacity, so level creation constantly fights BookFull.
// Coverage counters are asserted at the end so a pass cannot come from a
// workload that never reached the interesting paths.
#include "px/matching_engine.hpp"
#include "reference_engine.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

using namespace px;

namespace {

constexpr Price kTick = 100;
constexpr int kInstruments = 3;

enum class Profile { Shallow, Deep, Churn64 };

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
    std::uint64_t max_depth = 0;
    std::uint64_t deep_level_removals = 0; // cancel emptied a level in the worse half of a >=500-level side
    std::uint64_t far_end_inserts = 0;     // GTC price behind every level of a >=500-level side
};

Price random_price(std::mt19937_64& rng, Profile profile, Side side) {
    switch (profile) {
        case Profile::Shallow:
            return static_cast<Price>(90 + rng() % 21) * kTick; // 0.90 .. 1.10
        case Profile::Deep: {
            // Mostly within 1500 ticks of the mid; 5% in an outer zone out to 1900
            // ticks, where a new price is usually behind every existing level.
            constexpr Price kMidTicks = 2500;
            const auto away = rng() % 20 == 0 ? static_cast<Price>(1500 + rng() % 400) : static_cast<Price>(rng() % 1500);
            const auto through = static_cast<Price>(rng() % 20); // a little crossing near the mid
            return (side == Side::Buy ? kMidTicks - away + through : kMidTicks + away - through) * kTick;
        }
        case Profile::Churn64: {
            // Side-biased over 150 ticks: more distinct prices than a 64-level side holds.
            constexpr Price kMidTicks = 1000;
            const auto away = static_cast<Price>(rng() % 150);
            const auto through = static_cast<Price>(rng() % 5);
            return (side == Side::Buy ? kMidTicks - away + through : kMidTicks + away - through) * kTick;
        }
    }
    return kTick;
}

OrderRequest random_request(std::mt19937_64& rng, Profile profile, OrderId& next_id,
                            const std::vector<OrderId>& recent) {
    const auto roll = rng() % 100;
    const InstrumentId instr = static_cast<InstrumentId>(rng() % 100 == 0 ? kInstruments : rng() % kInstruments);
    const OwnerId owner = static_cast<OwnerId>(rng() % 6);
    const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
    if (roll < 30 && !recent.empty()) {
        // Cancel a recently used id — live, filled, cancelled or foreign-owned.
        return cancel_order(recent[rng() % recent.size()], instr, owner);
    }
    if (roll < 32) return cancel_order(1'000'000'000 + rng() % 1000, instr, owner); // never existed

    // New order. Ids are usually fresh; occasionally a recent one (duplicate if still live).
    OrderId id = next_id++;
    if (rng() % 50 == 0 && !recent.empty()) id = recent[rng() % recent.size()];
    Qty qty = static_cast<Qty>(1 + rng() % 40);
    if (rng() % 200 == 0) qty = 0;
    Price price = random_price(rng, profile, side);
    if (rng() % 200 == 0) price += kTick / 2; // off tick
    if (rng() % 400 == 0) price = -price;
    if (roll < 40) return market_order(id, instr, owner, side, qty);
    const Tif tif = roll < 55 ? Tif::IOC : Tif::GTC;
    return limit_order(id, instr, owner, side, price, qty, tif);
}

bool run_seed(std::uint64_t seed, Profile profile, int commands, const EngineConfig& cfg, int invariant_every,
              int ladder_every, Stats& stats) {
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
    std::vector<test::ReferenceEngine::Top> ref_ladder;
    const EventSink sink(&got, [](void* c, const ExecEvent& e) { static_cast<std::vector<ExecEvent>*>(c)->push_back(e); });

    for (int n = 0; n < commands; ++n) {
        const OrderRequest req = random_request(rng, profile, next_id, recent);
        if (req.type == ReqType::New) {
            if (recent.size() < 256) {
                recent.push_back(req.order_id);
            } else {
                recent[rng() % recent.size()] = req.order_id;
            }
        }

        // Coverage bookkeeping, from the reference's state BEFORE the command.
        if (req.type == ReqType::Cancel) {
            std::size_t rank = 0, depth = 0, level_orders = 0;
            if (ref.locate(req.order_id, rank, depth, level_orders) && level_orders == 1 && depth >= 500 &&
                rank >= depth / 2) {
                ++stats.deep_level_removals; // counted even if the cancel is refused (foreign owner)
            }
        } else if (req.ord == OrdType::Limit && req.tif == Tif::GTC) {
            std::size_t depth = 0;
            if (ref.is_far_end_price(req.instr, req.side, req.price, depth) && depth >= 500) ++stats.far_end_inserts;
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
                const std::uint32_t d = eng->depth(i, s);
                stats.max_depth = std::max<std::uint64_t>(stats.max_depth, d);
                if (d != ref.depth(i, s)) {
                    std::printf("DEPTH MISMATCH seed=%llu command=%d\n", static_cast<unsigned long long>(seed), n);
                    return false;
                }
                // Top of book every command; the whole ladder periodically.
                const bool whole = n % ladder_every == 0;
                const std::uint32_t ranks = whole ? d : (d > 0 ? 1u : 0u);
                if (ranks == 0) continue;
                ref.ladder(i, s, ref_ladder);
                for (std::uint32_t r = 0; r < ranks; ++r) {
                    const LevelView a = eng->level(i, s, r);
                    const auto& b = ref_ladder[r];
                    if (a.price != b.price || a.qty != b.qty || a.orders != b.orders) {
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
    const bool ok = eng->check_invariants(&why);
    if (!ok) std::printf("INVARIANT at end, seed=%llu: %s\n", static_cast<unsigned long long>(seed), why);
    return ok;
}

} // namespace

int main() {
    EngineConfig roomy; // defaults: 2048 levels per side, never full here
    EngineConfig tiny;  // small enough that BookFull fires regularly
    tiny.max_orders = 48;
    tiny.max_levels = 20;
    tiny.levels_per_side = 6;
    EngineConfig churn = roomy;
    churn.levels_per_side = 64;

    const struct Run {
        const char* name;
        std::uint64_t seed;
        Profile profile;
        int commands;
        const EngineConfig* cfg;
        int invariant_every;
        int ladder_every;
    } runs[] = {
        {"shallow", 1, Profile::Shallow, 100'000, &roomy, 1, 500},
        {"shallow", 2, Profile::Shallow, 100'000, &roomy, 64, 500},
        {"shallow/tiny", 3, Profile::Shallow, 100'000, &tiny, 1, 500},
        {"shallow/tiny", 4, Profile::Shallow, 100'000, &tiny, 64, 500},
        {"deep", 5, Profile::Deep, 60'000, &roomy, 256, 2000},
        {"deep", 6, Profile::Deep, 60'000, &roomy, 256, 2000},
        {"churn64", 7, Profile::Churn64, 100'000, &churn, 64, 500},
    };

    bool all_ok = true;
    Stats total;
    std::uint64_t churn_book_full = 0;
    for (const Run& r : runs) {
        Stats s;
        const bool ok = run_seed(r.seed, r.profile, r.commands, *r.cfg, r.invariant_every, r.ladder_every, s);
        PX_CHECK(ok);
        all_ok &= ok;
        std::printf("  %-13s seed %llu: %6d commands, %7llu events, %6llu trades, %5llu BookFull, deepest side %4llu "
                    "levels, %5llu deep level removals, %5llu far-end inserts: %s\n",
                    r.name, static_cast<unsigned long long>(r.seed), r.commands,
                    static_cast<unsigned long long>(s.events), static_cast<unsigned long long>(s.trades),
                    static_cast<unsigned long long>(s.book_full), static_cast<unsigned long long>(s.max_depth),
                    static_cast<unsigned long long>(s.deep_level_removals),
                    static_cast<unsigned long long>(s.far_end_inserts), ok ? "ok" : "MISMATCH");
        total.events += s.events;
        total.trades += s.trades;
        total.self_trades += s.self_trades;
        total.book_full += s.book_full;
        total.duplicates += s.duplicates;
        total.unknown_cancels += s.unknown_cancels;
        total.max_depth = std::max(total.max_depth, s.max_depth);
        total.deep_level_removals += s.deep_level_removals;
        total.far_end_inserts += s.far_end_inserts;
        if (r.profile == Profile::Churn64) churn_book_full += s.book_full;
    }
    std::printf("fuzz_vs_reference: %zu runs, %llu events (%llu trades, %llu self-trade cancels, %llu BookFull, "
                "%llu duplicate ids, %llu unknown cancels), deepest side %llu levels: %s\n",
                sizeof(runs) / sizeof(runs[0]), static_cast<unsigned long long>(total.events),
                static_cast<unsigned long long>(total.trades), static_cast<unsigned long long>(total.self_trades),
                static_cast<unsigned long long>(total.book_full), static_cast<unsigned long long>(total.duplicates),
                static_cast<unsigned long long>(total.unknown_cancels),
                static_cast<unsigned long long>(total.max_depth), all_ok ? "0 mismatches" : "MISMATCH");

    // The fuzz must actually reach the interesting paths, or a pass means little.
    PX_CHECK(total.trades > 10'000 && total.self_trades > 100 && total.book_full > 100 && total.duplicates > 10);
    PX_CHECK(total.max_depth >= 1000);
    PX_CHECK(churn_book_full > 1000); // the 64-level sides really were full
    PX_CHECK(total.deep_level_removals >= 1000);
    PX_CHECK(total.far_end_inserts >= 50); // each one shifts a whole 500+-level side
    PX_TEST_REPORT_AND_RETURN();
}
