// tests/test_gateway_dry_run.cpp — structural proof that DryRunSink never
// touches the network, mirroring the guarantee kalshi_rest.hpp's SAFETY
// POSTURE comment already documents for place_order/cancel_order themselves,
// now checked at the Gateway/Sink level. Also exercises Gateway::run's full
// dispatch logic (429/reject/accept/fill/cancel) end-to-end through real
// threads and real rings.
//
// The technique: point KalshiRestConfig::host at "invalid.invalid" — a domain
// name RFC 6761 reserves specifically as guaranteed non-resolving, not just a
// domain nobody happens to own. If DryRunSink ever accidentally attempted a
// real connect, getaddrinfo() would fail and place_order/cancel_order would
// return false. It doesn't: dry-run building+signing a request has nothing to
// do with whether the host is even resolvable. LiveSink against the same
// host is then checked to genuinely fail, proving the test would have caught
// a DryRunSink regression rather than passing vacuously.
#include "kalshi_gateway.hpp"
#include "pipeline/events.hpp"
#include "test_harness.hpp"
#include "ticker_registry.hpp"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace px;
using namespace px::live;
using namespace px::pipeline;

namespace {

// Generates a throwaway 2048-bit RSA key and writes it to `path` as PEM —
// load_private_key_pem (kalshi_auth.cpp) needs a real, parseable key to sign
// against even in dry-run mode (SAFETY POSTURE: it signs the full request,
// it just never sends it), and key material must never be embedded in
// source, so this test mints its own throwaway key rather than committing one.
//
// Writes via BIO_new_file rather than fopen()+PEM_write_PrivateKey(FILE*,...):
// the latter crashed/failed here with a "PEM routines::BIO lib" error — a
// known Windows gotcha where a FILE* handed across into OpenSSL's DLL, built
// against a different CRT than the one that opened it, doesn't behave the way
// its layout suggests. Letting OpenSSL open the file itself sidesteps the
// mismatch entirely.
[[nodiscard]] bool write_test_key(const std::string& path) {
    EVP_PKEY_CTX* gen = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!gen) return false;
    bool ok = EVP_PKEY_keygen_init(gen) > 0 && EVP_PKEY_CTX_set_rsa_keygen_bits(gen, 2048) > 0;
    EVP_PKEY* key = nullptr;
    ok = ok && EVP_PKEY_keygen(gen, &key) > 0;
    EVP_PKEY_CTX_free(gen);
    if (!ok || !key) return false;

    BIO* bio = BIO_new_file(path.c_str(), "wb");
    bool wrote = false;
    if (bio) {
        wrote = PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
        BIO_free(bio);
    }
    EVP_PKEY_free(key);
    return wrote;
}

// A fixed spin-count budget isn't a real time bound under scheduling jitter
// (a freshly-created thread may not get its first timeslice for a while,
// especially right after a heavy build) — wait against a wall-clock deadline
// instead. Shared by every Gateway::run test below.
[[nodiscard]] bool wait_for_report(RingC& ring, ExecReport& out, int seconds = 5) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ring.try_pop(out)) return true;
        std::this_thread::yield();
    }
    return false;
}

void test_dry_run_sink_place_never_sends() {
    const std::string key_path = "build/_test_gateway_key.pem";
    PX_CHECK(write_test_key(key_path));

    KalshiRestConfig cfg;
    cfg.host = "invalid.invalid"; // RFC 6761 — guaranteed non-resolving
    cfg.private_key_path = key_path;

    DryRunSink sink(cfg);
    OrderRequest req;
    req.market_ticker = "TEST-MARKET";
    req.side = Side::Buy;
    req.price = 50;
    req.qty = 10;

    bool did_send = true; // deliberately wrong initial value — place() must set it
    std::string response, error;
    const bool attempted = sink.place(req, did_send, response, error);

    PX_CHECK(attempted);              // signing succeeded — the key is real, this isn't a signing failure
    PX_CHECK(!did_send);              // structural: DryRunSink says so itself
    PX_CHECK(response.find("DRY RUN") != std::string::npos);
    PX_CHECK(response.find(cfg.host) != std::string::npos); // the request WAS built against this host...
    PX_CHECK(error.empty());          // ...but never actually resolved/dialed, hence no connect error

    std::remove(key_path.c_str());
}

