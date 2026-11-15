// venue/risk.hpp — the checks an order passes before it may reach the book.
//
// The engine checks that an order is well formed. It does not ask whether the
// order is sane: a size a hundred times too large, a price far from the market,
// a member sending in a loop. A venue asks those questions first, per member,
// in a gate ahead of the engine, and can shut one member off by hand.
//
//   size and value     one order may not exceed a quantity or a notional
//   price collar       a limit price may not be further than `collar` from the
//                      last trade; a market order becomes an IOC limit at the
//                      collar, so it cannot sweep the book to any price
//   open orders        a cap on how many orders one owner may have resting
//   throttle           a cap on one owner's messages per window. The window is
//                      counted in venue messages, never in wall-clock time, so
//                      a replay of the journal makes the same decisions
//   kill switch        blocks an owner and hands back a cancel for every order
//                      the owner has resting
//
// Cancels are never refused: a member must always be able to get out. An amend
// is checked like a new limit order at its new price and size, except against
// the open-order cap, since it adds no order.
//
// Everything here is integer and deterministic. The gate is given the inbound
// sequence number and the list of resting orders; it reads no clock.
#pragma once

#include "px/messages.hpp"
#include "venue/book_state.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace px::venue {

enum class RiskReject : std::uint8_t {
    None = 0,
    OrderTooLarge,
    ValueTooLarge,
    PriceOutsideCollar,
    TooManyOpenOrders,
    Throttled,
    Killed,
    VenueOnly, // an auction request from a session: only the venue opens or uncrosses one
};
inline constexpr const char* kRiskRejectNames[] = {"none",          "order too large",       "value too large",
                                                   "price outside collar", "too many open orders", "throttled",
                                                   "killed"};

struct RiskLimits {
    Qty max_order_qty = 0;               // 0 = no limit, here and below
    std::int64_t max_order_value = 0;    // price * qty, in price units
    Price collar = 0;                    // distance from the last trade, in price units
    std::uint32_t max_open_orders = 0;
    std::uint32_t max_messages = 0;      // per owner, per window
    std::uint64_t throttle_window = 1000; // venue messages per window
};

class RiskGate {
public:
    explicit RiskGate(const RiskLimits& limits) : lim_(limits) {}

    // May rewrite `req` (a market order under a collar). `resting` is the
    // venue's list of what rests, for the open-order cap.
    [[nodiscard]] RiskReject check(std::uint64_t in_seq, OrderRequest& req, const RestingBook& resting) {
        // Opening an auction and uncrossing it are the venue's to do, never a session's.
        if (req.type == ReqType::OpenAuction || req.type == ReqType::Uncross) return RiskReject::VenueOnly;
        if (req.type == ReqType::Cancel) return RiskReject::None;
        if (killed_.count(req.owner) != 0) return RiskReject::Killed;
        if (lim_.max_messages != 0) {
            Window& w = window_[req.owner];
            const std::uint64_t idx = in_seq / lim_.throttle_window;
            if (w.index != idx) w = Window{idx, 0};
            if (++w.count > lim_.max_messages) return RiskReject::Throttled;
        }
        if (lim_.max_order_qty != 0 && req.qty > lim_.max_order_qty) return RiskReject::OrderTooLarge;
        const auto ref = last_trade_.find(req.instr);
        if (lim_.collar != 0 && ref != last_trade_.end()) {
            const Price lo = ref->second - lim_.collar, hi = ref->second + lim_.collar;
            if (req.ord == OrdType::Market) {
                req.ord = OrdType::Limit; // protection: trade no further than the collar
                req.tif = Tif::IOC;
                req.price = req.side == Side::Buy ? hi : lo;
            } else if (req.price < lo || req.price > hi) {
                return RiskReject::PriceOutsideCollar;
            }
        }
        if (lim_.max_order_value != 0 && req.ord == OrdType::Limit &&
            req.price * static_cast<std::int64_t>(req.qty) > lim_.max_order_value)
            return RiskReject::ValueTooLarge;
        if (lim_.max_open_orders != 0 && req.type == ReqType::New && req.tif == Tif::GTC && req.ord == OrdType::Limit &&
            resting.open_count(req.owner) >= lim_.max_open_orders)
            return RiskReject::TooManyOpenOrders;
        return RiskReject::None;
    }

    // Fed every engine event, to know the last trade price.
    void on_event(const ExecEvent& e) {
        if (e.type == EventType::Trade) last_trade_[e.instr] = e.price;
    }
    // Blocks `owner` and returns the cancels that pull every order it has resting.
    [[nodiscard]] std::vector<OrderRequest> kill(OwnerId owner, const RestingBook& resting) {
        killed_.insert(owner);
        std::vector<OrderRequest> out;
        for (const RestingOrder& o : resting.of_owner(owner)) out.push_back(cancel_order(o.id, o.instr, o.owner));
        return out;
    }
    void revive(OwnerId owner) { killed_.erase(owner); }
    [[nodiscard]] bool killed(OwnerId owner) const { return killed_.count(owner) != 0; }

private:
    struct Window {
        std::uint64_t index = 0;
        std::uint32_t count = 0;
    };
    RiskLimits lim_;
    std::unordered_map<InstrumentId, Price> last_trade_;
    std::unordered_map<OwnerId, Window> window_;
    std::unordered_set<OwnerId> killed_;
};

} // namespace px::venue
