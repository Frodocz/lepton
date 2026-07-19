#include "lepton/net/ws_session.h"
#include "lepton/base/buffer_pool.h"
#include "lepton/net/endpoint.h"
#include "lepton/net/event_loop.h"
#include "lepton/net/stream.h"
#include "lepton/security/ws_handshake_hash.h"

#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>

namespace lepton::net {

class MockStream {
public:
    MockStream() = default;
    ~MockStream() = default;

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

    // Mock control helpers
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

static_assert(Stream<MockStream>, "MockStream must satisfy the Stream concept");

namespace {

TEST(WsSessionTest, HandshakeFlow) {
    EventLoopConfig loop_cfg;
    loop_cfg.busy_poll = true;
    EventLoop loop(loop_cfg);
    BufferPool pool(8, 2048, false);

    WsSession<MockStream> session(loop, pool);
    session.set_host("api.test.com");
    session.set_path("/v1/ws");
    session.set_sec_key("dGhlIHNhbXBsZSBub25jZQ==");
    
    // Compute expected accept hash using security/ws_handshake_hash.h
    char expected_accept[32];
    std::size_t acc_len = security::compute_ws_accept("dGhlIHNhbXBsZSBub25jZQ==", expected_accept, sizeof(expected_accept));
    ASSERT_GT(acc_len, 0u);
    session.set_expected_accept({expected_accept, acc_len});

    Endpoint ep;
    session.connect(ep);

    EXPECT_EQ(session.state(), WsState::Connecting);
    EXPECT_TRUE(session.transport().connect_called_);

    // 1. Simulate TCP connected -> starts TLS or goes directly to upgrading
    session.transport().open_phase_ = StreamPhase::Open;
    loop.step();

    EXPECT_EQ(session.state(), WsState::Upgrading);
    
    // Check that HTTP upgrade request was serialized and written to mock stream
    std::string sent_req(reinterpret_cast<char*>(session.transport().write_data_.data()), session.transport().write_data_.size());
    EXPECT_NE(sent_req.find("GET /v1/ws HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(sent_req.find("Host: api.test.com\r\n"), std::string::npos);
    EXPECT_NE(sent_req.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"), std::string::npos);

    // Clear write data for next tests
    session.transport().write_data_.clear();

    // 2. Simulate server sending the HTTP 101 Switching Protocols response
    std::string response = 
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + std::string(expected_accept, acc_len) + "\r\n\r\n";
    session.transport().read_data_.insert(session.transport().read_data_.end(), response.begin(), response.end());

    loop.step();
    EXPECT_EQ(session.state(), WsState::Open);
}

TEST(WsSessionTest, SendReceiveAndFragmentReassembly) {
    EventLoopConfig loop_cfg;
    loop_cfg.busy_poll = true;
    EventLoop loop(loop_cfg);
    BufferPool pool(8, 2048, false);

    WsSession<MockStream> session(loop, pool);
    session.set_host("localhost");
    session.set_path("/ws");
    session.set_sec_key("dGhlIHNhbXBsZSBub25jZQ==");
    
    char expected_accept[32];
    std::size_t acc_len = security::compute_ws_accept("dGhlIHNhbXBsZSBub25jZQ==", expected_accept, sizeof(expected_accept));
    session.set_expected_accept({expected_accept, acc_len});

    session.connect(Endpoint{});
    session.transport().open_phase_ = StreamPhase::Open;
    loop.step();

    std::string response = 
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + std::string(expected_accept, acc_len) + "\r\n\r\n";
    session.transport().read_data_.insert(session.transport().read_data_.end(), response.begin(), response.end());
    loop.step();
    ASSERT_EQ(session.state(), WsState::Open);

    // Setup message reception callback
    std::string received_msg;
    session.on_message([&received_msg](const WsMessageView& msg) {
        received_msg.assign(reinterpret_cast<const char*>(msg.payload.data()), msg.payload.size());
    });

    // 1. Receive single complete text frame from server (e.g. "Hello")
    // FIN=1, RSV=0, Opcode=Text (1) -> 0x81
    // Masked=0, Length=5 -> 0x05
    std::vector<uint8_t> frame = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};
    session.transport().read_data_.insert(session.transport().read_data_.end(), frame.begin(), frame.end());

    loop.step();
    EXPECT_EQ(received_msg, "Hello");
    received_msg.clear();

    // 2. Receive fragmented message (chunk 1: "Hi ", chunk 2: "there")
    // Chunk 1: FIN=0, Opcode=Text (1) -> 0x01. Len=3 -> 0x03
    std::vector<uint8_t> f1 = {0x01, 0x03, 'H', 'i', ' '};
    // Chunk 2: FIN=1, Opcode=Continuation (0) -> 0x80. Len=5 -> 0x05
    std::vector<uint8_t> f2 = {0x80, 0x05, 't', 'h', 'e', 'r', 'e'};

    session.transport().read_data_.insert(session.transport().read_data_.end(), f1.begin(), f1.end());
    loop.step();
    EXPECT_TRUE(received_msg.empty()); // Not delivered yet (fragmented)

    session.transport().read_data_.insert(session.transport().read_data_.end(), f2.begin(), f2.end());
    loop.step();
    EXPECT_EQ(received_msg, "Hi there"); // Fully assembled and delivered!
}

} // namespace
} // namespace lepton::net