void test_live_sink_place_against_same_host_actually_fails() {
    // Proves the above test isn't vacuous: the identical unresolvable host
    // genuinely breaks a sink that does attempt to send.
    const std::string key_path = "build/_test_gateway_key2.pem";
    PX_CHECK(write_test_key(key_path));

    KalshiRestConfig cfg;
    cfg.host = "invalid.invalid";
    cfg.private_key_path = key_path;

    LiveSink sink(cfg);
    OrderRequest req;
    req.market_ticker = "TEST-MARKET";
    req.side = Side::Buy;
    req.price = 50;
    req.qty = 10;

    bool did_send = false;
    std::string response, error;
    const bool attempted = sink.place(req, did_send, response, error);

    PX_CHECK(did_send);   // LiveSink always attempts — did_send is set before the connect, not after
    PX_CHECK(!attempted); // and the connect itself fails against an unresolvable host
    PX_CHECK(!error.empty());

    std::remove(key_path.c_str());
}

void test_dry_run_sink_cancel_never_sends() {
    const std::string key_path = "build/_test_gateway_key3.pem";
    PX_CHECK(write_test_key(key_path));

    KalshiRestConfig cfg;
    cfg.host = "invalid.invalid";
    cfg.private_key_path = key_path;

    DryRunSink sink(cfg);
    bool did_send = true;
    std::string response, error;
    const bool attempted = sink.cancel("some-kalshi-order-uuid", did_send, response, error);

    PX_CHECK(attempted);
    PX_CHECK(!did_send);
    PX_CHECK(response.find("DRY RUN") != std::string::npos);
    PX_CHECK(response.find("some-kalshi-order-uuid") != std::string::npos); // the id IS in the built path...
    PX_CHECK(error.empty());                                                // ...but nothing was dialed

    std::remove(key_path.c_str());
}

void test_live_sink_cancel_against_unresolvable_host_fails() {
    const std::string key_path = "build/_test_gateway_key4.pem";
    PX_CHECK(write_test_key(key_path));

    KalshiRestConfig cfg;
    cfg.host = "invalid.invalid";
    cfg.private_key_path = key_path;

    LiveSink sink(cfg);
    bool did_send = false;
    std::string response, error;
    const bool attempted = sink.cancel("some-kalshi-order-uuid", did_send, response, error);

    PX_CHECK(did_send);
    PX_CHECK(!attempted);
    PX_CHECK(!error.empty());

    std::remove(key_path.c_str());
}

void test_http_status_parsing() {
    int status = 0;
    PX_CHECK(parse_http_status("HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\n\r\n", status));
    PX_CHECK(status == 429);

    PX_CHECK(parse_http_status("HTTP/1.1 201 Created\r\n\r\n{}", status));
    PX_CHECK(status == 201);

    PX_CHECK(parse_http_status("HTTP/1.0 500 Internal Server Error\r\n\r\n", status));
    PX_CHECK(status == 500);

    PX_CHECK(!parse_http_status("[DRY RUN -- request built and signed, nothing sent]\nPOST /x", status));
    PX_CHECK(!parse_http_status("", status));
    PX_CHECK(!parse_http_status("garbage", status));
}

// A tiny always-succeeds fake Sink returning canned responses, used to test
// Gateway::run's dispatch logic without depending on real signing/network at
// all. Records what cancel() was actually called with, so a test can confirm
// Gateway resolved its own OrderId to the right Kalshi UUID.
class FakeSink final : public Sink {
public:
    explicit FakeSink(std::string place_response, std::string cancel_response = "")
        : place_response_(std::move(place_response)), cancel_response_(std::move(cancel_response)) {}

    bool place(const OrderRequest&, bool& did_send, std::string& response, std::string&) override {
        did_send = true;
        response = place_response_;
        return true;
    }
    bool cancel(const std::string& kalshi_order_id, bool& did_send, std::string& response, std::string&) override {
        did_send = true;
        last_cancel_kalshi_id = kalshi_order_id;
        cancel_call_count++;
        response = cancel_response_;
        return true;
    }

    std::string last_cancel_kalshi_id;
    int cancel_call_count = 0;

private:
    std::string place_response_;
    std::string cancel_response_;
};

void test_gateway_run_dispatches_429_to_ring_c() {
    TickerRegistry registry;
    MarketId m;
    PX_CHECK(registry.add("TEST-MARKET", m));

    Gateway gw(std::make_unique<FakeSink>("HTTP/1.1 429 Too Many Requests\r\n\r\n"), registry);

    RingB ring_b;
    RingC ring_c;
    std::atomic<bool> stop{false};
    std::thread t([&] { gw.run(ring_b, ring_c, stop); });

    OrderIntent intent;
    intent.id = 1;
    intent.market = m;
    intent.side = Side::Buy;
    intent.price = 50;
    intent.qty = 1;
    PX_CHECK(ring_b.try_push(intent));

    ExecReport got;
    PX_CHECK(wait_for_report(ring_c, got));
    PX_CHECK(got.kind == ExecKind::RateLimited);
    PX_CHECK(got.market == m);
    PX_CHECK(got.id == 1);

    stop.store(true, std::memory_order_relaxed);
    t.join();
}

