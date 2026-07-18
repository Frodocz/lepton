#include "lepton/net/http.h"
#include "lepton/base/lepton_error.h"
#include "lepton/base/logger.h"
#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>

namespace lepton::net {
namespace {

// 1. Test bitwise case-insensitive starts_with comparison
TEST(HttpTest, CaseInsensitiveIstartsWith) {
    EXPECT_TRUE(detail::istarts_with("content-length: 123", "content-length:"));
    EXPECT_TRUE(detail::istarts_with("Content-Length: 123", "content-length:"));
    EXPECT_TRUE(detail::istarts_with("CONTENT-LENGTH: 123", "content-length:"));
    EXPECT_FALSE(detail::istarts_with("content-len", "content-length:"));
    EXPECT_FALSE(detail::istarts_with("content-length", "content-length:"));
}

// 2. Test standard HTTP/1.1 WebSocket Upgrade request builder and response validator
TEST(HttpTest, WsUpgradeHandshake) {
    char req_buf[512];
    std::size_t n = build_ws_upgrade(req_buf, "localhost", "/chat", "dGhlIHNhbXBsZSBub25jZQ==");
    ASSERT_GT(n, 0u);
    std::string req(req_buf, n);
    EXPECT_NE(req.find("GET /chat HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(req.find("Host: localhost\r\n"), std::string::npos);
    EXPECT_NE(req.find("Upgrade: websocket\r\n"), std::string::npos);
    EXPECT_NE(req.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"), std::string::npos);

    // Valid response validation
    std::string resp = 
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    std::span<const char> resp_span{resp.data(), resp.size()};
    auto upgrade_res = validate_ws_upgrade(resp_span, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    EXPECT_TRUE(upgrade_res.complete);
    EXPECT_TRUE(upgrade_res.accepted);
    EXPECT_EQ(upgrade_res.headers_len, resp.size());
}

// 3. Test HTTP REST Request Builder (POST request with body and auto Content-Length)
TEST(HttpTest, BuildHttpRequestWithBody) {
    char req_buf[1024];
    std::string_view headers[] = {
        "Content-Type: application/json",
        "X-MBX-APIKEY: someapikey"
    };
    std::string body_str = "{\"symbol\":\"BTCUSDT\",\"price\":\"60000\"}";
    std::span<const uint8_t> body{reinterpret_cast<const uint8_t*>(body_str.data()), body_str.size()};

    std::size_t n = build_http_request(req_buf, HttpMethod::Post, "api.binance.com", "/api/v3/order", headers, body);
    ASSERT_GT(n, 0u);
    std::string req(req_buf, n);

    EXPECT_NE(req.find("POST /api/v3/order HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(req.find("Host: api.binance.com\r\n"), std::string::npos);
    EXPECT_NE(req.find("Content-Type: application/json\r\n"), std::string::npos);
    EXPECT_NE(req.find("Content-Length: 36\r\n"), std::string::npos);
    EXPECT_EQ(req.substr(req.size() - body_str.size()), body_str);
}

// 4. Test normal HTTP response parsing with Content-Length
TEST(HttpTest, ParseResponseWithContentLength) {
    std::string raw_resp = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 12\r\n\r\n"
        "hello world!";
    std::vector<uint8_t> buf(raw_resp.begin(), raw_resp.end());
    
    auto res = parse_http_response(buf);
    EXPECT_TRUE(res.complete);
    EXPECT_EQ(res.status, 200);
    EXPECT_EQ(res.consumed, raw_resp.size());
    std::string body(reinterpret_cast<const char*>(res.body.data()), res.body.size());
    EXPECT_EQ(body, "hello world!");
}

// 5. Test chunked HTTP response decoding in-place (no trailers)
TEST(HttpTest, ParseChunkedResponseNoTrailers) {
    std::string raw_resp = 
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "5\r\n"
        "hello\r\n"
        "6\r\n"
        " world\r\n"
        "0\r\n\r\n";
    std::vector<uint8_t> buf(raw_resp.begin(), raw_resp.end());
    
    auto res = parse_http_response(buf);
    EXPECT_TRUE(res.complete);
    EXPECT_EQ(res.status, 200);
    EXPECT_EQ(res.consumed, raw_resp.size());
    std::string body(reinterpret_cast<const char*>(res.body.data()), res.body.size());
    EXPECT_EQ(body, "hello world");
}

// 6. Test chunked HTTP response decoding in-place with trailing headers (Trailers)
TEST(HttpTest, ParseChunkedResponseWithTrailers) {
    std::string raw_resp = 
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "5\r\n"
        "hello\r\n"
        "6\r\n"
        " world\r\n"
        "0\r\n"
        "X-Trailer-Hash: abcdef123456\r\n"
        "X-Trailer-Second: foo\r\n"
        "\r\n";
    std::vector<uint8_t> buf(raw_resp.begin(), raw_resp.end());
    
    auto res = parse_http_response(buf);
    EXPECT_TRUE(res.complete);
    EXPECT_EQ(res.status, 200);
    EXPECT_EQ(res.consumed, raw_resp.size());
    std::string body(reinterpret_cast<const char*>(res.body.data()), res.body.size());
    EXPECT_EQ(body, "hello world");
}

// 7. Test malformed HTTP status lines are handled safely
TEST(HttpTest, ParseMalformedStatusLine) {
    // Scenario A: Non-numeric status characters
    std::string raw_resp_a = 
        "HTTP/1.1 BAD STATUS\r\n"
        "Content-Length: 0\r\n\r\n";
    std::vector<uint8_t> buf_a(raw_resp_a.begin(), raw_resp_a.end());
    auto res_a = parse_http_response(buf_a);
    EXPECT_TRUE(res_a.complete);
    EXPECT_EQ(res_a.status, 502); // Correctly fallback to 502 Bad Gateway

    // Scenario B: Status code is too short
    std::string raw_resp_b = 
        "HTTP/1.1 2\r\n"
        "Content-Length: 0\r\n\r\n";
    std::vector<uint8_t> buf_b(raw_resp_b.begin(), raw_resp_b.end());
    auto res_b = parse_http_response(buf_b);
    EXPECT_TRUE(res_b.complete);
    EXPECT_EQ(res_b.status, 502); // Correctly fallback to 502 Bad Gateway
}

} // namespace
} // namespace lepton::net
