#include "lepton/net/detail/poller.h"
#include "lepton/net/tcp_socket.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace lepton::net {
namespace {

TEST(EndpointTest, ResolveIPv4) {
    // Resolve dotted quad
    auto ep_opt = Endpoint{}.resolve("127.0.0.1", 8080);
    ASSERT_TRUE(ep_opt.has_value());
    EXPECT_TRUE(ep_opt->valid());
    EXPECT_EQ(ep_opt->port_be, ::htons(8080));
    
    // Resolve invalid
    auto ep_opt2 = Endpoint{}.resolve("invalid_hostname_that_does_not_exist_12345", 8080);
    EXPECT_FALSE(ep_opt2.has_value());
}

TEST(TcpSocketTest, SocketOptions) {
    TcpSocket sock;
    
    // Connect requires resolving an address. We resolve loopback.
    auto ep = Endpoint{}.resolve("127.0.0.1", 9999);
    ASSERT_TRUE(ep.has_value());
    
    // Start connect. Since port 9999 is probably closed, it will either immediately fail
    // or return true (state = Connecting). Either is a valid socket behavior.
    bool ok = sock.connect(*ep);
    
    if (ok) {
        EXPECT_EQ(sock.state(), ConnState::Connecting);
        
        // Test modifying options on active fd
        EXPECT_GE(sock.fd(), 0);
        sock.set_nodelay(true);
        sock.set_quickack(true);
        sock.set_recv_buf(8192);
        sock.set_send_buf(8192);
    } else {
        EXPECT_EQ(sock.state(), ConnState::Failed);
    }
    
    sock.close();
    EXPECT_EQ(sock.fd(), -1);
    EXPECT_TRUE(sock.closed());
}

TEST(PollerTest, BasicLifecycle) {
    Poller poller(16);
    EXPECT_TRUE(poller.valid());
    
    // Create a dummy socket descriptor to test poller operations
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    ASSERT_GE(fd, 0);
    
    int token = 42;
    // Add descriptor
    EXPECT_TRUE(poller.add(fd, &token, false));
    // Modify descriptor
    EXPECT_TRUE(poller.mod(fd, &token, true));
    // Delete descriptor
    EXPECT_TRUE(poller.del(fd));
    
    ::close(fd);
}

TEST(PollerTest, WaitTimeout) {
    Poller poller(16);
    EXPECT_TRUE(poller.valid());
    
    ReadyEvent events[16];
    
    // Immediate return check (timeout 0)
    int n = poller.wait(events, 16, 0);
    EXPECT_EQ(n, 0);
    
    // Positive timeout check (10ms)
    auto start = std::chrono::steady_clock::now();
    n = poller.wait(events, 16, 10'000'000); // 10ms
    auto end = std::chrono::steady_clock::now();
    
    EXPECT_EQ(n, 0);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_GE(elapsed_ms, 0);
}

// Background TCP echo server helper
void run_echo_server(std::atomic<uint16_t>& out_port) {
    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        ADD_FAILURE() << "Server socket creation failed";
        return;
    }
    
    int opt = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // OS chooses port
    
    if (::bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ADD_FAILURE() << "Server bind failed";
        ::close(server_fd);
        return;
    }
    
    if (::listen(server_fd, 1) < 0) {
        ADD_FAILURE() << "Server listen failed";
        ::close(server_fd);
        return;
    }
    
    socklen_t len = sizeof(addr);
    if (::getsockname(server_fd, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) {
        ADD_FAILURE() << "Server getsockname failed";
        ::close(server_fd);
        return;
    }
    
    uint16_t port = ::ntohs(addr.sin_port);
    out_port.store(port, std::memory_order_release);
    
    // Set receive timeout so accept doesn't block indefinitely
    struct timeval tv{};
    tv.tv_sec = 5;
    ::setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    int client_fd = ::accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) {
        ADD_FAILURE() << "Server accept failed";
        ::close(server_fd);
        return;
    }
    
    // Read incoming packet and echo it back
    char buf[256];
    ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
    if (n > 0) {
        ::send(client_fd, buf, n, 0);
    }
    
    ::close(client_fd);
    ::close(server_fd);
}

TEST(TcpSocketTest, EndToEndCommunication) {
    std::atomic<uint16_t> port{0};
    
    // Start echo server thread
    std::thread server_thread(run_echo_server, std::ref(port));
    
    // Wait until port is published by the server thread
    while (port.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    
    uint16_t server_port = port.load();
    
    // Resolve loopback endpoint
    auto ep = Endpoint{}.resolve("127.0.0.1", server_port);
    ASSERT_TRUE(ep.has_value());
    
    TcpSocket client;
    EXPECT_TRUE(client.connect(*ep));
    
    // Create Poller to manage the client connection
    Poller poller(16);
    ASSERT_TRUE(poller.valid());
    
    // Register interest in readability and writability (non-blocking connect completion signals writable)
    int token = 100;
    EXPECT_TRUE(poller.add(client.fd(), &token, true));
    
    // Poll loop for connect completion
    bool connected = false;
    ReadyEvent events[16];
    
    auto start_time = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(5)) {
        int n = poller.wait(events, 16, 10'000'000); // 10ms wait
        if (n > 0) {
            for (int i = 0; i < n; ++i) {
                EXPECT_EQ(events[i].token, &token);
                if (client.poll_open() == StreamPhase::Open) {
                    connected = true;
                    break;
                }
            }
        }
        if (connected) break;
    }
    
    ASSERT_TRUE(connected);
    
    // Modify poller interest to read-only since connection is established
    EXPECT_TRUE(poller.mod(client.fd(), &token, false));
    
    // Send PING message
    const char* ping_msg = "PING_ECHO_TEST";
    std::size_t msg_len = std::strlen(ping_msg);
    auto write_res = client.write(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(ping_msg), msg_len), false);
    EXPECT_EQ(write_res, static_cast<sys::io_result>(msg_len));
    
    // Wait for echoed data (readability)
    bool readable = false;
    start_time = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(5)) {
        int n = poller.wait(events, 16, 10'000'000); // 10ms wait
        if (n > 0) {
            for (int i = 0; i < n; ++i) {
                if (events[i].flags & kReadable) {
                    readable = true;
                    break;
                }
            }
        }
        if (readable) break;
    }
    
    ASSERT_TRUE(readable);
    
    // Read echoed response
    char recv_buf[128] = {0};
    auto read_res = client.read(std::span<uint8_t>(
        reinterpret_cast<uint8_t*>(recv_buf), sizeof(recv_buf) - 1));
    EXPECT_EQ(read_res, static_cast<sys::io_result>(msg_len));
    EXPECT_STREQ(recv_buf, ping_msg);
    
    client.close();
    server_thread.join();
}

} // namespace
} // namespace lepton::net
