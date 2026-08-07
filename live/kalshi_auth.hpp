// live/kalshi_auth.hpp — RSA-PSS request signing for the Kalshi trading API.
//
// Every REST and WebSocket request is signed per Kalshi's documented scheme:
// KALSHI-ACCESS-SIGNATURE = base64(RSA-PSS-SHA256_sign(timestamp_ms + method + path))
// where `path` excludes the query string, and timestamp_ms is the current Unix
// time in *milliseconds* as a decimal string. Verified against
// https://docs.kalshi.com/getting_started/quick_start_websockets (see the plan
// doc's Sources). Key material is never embedded in source: load_private_key_pem
// reads a PEM file path supplied at runtime (typically from an env var — see
// tools/kalshi_shadow.cpp), and this whole directory is covered by .gitignore's
// *.pem/*.key rule.
#pragma once

#include <memory>
#include <string>

namespace px::live {

class PrivateKey {
public:
    explicit PrivateKey(void* evp_pkey) noexcept : pkey_(evp_pkey) {}
    ~PrivateKey();
    PrivateKey(const PrivateKey&) = delete;
    PrivateKey& operator=(const PrivateKey&) = delete;

    [[nodiscard]] void* raw() const noexcept { return pkey_; }

private:
    void* pkey_; // EVP_PKEY* — kept as void* here so this header doesn't drag
                 // <openssl/evp.h> into every translation unit that includes it.
};

// Loads an RSA private key from a PEM file (as issued by Kalshi when an API key
// is created). Returns nullptr and fills `error` on failure.
[[nodiscard]] std::unique_ptr<PrivateKey> load_private_key_pem(const std::string& path, std::string& error);

// Current Unix time in milliseconds, decimal — exactly what KALSHI-ACCESS-TIMESTAMP
// expects. A stale or second-granularity timestamp is a documented, common cause
// of signature validation failures.
[[nodiscard]] std::string timestamp_ms_now();

// Signs (timestamp_ms + method + path) with RSA-PSS/SHA-256 (salt length ==
// digest length) and returns the base64-encoded signature — the exact value for
// the KALSHI-ACCESS-SIGNATURE header. `method` is upper-case ("GET"/"POST"/...),
// `path` starts with '/' and excludes the query string. Returns empty and fills
// `error` on failure.
[[nodiscard]] std::string sign_request(const PrivateKey& key, const std::string& timestamp_ms,
                                        const std::string& method, const std::string& path,
                                        std::string& error);

} // namespace px::live
