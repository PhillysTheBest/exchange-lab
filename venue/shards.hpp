// venue/shards.hpp — more than one matcher: instruments split across engines.
//
// One engine on one thread is the right unit: it needs no locks because nothing
// is shared. To use more cores a venue does not make that engine concurrent. It
// runs several of them, gives each a fixed set of instruments, and puts a
// router in front that sends every request to the engine that owns its
// instrument. Orders in different instruments never interact, so nothing has to
// be coordinated between shards.
//
//   [router] --ring--> [matcher 0: instruments 0, K, 2K, ...]
//            --ring--> [matcher 1: instruments 1, K+1, ...]
//            ...
//
// What this buys, beyond cores: each shard has its own pool of orders and price
// levels, so one busy instrument can no longer use up the room another needs.
//
// What stays true: each shard's event stream is a function of the requests
// routed to it, in order. Thread timing cannot change it. tests/test_venue.cpp
// checks every shard's hash against a single-threaded run of that shard's
// requests, and every instrument's final book against the unsharded engine's.
//
// What changes: event sequence numbers are per shard, so there is no longer one
// global order of events across instruments. A real venue has the same
// property and publishes one feed channel per shard for that reason. Order ids
// must also be unique across the venue, because a duplicate id in another shard
// is no longer seen.
//
// Every shard's engine is given the full instrument table so that instrument
// ids mean the same thing everywhere; a shard simply never receives the
// instruments it does not own.
#pragma once

#include "px/matching_engine.hpp"
#include "px/platform.hpp"
#include "px/spsc_ring.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace px::venue {

class ShardedEngines {
public:
    using Ring = SpscRing<OrderRequest, std::size_t{1} << 14>;

    // `setup` adds the instruments to one engine; it is called once per shard.
    ShardedEngines(std::uint32_t shards, const EngineConfig& cfg, const std::function<void(MatchingEngine&)>& setup) {
        for (std::uint32_t i = 0; i < shards; ++i) {
            auto s = std::make_unique<Shard>();
            s->engine = std::make_unique<MatchingEngine>(cfg);
            setup(*s->engine);
            s->ring = std::make_unique<Ring>();
            shards_.push_back(std::move(s));
        }
        for (auto& s : shards_) {
            Shard* sh = s.get();
            sh->thread = std::thread([sh] {
                const EventSink sink(&sh->hash, [](void* c, const ExecEvent& e) { static_cast<EventHasher*>(c)->add(e); });
                for (;;) {
                    OrderRequest req;
                    if (sh->ring->try_pop(req)) {
                        sh->engine->process(req, sink);
                        ++sh->processed;
                    } else if (sh->stop.load(std::memory_order_acquire)) {
                        if (!sh->ring->try_pop(req)) break; // stop was set after the last push: one more look
                        sh->engine->process(req, sink);
                        ++sh->processed;
                    } else {
                        cpu_relax();
                    }
                }
            });
        }
    }
    ShardedEngines(const ShardedEngines&) = delete;
    ShardedEngines& operator=(const ShardedEngines&) = delete;
    ~ShardedEngines() { finish(); }

    [[nodiscard]] std::uint32_t shard_of(InstrumentId instr) const noexcept {
        return static_cast<std::uint32_t>(instr % shards_.size());
    }

    // The router. One thread may call this.
    void route(const OrderRequest& req) noexcept {
        Ring& r = *shards_[shard_of(req.instr)]->ring;
        while (!r.try_push(req)) cpu_relax();
    }

    // Lets every shard drain and stops its thread. After this the engines and
    // hashes may be read.
    void finish() {
        for (auto& s : shards_) s->stop.store(true, std::memory_order_release);
        for (auto& s : shards_)
            if (s->thread.joinable()) s->thread.join();
    }

    [[nodiscard]] std::uint32_t shards() const noexcept { return static_cast<std::uint32_t>(shards_.size()); }
    [[nodiscard]] const MatchingEngine& engine(std::uint32_t shard) const noexcept { return *shards_[shard]->engine; }
    [[nodiscard]] std::uint64_t event_hash(std::uint32_t shard) const noexcept { return shards_[shard]->hash.value(); }
    [[nodiscard]] std::uint64_t events(std::uint32_t shard) const noexcept { return shards_[shard]->hash.count(); }
    [[nodiscard]] std::uint64_t processed(std::uint32_t shard) const noexcept { return shards_[shard]->processed; }

private:
    struct Shard {
        std::unique_ptr<MatchingEngine> engine;
        std::unique_ptr<Ring> ring;
        EventHasher hash;
        std::uint64_t processed = 0;
        std::atomic<bool> stop{false};
        std::thread thread;
    };
    std::vector<std::unique_ptr<Shard>> shards_;
};

} // namespace px::venue
