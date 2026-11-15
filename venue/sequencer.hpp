// venue/sequencer.hpp — many senders, one order of events.
//
// The engine takes requests from one caller, so by itself it never has to say
// which of two orders came first. A venue with many connections does. The
// sequencer is the single place where that is decided: it takes messages from
// every session, puts them in one line, and gives each a number. Everything
// downstream (risk, the journal, the engine, a standby) sees that one numbered
// stream and nothing else, which is why they can all agree.
//
// Two rules are enforced here.
//
//   Each session numbers its own messages from 1. A message that repeats a
//   number already taken is a resend and is dropped, so a client that retries
//   after a timeout cannot enter the same order twice. A message that skips a
//   number is refused, so the client knows to resend what was lost.
//
//   Fairness between sessions is round-robin: each pass takes at most one
//   message from each session's ring, and the session a pass starts with moves
//   on by one each time. No session can starve another by sending faster. This
//   is a stated rule, not a claim about arrival time: two messages in different
//   rings have no order until the sequencer gives them one.
//
// MergeSequencer is the threaded form: one lock-free ring per session (the
// engine's own SpscRing), one sequencer thread polling them.
#pragma once

#include "px/messages.hpp"
#include "px/spsc_ring.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace px::venue {

struct Inbound {
    OrderRequest req;
    std::uint64_t client_seq = 0; // the session's own message number, from 1
    std::uint32_t session = 0;
};
static_assert(std::is_trivially_copyable_v<Inbound>);

enum class Admit : std::uint8_t { Accepted, Duplicate, Gap, UnknownSession };

class Sequencer {
public:
    explicit Sequencer(std::uint32_t sessions) : last_(sessions, 0) {}

    // On Accepted, `in_seq` is the message's place in the venue's one stream.
    [[nodiscard]] Admit admit(std::uint32_t session, std::uint64_t client_seq, std::uint64_t& in_seq) noexcept {
        if (session >= last_.size()) return Admit::UnknownSession;
        if (client_seq <= last_[session]) return Admit::Duplicate;
        if (client_seq != last_[session] + 1) return Admit::Gap;
        last_[session] = client_seq;
        in_seq = ++in_seq_;
        return Admit::Accepted;
    }
    // The next number `session` must send: what a client is told when it reconnects.
    [[nodiscard]] std::uint64_t expected(std::uint32_t session) const noexcept { return last_[session] + 1; }
    [[nodiscard]] std::uint64_t in_seq() const noexcept { return in_seq_; }
    [[nodiscard]] std::uint32_t sessions() const noexcept { return static_cast<std::uint32_t>(last_.size()); }

private:
    std::vector<std::uint64_t> last_;
    std::uint64_t in_seq_ = 0;
};

template <std::size_t Capacity = std::size_t{1} << 12>
class MergeSequencer {
public:
    using Ring = SpscRing<Inbound, Capacity>;
    explicit MergeSequencer(std::uint32_t sessions) : seq_(sessions) {
        for (std::uint32_t i = 0; i < sessions; ++i) rings_.push_back(std::make_unique<Ring>());
    }
    // The producer side of session `i`: exactly one thread may push to it.
    [[nodiscard]] Ring& ring(std::uint32_t i) noexcept { return *rings_[i]; }

    // One round-robin pass. Calls `out(in_seq, inbound)` for each message
    // accepted; returns how many messages were taken off the rings.
    template <typename Out>
    std::size_t poll(Out&& out) {
        const std::uint32_t n = seq_.sessions();
        std::size_t taken = 0;
        for (std::uint32_t k = 0; k < n; ++k) {
            const std::uint32_t s = (start_ + k) % n;
            Inbound in;
            if (!rings_[s]->try_pop(in)) continue;
            ++taken;
            std::uint64_t num = 0;
            const Admit a = seq_.admit(s, in.client_seq, num); // the ring a message came in on is its session
            if (a == Admit::Accepted) {
                in.session = s;
                out(num, in);
            } else if (a == Admit::Duplicate) {
                ++duplicates_;
            } else {
                ++gaps_;
            }
        }
        start_ = (start_ + 1) % n;
        return taken;
    }
    [[nodiscard]] const Sequencer& sequencer() const noexcept { return seq_; }
    [[nodiscard]] std::uint64_t duplicates() const noexcept { return duplicates_; }
    [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

private:
    Sequencer seq_;
    std::vector<std::unique_ptr<Ring>> rings_;
    std::uint32_t start_ = 0;
    std::uint64_t duplicates_ = 0, gaps_ = 0;
};

} // namespace px::venue
