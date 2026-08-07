// live/win_root_certs.hpp — reads the Windows "ROOT" certificate store as raw
// DER blobs, so tls_socket.cpp can feed them into OpenSSL's X509_STORE.
//
// Deliberately isolated in its own translation unit that includes ONLY
// <wincrypt.h>, never any OpenSSL header: wincrypt.h #defines X509_NAME (and
// a few other identifiers) as macros, which collide with OpenSSL's own
// X509_NAME typedef if both are visible in the same TU. Keeping the Win32
// CryptoAPI calls here and handing back plain byte buffers sidesteps the
// collision entirely instead of fighting it with undef tricks.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace px::live {

// Each entry is one certificate, DER-encoded, exactly as stored in the Windows
// "ROOT" system store (Trusted Root Certification Authorities). Returns an
// empty vector and fills `error` if the store couldn't be opened.
[[nodiscard]] std::vector<std::vector<std::uint8_t>> load_windows_root_certs_der(std::string& error);

} // namespace px::live
