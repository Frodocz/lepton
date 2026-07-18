#pragma once

/// @file ws_handshake_hash.h
/// @brief Sec-WebSocket-Accept computation for the WS upgrade handshake.
///
/// accept = base64( SHA1( sec_key_b64 + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" ) )
///
/// Lives in `security` (uses OpenSSL SHA1) so `net/http.h` stays crypto-free;
/// WsSession passes the sent key here to get the expected accept for validation.

#include "lepton/security/base64.h"

#include <cstddef>
#include <cstring>
#include <string_view>

#include <openssl/sha.h>

namespace lepton::security {

/// GUID defined by RFC 6455 §1.3.
inline constexpr std::string_view kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// Turn 16 random bytes into a base64 Sec-WebSocket-Key. `out` needs >= 25 bytes
/// (24 chars + NUL). Returns the length written (0 on overflow).
inline std::size_t make_sec_websocket_key(const uint8_t bytes[16], char* out,
                                          std::size_t out_cap) noexcept {
    return lepton::security::base64_encode(bytes, 16, out, out_cap);
}

/// Compute the expected Sec-WebSocket-Accept for a given sent key.
/// `out` needs >= 29 bytes (28 chars + NUL). Returns length written (0 on error).
inline std::size_t compute_ws_accept(std::string_view sec_key_b64, char* out,
                                     std::size_t out_cap) noexcept {
    // SHA1 over (key || GUID) without a heap allocation.
    unsigned char buf[64];  // 24 (key) + 36 (guid) = 60 <= 64
    const std::size_t total = sec_key_b64.size() + kWsGuid.size();
    if (total > sizeof(buf)) {
        return 0;
    }
    std::memcpy(buf, sec_key_b64.data(), sec_key_b64.size());
    std::memcpy(buf + sec_key_b64.size(), kWsGuid.data(), kWsGuid.size());

    unsigned char digest[SHA_DIGEST_LENGTH];  // 20 bytes
    ::SHA1(buf, total, digest);

    return lepton::security::base64_encode(digest, SHA_DIGEST_LENGTH, out, out_cap);
}

} // namespace lepton::security
