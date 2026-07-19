#include "lepton/base/buffer_pool.h"
#include "lepton/net/endpoint.h"
#include "lepton/net/event_loop.h"
#include "lepton/net/http_client.h"
#include "lepton/net/stream.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace lepton::net {

// Reuse MockStream from test_ws_session.cpp, defined locally here to keep tests independent.
class MockHttpStream {
public:
    MockHttpStream() = default;
    ~MockHttpStream() = default;

    bool connect(const Endpoint& ep) noexcept {
        ep_ = ep;
        connect_called_ = true;
        return connect_success_;
    }

    StreamPhase poll_open() noexcept {
        return open_phase_;
    }

    int fd() const noexcept { return fd_; }
    bool closed() const noexcept { return closed_; }

    sys::io_result read(std::span<uint8_t> dst) noexcept {
        if (read_data_.empty()) {
            return -EAGAIN;
        }
        std::size_t n = std::min(dst.size(), read_data_.size());
        std::memcpy(dst.data(), read_data_.data(), n);
        read_data_.erase(read_data_.begin(), read_data_.begin() + n);
        return static_cast<sys::io_result>(n);
    }

    sys::io_result write(std::span<const uint8_t> src, bool more) noexcept {
        write_more_ = more;
        write_data_.insert(write_data_.end(), src.begin(), src.end());
        return static_cast<sys::io_result>(src.size());
    }

    std::size_t writable_hint() const noexcept { return 65536; }

    void close() noexcept {
        closed_ = true;
    }

    // Mock controls
    Endpoint ep_{};
    int fd_{42};
    bool closed_{false};
    bool connect_called_{false};
    bool connect_success_{true};
    StreamPhase open_phase_{StreamPhase::Connecting};
    std::vector<uint8_t> read_data_{};
    std::vector<uint8_t> write_data_{};
    bool write_more_{false};

    void set_hostname(std::string_view host) {
        host_ = host;
    }
    std::string host_{};
};

static_assert(Stream<MockHttpStream>, "MockHttpStream must satisfy the Stream concept");

namespace {

TEST(HttpClientTest, ConnectionTimeout) {
    EventLoopConfig loop_cfg;
    loop_cfg.busy_poll = true;
    EventLoop loop(loop_cfg);
    BufferPool pool(4, 2048, false);

    HttpClient<MockHttpStream> client(loop, pool);
    client.set_connect_timeout_ns(100); // extremely short connect timeout
    
    bool error_called = false;
    client.on_error([&error_called]() {
        error_called = true;
    });

    client.connect(Endpoint{});
    EXPECT_EQ(client.state(), HttpState::Connecting);

    // Sleep briefly to exceed the 100ns timeout
    std::this_thread::sleep_for(std::chrono::microseconds(10));

    // Reactor tick -> triggers on_ready which checks timeouts
    loop.step();

    EXPECT_EQ(client.state(), HttpState::Failed);
    EXPECT_TRUE(error_called);
}

TEST(HttpClientTest, RequestTimeout) {
    EventLoopConfig loop_cfg;
    loop_cfg.busy_poll = true;
    EventLoop loop(loop_cfg);
    BufferPool pool(4, 2048, false);

    HttpClient<MockHttpStream> client(loop, pool);
    client.set_host("api.rest.com");
    client.set_request_timeout_ns(100); // 100ns request timeout

    bool error_called = false;
    client.on_error([&error_called]() {
        error_called = true;
    });

    client.connect(Endpoint{});
    client.transport().open_phase_ = StreamPhase::Open;
    loop.step(); // Connecting -> Idle
    ASSERT_EQ(client.state(), HttpState::Idle);

    // Submit request
    HttpSubmit sub = client.request(HttpMethod::Get, "/v1/orders", {}, {});
    ASSERT_EQ(sub, HttpSubmit::Ok);
    EXPECT_EQ(client.state(), HttpState::Receiving); // Small request written in one go -> Receiving

    // Sleep to exceed 100ns request deadline
    std::this_thread::sleep_for(std::chrono::microseconds(10));

    // Reactor tick
    loop.step();

    EXPECT_EQ(client.state(), HttpState::Failed);
    EXPECT_TRUE(error_called);
}

TEST(HttpClientTest, KeepAliveRoundtripAndPipelineTrailing) {
    EventLoopConfig loop_cfg;
    loop_cfg.busy_poll = true;
    EventLoop loop(loop_cfg);
    BufferPool pool(4, 2048, false);

    HttpClient<MockHttpStream> client(loop, pool);
    client.set_host("api.rest.com");

    client.connect(Endpoint{});
    client.transport().open_phase_ = StreamPhase::Open;
    loop.step(); // transition to Idle

    std::string response_body;
    client.on_response([&response_body](const HttpResponseView& resp) {
        response_body.assign(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
    });

    // 1. Submit first REST request
    HttpSubmit sub = client.request(HttpMethod::Post, "/v1/orders", {}, {reinterpret_cast<const uint8_t*>("{\"qty\":1}"), 9});
    ASSERT_EQ(sub, HttpSubmit::Ok);

    std::string request_written(reinterpret_cast<char*>(client.transport().write_data_.data()), client.transport().write_data_.size());
    EXPECT_NE(request_written.find("POST /v1/orders HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(request_written.find("Content-Length: 9\r\n"), std::string::npos);
    EXPECT_NE(request_written.find("{\"qty\":1}"), std::string::npos);
    client.transport().write_data_.clear();

    // Respond from mock server (with a trailing partial extra response block to check pipeline parsing)
    std::string response = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 15\r\n"
        "Connection: keep-alive\r\n\r\n"
        "{\"order_id\":42}";
    
    // Append the response, plus a trailing "HTTP/1." of a second response to test buffer preservation
    std::string server_input = response + "HTTP/1.";
    client.transport().read_data_.insert(client.transport().read_data_.end(), server_input.begin(), server_input.end());

    loop.step();

    EXPECT_EQ(response_body, "{\"order_id\":42}");
    EXPECT_EQ(client.state(), HttpState::Idle); // Keep-alive active: returns to Idle!

    // 2. Submit second request on the same client connection (verifies connection reuse)
    sub = client.request(HttpMethod::Get, "/v1/status", {}, {});
    ASSERT_EQ(sub, HttpSubmit::Ok);

    request_written.assign(reinterpret_cast<char*>(client.transport().write_data_.data()), client.transport().write_data_.size());
    EXPECT_NE(request_written.find("GET /v1/status HTTP/1.1\r\n"), std::string::npos);
    client.transport().write_data_.clear();

    // Respond to second request (the read buffer already has "HTTP/1." from before, so we append the rest of the second response)
    std::string second_response = 
        "1 200 OK\r\n"
        "Content-Length: 6\r\n\r\n"
        "ACTIVE";
    client.transport().read_data_.insert(client.transport().read_data_.end(), second_response.begin(), second_response.end());

    loop.step();
    EXPECT_EQ(response_body, "ACTIVE");
    EXPECT_EQ(client.state(), HttpState::Idle);
}

} // namespace
} // namespace lepton::net
