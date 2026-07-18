#include "lepton/net/ws_frame.h"
#include "lepton/net/ws_mask.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

namespace lepton::net {
namespace {

TEST(WsFrameTest, ParseBasicHeader) {
    // 1. Basic masked text frame with small payload (5 bytes)
    // FIN=1, RSV1-3=0, Opcode=Text (0x1) -> 0x81
    // Masked=1, PayloadLen=5 -> 0x85
    // Masking Key: 0x11, 0x22, 0x33, 0x44
    std::vector<uint8_t> buf = {0x81, 0x85, 0x11, 0x22, 0x33, 0x44, 'H', 'e', 'l', 'l', 'o'};
    WsFrameHeader hdr;
    std::size_t res = ws_parse_header(buf, hdr);
    EXPECT_EQ(res, 6u);
    EXPECT_TRUE(hdr.fin);
    EXPECT_TRUE(hdr.masked);
    EXPECT_EQ(hdr.opcode, WsOpcode::Text);
    EXPECT_EQ(hdr.payload_len, 5u);
    EXPECT_EQ(hdr.mask_key[0], 0x11);
    EXPECT_EQ(hdr.mask_key[1], 0x22);
    EXPECT_EQ(hdr.mask_key[2], 0x33);
    EXPECT_EQ(hdr.mask_key[3], 0x44);
    EXPECT_EQ(hdr.header_size, 6u);
}

TEST(WsFrameTest, ParseExtendedLength16) {
    // 2. 16-bit extended length (e.g. 500 bytes)
    // FIN=1, RSV1-3=0, Opcode=Binary (0x2) -> 0x82
    // Masked=0, PayloadLen=126 -> 0x7E (126)
    // Extended length: 500 -> 0x01, 0xF4
    std::vector<uint8_t> buf = {0x82, 0x7E, 0x01, 0xF4};
    WsFrameHeader hdr;
    std::size_t res = ws_parse_header(buf, hdr);
    EXPECT_EQ(res, 4u);
    EXPECT_TRUE(hdr.fin);
    EXPECT_FALSE(hdr.masked);
    EXPECT_EQ(hdr.opcode, WsOpcode::Binary);
    EXPECT_EQ(hdr.payload_len, 500u);
    EXPECT_EQ(hdr.header_size, 4u);
}

TEST(WsFrameTest, MinimalEncodingCheck16) {
    // Extended length 16 representing < 126 (e.g. 100 bytes) -> Protocol violation
    // FIN=1, RSV=0, Opcode=Binary -> 0x82
    // Masked=0, PayloadLen=126 -> 0x7E
    // Extended length: 100 -> 0x00, 0x64
    std::vector<uint8_t> buf = {0x82, 0x7E, 0x00, 0x64};
    WsFrameHeader hdr;
    std::size_t res = ws_parse_header(buf, hdr);
    EXPECT_EQ(res, kWsParseError);
}

TEST(WsFrameTest, ParseExtendedLength64) {
    // 3. 64-bit extended length (e.g. 70000 bytes, which exceeds 65535)
    // FIN=1, RSV=0, Opcode=Text -> 0x81
    // Masked=0, PayloadLen=127 -> 0x7F
    // Extended length: 70000 (0x0000000000011170)
    std::vector<uint8_t> buf = {0x81, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x11, 0x70};
    WsFrameHeader hdr;
    std::size_t res = ws_parse_header(buf, hdr);
    EXPECT_EQ(res, 10u);
    EXPECT_TRUE(hdr.fin);
    EXPECT_FALSE(hdr.masked);
    EXPECT_EQ(hdr.opcode, WsOpcode::Text);
    EXPECT_EQ(hdr.payload_len, 70000u);
}

TEST(WsFrameTest, MinimalEncodingCheck64) {
    // Extended length 64 representing <= 65535 (e.g. 5000 bytes) -> Protocol violation
    // FIN=1, RSV=0, Opcode=Text -> 0x81
    // Masked=0, PayloadLen=127 -> 0x7F
    // Extended length: 5000 (0x0000000000001388)
    std::vector<uint8_t> buf = {0x81, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0x88};
    WsFrameHeader hdr;
    std::size_t res = ws_parse_header(buf, hdr);
    EXPECT_EQ(res, kWsParseError);
}

TEST(WsFrameTest, RSVCheck) {
    // RSV1 set -> 0xF1 (11110001) instead of 0x81 (10000001) -> Protocol violation
    std::vector<uint8_t> buf = {0xF1, 0x05, 0x11, 0x22, 0x33, 0x44, 'H', 'e', 'l', 'l', 'o'};
    WsFrameHeader hdr;
    std::size_t res = ws_parse_header(buf, hdr);
    EXPECT_EQ(res, kWsParseError);
}

TEST(WsFrameTest, ControlFrameChecks) {
    // Ping with payload > 125 -> Protocol violation
    std::vector<uint8_t> buf = {0x89, 0x7E, 0x00, 0x80}; // Opcode=Ping, Len=128
    WsFrameHeader hdr;
    std::size_t res = ws_parse_header(buf, hdr);
    EXPECT_EQ(res, kWsParseError);

    // Fragmented Ping (FIN=0) -> Protocol violation
    std::vector<uint8_t> buf2 = {0x09, 0x05, 0x11, 0x22, 0x33, 0x44, 'H', 'e', 'l', 'l', 'o'};
    res = ws_parse_header(buf2, hdr);
    EXPECT_EQ(res, kWsParseError);
}

TEST(WsFrameTest, EncodeClientHeader) {
    uint8_t mask_key[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t dst[32];
    
    // Encode small frame (50 bytes)
    std::size_t res = ws_encode_client_header(dst, WsOpcode::Text, 50, mask_key, true);
    EXPECT_EQ(res, 6u); // 2 base + 4 mask
    EXPECT_EQ(dst[0], 0x81); // FIN, Text
    EXPECT_EQ(dst[1], 0x80 | 50); // Masked, Len=50
    EXPECT_EQ(std::memcmp(dst + 2, mask_key, 4), 0);

    // Encode 16-bit extended frame (500 bytes)
    res = ws_encode_client_header(dst, WsOpcode::Binary, 500, mask_key, true);
    EXPECT_EQ(res, 8u); // 2 base + 2 ext + 4 mask
    EXPECT_EQ(dst[0], 0x82);
    EXPECT_EQ(dst[1], 0x80 | 126);
    EXPECT_EQ(dst[2], 0x01); // 500 >> 8
    EXPECT_EQ(dst[3], 0xF4); // 500 & 0xFF
    EXPECT_EQ(std::memcmp(dst + 4, mask_key, 4), 0);

    // Encode 64-bit extended frame (70000 bytes)
    res = ws_encode_client_header(dst, WsOpcode::Text, 70000, mask_key, true);
    EXPECT_EQ(res, 14u); // 2 base + 8 ext + 4 mask
    EXPECT_EQ(dst[0], 0x81);
    EXPECT_EQ(dst[1], 0x80 | 127);
    uint64_t val;
    std::memcpy(&val, dst + 2, 8);
    EXPECT_EQ(net_to_host_64(val), 70000u);
    EXPECT_EQ(std::memcmp(dst + 10, mask_key, 4), 0);
}

// Reference implementation for masking validation (pure byte-by-byte scalar)
void reference_mask(uint8_t* data, std::size_t len, const uint8_t key[4], std::size_t offset = 0) {
    for (std::size_t i = 0; i < len; ++i) {
        data[i] ^= key[(offset + i) & 3];
    }
}

// Isolated scalar-only implementation
void ws_mask_scalar_only(uint8_t* data, std::size_t len, const uint8_t key_bytes[4], std::size_t key_offset = 0) {
    for (std::size_t i = 0; i < len; ++i) {
        data[i] ^= key_bytes[(key_offset + i) & 3];
    }
}

// Isolated 64-bit-XOR-only implementation (skipping AVX2 completely)
void ws_mask_64bit_only(uint8_t* data, std::size_t len, const uint8_t key_bytes[4], std::size_t key_offset = 0) {
    std::size_t i = 0;
    if (len >= 8) {
        uint8_t rot[4];
        for (int k = 0; k < 4; ++k) {
            rot[k] = key_bytes[(key_offset + i + static_cast<std::size_t>(k)) & 3];
        }
        uint32_t pat;
        std::memcpy(&pat, rot, 4);
        uint64_t mask64 = (static_cast<uint64_t>(pat) << 32) | pat;

        for (; i + 8 <= len; i += 8) {
            uint64_t chunk;
            std::memcpy(&chunk, data + i, 8);
            chunk ^= mask64;
            std::memcpy(data + i, &chunk, 8);
        }
    }
    for (; i < len; ++i) {
        data[i] ^= key_bytes[(key_offset + i) & 3];
    }
}

// Isolated AVX2-only implementation (skipping 64-bit-XOR and falling back directly to scalar tail)
void ws_mask_avx2_only(uint8_t* data, std::size_t len, const uint8_t key_bytes[4], std::size_t key_offset = 0) {
    std::size_t i = 0;
#if defined(__AVX2__)
    if (len >= 32) {
        uint8_t rot[4];
        for (int k = 0; k < 4; ++k) {
            rot[k] = key_bytes[(key_offset + static_cast<std::size_t>(k)) & 3];
        }
        uint32_t pat;
        std::memcpy(&pat, rot, 4);
        const __m256i vkey = _mm256_set1_epi32(static_cast<int>(pat));

        for (; i + 32 <= len; i += 32) {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
            v = _mm256_xor_si256(v, vkey);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(data + i), v);
        }
    }
#endif
    for (; i < len; ++i) {
        data[i] ^= key_bytes[(key_offset + i) & 3];
    }
}

TEST(WsMaskTest, MaskEquivalence) {
    const uint8_t mask_key[4] = {0x12, 0x34, 0x56, 0x78};
    std::mt19937 rng(42);
    
    // Test sizes spanning scalar tail, 64-bit tail, and AVX2 vector paths
    std::vector<std::size_t> test_sizes = {
        0, 1, 3, 7, 8, 9, 15, 16, 23, 24, 31, 32, 33, 47, 48, 63, 64, 65, 127, 128, 129, 250, 1000
    };

    for (std::size_t size : test_sizes) {
        for (std::size_t offset : {0u, 1u, 2u, 3u}) {
            // Generate random test data
            std::vector<uint8_t> base_data(size);
            for (std::size_t j = 0; j < size; ++j) {
                base_data[j] = static_cast<uint8_t>(rng() & 0xFF);
            }
            
            // 1. Get reference masked data
            std::vector<uint8_t> ref_data = base_data;
            reference_mask(ref_data.data(), size, mask_key, offset);

            // 2. Validate scalar-only path
            std::vector<uint8_t> scalar_data = base_data;
            ws_mask_scalar_only(scalar_data.data(), size, mask_key, offset);
            EXPECT_EQ(scalar_data, ref_data) << "Scalar-only path mismatch at size " << size;

            // 3. Validate 64-bit-XOR-only path
            std::vector<uint8_t> bit64_data = base_data;
            ws_mask_64bit_only(bit64_data.data(), size, mask_key, offset);
            EXPECT_EQ(bit64_data, ref_data) << "64-bit-XOR-only path mismatch at size " << size;

            // 4. Validate AVX2-only path (if AVX2 is compiled)
            std::vector<uint8_t> avx2_data = base_data;
            ws_mask_avx2_only(avx2_data.data(), size, mask_key, offset);
            EXPECT_EQ(avx2_data, ref_data) << "AVX2-only path mismatch at size " << size;

            // 5. Validate library's integrated ws_mask (combining AVX2 + 64-bit tail + scalar tail)
            std::vector<uint8_t> lib_data = base_data;
            ws_mask(lib_data.data(), size, mask_key, offset);
            EXPECT_EQ(lib_data, ref_data) << "Library ws_mask mismatch at size " << size << ", offset " << offset;
        }
    }
}

TEST(WsMaskTest, MaskKeyGenerator) {
    MaskKeyGen gen;
    uint32_t key1 = gen.next();
    uint32_t key2 = gen.next();
    uint32_t key3 = gen.next();
    
    // Assert keys are not zero and are not equal consecutive values (highly likely)
    EXPECT_NE(key1, 0u);
    EXPECT_NE(key2, 0u);
    EXPECT_NE(key1, key2);
    EXPECT_NE(key2, key3);
}

} // namespace
} // namespace lepton::net
