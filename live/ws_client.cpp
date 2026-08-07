#include "ws_client.hpp"
#include "base64.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>

namespace px::live {

namespace {
constexpr const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"; // RFC 6455 magic

std::string sha1_base64(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);
    return base64_encode(digest, static_cast<int>(digest_len));
}

std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
} // namespace

bool WebSocketClient::fill_at_least(std::size_t n) {
    while (recv_buf_.size() - recv_pos_ < n) {
        char tmp[4096];
        const int got = sock_.read(tmp, sizeof(tmp));
        if (got <= 0) return false;
        recv_buf_.append(tmp, static_cast<std::size_t>(got));
    }
    return true;
}

bool WebSocketClient::connect(const std::string& host, unsigned short port, const std::string& path,
                               const std::vector<Header>& extra_headers, std::string& error) {
    host_ = host;
    if (!sock_.connect(host, port, error)) return false;

    unsigned char key_bytes[16];
    if (RAND_bytes(key_bytes, sizeof(key_bytes)) != 1) {
        error = "RAND_bytes failed";
        return false;
    }
    const std::string ws_key = base64_encode(key_bytes, sizeof(key_bytes));

    std::string req = "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + ws_key + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    for (const auto& h : extra_headers) req += h.name + ": " + h.value + "\r\n";
    req += "\r\n";

    if (!sock_.write_all(req.data(), req.size())) {
        error = "failed to send WS handshake request";
        return false;
    }

    // Buffer bytes until the header/body separator shows up.
    std::size_t header_end = std::string::npos;
    for (;;) {
        header_end = recv_buf_.find("\r\n\r\n", recv_pos_);
        if (header_end != std::string::npos) break;
        if (!fill_at_least(recv_buf_.size() - recv_pos_ + 1)) {
            error = "connection closed during WS handshake";
            return false;
        }
    }
    const std::string response = recv_buf_.substr(recv_pos_, header_end - recv_pos_);
    recv_pos_ = header_end + 4;
    // HTTP header *names* (not values) are case-insensitive (RFC 7230 §3.2); real
    // servers vary (e.g. "sec-websocket-accept:" all-lowercase is common) — search
    // a lowercased copy but extract from the original so header values keep their
    // exact casing (the accept token itself is a case-sensitive base64 string).
    const std::string response_lower = to_lower_copy(response);

    if (response_lower.compare(0, 12, "http/1.1 101") != 0 && response_lower.compare(0, 12, "http/1.0 101") != 0) {
        const auto line_end = response.find("\r\n");
        error = "WS handshake rejected: " + response.substr(0, line_end);
        return false;
    }

    const std::string accept_header = "sec-websocket-accept:";
    auto pos = response_lower.find(accept_header);
    if (pos == std::string::npos) {
        error = "missing Sec-WebSocket-Accept header in handshake response";
        return false;
    }
    pos += accept_header.size();
    while (pos < response.size() && response[pos] == ' ') ++pos;
    const auto line_end = response.find("\r\n", pos);
    const std::string server_accept = response.substr(pos, line_end - pos);
    const std::string expected_accept = sha1_base64(ws_key + kWsGuid);

    if (server_accept != expected_accept) {
        error = "Sec-WebSocket-Accept mismatch — possible proxy/middlebox tampering";
        return false;
    }
    return true;
}

bool WebSocketClient::send_frame(std::uint8_t opcode, const void* payload, std::size_t len) {
    std::string out;
    out.push_back(static_cast<char>(0x80 | (opcode & 0x0F))); // FIN=1

    unsigned char mask_key[4];
    RAND_bytes(mask_key, sizeof(mask_key)); // client frames MUST be masked (RFC 6455 §5.1)

    if (len < 126) {
        out.push_back(static_cast<char>(0x80 | len));
    } else if (len <= 0xFFFF) {
        out.push_back(static_cast<char>(0x80 | 126));
        out.push_back(static_cast<char>((len >> 8) & 0xFF));
        out.push_back(static_cast<char>(len & 0xFF));
    } else {
        out.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
    }
    out.append(reinterpret_cast<const char*>(mask_key), 4);

    const auto* p = static_cast<const unsigned char*>(payload);
    const std::size_t offset = out.size();
    out.resize(offset + len);
    for (std::size_t i = 0; i < len; ++i) out[offset + i] = static_cast<char>(p[i] ^ mask_key[i % 4]);

    return sock_.write_all(out.data(), out.size());
}

bool WebSocketClient::send_text(const std::string& payload) {
    return send_frame(0x1, payload.data(), payload.size());
}

bool WebSocketClient::receive_message(std::string& out, std::string& error) {
    out.clear();

    for (;;) {
        if (!fill_at_least(2)) {
            error = "connection closed while reading frame header";
            return false;
        }
        const auto b0 = static_cast<unsigned char>(recv_buf_[recv_pos_]);
        const auto b1 = static_cast<unsigned char>(recv_buf_[recv_pos_ + 1]);
        const bool fin = (b0 & 0x80) != 0;
        const std::uint8_t opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0; // server frames shouldn't be masked; tolerate if they are
        std::uint64_t payload_len = b1 & 0x7F;
        std::size_t header_len = 2;

        if (payload_len == 126) {
            if (!fill_at_least(4)) { error = "truncated frame header"; return false; }
            payload_len = (static_cast<std::uint64_t>(static_cast<unsigned char>(recv_buf_[recv_pos_ + 2])) << 8)
                        |  static_cast<std::uint64_t>(static_cast<unsigned char>(recv_buf_[recv_pos_ + 3]));
            header_len = 4;
        } else if (payload_len == 127) {
            if (!fill_at_least(10)) { error = "truncated frame header"; return false; }
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) | static_cast<unsigned char>(recv_buf_[recv_pos_ + 2 + i]);
            }
            header_len = 10;
        }

        const std::size_t mask_len = masked ? 4 : 0;
        if (!fill_at_least(header_len + mask_len + payload_len)) {
            error = "truncated frame payload";
            return false;
        }

        const unsigned char* mask_key =
            masked ? reinterpret_cast<const unsigned char*>(recv_buf_.data() + recv_pos_ + header_len) : nullptr;
        const char* payload_ptr = recv_buf_.data() + recv_pos_ + header_len + mask_len;

        std::string payload(payload_ptr, static_cast<std::size_t>(payload_len));
        if (masked) {
            for (std::size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask_key[i % 4]);
            }
        }

        recv_pos_ += header_len + mask_len + static_cast<std::size_t>(payload_len);
        if (recv_pos_ > (1u << 16)) { // periodic compaction — not on a hot path
            recv_buf_.erase(0, recv_pos_);
            recv_pos_ = 0;
        }

        if (opcode == 0x9) { // ping -> pong (best-effort; failure surfaces on the next real send/recv)
            (void)send_frame(0xA, payload.data(), payload.size());
            continue;
        }
        if (opcode == 0xA) { // unsolicited pong
            continue;
        }
        if (opcode == 0x8) { // close
            (void)send_frame(0x8, payload.data(), payload.size() <= 125 ? payload.size() : 0);
            error = "peer sent a WebSocket close frame";
            return false;
        }

        out += payload;
        if (fin) return true;
        // else: more continuation frames follow — loop without resetting `out`
    }
}

void WebSocketClient::close() {
    sock_.close();
}

} // namespace px::live
