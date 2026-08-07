// live/base64.hpp — thin wrapper over OpenSSL's EVP_EncodeBlock/DecodeBlock.
// Shared by kalshi_auth.cpp (signature encoding) and ws_client.cpp (the
// Sec-WebSocket-Key/Accept handshake) rather than duplicated in both.
#pragma once

#include <openssl/evp.h>

#include <string>

namespace px::live {

inline std::string base64_encode(const unsigned char* data, int len) {
    std::string out;
    out.resize(static_cast<std::size_t>(4 * ((len + 2) / 3)) + 1);
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, len);
    out.resize(static_cast<std::size_t>(n));
    return out;
}

// Decodes base64 `in` into raw bytes. Note EVP_DecodeBlock always emits a
// multiple of 3 bytes and pads internally with zero bits for the last group,
// so trailing '=' padding in `in` must be trimmed by the caller from the
// *encoded* length first if an exact decoded length matters — the two call
// sites here (WS accept-key comparison, which re-encodes to compare) don't
// need that trim.
inline std::string base64_decode(const std::string& in) {
    std::string out;
    out.resize(static_cast<std::size_t>(3 * (in.size() / 4)) + 1);
    const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                   reinterpret_cast<const unsigned char*>(in.data()),
                                   static_cast<int>(in.size()));
    if (n < 0) { out.clear(); return out; }
    out.resize(static_cast<std::size_t>(n));
    return out;
}

} // namespace px::live
