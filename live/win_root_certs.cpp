#include "win_root_certs.hpp"

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace px::live {

#ifdef _WIN32

std::vector<std::vector<std::uint8_t>> load_windows_root_certs_der(std::string& error) {
    std::vector<std::vector<std::uint8_t>> out;

    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store) {
        error = "CertOpenSystemStoreW(ROOT) failed";
        return out;
    }

    PCCERT_CONTEXT ctx = nullptr;
    while ((ctx = CertEnumCertificatesInStore(store, ctx)) != nullptr) {
        out.emplace_back(ctx->pbCertEncoded, ctx->pbCertEncoded + ctx->cbCertEncoded);
    }

    CertCloseStore(store, 0);
    if (out.empty()) error = "Windows ROOT store enumerated zero certificates";
    return out;
}

#else // !_WIN32

// On Linux, OpenSSL's SSL_CTX_set_default_verify_paths() finds the system CA
// bundle on its own (e.g. /etc/ssl/certs) — tls_socket.cpp falls back to that
// when this returns empty, so this isn't a gap, just a platform split.
std::vector<std::vector<std::uint8_t>> load_windows_root_certs_der(std::string& error) {
    error = "not applicable on this platform (using OpenSSL's default verify paths instead)";
    return {};
}

#endif

} // namespace px::live
