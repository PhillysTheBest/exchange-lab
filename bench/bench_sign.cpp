// bench/bench_sign.cpp — keeps the "why T3 exists" numbers honest in-repo.
//
// The Stage 2 architecture puts RSA-PSS signing on its own thread (T3) rather
// than inline in T2's book-update loop, on the claim that signing is roughly
// four orders of magnitude more expensive than a book update and has a heavy
// tail — this benchmark is what that claim is checked against, not asserted.
// It calls the actual production function, kalshi_auth.hpp's sign_request,
// against a throwaway locally-generated key — not a reimplementation of the
// signing call, so a regression in the real code path shows up here too.
#include "kalshi_auth.hpp"
#include "px/histogram.hpp"
#include "px/tsc_clock.hpp"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace px;
using namespace px::live;

namespace {

constexpr std::uint64_t kWarmup = 20;
constexpr std::uint64_t kIters = 300; // RSA-2048 sign is ~1ms — 300 iters is already a ~0.3s run

// Mirrors tests/test_gateway_dry_run.cpp's write_test_key: BIO_new_file, not
// fopen()+PEM_write_PrivateKey(FILE*,...), which fails on this toolchain with
// a CRT-boundary "PEM routines::BIO lib" error (see kalshi_auth.cpp's fix and
// the plan doc for the repro). No key material is embedded in source; this
// key is generated fresh every run and deleted before main() returns.
[[nodiscard]] bool write_throwaway_key(const std::string& path) {
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

} // namespace

int main() {
    const std::string key_path = "build/_bench_sign_key.pem";
    if (!write_throwaway_key(key_path)) {
        std::fprintf(stderr, "failed to generate a throwaway RSA key for this benchmark\n");
        return 1;
    }

    std::string error;
    auto key = load_private_key_pem(key_path, error);
    std::remove(key_path.c_str());
    if (!key) {
        std::fprintf(stderr, "load_private_key_pem failed: %s\n", error.c_str());
        return 1;
    }

    TscClock clock;
    std::printf("invariant TSC: %s, calibrated %.4f ticks/ns\n\n",
                clock.invariant_tsc() ? "yes" : "no (results may drift)", clock.ticks_per_ns());

    // Signs the same (timestamp, method, path) shape kalshi_feed.cpp actually
    // signs for the WS upgrade — see KalshiFeed::connect.
    const std::string ts = timestamp_ms_now();
    const std::string method = "GET";
    const std::string path = "/trade-api/ws/v2";

    LatencyHistogram hist;
    hist.reserve(kIters);

    for (std::uint64_t i = 0; i < kWarmup + kIters; ++i) {
        std::string sign_err;
        const Tsc t0 = read_tscp();
        const std::string sig = sign_request(*key, ts, method, path, sign_err);
        const Tsc t1 = read_tscp();
        if (sig.empty()) {
            std::fprintf(stderr, "sign_request failed mid-benchmark: %s\n", sign_err.c_str());
            return 1;
        }
        if (i >= kWarmup) hist.record(clock.to_ns(t1 - t0));
    }

    LatencyHistogram::print("RSA-2048 PSS sign_request()", hist.summarize());

    const auto p = hist.summarize();
    std::printf("\nfor comparison: a ShadowBook level update is on the order of 50-100 ns "
                "(see bench_engine's order-submit numbers) -- this puts a single sign call "
                "at roughly %.0fx-%.0fx that cost at p50/p99, which is the entire justification "
                "for keeping signing off T2's thread (see pipeline/brain.hpp, live/kalshi_gateway.hpp).\n",
                p.p50 / 75.0, p.p99 / 75.0);
    return 0;
}
