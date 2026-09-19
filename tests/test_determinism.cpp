// tests/test_determinism.cpp — the same input must produce the same output,
// bit for bit, however it is executed. A seeded stream is run (1) directly on
// one thread, (2) through the three-thread ring pipeline, and (3) through the
// pipeline again. All three event-stream hashes and counts, and the final book
// state, must be identical; a different seed must produce a different hash.
#include "px/matching_engine.hpp"
#include "sim/order_stream.hpp"
#include "sim/pipeline.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <memory>

using namespace px;

namespace {

std::unique_ptr<MatchingEngine> fresh(const sim::StreamConfig& cfg) {
    auto e = std::make_unique<MatchingEngine>();
    sim::add_instruments(*e, cfg);
    return e;
}

bool same_books(const MatchingEngine& a, const MatchingEngine& b) {
    if (a.resting_orders() != b.resting_orders() || a.live_levels() != b.live_levels()) return false;
    for (InstrumentId i = 0; i < a.instrument_count(); ++i) {
        for (const Side s : {Side::Buy, Side::Sell}) {
            if (a.depth(i, s) != b.depth(i, s)) return false;
            for (std::uint32_t r = 0; r < a.depth(i, s); ++r) {
                const LevelView x = a.level(i, s, r);
                const LevelView y = b.level(i, s, r);
                if (x.price != y.price || x.qty != y.qty || x.orders != y.orders) return false;
            }
        }
    }
    return true;
}

} // namespace

int main() {
    sim::StreamConfig cfg;
    cfg.orders = 300'000;
    cfg.seed = 7;
    const auto stream = sim::generate_order_stream(cfg);

    auto direct_engine = fresh(cfg);
    const EventHasher direct = sim::run_direct(*direct_engine, stream);

    auto pipe_engine_1 = fresh(cfg);
    const auto p1 = sim::run_pipeline(*pipe_engine_1, stream);
    auto pipe_engine_2 = fresh(cfg);
    const auto p2 = sim::run_pipeline(*pipe_engine_2, stream);

    std::printf("direct    hash=%016llx events=%llu\n", static_cast<unsigned long long>(direct.value()),
                static_cast<unsigned long long>(direct.count()));
    std::printf("pipeline  hash=%016llx events=%llu\n", static_cast<unsigned long long>(p1.event_hash),
                static_cast<unsigned long long>(p1.events));
    std::printf("pipeline  hash=%016llx events=%llu\n", static_cast<unsigned long long>(p2.event_hash),
                static_cast<unsigned long long>(p2.events));

    PX_CHECK(direct.count() > stream.size());
    PX_CHECK(p1.event_hash == direct.value() && p1.events == direct.count());
    PX_CHECK(p2.event_hash == direct.value() && p2.events == direct.count());
    PX_CHECK(same_books(*direct_engine, *pipe_engine_1));
    PX_CHECK(same_books(*direct_engine, *pipe_engine_2));
    PX_CHECK(direct_engine->check_invariants());

    // Regenerating from the same seed gives the same stream; a new seed does not.
    const auto again = sim::generate_order_stream(cfg);
    auto again_engine = fresh(cfg);
    PX_CHECK(sim::run_direct(*again_engine, again).value() == direct.value());

    sim::StreamConfig other = cfg;
    other.seed = 8;
    const auto other_stream = sim::generate_order_stream(other);
    auto other_engine = fresh(other);
    PX_CHECK(sim::run_direct(*other_engine, other_stream).value() != direct.value());

    PX_TEST_REPORT_AND_RETURN();
}
