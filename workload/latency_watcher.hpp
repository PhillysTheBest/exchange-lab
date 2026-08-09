// workload/latency_watcher.hpp — Stage 5, Sections 4/5/6: measuring
// per-event latency under SUSTAINED, many-events-in-flight load — the
// opposite regime from bench_ring.cpp/bench_pipeline_e2e.cpp's deliberate
// one-event-at-a-time ping-pong discipline (see those files' own header
// comments for why that discipline exists: it isolates per-op cost from
// queueing delay). Throughput/saturation/backpressure/scaling measurement
// needs many events in flight BY DESIGN — that's what "find the saturation
// knee" and "measure backpressure" actually mean — so it needs its own,
// differently-justified technique, not a reuse of the ping-pong one.
//
// TECHNIQUE: a dedicated watcher thread, pinned to its own core, busy-polls
// a monotonically-increasing completion counter (e.g.
// pipeline::Brain::book_events_applied()) and stamps read_tscp() the instant
// it observes the counter advance. The producer stamps read_tscp()
// immediately before pushing event #N, indexed by N. latency[N] =
// t_apply[N] - t_send[N].
//
// VALIDITY, stated explicitly rather than assumed silently: this compares
// TSC timestamps taken on two DIFFERENT cores (producer's core, watcher's
// core) — valid only because invariant TSC is hardware-synchronized across
// cores on a single package (confirmed on this machine by
// TscClock::invariant_tsc(); this project's own bench_ring.cpp/
// bench_pipeline_e2e.cpp never need this assumption because they only ever
// diff two timestamps taken by the SAME thread). This would need
// re-justifying on a multi-socket system — not a concern on the single-
// package laptop this project measures on, but worth stating rather than
// leaving implicit.
//
// PRECISION LIMIT, also stated rather than hidden: if the counter advances
// by more than 1 between two consecutive watcher polls (only plausible at
// extreme throughput, or if the watcher thread is preempted), this file
// cannot attribute a distinct completion timestamp to each individual
// coalesced event — it stamps all of them with the single observed
// timestamp and increments gaps(), rather than silently guessing. A
// near-zero gaps() count is this technique's own evidence that it isolated
// real per-event completion times, not an assumption free of evidence.
#pragma once

#include "px/histogram.hpp"
#include "px/platform.hpp"
#include "px/tsc_clock.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

namespace px::workload {

class LatencyWatcher {
public:
    // `capacity` should be >= the largest number of events that can be
    // in flight at once (bounded by ring capacity in practice — see
    // pipeline/events.hpp's kRingACapacity) so a send timestamp is never
    // overwritten before its matching completion is observed.
    explicit LatencyWatcher(std::size_t capacity) : send_ts_(capacity, 0), capacity_(capacity) {}

    // Producer side: call immediately before pushing event #seq (0-based,
    // strictly increasing, one call per event) onto its ring.
    void record_send(std::uint64_t seq) noexcept { send_ts_[seq % capacity_] = read_tscp(); }

    // Watcher side: call on a dedicated, pinned thread. Polls `read_counter`
    // in a tight busy-loop (cpu_relax() between polls) until it reaches
    // `target_count` or `stop` is set, recording one latency sample into
    // `hist` per counter advance observed (see PRECISION LIMIT above for
    // what happens when more than one advance happens between two polls).
    template <typename CounterFn>
    void watch(CounterFn read_counter, std::uint64_t target_count, const std::atomic<bool>& stop, TscClock& clock,
               LatencyHistogram& hist) noexcept {
        std::uint64_t last = read_counter();
        while (last < target_count && !stop.load(std::memory_order_relaxed)) {
            const std::uint64_t cur = read_counter();
            if (cur > last) {
                const Tsc t_apply = read_tscp();
                if (cur - last > 1) ++gaps_;
                for (std::uint64_t s = last; s < cur; ++s) {
                    const Tsc t_send = send_ts_[s % capacity_];
                    if (t_send != 0) hist.record(clock.to_ns(t_apply - t_send));
                }
                last = cur;
            } else {
                px::cpu_relax();
            }
        }
    }

    // Number of times the watched counter advanced by more than 1 between
    // two consecutive polls — see PRECISION LIMIT above. Report this
    // honestly alongside every latency number this class produces.
    [[nodiscard]] std::size_t gaps() const noexcept { return gaps_; }

private:
    std::vector<Tsc> send_ts_;
    std::size_t capacity_;
    std::size_t gaps_ = 0;
};

} // namespace px::workload
