// tests/test_gateway_dry_run.cpp — structural proof that DryRunSink never
// touches the network, mirroring the guarantee kalshi_rest.hpp's SAFETY
// POSTURE comment already documents for place_order itself, now checked at
// the Gateway/Sink level.
//
// The technique: point KalshiRestConfig::host at "invalid.invalid" — a domain
// name RFC 6761 reserves specifically as guaranteed non-resolving, not just a
// domain nobody happens to own. If DryRunSink ever accidentally attempted a
// real connect, getaddrinfo() would fail and place_order would return false.
// It doesn't: dry-run building+signing a request has nothing to do with
// whether the host is even resolvable. LiveSink against the same host is
// then checked to genuinely fail, proving the test would have caught a
// DryRunSink regression rather than passing vacuously.
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

void test_dry_run_sink_never_sends() {
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

void test_live_sink_against_same_host_actually_fails() {
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

// A tiny always-succeeds fake Sink, used to test Gateway::run's dispatch
// logic (429 -> RateLimited, other non-2xx -> Reject, 2xx -> nothing on ring
// C) without depending on real signing/network at all.
class FakeSink final : public Sink {
public:
    explicit FakeSink(std::string canned_response) : response_(std::move(canned_response)) {}
    bool place(const OrderRequest&, bool& did_send, std::string& response, std::string&) override {
        did_send = true;
        response = response_;
        return true;
    }

private:
    std::string response_;
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
    intent.market = m;
    intent.side = Side::Buy;
    intent.price = 50;
    intent.qty = 1;
    PX_CHECK(ring_b.try_push(intent));

    // A fixed spin-count budget isn't a real time bound under scheduling
    // jitter (a freshly-created thread may not get its first timeslice for a
    // while, especially right after a heavy build) — wait against a wall-clock
    // deadline instead.
    ExecReport got;
    bool received = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ring_c.try_pop(got)) { received = true; break; }
        std::this_thread::yield();
    }
    PX_CHECK(received);
    PX_CHECK(got.kind == ExecKind::RateLimited);
    PX_CHECK(got.market == m);

    stop.store(true, std::memory_order_relaxed);
    t.join();
}

void test_gateway_run_2xx_produces_no_exec_report() {
    TickerRegistry registry;
    MarketId m;
    PX_CHECK(registry.add("TEST-MARKET", m));

    Gateway gw(std::make_unique<FakeSink>("HTTP/1.1 201 Created\r\n\r\n{}"), registry);

    RingB ring_b;
    RingC ring_c;
    std::atomic<bool> stop{false};
    std::thread t([&] { gw.run(ring_b, ring_c, stop); });

    OrderIntent intent;
    intent.market = m;
    intent.side = Side::Buy;
    intent.price = 50;
    intent.qty = 1;
    PX_CHECK(ring_b.try_push(intent));

    // Give the gateway thread a generous chance to (wrongly) push something.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ExecReport got;
    PX_CHECK(!ring_c.try_pop(got)); // 2xx is an accept, not a fill — nothing belongs on ring C for it

    stop.store(true, std::memory_order_relaxed);
    t.join();
}

} // namespace

int main() {
    test_dry_run_sink_never_sends();
    test_live_sink_against_same_host_actually_fails();
    test_http_status_parsing();
    test_gateway_run_dispatches_429_to_ring_c();
    test_gateway_run_2xx_produces_no_exec_report();
    PX_TEST_REPORT_AND_RETURN();
}
