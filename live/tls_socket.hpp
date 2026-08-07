// live/tls_socket.hpp — blocking TLS client socket (Winsock2 + OpenSSL).
//
// Not on any hot path — this is Phase 3 exchange connectivity, not the matching
// core, so it's free to use std::string/exceptions-off-but-error-string-returns
// throughout rather than the zero-allocation discipline elsewhere in this
// codebase. Certificate verification uses the real Windows trust store (pulled
// into OpenSSL's X509_STORE via CryptoAPI) rather than disabling verification or
// shipping a vendored CA bundle — this talks to a real financial exchange.
#pragma once

#include <cstddef>
#include <string>

namespace px::live {

class TlsSocket {
public:
    TlsSocket();
    ~TlsSocket();
    TlsSocket(const TlsSocket&) = delete;
    TlsSocket& operator=(const TlsSocket&) = delete;

    // Resolves `host`, opens a TCP connection to (host, port), and performs the
    // TLS handshake with SNI set to `host` and full chain+hostname verification
    // against the Windows root store. Returns false with `error` populated on
    // any failure at any stage.
    [[nodiscard]] bool connect(const std::string& host, unsigned short port, std::string& error);

    // Returns bytes read (0 on clean EOF), or -1 on error.
    [[nodiscard]] int read(void* buf, std::size_t len);
    // Returns true iff all `len` bytes were written.
    [[nodiscard]] bool write_all(const void* buf, std::size_t len);

    void close();

private:
    struct Impl;
    Impl* impl_;
};

} // namespace px::live