void test_gateway_run_2xx_realistic_body_produces_acked() {
    TickerRegistry registry;
    MarketId m;
    PX_CHECK(registry.add("TEST-MARKET", m));

    Gateway gw(std::make_unique<FakeSink>("HTTP/1.1 201 Created\r\n\r\n"
                                           "{\"order_id\":\"kalshi-uuid-1\",\"client_order_id\":\"px-2\","
                                           "\"fill_count\":\"0.00\",\"remaining_count\":\"10.00\",\"ts_ms\":1}"),
               registry);

    RingB ring_b;
    RingC ring_c;
    std::atomic<bool> stop{false};
    std::thread t([&] { gw.run(ring_b, ring_c, stop); });

    OrderIntent intent;
    intent.id = 2;
    intent.market = m;
    intent.side = Side::Buy;
    intent.price = 50;
    intent.qty = 10;
    PX_CHECK(ring_b.try_push(intent));

    ExecReport got;
    PX_CHECK(wait_for_report(ring_c, got));
    PX_CHECK(got.kind == ExecKind::Acked);
    PX_CHECK(got.id == 2);
    PX_CHECK(got.qty == 10); // remaining_count

    stop.store(true, std::memory_order_relaxed);
    t.join();
}

void test_gateway_run_2xx_with_fill_produces_acked_and_fill() {
    TickerRegistry registry;
    MarketId m;
    PX_CHECK(registry.add("TEST-MARKET", m));

    Gateway gw(std::make_unique<FakeSink>("HTTP/1.1 201 Created\r\n\r\n"
                                           "{\"order_id\":\"kalshi-uuid-2\",\"client_order_id\":\"px-3\","
                                           "\"fill_count\":\"4.00\",\"remaining_count\":\"6.00\","
                                           "\"average_fill_price\":\"0.6300\",\"ts_ms\":1}"),
               registry);

    RingB ring_b;
    RingC ring_c;
    std::atomic<bool> stop{false};
    std::thread t([&] { gw.run(ring_b, ring_c, stop); });

    OrderIntent intent;
    intent.id = 3;
    intent.market = m;
    intent.side = Side::Buy;
    intent.price = 62;
    intent.qty = 10;
    PX_CHECK(ring_b.try_push(intent));

    ExecReport acked;
    PX_CHECK(wait_for_report(ring_c, acked));
    PX_CHECK(acked.kind == ExecKind::Acked);
    PX_CHECK(acked.qty == 6); // remaining_count

    ExecReport fill;
    PX_CHECK(wait_for_report(ring_c, fill));
    PX_CHECK(fill.kind == ExecKind::Fill);
    PX_CHECK(fill.id == 3);
    PX_CHECK(fill.qty == 4);   // fill_count
    PX_CHECK(fill.price == 63); // average_fill_price

    stop.store(true, std::memory_order_relaxed);
    t.join();
}

void test_gateway_run_2xx_malformed_body_produces_no_exec_report() {
    // A 2xx whose body this project can't parse (missing order_id) must not
    // be escalated to a Reject — the order genuinely WAS accepted by Kalshi;
    // fabricating a rejection would misrepresent what happened. Same
    // "unparseable — visible via on_sent, not fatal" philosophy as the rest
    // of this codebase's response handling.
    TickerRegistry registry;
    MarketId m;
    PX_CHECK(registry.add("TEST-MARKET", m));

    Gateway gw(std::make_unique<FakeSink>("HTTP/1.1 201 Created\r\n\r\n{}"), registry);

    RingB ring_b;
    RingC ring_c;
    std::atomic<bool> stop{false};
    std::thread t([&] { gw.run(ring_b, ring_c, stop); });

    OrderIntent intent;
    intent.id = 4;
    intent.market = m;
    intent.side = Side::Buy;
    intent.price = 50;
    intent.qty = 1;
    PX_CHECK(ring_b.try_push(intent));

    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // generous chance to (wrongly) push something

    ExecReport got;
    PX_CHECK(!ring_c.try_pop(got));

    stop.store(true, std::memory_order_relaxed);
    t.join();
}

