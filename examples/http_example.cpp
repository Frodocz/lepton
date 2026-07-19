#include "lepton/base/buffer_pool.h"
#include "lepton/base/logger.h"
#include "lepton/net/event_loop.h"
#include "lepton/net/http_client.h"
#include "lepton/net/tcp_socket.h"

#include <chrono>
#include <iostream>
#include <thread>

using namespace lepton;

int main() {
    // 0. Initialize Logger and bind manual backend worker to this thread
    lepton::init_logger({
        .level = lepton::LogLevel::Info,
        .to_console = true
    });
    lepton::PollScope logger_scope;

    // 1. Initialize EventLoop and BufferPool
    net::EventLoopConfig loop_cfg{.busy_poll = false}; // Epoll mode for low idle CPU usage
    net::EventLoop loop(loop_cfg);
    BufferPool pool(16, 4096, false); // 16 buffers of 4KB each

    // 2. Resolve endpoint (example.com on port 80)
    auto ep_opt = net::Endpoint{}.resolve("example.com", 80);
    if (!ep_opt) {
        LEPTON_LOG_ERROR("Failed to resolve example.com");
        return 1;
    }
    net::Endpoint ep = *ep_opt;

    // 3. Create HttpClient templated on TcpSocket
    net::HttpClient<net::TcpSocket> client(loop, pool);
    client.set_host("example.com");
    client.set_connect_timeout_ns(5'000'000'000); // 5s
    client.set_request_timeout_ns(5'000'000'000); // 5s

    // Set TCP quick ACK on the transport to reduce handshake latency
    client.transport().set_quickack(true);

    bool finished = false;

    // Register callback handlers
    client.on_response([&finished](const net::HttpResponseView& resp) {
        LEPTON_LOG_INFO("=== Received HTTP Response ===");
        LEPTON_LOG_INFO("Status: {}", resp.status);
        LEPTON_LOG_INFO("Body:\n{}", std::string_view(reinterpret_cast<const char*>(resp.body.data()), resp.body.size()));
        finished = true;
    });

    client.on_error([&finished, &client]() {
        LEPTON_LOG_ERROR("HTTP request failed or timed out. State: {}", static_cast<int>(client.state()));
        finished = true;
    });

    // 4. Begin connection sequence
    LEPTON_LOG_INFO("Connecting to example.com:80 (HTTP)...");
    client.connect(ep);

    // 5. Run EventLoop and submit request once connection is Idle
    bool request_sent = false;
    while (!finished) {
        loop.step();
        lepton::poll_logger_for(100);
        
        if (!request_sent && client.ready() && client.state() == net::HttpState::Idle) {
            LEPTON_LOG_INFO("Connection established. Submitting GET / request...");
            net::HttpSubmit sub = client.request(net::HttpMethod::Get, "/", {}, {});
            if (sub != net::HttpSubmit::Ok) {
                LEPTON_LOG_ERROR("Failed to submit HTTP request: {}", static_cast<int>(sub));
                finished = true;
            }
            request_sent = true;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}
