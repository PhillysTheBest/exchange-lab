// venue/recovery.hpp — an engine that can be brought back.
//
// DurableEngine stands in front of a MatchingEngine it does not own. Every
// request is numbered, written to the journal and only then processed, and the
// engine's events pass through on their way to the caller so the resting orders
// are always known (venue/book_state.hpp).
//
// Recovery is two steps. restore() rebuilds the book from a snapshot in a fresh
// engine; replay() applies every journal record after the snapshot. Because the
// engine is deterministic, what comes out is the event stream the lost engine
// would have produced, with the same sequence numbers: tests/test_venue.cpp cuts
// a run at an arbitrary request, recovers, finishes the run and compares the hash
// of the whole event stream with an uninterrupted run's.
//
// A standby is the same thing running continuously: a second engine that follows
// the journal with follow(), and is promoted by simply giving it requests.
//
// The engine gains no API from this, so no engine hash can move.
#pragma once

#include "px/matching_engine.hpp"
#include "venue/book_state.hpp"
#include "venue/journal.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace px::venue {

class DurableEngine {
public:
    // `journal_path` empty: nothing is written (a standby that only follows).
    DurableEngine(MatchingEngine& engine, const std::string& journal_path) : engine_(engine) {
        if (!journal_path.empty()) journal_ = std::make_unique<JournalWriter>(journal_path);
    }

    // Journal first, then match. Returns false, and does not match, if the
    // journal write failed: an order that was not recorded must not trade.
    bool process(const OrderRequest& req, EventSink sink) {
        const std::uint64_t n = in_seq_ + 1;
        if (journal_ && !journal_->append(n, req)) return false;
        apply(n, req, sink);
        return true;
    }

    // Applies a record that is already in the journal (replay, or a standby).
    // Records at or before the last one applied are skipped, so replaying an
    // overlap is harmless. Returns false on a gap: a record was lost.
    bool apply_recorded(const JournalRecord& rec, EventSink sink) {
        if (rec.in_seq <= in_seq_) return true;
        if (rec.in_seq != in_seq_ + 1) return false;
        apply(rec.in_seq, rec.req, sink);
        return true;
    }

    // Reads whatever the journal has gained and applies it. Returns the number
    // of records applied, or -1 on a gap or an unreadable journal.
    long follow(JournalReader& reader, EventSink sink) {
        std::vector<JournalRecord> recs;
        if (!reader.read_more(recs)) return -1;
        long applied = 0;
        for (const JournalRecord& r : recs) {
            if (r.in_seq <= in_seq_) continue;
            if (!apply_recorded(r, sink)) return -1;
            ++applied;
        }
        return applied;
    }

    [[nodiscard]] Snapshot snapshot() const {
        Snapshot s;
        s.in_seq = in_seq_;
        s.next_event_seq = engine_.next_seq() + seq_offset_;
        s.orders = book_.in_rest_order();
        return s;
    }

    // Rebuilds the book in a fresh engine (instruments already added, nothing
    // processed). False if the engine is not fresh or an order does not rest.
    [[nodiscard]] bool restore(const Snapshot& s) {
        if (in_seq_ != 0 || engine_.next_seq() != 1 || engine_.resting_orders() != 0) return false;
        bool ok = true;
        for (const RestingOrder& o : s.orders) {
            const OrderRequest req = limit_order(o.id, o.instr, o.owner, o.side, o.price, o.leaves);
            Rebuild ctx{this, &ok};
            book_.begin(req);
            engine_.process(req, EventSink(&ctx, [](void* c, const ExecEvent& e) {
                auto* r = static_cast<Rebuild*>(c);
                if (e.type != EventType::Ack) *r->ok = false; // anything but a plain rest is a bad snapshot
                r->self->book_.on_event(e);
            }));
            book_.end();
        }
        if (!ok || engine_.resting_orders() != s.orders.size() || s.next_event_seq < engine_.next_seq()) return false;
        seq_offset_ = s.next_event_seq - engine_.next_seq();
        in_seq_ = s.in_seq;
        return true;
    }

    [[nodiscard]] std::uint64_t in_seq() const noexcept { return in_seq_; }
    [[nodiscard]] const RestingBook& resting() const noexcept { return book_; }
    [[nodiscard]] MatchingEngine& engine() noexcept { return engine_; }

private:
    struct Rebuild {
        DurableEngine* self;
        bool* ok;
    };
    struct Pass {
        DurableEngine* self;
        EventSink out;
    };
    void apply(std::uint64_t n, const OrderRequest& req, EventSink sink) {
        Pass ctx{this, sink};
        book_.begin(req);
        engine_.process(req, EventSink(&ctx, [](void* c, const ExecEvent& e) {
            auto* p = static_cast<Pass*>(c);
            p->self->book_.on_event(e);
            ExecEvent out = e;
            out.seq += p->self->seq_offset_;
            p->out(out);
        }));
        book_.end();
        in_seq_ = n;
    }

    MatchingEngine& engine_;
    std::unique_ptr<JournalWriter> journal_;
    RestingBook book_;
    std::uint64_t in_seq_ = 0;
    SeqNum seq_offset_ = 0;
};

} // namespace px::venue
