#include "kalshi_auth.hpp"
#include "base64.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <chrono>
#include <vector>

namespace px::live {

namespace {
std::string openssl_error_string() {
    char buf[256];
    const unsigned long code = ERR_get_error();
    if (code == 0) return "unknown OpenSSL error";
    ERR_error_string_n(code, buf, sizeof(buf));
    return buf;
}
} // namespace

PrivateKey::~PrivateKey() {
    if (pkey_) EVP_PKEY_free(static_cast<EVP_PKEY*>(pkey_));
}

std::unique_ptr<PrivateKey> load_private_key_pem(const std::string& path, std::string& error) {
    // BIO_new_file, not fopen()+PEM_read_PrivateKey(FILE*,...): the latter
    // fails on this toolchain (OpenSSL 3.2 via MSYS2 UCRT64) with "DECODER
    // routines::unsupported" — a FILE* handed across into OpenSSL's DLL,
    // built against a different CRT than the one that opened it, doesn't
    // behave the way its layout suggests. Confirmed by direct repro before
    // this fix; letting OpenSSL open the file itself sidesteps the mismatch
    // entirely, and is also simply the more portable way to drive PEM I/O.
    BIO* bio = BIO_new_file(path.c_str(), "rb");
    if (!bio) {
        error = "cannot open key file: " + path;
        return nullptr;
    }
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        error = "PEM_read_bio_PrivateKey failed: " + openssl_error_string();
        return nullptr;
    }
    if (EVP_PKEY_id(pkey) != EVP_PKEY_RSA) {
        EVP_PKEY_free(pkey);
        error = "key is not RSA (Kalshi API keys are RSA)";
        return nullptr;
    }
    return std::make_unique<PrivateKey>(pkey);
}

std::string timestamp_ms_now() {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return std::to_string(ms);
}

std::string sign_request(const PrivateKey& key, const std::string& timestamp_ms,
                          const std::string& method, const std::string& path, std::string& error) {
    const std::string message = timestamp_ms + method + path;

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        error = "EVP_MD_CTX_new failed";
        return {};
    }

    EVP_PKEY_CTX* pctx = nullptr;
    if (EVP_DigestSignInit(mdctx, &pctx, EVP_sha256(), nullptr, static_cast<EVP_PKEY*>(key.raw())) <= 0) {
        error = "EVP_DigestSignInit failed: " + openssl_error_string();
        EVP_MD_CTX_free(mdctx);
        return {};
    }
    if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0) {
        error = "EVP_PKEY_CTX_set_rsa_padding failed: " + openssl_error_string();
        EVP_MD_CTX_free(mdctx);
        return {};
    }
    // Salt length == digest length is the conventional PSS choice and what
    // Kalshi's own client libraries use.
    if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) <= 0) {
        error = "EVP_PKEY_CTX_set_rsa_pss_saltlen failed: " + openssl_error_string();
        EVP_MD_CTX_free(mdctx);
        return {};
    }

    std::size_t sig_len = 0;
    if (EVP_DigestSign(mdctx, nullptr, &sig_len,
                        reinterpret_cast<const unsigned char*>(message.data()), message.size()) <= 0) {
        error = "EVP_DigestSign (size query) failed: " + openssl_error_string();
        EVP_MD_CTX_free(mdctx);
        return {};
    }
    std::vector<unsigned char> sig(sig_len);
    if (EVP_DigestSign(mdctx, sig.data(), &sig_len,
                        reinterpret_cast<const unsigned char*>(message.data()), message.size()) <= 0) {
        error = "EVP_DigestSign failed: " + openssl_error_string();
        EVP_MD_CTX_free(mdctx);
        return {};
    }
    EVP_MD_CTX_free(mdctx);

    return base64_encode(sig.data(), static_cast<int>(sig_len));
}

} // namespace px::live
