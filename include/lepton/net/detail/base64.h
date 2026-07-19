#pragma once

/// @file base64.h
/// @brief Minimal standard base64 encode (RFC 4648). Used for the
///        WebSocket Sec-WebSocket-Key.

#include <cstddef>
#include <cstdint>

namespace lepton::security {

/// Encode `n` input bytes into `out` (NUL-terminated). Returns the number of
/// characters written (excluding NUL), or 0 if `out_cap` is too small.
/// Required out_cap >= 4*((n+2)/3) + 1.
inline std::size_t base64_encode(const uint8_t* in, std::size_t n,
                                 char* out, std::size_t out_cap) noexcept {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    const std::size_t need = 4 * ((n + 2) / 3);
    if (out_cap < need + 1) {
        return 0;
    }

    std::size_t o = 0;
    std::size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
        out[o++] = kTable[(v >> 18) & 0x3F];
        out[o++] = kTable[(v >> 12) & 0x3F];
        out[o++] = kTable[(v >> 6) & 0x3F];
        out[o++] = kTable[v & 0x3F];
    }
    const std::size_t rem = n - i;
    if (rem == 1) {
        uint32_t v = uint32_t(in[i]) << 16;
        out[o++] = kTable[(v >> 18) & 0x3F];
        out[o++] = kTable[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
        out[o++] = kTable[(v >> 18) & 0x3F];
        out[o++] = kTable[(v >> 12) & 0x3F];
        out[o++] = kTable[(v >> 6) & 0x3F];
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

} // namespace lepton::security
