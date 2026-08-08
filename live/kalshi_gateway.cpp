#include "kalshi_gateway.hpp"
#include "px/platform.hpp" // cpu_relax()

#include <cctype>

namespace px::live {

bool parse_http_status(const std::string& response, int& status_out) {
    // "HTTP/1.1 429 Too Many Requests\r\n..." — status is the 3-digit token
    // between the first two spaces.
    if (response.compare(0, 5, "HTTP/") != 0) return false;
    const auto sp1 = response.find(' ');
    if (sp1 == std::string::npos) return false;
    const auto sp2 = response.find(' ', sp1 + 1);
    if (sp2 == std::string::npos || sp2 - sp1 - 1 != 3) return false;

    int code = 0;
    for (std::size_t i = sp1 + 1; i < sp2; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(response[i]))) return false;
        code = code * 10 + (response[i] - '0');
    }
    status_out = code;
    return true;
}

namespace {
void push_status_report(pipeline::RingC& exec_out, MarketId market, OrderId id, int status) {
    pipeline::ExecReport er;
    er.market = market;
    er.id = id;
    if (status == 429) {
        er.kind = pipeline::ExecKind::RateLimited;
    } else {
        er.kind = pipeline::ExecKind::Reject;
    }
    while (!exec_out.try_push(er)) cpu_relax(); // T2 must see every reject/rate-limit signal — no silent drops
}
} // namespace

void Gateway::handle_intent(const pipeline::OrderIntent& intent, pipeline::RingC& exec_out) {
    if (intent.kind == pipeline::IntentKind::CancelOrder) {
        handle_cancel(intent, exec_out);
    } else {
        handle_new_order(intent, exec_out);
    }
}

void Gateway::handle_new_order(const pipeline::OrderIntent& intent, pipeline::RingC& exec_out) {
    OrderRequest req;
    req.market_ticker = registry_.ticker_of(intent.market);
    req.side = intent.side;
    req.price = intent.price;
    req.qty = intent.qty;
    req.tif = intent.tif;
    req.client_order_id = "px-" + std::to_string(intent.id); // Brain's own id, not a Gateway-local counter —
                                                               // see the plan doc's "Order-ID ring safety" section

    bool did_send = false;
    std::string response, error;
    const bool attempted = sink_->place(req, did_send, response, error);

    if (on_sent) on_sent(intent, response, error);

    if (!attempted) return; // couldn't even build/sign the request — no exchange interaction to report
    if (!did_send) return;  // dry run — nothing real happened, nothing to reconcile on ring C

    int status = 0;
    if (!parse_http_status(response, status)) return; // unparseable — visible via on_sent, not fatal here

    if (status == 429 || status < 200 || status >= 300) {
        push_status_report(exec_out, intent.market, intent.id, status);
        return;
    }

    // 2xx: parse the create-order response body. A parse failure here means
    // Kalshi accepted the order but returned a body this project can't read
    // — visible via on_sent, not escalated to a Reject (the order genuinely
    // WAS accepted; fabricating a Reject would misrepresent what happened).
    OrderResponse oresp;
    if (!parse_order_response(response, oresp)) return;

    kalshi_uuid_of_[intent.id] = oresp.order_id;

    pipeline::ExecReport acked;
    acked.kind = pipeline::ExecKind::Acked;
    acked.market = intent.market;
    acked.id = intent.id;
    acked.side = intent.side;
    acked.qty = oresp.remaining_count;
    while (!exec_out.try_push(acked)) cpu_relax();

    // A marketable order can fill synchronously in the same response that
    // acks it (fill_count > 0 on a 201) — see the FILL TRACKING note in
    // kalshi_gateway.hpp for what this does and doesn't cover.
    if (oresp.fill_count > 0) {
        pipeline::ExecReport fill;
        fill.kind = pipeline::ExecKind::Fill;
        fill.market = intent.market;
        fill.id = intent.id;
        fill.side = intent.side;
        fill.price = oresp.has_average_fill_price ? oresp.average_fill_price_cents : intent.price;
        fill.qty = oresp.fill_count;
        while (!exec_out.try_push(fill)) cpu_relax();
    }
}

void Gateway::handle_cancel(const pipeline::OrderIntent& intent, pipeline::RingC& exec_out) {
    const auto it = kalshi_uuid_of_.find(intent.id);
    if (it == kalshi_uuid_of_.end()) {
        // Nothing to cancel — Gateway never learned a Kalshi order_id for
        // this local id (never acked, already filled/cancelled, or a stale
        // decision from a strategy racing its own state). Not an error:
        // MarketState::outstanding_orders (strategy/market_state.hpp) is
        // acked-only by construction, so a well-behaved Strategy structurally
        // can't target this — this branch exists for defense in depth, not
        // because it's expected to fire.
        if (on_sent) on_sent(intent, "", "cancel target not tracked by this gateway");
        return;
    }
    const std::string kalshi_order_id = it->second;

    bool did_send = false;
    std::string response, error;
    const bool attempted = sink_->cancel(kalshi_order_id, did_send, response, error);

    if (on_sent) on_sent(intent, response, error);

    if (!attempted) return;
    if (!did_send) return;

    int status = 0;
    if (!parse_http_status(response, status)) return;

    if (status == 429 || status < 200 || status >= 300) {
        push_status_report(exec_out, intent.market, intent.id, status);
        return;
    }

    CancelResponse cresp;
    if (!parse_cancel_response(response, cresp)) return;

    kalshi_uuid_of_.erase(it); // a cancel always resolves the whole remaining order — nothing left to track

    pipeline::ExecReport cancelled;
    cancelled.kind = pipeline::ExecKind::Cancelled;
    cancelled.market = intent.market;
    cancelled.id = intent.id;
    cancelled.side = intent.side;
    cancelled.qty = cresp.reduced_by;
    while (!exec_out.try_push(cancelled)) cpu_relax();
}

void Gateway::run(pipeline::RingB& intents_in, pipeline::RingC& exec_out, std::atomic<bool>& stop_flag) {
    while (!stop_flag.load(std::memory_order_relaxed)) {
        pipeline::OrderIntent intent;
        if (!intents_in.try_pop(intent)) {
            cpu_relax();
            continue;
        }
        handle_intent(intent, exec_out);
    }
}

} // namespace px::live
