#pragma once

/// @file hw_endian.h
/// @brief Fast byte-swapping and network-to-host endianness conversions.
///
/// Adapted to lepton namespace and LEPTON_ALWAYS_INLINE.
/// Uses standard C++20 <bit> (std::endian) and if constexpr to determine
/// host endianness at compile-time, eliminating preprocessor macros.

#include "lepton/base/attributes.h"

#include <bit>
#include <cstdint>

namespace lepton {

static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big,
              "Mixed-endian environments are not supported by lepton");

LEPTON_ALWAYS_INLINE constexpr uint16_t byte_swap_16(uint16_t x) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap16(x);
#elif defined(_MSC_VER)
    return _byteswap_ushort(x);
#else
    return static_cast<uint16_t>((x >> 8) | (x << 8));
#endif
}
static_assert(byte_swap_16(0x1234U) == 0x3412U);

LEPTON_ALWAYS_INLINE constexpr uint32_t byte_swap_32(uint32_t x) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(x);
#elif defined(_MSC_VER)
    return _byteswap_ulong(x);
#else
    return ((x >> 24) & 0xff) |
           ((x >> 8) & 0xff00) |
           ((x << 8) & 0xff0000) |
           ((x << 24) & 0xff000000);
#endif
}
static_assert(byte_swap_32(0x12345678U) == 0x78563412U);

LEPTON_ALWAYS_INLINE constexpr uint64_t byte_swap_64(uint64_t x) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(x);
#elif defined(_MSC_VER)
    return _byteswap_uint64(x);
#else
    return ((x >> 56) & 0xffULL) |
           ((x >> 48) & 0xff00ULL) |
           ((x >> 40) & 0xff0000ULL) |
           ((x >> 32) & 0xff000000ULL) |
           ((x << 8) & 0xff00000000ULL) |
           ((x << 24) & 0xff0000000000ULL) |
           ((x << 40) & 0xff000000000000ULL) |
           ((x << 56) & 0xff00000000000000ULL);
#endif
}
static_assert(byte_swap_64(0x1234567876543210ULL) == 0x1032547678563412ULL);

LEPTON_ALWAYS_INLINE constexpr uint16_t net_to_host_16(uint16_t x) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return byte_swap_16(x);
    } else {
        return x;
    }
}

LEPTON_ALWAYS_INLINE constexpr uint16_t host_to_net_16(uint16_t x) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return byte_swap_16(x);
    } else {
        return x;
    }
}

LEPTON_ALWAYS_INLINE constexpr uint32_t net_to_host_32(uint32_t x) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return byte_swap_32(x);
    } else {
        return x;
    }
}

LEPTON_ALWAYS_INLINE constexpr uint32_t host_to_net_32(uint32_t x) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return byte_swap_32(x);
    } else {
        return x;
    }
}

LEPTON_ALWAYS_INLINE constexpr uint64_t net_to_host_64(uint64_t x) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return byte_swap_64(x);
    } else {
        return x;
    }
}

LEPTON_ALWAYS_INLINE constexpr uint64_t host_to_net_64(uint64_t x) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return byte_swap_64(x);
    } else {
        return x;
    }
}

} // namespace lepton
