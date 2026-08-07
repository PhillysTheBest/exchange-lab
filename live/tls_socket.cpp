#include "tls_socket.hpp"
#include "win_root_certs.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif

#include <mutex>

namespace px::live {

namespace {

#ifdef _WIN32
void ensure_winsock_initialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}
#endif

std::string openssl_error_string() {
    char buf[256];
    const unsigned long code = ERR_get_error();
    if (code == 0) return "unknown OpenSSL error";
    ERR_error_string_n(code, buf, sizeof(buf));
    return buf;
}

// Loads trust anchors into ctx's X509_STORE: the Windows ROOT store on
// Windows, or OpenSSL's own default verify paths elsewhere. Real chain +
// hostname verification depends on this being non-empty.
bool load_trust_anchors(SSL_CTX* ctx, std::string& error) {
    std::string werror;
    const auto certs = load_windows_root_certs_der(werror);
    if (certs.empty()) {
        // Not Windows, or the Windows loader failed — fall back to OpenSSL's
        // own notion of the system store.
        if (SSL_CTX_set_default_verify_paths(ctx) == 1) return true;
        error = "no trust anchors available (" + werror + "; " + openssl_error_string() + ")";
        return false;
    }
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    int added = 0;
    for (const auto& der : certs) {
        const unsigned char* p = der.data();
        X509* x509 = d2i_X509(nullptr, &p, static_cast<long>(der.size()));
        if (x509) {
            if (X509_STORE_add_cert(store, x509) == 1) ++added;
            X509_free(x509);
        }
    }
    if (added == 0) {
        error = "loaded 0 usable certs from the Windows ROOT store";
        return false;
    }
    return true;
}

} // namespace

struct TlsSocket::Impl {
    socket_t sock = kInvalidSocket;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
};

TlsSocket::TlsSocket() : impl_(new Impl) {
#ifdef _WIN32
    ensure_winsock_initialized();
#endif
}

TlsSocket::~TlsSocket() {
    close();
    delete impl_;
}

bool TlsSocket::connect(const std::string& host, unsigned short port, std::string& error) {
    // --- TCP connect (try every address getaddrinfo returns) -------------------
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        error = "getaddrinfo failed for host: " + host;
        return false;
    }

    socket_t sock = kInvalidSocket;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        sock = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == kInvalidSocket) continue;
        if (::connect(sock, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) break;
#ifdef _WIN32
        closesocket(sock);
#else
        ::close(sock);
#endif
        sock = kInvalidSocket;
    }
    freeaddrinfo(res);
    if (sock == kInvalidSocket) {
        error = "TCP connect failed to " + host + ":" + port_str;
        return false;
    }
    impl_->sock = sock;

    // --- TLS handshake -----------------------------------------------------------
    impl_->ctx = SSL_CTX_new(TLS_client_method());
    if (!impl_->ctx) {
        error = "SSL_CTX_new failed: " + openssl_error_string();
        return false;
    }
    SSL_CTX_set_min_proto_version(impl_->ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(impl_->ctx, SSL_VERIFY_PEER, nullptr);
    if (!load_trust_anchors(impl_->ctx, error)) return false;

    impl_->ssl = SSL_new(impl_->ctx);
    if (!impl_->ssl) {
        error = "SSL_new failed: " + openssl_error_string();
        return false;
    }
    SSL_set_tlsext_host_name(impl_->ssl, host.c_str()); // SNI
    SSL_set1_host(impl_->ssl, host.c_str());            // hostname verification target
    SSL_set_fd(impl_->ssl, static_cast<int>(impl_->sock));

    if (SSL_connect(impl_->ssl) != 1) {
        error = "TLS handshake failed: " + openssl_error_string();
        return false;
    }
    if (SSL_get_verify_result(impl_->ssl) != X509_V_OK) {
        error = "certificate verification failed";
        return false;
    }
    return true;
}

int TlsSocket::read(void* buf, std::size_t len) {
    if (!impl_->ssl) return -1;
    const int n = SSL_read(impl_->ssl, buf, static_cast<int>(len));
    return n > 0 ? n : (n == 0 ? 0 : -1);
}

bool TlsSocket::write_all(const void* buf, std::size_t len) {
    if (!impl_->ssl) return false;
    const auto* p = static_cast<const unsigned char*>(buf);
    std::size_t sent = 0;
    while (sent < len) {
        const int n = SSL_write(impl_->ssl, p + sent, static_cast<int>(len - sent));
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

void TlsSocket::close() {
    if (impl_->ssl) {
        SSL_shutdown(impl_->ssl);
        SSL_free(impl_->ssl);
        impl_->ssl = nullptr;
    }
    if (impl_->ctx) {
        SSL_CTX_free(impl_->ctx);
        impl_->ctx = nullptr;
    }
    if (impl_->sock != kInvalidSocket) {
#ifdef _WIN32
        closesocket(impl_->sock);
#else
        ::close(impl_->sock);
#endif
        impl_->sock = kInvalidSocket;
    }
}

} // namespace px::live
