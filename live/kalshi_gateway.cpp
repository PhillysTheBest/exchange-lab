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

void Gateway::handle_intent(const pipeline::OrderIntent& intent, pipeline::RingC& exec_out) {
    OrderRequest req;
    req.market_ticker = registry_.ticker_of(intent.market);
    req.side = intent.side;
    req.price = intent.price;
    req.qty = intent.qty;
    req.client_order_id = "px-" + std::to_string(next_client_order_id_++);

    bool did_send = false;
    std::string response, error;
    const bool attempted = sink_->place(req, did_send, response, error);

    if (on_sent) on_sent(intent, response, error);

    if (!attempted) return; // couldn't even build/sign the request — no exchange interaction to report
    if (!did_send) return;  // dry run — nothing real happened, nothing to reconcile on ring C

    int status = 0;
    if (!parse_http_status(response, status)) return; // unparseable — visible via on_sent, not fatal here

    pipeline::ExecReport er;
    er.market = intent.market;
    er.side = intent.side;
    if (status == 429) {
        er.kind = pipeline::ExecKind::RateLimited;
    } else if (status < 200 || status >= 300) {
        er.kind = pipeline::ExecKind::Reject;
    } else {
        return; // 2xx: Kalshi accepted the order — not a fill (see the FILL TRACKING GAP note)
    }
    while (!exec_out.try_push(er)) cpu_relax(); // T2 must see every reject/rate-limit signal — no silent drops
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
