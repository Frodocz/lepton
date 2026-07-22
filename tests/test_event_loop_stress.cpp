#include "lepton/base/buffer_pool.h"
#include "lepton/net/event_loop.h"
#include "lepton/net/tcp_socket.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

namespace lepton {
namespace {

constexpr int64_t kStressTargetMsg = 200'000;
constexpr std::size_t kPayloadSize = 128;

struct MsgHeader {
    int64_t seq;
    uint32_t len;
};

class StressEchoServer : public net::Pollable {
public:
    StressEchoServer(net::TcpSocket&& socket) : socket_(std::move(socket)) {}

    int fd() const noexcept override { return socket_.fd(); }
    bool wants_write() const noexcept override { return false; }

    void on_ready(uint32_t /*flags*/) override {
        std::span<uint8_t> dst = {buffer_ + offset_, sizeof(buffer_) - offset_};
        net::sys::io_result n = socket_.read(dst);
        if (n > 0) {
            offset_ += static_cast<std::size_t>(n);
            std::size_t processed = 0;
            while (offset_ - processed >= sizeof(MsgHeader) + kPayloadSize) {
                MsgHeader hdr;
                std::memcpy(&hdr, buffer_ + processed, sizeof(MsgHeader));
                
                // Echo it back immediately
                std::span<const uint8_t> echo_data = {buffer_ + processed, sizeof(MsgHeader) + kPayloadSize};
                net::sys::io_result nw = socket_.write(echo_data, /*more=*/false);
                if (nw < 0) {
                    break;
                }
                
                processed += sizeof(MsgHeader) + kPayloadSize;
                msg_count_++;
            }
            if (processed > 0) {
                if (processed < offset_) {
                    std::memmove(buffer_, buffer_ + processed, offset_ - processed);
                    offset_ -= processed;
                } else {
                    offset_ = 0;
                }
            }
        }
    }

    int64_t msg_count() const { return msg_count_; }

private:
    net::TcpSocket socket_;
    uint8_t buffer_[2048];
    std::size_t offset_{0};
    int64_t msg_count_{0};
};

#if defined(LEPTON_USE_FSTACK)
TEST(EventLoopStressTest, SkippedUnderFStack) {
    GTEST_SKIP() << "Skipping EventLoopStressTest under F-Stack mode due to multi-threaded/shared-nothing constraints.";
}
#else
TEST(EventLoopStressTest, HighThroughputBusyPollLoopback) {
    // 1. Setup Mock Server Socket
    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(server_fd, 0);
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // Dynamic port
    
    ASSERT_GE(::bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)), 0);
    ASSERT_GE(::listen(server_fd, 1), 0);

    socklen_t addr_len = sizeof(addr);
    ASSERT_GE(::getsockname(server_fd, (struct sockaddr*)&addr, &addr_len), 0);
    int port = ::ntohs(addr.sin_port);

    std::atomic<bool> server_running{true};
    std::atomic<int64_t> server_processed{0};

    // 2. Spawn EventLoop Thread (Busy Poll Mode)
    std::thread loop_thread([server_fd, &server_running, &server_processed]() {
        net::EventLoopConfig loop_cfg{.busy_poll = true};
        net::EventLoop loop(loop_cfg);

        int client_fd = ::accept(server_fd, nullptr, nullptr);
        if (client_fd >= 0) {
            net::TcpSocket socket;
            socket.assign(client_fd);
            
            StressEchoServer server(std::move(socket));
            loop.add(server);

            int64_t last_msg_count = 0;
            while (server_running && server.msg_count() < kStressTargetMsg) {
                loop.step();
                if (server.msg_count() == last_msg_count) {
                    std::this_thread::yield();
                } else {
                    last_msg_count = server.msg_count();
                }
            }
            server_processed.store(server.msg_count());
        }
        ::close(server_fd);
    });

    // 3. Connect Client and Pump Messages
    std::thread client_thread([port, &server_running, &server_processed]() {
        net::TcpSocket client_socket;
        auto ep = net::Endpoint{}.resolve("127.0.0.1", port);
        ASSERT_TRUE(ep.has_value());
        
        while (!client_socket.connect(*ep)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        uint8_t payload[kPayloadSize];
        std::memset(payload, 0x5a, sizeof(payload));

        uint8_t read_buf[2048];
        std::size_t offset = 0;
        int64_t sent = 0;
        int64_t received = 0;

        while (received < kStressTargetMsg && server_running.load()) {
            bool did_work = false;

            // Write payload
            if (sent < kStressTargetMsg && sent - received < 1000) {
                MsgHeader hdr{sent, kPayloadSize};
                uint8_t send_buf[sizeof(MsgHeader) + kPayloadSize];
                std::memcpy(send_buf, &hdr, sizeof(MsgHeader));
                std::memcpy(send_buf + sizeof(MsgHeader), payload, kPayloadSize);
                
                std::span<const uint8_t> src = {send_buf, sizeof(send_buf)};
                net::sys::io_result nw = client_socket.write(src, /*more=*/false);
                if (nw > 0) {
                    did_work = true;
                    sent++;
                }
            }

            // Read echo
            std::span<uint8_t> dst = {read_buf + offset, sizeof(read_buf) - offset};
            net::sys::io_result nr = client_socket.read(dst);
            if (nr > 0) {
                did_work = true;
                offset += static_cast<std::size_t>(nr);
                std::size_t processed = 0;
                while (offset - processed >= sizeof(MsgHeader) + kPayloadSize) {
                    MsgHeader rcv_hdr;
                    std::memcpy(&rcv_hdr, read_buf + processed, sizeof(MsgHeader));
                    EXPECT_EQ(rcv_hdr.seq, received);
                    EXPECT_EQ(rcv_hdr.len, kPayloadSize);
                    processed += sizeof(MsgHeader) + kPayloadSize;
                    received++;
                }
                if (processed > 0) {
                    if (processed < offset) {
                        std::memmove(read_buf, read_buf + processed, offset - processed);
                        offset -= processed;
                    } else {
                        offset = 0;
                    }
                }
            }

            if (!did_work) {
                std::this_thread::yield();
            }
        }
    });

    client_thread.join();
    server_running.store(false);
    loop_thread.join();

    EXPECT_EQ(server_processed.load(), kStressTargetMsg);
}
#endif

} // namespace
} // namespace lepton
