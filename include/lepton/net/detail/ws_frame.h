#pragma once

/// @file ws_frame.h
/// @brief RFC 6455 frame encode/parse.
///
/// Parsing follows a strict length-before-read discipline (never dereference an
/// extended-length or mask-key field before confirming the buffer holds it) —
/// deliberately avoiding speculative over-reads. The encoder writes the 2..14
/// byte header into a caller-provided prefix so the payload is not moved (works
/// with IoBuffer::prepend + ws_mask).
/// Uses hw_endian.h for high-performance network-to-host conversions without loops.

#include "lepton/base/attributes.h"
#include "lepton/base/hw_endian.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace lepton::net {

enum class WsOpcode : uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    // 3-7 are reserved for future non-control frames
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xa,
    // 0xb-f are reserved for future control frames
};

[[nodiscard]] inline bool ws_is_control(WsOpcode op) noexcept {
    return (static_cast<uint8_t>(op) & 0x08u) != 0;
}

struct WsFrameHeader {
    bool fin{true};
    bool masked{false};
    WsOpcode opcode{WsOpcode::Text};
    uint64_t payload_len{0};
    uint8_t mask_key[4]{};
    std::size_t header_size{0}; ///< bytes the header occupies
};

/// Sentinel returned by ws_parse_header on a protocol violation.
inline constexpr std::size_t kWsParseError = std::numeric_limits<std::size_t>::max();

/// Parse a frame header from `buf`. Returns:
///   0             -> incomplete (need more bytes; call again later)
///   kWsParseError -> protocol violation (caller must fail the connection)
///   otherwise     -> header byte count; `out` is filled.
[[nodiscard]] inline std::size_t ws_parse_header(std::span<const uint8_t> buf,
                                                 WsFrameHeader& out) noexcept {
    const std::size_t len = buf.size();
    if (len < 2) [[unlikely]] {
        return 0;
    }
    const uint8_t b0 = buf[0];
    const uint8_t b1 = buf[1];

    // RSV1-3 must be zero (no extensions negotiated).
    if ((b0 & 0x70u) != 0) {
        return kWsParseError;
    }

    out.fin = (b0 & 0x80u) != 0;
    out.opcode = static_cast<WsOpcode>(b0 & 0x0Fu);

    // Reject reserved opcodes (3-7 non-control, 0xB-0xF control).
    switch (out.opcode) {
        case WsOpcode::Continuation:
        case WsOpcode::Text:
        case WsOpcode::Binary:
        case WsOpcode::Close:
        case WsOpcode::Ping:
        case WsOpcode::Pong:
            break;
        default:
            return kWsParseError;
    }

    out.masked = (b1 & 0x80u) != 0;
    uint64_t payload_len = b1 & 0x7Fu;
    std::size_t pos = 2;

    if (payload_len == 126) {
        if (len < 4) {
            return 0;
        }
        uint16_t raw_len;
        std::memcpy(&raw_len, buf.data() + 2, 2);
        payload_len = net_to_host_16(raw_len);
        
        // Minimal encoding verification: 16-bit format must represent sizes >= 126
        if (payload_len < 126) {
            return kWsParseError;
        }
        pos = 4;
    } else if (payload_len == 127) {
        if (len < 10) {
            return 0;
        }
        uint64_t raw_len;
        std::memcpy(&raw_len, buf.data() + 2, 8);
        payload_len = net_to_host_64(raw_len);
        
        // The most-significant bit of a 64-bit length MUST be 0 (RFC §5.2).
        // Minimal encoding verification: 64-bit format must represent sizes > 65535
        if ((payload_len & (uint64_t(1) << 63)) || payload_len <= 0xFFFF) {
            return kWsParseError;
        }
        pos = 10;
    }

    // Control frames: payload <= 125 and FIN must be set (RFC §5.5).
    if (ws_is_control(out.opcode) && (payload_len > 125 || !out.fin)) [[unlikely]] {
        return kWsParseError;
    }

    if (out.masked) {
        if (len < pos + 4) {
            return 0;
        }
        std::memcpy(out.mask_key, buf.data() + pos, 4);
        pos += 4;
    }

    out.payload_len = payload_len;
    out.header_size = pos;
    return pos;
}

/// Size of the header for a client frame of `payload_len` bytes (mask included).
[[nodiscard]] inline std::size_t ws_client_header_size(std::size_t payload_len) noexcept {
    std::size_t h = 2 + 4;  // base + 4-byte mask key (client frames are masked)
    if (payload_len > 0xFFFF) {
        h += 8;
    } else if (payload_len >= 126) {
        h += 2;
    }
    return h;
}

/// Encode a client frame header for `payload_len` into `hdr_dst` (must be
/// >= ws_client_header_size). Returns bytes written, or 0 if too small.
/// The payload is assumed to sit immediately after the returned header and is
/// masked in place by the caller (ws_mask.h) — keeping masking a separate step
/// so this codec stays pure and testable.
[[nodiscard]] inline std::size_t ws_encode_client_header(std::span<uint8_t> hdr_dst,
                                                         WsOpcode opcode,
                                                         std::size_t payload_len,
                                                         const uint8_t mask_key[4],
                                                         bool fin) noexcept {
    const std::size_t need = ws_client_header_size(payload_len);
    if (hdr_dst.size() < need) {
        return 0;
    }
    uint8_t* p = hdr_dst.data();

    p[0] = static_cast<uint8_t>((fin ? 0x80u : 0x00u) | static_cast<uint8_t>(opcode));

    std::size_t pos = 1;
    if (payload_len < 126) {
        p[pos++] = static_cast<uint8_t>(0x80u | payload_len);  // 0x80 = masked
    } else if (payload_len <= 0xFFFF) {
        p[pos++] = static_cast<uint8_t>(0x80u | 126u);
        uint16_t val = host_to_net_16(static_cast<uint16_t>(payload_len));
        std::memcpy(p + pos, &val, 2);
        pos += 2;
    } else {
        p[pos++] = static_cast<uint8_t>(0x80u | 127u);
        uint64_t val = host_to_net_64(static_cast<uint64_t>(payload_len));
        std::memcpy(p + pos, &val, 8);
        pos += 8;
    }
    std::memcpy(p + pos, mask_key, 4);
    pos += 4;
    return pos;
}

} // namespace lepton::net
