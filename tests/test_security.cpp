#include "lepton/base/lepton_error.h"
#include "lepton/base/logger.h"
#include "lepton/security/tls_context.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace lepton::security {
namespace {

// 1. Test successful initialization with standard list of ALPN names
TEST(TlsContextTest, BasicInitialization) {
    TlsContext::Options opts;
    opts.verify_peer = false;
    opts.alpn = {"h2", "http/1.1"};
    
    LEPTON_TRY {
        TlsContext ctx(opts);
        EXPECT_TRUE(ctx.valid());
        EXPECT_FALSE(ctx.verify_peer());
        EXPECT_NE(ctx.raw(), nullptr);
    }
    LEPTON_CATCH_ALL() {
        FAIL() << "TLS Context initialization threw an unexpected exception for valid ALPN";
    }
}

// 2. Test successful initialization with empty or null ALPN list
TEST(TlsContextTest, EmptyAndSeparatorOnlyAlpn) {
    TlsContext::Options opts;
    opts.verify_peer = false;
    
    // Scenario A: Empty list
    opts.alpn = {};
    LEPTON_TRY {
        TlsContext ctx(opts);
        EXPECT_TRUE(ctx.valid());
    }
    LEPTON_CATCH_ALL() {
        FAIL() << "Failed to initialize TlsContext with empty ALPN";
    }

    // Scenario B: Empty strings within the list (should skip empty elements cleanly)
    opts.alpn = {"", "h2", "", "http/1.1", ""};
    LEPTON_TRY {
        TlsContext ctx(opts);
        EXPECT_TRUE(ctx.valid());
    }
    LEPTON_CATCH_ALL() {
        FAIL() << "Failed to initialize TlsContext with empty strings in ALPN list";
    }
}

// 3. Test that ALPN list size exceeding the 128 bytes limit falls back to heap path and initializes successfully
TEST(TlsContextTest, LargeAlpnFallbackPath) {
    TlsContext::Options opts;
    opts.verify_peer = false;

    // Generate a list of protocols that exceeds 128 bytes in total wire format (e.g. 10 protocols of 15 bytes each)
    std::vector<std::string> storage;
    for (int i = 0; i < 10; ++i) {
        storage.push_back(lepton::fmt::format("proto_version_{}", i)); // 15 chars
    }
    
    std::vector<std::string_view> alpn_list;
    for (const auto& s : storage) {
        alpn_list.push_back(s);
    }
    opts.alpn = alpn_list;

    LEPTON_TRY {
        TlsContext ctx(opts);
        EXPECT_TRUE(ctx.valid());
    }
    LEPTON_CATCH_ALL() {
        FAIL() << "Failed to initialize TlsContext with large ALPN list (heap fallback path)";
    }
}

// 4. Test that an individual ALPN protocol name exceeding 255 bytes limit throws LeptonError
TEST(TlsContextTest, ProtocolTooLongThrows) {
    TlsContext::Options opts;
    opts.verify_peer = false;

    // 260 character protocol name
    std::string long_proto(260, 'a');
    opts.alpn = {long_proto};

    bool threw = false;
    LEPTON_TRY {
        TlsContext ctx(opts);
    }
    LEPTON_CATCH(const lepton::LeptonError& e) {
        threw = true;
        EXPECT_NE(std::string(e.what()).find("exceeds maximum RFC 7301 limit"), std::string::npos);
    }
    LEPTON_CATCH_ALL() {
        FAIL() << "Threw unexpected exception type for invalid ALPN size";
    }
    EXPECT_TRUE(threw) << "Expected TlsContext to throw LeptonError for 260-byte ALPN protocol name";
}

// 5. Test that a nonexistent CA path correctly throws LeptonError (short path, stack-allocated check)
TEST(TlsContextTest, NonexistentCaPathThrows) {
    TlsContext::Options opts;
    opts.verify_peer = true;
    opts.ca_file = "/tmp/nonexistent_ca_bundle_12345.pem"; // Under 256 bytes

    bool threw = false;
    LEPTON_TRY {
        TlsContext ctx(opts);
    }
    LEPTON_CATCH(const lepton::LeptonError& e) {
        threw = true;
        EXPECT_NE(std::string(e.what()).find("load_verify_locations failed"), std::string::npos);
    }
    LEPTON_CATCH_ALL() {
        FAIL() << "Threw unexpected exception type for invalid CA path";
    }
    EXPECT_TRUE(threw) << "Expected TlsContext to throw LeptonError for nonexistent CA file";
}

// 6. Test that a nonexistent CA path correctly throws LeptonError (long path, fallback heap-allocated check)
TEST(TlsContextTest, LongNonexistentCaPathThrows) {
    TlsContext::Options opts;
    opts.verify_peer = true;
    // Generate a nonexistent path of 300 characters using lepton::fmt::format (exceeds the 256-byte stack limit)
    std::string long_path = lepton::fmt::format("/tmp/{}.pem", std::string(280, 'a'));
    opts.ca_file = long_path;

    bool threw = false;
    LEPTON_TRY {
        TlsContext ctx(opts);
    }
    LEPTON_CATCH(const lepton::LeptonError& e) {
        threw = true;
        EXPECT_NE(std::string(e.what()).find("load_verify_locations failed"), std::string::npos);
    }
    LEPTON_CATCH_ALL() {
        FAIL() << "Threw unexpected exception type for long invalid CA path";
    }
    EXPECT_TRUE(threw) << "Expected TlsContext to throw LeptonError for long nonexistent CA file";
}

// 7. Test successful initialization with default system verify paths
TEST(TlsContextTest, DefaultVerifyPaths) {
    TlsContext::Options opts;
    opts.verify_peer = true;
    opts.ca_file = ""; // Empty CA file forces using default system CAs

    LEPTON_TRY {
        TlsContext ctx(opts);
        EXPECT_TRUE(ctx.valid());
        EXPECT_TRUE(ctx.verify_peer());
    }
    LEPTON_CATCH_ALL() {
        FAIL() << "Failed to initialize TlsContext with default system verify paths";
    }
}

} // namespace
} // namespace lepton::security
