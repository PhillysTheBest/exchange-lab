// venue/venue.hpp — the stages in order, around one engine.
//
//   session message -> Sequencer -> RiskGate -> journal -> MatchingEngine -> Publisher
//                                                                            |-> private reports
//                                                                            '-> public feed
//
// submit() is the whole path for one message and runs on one thread, the way
// the engine does. Nothing here is on the engine's timed path, and the engine
// is used through process() and nothing else.
//
// The journal sits after the risk gate: it records exactly what the engine was
// given, including a market order the gate turned into a collared limit. So
// recovery restores the book and the event stream exactly. What it does not
// restore is the gate's own memory (throttle counts, the kill list, the last
// trade price): after a restart those start empty, and a venue would reload
// the kill list from its own record.
#pragma once

#include "px/matching_engine.hpp"
#include "venue/feed.hpp"
#include "venue/recovery.hpp"
#include "venue/risk.hpp"
#include "venue/sequencer.hpp"

#include <cstdint>
#include <string>

namespace px::venue {

// Report::reason for an order the risk gate refused: this plus the RiskReject.
inline constexpr std::uint8_t kRiskReasonBase = 100;

struct SubmitResult {
    Admit admit = Admit::Accepted;
    RiskReject risk = RiskReject::None;
    std::uint64_t in_seq = 0;
};

class Venue {
public:
    Venue(MatchingEngine& engine, std::uint32_t sessions, const RiskLimits& limits, const std::string& journal_path,
          Publisher::FeedFn feed, Publisher::ReportFn report)
        : seq_(sessions), risk_(limits), durable_(engine, journal_path), report_(report),
          pub_(std::move(feed), std::move(report)) {}

    SubmitResult submit(std::uint32_t session, std::uint64_t client_seq, const OrderRequest& request) {
        OrderRequest req = request; // the gate may rewrite it
        SubmitResult r;
        r.admit = seq_.admit(session, client_seq, r.in_seq);
        if (r.admit != Admit::Accepted) return r;
        r.risk = risk_.check(r.in_seq, req, durable_.resting());
        if (r.risk != RiskReject::None) {
            if (report_) {
                Report rep;
                rep.order_id = req.order_id;
                rep.owner = req.owner;
                rep.type = ReportType::Rejected;
                rep.reason = static_cast<std::uint8_t>(kRiskReasonBase + static_cast<std::uint8_t>(r.risk));
                report_(rep); // refused before the engine: outside the numbered report stream
            }
            return r;
        }
        match(req);
        return r;
    }

    // The operator's kill switch: blocks the owner and pulls its resting orders.
    std::size_t kill(OwnerId owner) {
        const auto cancels = risk_.kill(owner, durable_.resting());
        for (const OrderRequest& c : cancels) match(c);
        return cancels.size();
    }

    // Cancel on disconnect: pulls an owner's resting orders without blocking it.
    std::size_t pull_orders(OwnerId owner) {
        const auto resting = durable_.resting().of_owner(owner);
        for (const RestingOrder& o : resting) match(cancel_order(o.id, o.instr, o.owner));
        return resting.size();
    }

    [[nodiscard]] Sequencer& sequencer() noexcept { return seq_; }
    [[nodiscard]] RiskGate& risk() noexcept { return risk_; }
    [[nodiscard]] DurableEngine& durable() noexcept { return durable_; }
    [[nodiscard]] Publisher& publisher() noexcept { return pub_; }

private:
    void match(const OrderRequest& req) {
        pub_.begin(req);
        durable_.process(req, EventSink(this, [](void* c, const ExecEvent& e) {
            auto* v = static_cast<Venue*>(c);
            v->risk_.on_event(e);
            v->pub_.on_event(e);
        }));
        pub_.end();
    }

    Sequencer seq_;
    RiskGate risk_;
    DurableEngine durable_;
    Publisher::ReportFn report_;
    Publisher pub_;
};

} // namespace px::venue