void test_gateway_full_new_order_then_cancel_flow() {
    TickerRegistry registry;
    MarketId m;
    PX_CHECK(registry.add("TEST-MARKET", m));

    auto sink = std::make_unique<FakeSink>(
        "HTTP/1.1 201 Created\r\n\r\n"
        "{\"order_id\":\"kalshi-uuid-abc\",\"client_order_id\":\"px-7\",\"fill_count\":\"0.00\","
        "\"remaining_count\":\"10.00\",\"ts_ms\":1}",
        "HTTP/1.1 200 OK\r\n\r\n"
        "{\"order_id\":\"kalshi-uuid-abc\",\"client_order_id\":\"px-7\",\"reduced_by\":\"10.00\",\"ts_ms\":2}");
    FakeSink* sink_ptr = sink.get();
    Gateway gw(std::move(sink), registry);

    RingB ring_b;
    RingC ring_c;
    std::atomic<bool> stop{false};
    std::thread t([&] { gw.run(ring_b, ring_c, stop); });

    OrderIntent new_intent;
    new_intent.kind = IntentKind::NewOrder;
    new_intent.id = 7;
    new_intent.market = m;
    new_intent.side = Side::Buy;
    new_intent.price = 50;
    new_intent.qty = 10;
    PX_CHECK(ring_b.try_push(new_intent));

    ExecReport acked;
    PX_CHECK(wait_for_report(ring_c, acked));
    PX_CHECK(acked.kind == ExecKind::Acked);
    PX_CHECK(acked.id == 7);

    OrderIntent cancel_intent;
    cancel_intent.kind = IntentKind::CancelOrder;
    cancel_intent.id = 7; // Brain's own id — never Kalshi's UUID (see kalshi_gateway.hpp)
    cancel_intent.market = m;
    PX_CHECK(ring_b.try_push(cancel_intent));

    ExecReport cancelled;
    PX_CHECK(wait_for_report(ring_c, cancelled));
    PX_CHECK(cancelled.kind == ExecKind::Cancelled);
    PX_CHECK(cancelled.id == 7);
    PX_CHECK(cancelled.qty == 10); // reduced_by

    // Gateway resolved OUR id 7 to Kalshi's real UUID before calling cancel().
    PX_CHECK(sink_ptr->last_cancel_kalshi_id == "kalshi-uuid-abc");
    PX_CHECK(sink_ptr->cancel_call_count == 1);

    stop.store(true, std::memory_order_relaxed);
    t.join();
}

void test_gateway_cancel_of_untracked_id_does_not_call_sink() {
    // Structural defense-in-depth: MarketState::outstanding_orders is
    // acked-only (strategy/order_tracker.hpp), so a well-behaved Strategy
    // can't construct this in practice — but Gateway must still handle it
    // gracefully rather than calling cancel() with garbage.
    TickerRegistry registry;
    MarketId m;
    PX_CHECK(registry.add("TEST-MARKET", m));

    auto sink = std::make_unique<FakeSink>("", "HTTP/1.1 200 OK\r\n\r\n{}");
    FakeSink* sink_ptr = sink.get();
    Gateway gw(std::move(sink), registry);

    RingB ring_b;
    RingC ring_c;
    std::atomic<bool> stop{false};
    std::thread t([&] { gw.run(ring_b, ring_c, stop); });

    OrderIntent cancel_intent;
    cancel_intent.kind = IntentKind::CancelOrder;
    cancel_intent.id = 999; // never seen a NewOrder for this id
    cancel_intent.market = m;
    PX_CHECK(ring_b.try_push(cancel_intent));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ExecReport got;
    PX_CHECK(!ring_c.try_pop(got));
    PX_CHECK(sink_ptr->cancel_call_count == 0); // never even reached the sink

    stop.store(true, std::memory_order_relaxed);
    t.join();
}

} // namespace

int main() {
    test_dry_run_sink_place_never_sends();
    test_live_sink_place_against_same_host_actually_fails();
    test_dry_run_sink_cancel_never_sends();
    test_live_sink_cancel_against_unresolvable_host_fails();
    test_http_status_parsing();
    test_gateway_run_dispatches_429_to_ring_c();
    test_gateway_run_2xx_realistic_body_produces_acked();
    test_gateway_run_2xx_with_fill_produces_acked_and_fill();
    test_gateway_run_2xx_malformed_body_produces_no_exec_report();
    test_gateway_full_new_order_then_cancel_flow();
    test_gateway_cancel_of_untracked_id_does_not_call_sink();
    PX_TEST_REPORT_AND_RETURN();
}
