#include "pipeline/brain.hpp"
#include "px/platform.hpp" // cpu_relax()

#include <chrono>

namespace px::pipeline {

namespace {
[[nodiscard]] std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

void Brain::apply_book_event(const BookEvent& e) noexcept {
    ShadowBook& book = books_[e.market];
    switch (e.kind) {
        case BookEventKind::SnapshotBegin:
            book.clear(); // a fresh LevelSet run follows — see pipeline/ingress.cpp
            break;
        case BookEventKind::LevelSet:
            book.set_level(e.side, e.price, e.qty);
            break;
        case BookEventKind::LevelDelta:
            book.apply_delta(e.side, e.price, e.qty_delta);
            break;
        case BookEventKind::Ticker:
            // Informational only here — the shadow book itself is driven by
            // orderbook_delta, not by the ticker channel. Cross-checking the
            // two against each other is exactly what tools/kalshi_shadow.cpp
            // does at the single-market debugging level; not duplicated here.
            break;
    }
    ++book_events_applied_;
}

void Brain::apply_exec_report(const ExecReport& e) noexcept {
    switch (e.kind) {
        case ExecKind::Fill:
            risk_.record_fill(e.market, e.side, e.qty);
            break;
        case ExecKind::RateLimited:
            risk_.on_rate_limited_response(now_ns());
            break;
        case ExecKind::Reject:
            break; // the order simply didn't happen — nothing to reconcile
    }
}

void Brain::evaluate_signals(const strategy::OracleEvent& e, RingB& intent_out) {
    for (auto& [market, sig] : signals_) {
        const ShadowBook& book = books_[market];
        Price bb = 0, ba = 100; // matches strategy_demo.cpp's convention for "side empty"
        if (!book.best_bid(bb)) bb = 0;
        if (!book.best_ask(ba)) ba = 100;

        OrderIntent intent;
        if (!sig->evaluate(e, bb, ba, intent)) continue;

        const risk::Verdict verdict = risk_.check(intent.market, intent.side, intent.price, intent.qty, now_ns());
        if (verdict != risk::Verdict::Accept) {
            ++intents_rejected_;
            if (on_reject) on_reject(intent, verdict);
            continue;
        }

        ++intents_accepted_;
        // An accepted intent must not be silently dropped — same backpressure
        // reasoning as ring A in pipeline/ingress.cpp, not a discard.
        while (!intent_out.try_push(intent)) cpu_relax();
    }
}

void Brain::run(RingA& book_in, strategy::OracleRing& oracle_in, RingC& exec_in, RingB& intent_out,
                 std::atomic<bool>& stop_flag) {
    while (!stop_flag.load(std::memory_order_relaxed)) {
        bool did_work = false;

        BookEvent be;
        if (book_in.try_pop(be)) {
            apply_book_event(be);
            did_work = true;
        }

        ExecReport er;
        if (exec_in.try_pop(er)) {
            apply_exec_report(er);
            did_work = true;
        }

        strategy::OracleEvent oe;
        if (oracle_in.try_pop(oe)) {
            evaluate_signals(oe, intent_out);
            did_work = true;
        }

        if (!did_work) cpu_relax();
    }
}

} // namespace px::pipeline
