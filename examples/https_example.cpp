#include "lepton/base/buffer_pool.h"
#include "lepton/base/logger.h"
#include "lepton/net/event_loop.h"
#include "lepton/net/http_client.h"
#include "lepton/net/security/tls_context.h"
#include "lepton/net/security/tls_stream.h"

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
    lepton::PollLoggerScope logger_scope;

    // 1. Initialize EventLoop, BufferPool, and TlsContext
    net::EventLoopConfig loop_cfg{.busy_poll = false};
    net::EventLoop loop(loop_cfg);
    BufferPool pool(16, 4096, false);

    // TLS context configured to verify peer certificate using default trust anchors
    security::TlsContext::Options tls_opts{
        .verify_peer = false
    };
    security::TlsContext tls_ctx(tls_opts);

    // 2. Resolve endpoint (example.com on port 443)
    auto ep_opt = net::Endpoint{}.resolve("example.com", 443);
    if (!ep_opt) {
        LEPTON_LOG_ERROR("Failed to resolve example.com");
        return 1;
    }
    net::Endpoint ep = *ep_opt;

    // 3. Create HttpClient templated on TlsStream
    net::HttpClient<security::TlsStream> client(loop, pool);
    client.set_host("example.com");
    client.set_connect_timeout_ns(5'000'000'000); // 5s
    client.set_request_timeout_ns(5'000'000'000); // 5s

    // Set TLS context and SNI hostname on the TlsStream transport
    client.transport().set_context(tls_ctx);
    client.transport().set_hostname("example.com");

    bool finished = false;

    // Register callback handlers
    client.on_response([&finished, &client](const net::HttpResponseView& resp) {
        LEPTON_LOG_INFO("=== Received HTTPS Response ===");
        LEPTON_LOG_INFO("Status: {}", resp.status);
        LEPTON_LOG_INFO("Body size: {} bytes", resp.body.size());
        LEPTON_LOG_INFO("Body:\n{}", std::string_view(reinterpret_cast<const char*>(resp.body.data()), resp.body.size()));
        finished = true;
    });

    client.on_error([&finished, &client]() {
        LEPTON_LOG_ERROR("HTTPS request failed or timed out. State: {}", static_cast<int>(client.state()));
        finished = true;
    });

    // 4. Begin connection sequence
    LEPTON_LOG_INFO("Connecting to example.com:443 (HTTPS)...");
    client.connect(ep);

    // 5. Run EventLoop and submit request once TLS session is active
    bool request_sent = false;
    while (!finished) {
        loop.step();
        lepton::poll_logger_for(100);
        
        if (!request_sent && client.ready() && client.state() == net::HttpState::Idle) {
            LEPTON_LOG_INFO("TLS session established. Submitting GET / request...");
            net::HttpSubmit sub = client.request(net::HttpMethod::Get, "/", {}, {});
            if (sub != net::HttpSubmit::Ok) {
                LEPTON_LOG_ERROR("Failed to submit HTTPS request: {}", static_cast<int>(sub));
                finished = true;
            }
            request_sent = true;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}
