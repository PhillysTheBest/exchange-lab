// marketdata/session_tape.hpp — an arena session written out as a tape.
//
// The known-answer control (docs/DATA.md) needs data whose true parameters are
// known, and calibration needs to measure a simulated market exactly the way it
// measures a real one. Both come from here: a session's event stream, taken
// through Session's read-only tap, becomes an order-by-order (L3) tape, and
// marketdata/stats.hpp reads it like any imported file.
//
// The engine's protocol (px/messages.hpp) maps onto the tape this way:
//   Ack                 nothing yet: the order rests only if something is left
//                       once the request has matched, and then it is one A row,
//                       after the request's trades, as an exchange's feed shows it
//   Trade               T: the taker's side, the resting order's id, its price
//   Cancelled User      C: a resting order cancelled
//   Cancelled SelfTrade C: a resting order removed by self-trade prevention
//   Cancelled IocRemainder, Rejected: nothing, because nothing rested
//
// Several sessions go on one tape one after another, each opening with an R row
// so the book and the statistics' windows start afresh.
#pragma once

#include "arena/session.hpp"
#include "marketdata/tape.hpp"

#include <cstdint>
#include <string>

namespace px::data {

class SessionTapeWriter {
public:
    // `tick` and `price_scale` are the session's: price units a tick, and a currency unit.
    SessionTapeWriter(std::string name, std::int64_t tick, std::int64_t price_scale) {
        tape_.h.name = std::move(name);
        tape_.h.instrument = "ARENA";
        tape_.h.format = "arena";
        tape_.h.importer = "session 1";
        tape_.h.source_file = "-";
        tape_.h.source_cksum = "-";
        tape_.h.depth = Depth::L3;
        tape_.h.price_scale = price_scale;
        tape_.h.tick = tick;
        tape_.h.origin_kind = TimeKind::Relative;
    }

    // Starts writing `s`, whose clock starts at zero, at `offset_ns` on the tape.
    void attach(arena::Session& s, std::int64_t offset_ns) {
        flush();
        offset_ = offset_ns;
        if (!tape_.ev.empty()) tape_.ev.push_back(TapeEvent{offset_, 'R', '-', 0, 0, 0});
        s.set_event_tap(&SessionTapeWriter::tap, this);
    }

    // Stops writing `s`. Call once its last step has run.
    void detach(arena::Session& s) {
        flush();
        s.set_event_tap(nullptr, nullptr);
    }

    // Writes out the last request's resting order, which otherwise waits for the
    // next request. Between two steps the tape then matches the engine's book.
    void sync() { flush(); }

    [[nodiscard]] Tape& tape() noexcept { return tape_; }

    // What the tap turned into rows, for the tests: requests whose order rested, trades, cancels.
    [[nodiscard]] std::uint64_t rested() const noexcept { return rested_; }

private:
    static void tap(void* ctx, arena::SimTime at, const ExecEvent& e) {
        static_cast<SessionTapeWriter*>(ctx)->on_event(at, e);
    }

    void on_event(arena::SimTime at, const ExecEvent& e) {
        const std::int64_t t = offset_ + at;
        const char side = e.side == Side::Buy ? 'B' : 'S';
        switch (e.type) {
            case EventType::Ack:
                flush();
                pending_ = true;
                p_id_ = e.order_id;
                p_side_ = side;
                p_price_ = e.price;
                p_leaves_ = e.leaves;
                p_t_ = t;
                return;
            case EventType::Trade:
                if (pending_ && e.order_id == p_id_) p_leaves_ = e.leaves;
                tape_.ev.push_back(TapeEvent{t, 'T', side, e.maker_id, e.price / tape_.h.tick, e.qty});
                return;
            case EventType::Cancelled: {
                const auto why = static_cast<CancelReason>(e.reason);
                if (why == CancelReason::IocRemainder) {
                    if (pending_ && e.order_id == p_id_) pending_ = false;
                    return;
                }
                if (why == CancelReason::User) flush(); // a cancel is a request of its own
                tape_.ev.push_back(TapeEvent{t, 'C', side, e.order_id, e.price / tape_.h.tick, e.qty});
                return;
            }
            case EventType::Rejected:
                flush();
                return;
            case EventType::EndOfStream:
                return;
        }
    }

    // The order a request brought rests with whatever matching left of it.
    void flush() {
        if (pending_ && p_leaves_ > 0) {
            tape_.ev.push_back(TapeEvent{p_t_, 'A', p_side_, p_id_, p_price_ / tape_.h.tick, p_leaves_});
            ++rested_;
        }
        pending_ = false;
    }

    Tape tape_;
    std::int64_t offset_ = 0;
    bool pending_ = false;
    std::uint64_t p_id_ = 0;
    char p_side_ = 'B';
    std::int64_t p_price_ = 0;
    std::int64_t p_leaves_ = 0;
    std::int64_t p_t_ = 0;
    std::uint64_t rested_ = 0;
};

} // namespace px::data
