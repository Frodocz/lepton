#include "lepton/net/tcp_socket.h"
#include <gtest/gtest.h>

#include <arpa/inet.h>

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

} // namespace
} // namespace lepton::net
