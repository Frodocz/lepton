#pragma once

/// @file ws_mask.h
/// @brief WebSocket client-frame masking (RFC 6455 §5.3).
///
/// Masking is part of the WebSocket *framing* protocol (a mandatory XOR on
/// client→server frames), not cryptography.
///
/// In-place XOR with an AVX2 fast path (scalar fallback), with correct
/// mask-key rotation across chunk boundaries.
/// The per-frame 32-bit key comes from a fast, loop-local PRNG seeded once
/// from the OS — NOT a per-frame CSPRNG call (too slow on the order hot
/// path) and NOT a constant zero mask (non-conformant and fingerprintable).

#include "lepton/base/attributes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace lepton::net {

/// @brief SplitMix64 Pseudo-Random Number Generator (PRNG).
/// High statistical quality (passes BigCrush), 64-bit state, period 2^64.
/// Requires 2 64-bit multiplications per next() call.
class SplitMix64 {
public:
    explicit SplitMix64(uint64_t seed) noexcept : state_{seed} {}

    LEPTON_ALWAYS_INLINE uint32_t next() noexcept {
        uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31);
        return static_cast<uint32_t>(z >> 32);
    }

private:
    uint64_t state_;
};

/// @brief PCG-XSH-RR 64/32 Pseudo-Random Number Generator (PRNG).
/// Superior statistical qualities, harder to predict than SplitMix64 due to
/// state-dependent bit-rotation permutation, and faster (only 1 64-bit multiplication).
class PcgXshRr6432 {
public:
    explicit PcgXshRr6432(uint64_t seed) noexcept : state_{seed} {}

    LEPTON_ALWAYS_INLINE uint32_t next() noexcept {
        uint64_t oldstate = state_;
        // Multiplier from PCG paper, odd increment to guarantee full period
        state_ = oldstate * 6364136223846793005ULL + 1442695040888963407ULL;
        
        uint32_t xorshifted = static_cast<uint32_t>(((oldstate >> 18u) ^ oldstate) >> 27u);
        uint32_t rot = static_cast<uint32_t>(oldstate >> 59u);
        // Warning-free bitwise rotate-right: recognized by compilers as a single 'ror' or 'rorx' instruction
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

private:
    uint64_t state_;
};

/// Fast per-frame mask-key generator. One instance per session (loop-local, so
/// no synchronization). Seeded once from the OS; next() is a few ALU ops.
class MaskKeyGen {
public:
    MaskKeyGen() noexcept : impl_{seed_from_os()} {}

    /// Retrieve the next 32-bit mask key.
    /// Defaults to PcgXshRr6432 as it is statistically safer and faster than SplitMix64.
    LEPTON_ALWAYS_INLINE uint32_t next() noexcept {
        return impl_.next();
    }

private:
    static uint64_t seed_from_os() noexcept;

    // PCG is the fastest and safest default generator.
    // To switch back to SplitMix64, change this type to SplitMix64.
    PcgXshRr6432 impl_;
};

/// XOR-mask `data[0..len)` in place. `key_bytes` is the 4-byte masking key laid
/// out as it appears on the wire (network order in the frame). `key_offset` is
/// the payload index at which masking resumes (0 for a fresh frame; used when a
/// frame is masked in more than one call) so the key rotation stays aligned.
LEPTON_ALWAYS_INLINE void ws_mask(uint8_t* data, std::size_t len,
                                  const uint8_t key_bytes[4],
                                  std::size_t key_offset = 0) noexcept {
    if (len == 0) {
        return;
    }

    std::size_t i = 0;

#if defined(__AVX2__)
    if (len >= 32) {
        // Construct a rotated 4-byte pattern for the current offset, then
        // broadcast to 256 bits. Because 32 is a multiple of 4, the pattern
        // repeats cleanly across the vector and across iterations.
        uint8_t rot[4];
        for (int k = 0; k < 4; ++k) {
            rot[k] = key_bytes[(key_offset + static_cast<std::size_t>(k)) & 3];
        }
        uint32_t pat;
        std::memcpy(&pat, rot, 4);
        const __m256i vkey = _mm256_set1_epi32(static_cast<int>(pat));

        for (; i + 32 <= len; i += 32) {
            // Note: __m256i in GCC has __attribute__((__may_alias__)) and can be safely casted
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
            v = _mm256_xor_si256(v, vkey);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(data + i), v);
        }
    }
#endif

    // 64-bit (8-byte) XOR quick-tail path for remainder >= 8 bytes (or small buffers)
    if (i + 8 <= len) {
        uint8_t rot[4];
        for (int k = 0; k < 4; ++k) {
            rot[k] = key_bytes[(key_offset + i + static_cast<std::size_t>(k)) & 3];
        }
        uint32_t pat;
        std::memcpy(&pat, rot, 4);
        uint64_t mask64 = (static_cast<uint64_t>(pat) << 32) | pat;

        for (; i + 8 <= len; i += 8) {
            uint64_t chunk;
            // Use std::memcpy to avoid Undefined Behavior (Strict Aliasing violations)
            std::memcpy(&chunk, data + i, 8);
            chunk ^= mask64;
            std::memcpy(data + i, &chunk, 8);
        }
    }

    // Scalar tail (remaining < 8 bytes)
    for (; i < len; ++i) {
        data[i] ^= key_bytes[(key_offset + i) & 3];
    }
}

} // namespace lepton::net
