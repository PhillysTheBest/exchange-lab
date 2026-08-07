// live/ws_client.hpp — RFC 6455 WebSocket client over TlsSocket.
//
// Implements exactly what this project needs against Kalshi's WS endpoint:
// the opening HTTP Upgrade handshake (with caller-supplied extra headers, for
// the KALSHI-ACCESS-* auth headers), masked client->server text frames, and
// unmasked server->client frame reassembly across continuation frames. Pings
// are answered with pongs automatically inside receive_message(); close
// frames terminate the read loop.
#pragma once

#include "tls_socket.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace px::live {

class WebSocketClient {
public:
    struct Header {
        std::string name;
        std::string value;
    };

    // Performs the TCP+TLS connect, then the WS opening handshake: an HTTP GET
    // to `path` with the standard Upgrade/Connection/Sec-WebSocket-* headers
    // plus `extra_headers`, validating the server's 101 response and its
    // Sec-WebSocket-Accept value. Returns false with `error` set on any failure.
    [[nodiscard]] bool connect(const std::string& host, unsigned short port, const std::string& path,
                                const std::vector<Header>& extra_headers, std::string& error);

    [[nodiscard]] bool send_text(const std::string& payload);

    // Blocks for the next complete text/binary message (concatenated across any
    // continuation frames). Returns false on error or a received close frame.
    [[nodiscard]] bool receive_message(std::string& out, std::string& error);

    void close();

private:
    [[nodiscard]] bool send_frame(std::uint8_t opcode, const void* payload, std::size_t len);
    // Reads from sock_ until recv_buf_ (from recv_pos_ onward) has >= n bytes
    // buffered. Frame/header boundaries never line up with TLS read boundaries,
    // so all parsing works off this buffer rather than raw socket reads.
    [[nodiscard]] bool fill_at_least(std::size_t n);

    TlsSocket sock_;
    std::string host_;
    std::string recv_buf_;
    std::size_t recv_pos_ = 0;
};

} // namespace px::live
