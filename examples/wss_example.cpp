#include "lepton/base/buffer_pool.h"
#include "lepton/base/logger.h"
#include "lepton/net/event_loop.h"
#include "lepton/net/ws_session.h"
#include "lepton/net/security/tls_context.h"
#include "lepton/net/security/tls_stream.h"

#include <chrono>
#include <iostream>
#include <thread>

using namespace lepton;

int main() {
    // 0. Initialize Logger
    lepton::init_logger({
        .level = lepton::LogLevel::Debug,
        .to_console = true
    });

    // Background logger thread managed by std::jthread and std::stop_token
    std::jthread logger_thread([](std::stop_token stoken) {
        lepton::PollLoggerScope scope;
        while (!stoken.stop_requested()) {
            lepton::poll_logger_for(100);
        }
    });

    // 1. Initialize EventLoop, BufferPool, and TlsContext
    net::EventLoopConfig loop_cfg{.busy_poll = false};
    net::EventLoop loop(loop_cfg);
    BufferPool pool(16, 4096, false);

    // TLS context using default system paths
    security::TlsContext::Options tls_opts{
        .verify_peer = false
    };
    security::TlsContext tls_ctx(tls_opts);

    // 2. Resolve endpoint (OKX public WS server: ws.okx.com on port 8443)
    auto ep_opt = net::Endpoint{}.resolve("ws.okx.com", 8443);
    if (!ep_opt) {
        LEPTON_LOG_ERROR("Failed to resolve ws.okx.com");
        return 1;
    }
    net::Endpoint ep = *ep_opt;

    // 3. Create WsSession templated on TlsStream
    net::WsSession<security::TlsStream> ws(loop, pool);
    ws.set_host("ws.okx.com");
    ws.set_path("/ws/v5/public");
    ws.set_sec_key("dGhlIHNhbXBsZSBub25jZQ==");
    ws.set_ping_interval_ns(15'000'000'000);   // 15s ping interval
    ws.set_pong_timeout_ns(5'000'000'000);      // 5s pong timeout

    // Configure transport security context and SNI hostname on the transport stream
    ws.transport().set_context(tls_ctx);
    ws.transport().set_hostname("ws.okx.com");

    int msg_count = 0;
    bool finished = false;

    // Register callback handlers
    ws.on_state([](net::WsState state) {
        LEPTON_LOG_INFO("WSS State Changed: {}", static_cast<int>(state));
    });

    ws.on_message([&msg_count, &finished](const net::WsMessageView& msg) {
        LEPTON_LOG_INFO("=== Received WSS Ticker Message ({}) ===", ++msg_count);
        LEPTON_LOG_INFO("{}", std::string_view(reinterpret_cast<const char*>(msg.payload.data()), msg.payload.size()));
        
        // Retrieve 3 tick messages then gracefully exit the example
        if (msg_count >= 3) {
            finished = true;
        }
    });

    // 4. Begin connection sequence
    LEPTON_LOG_INFO("Connecting to ws.okx.com:8443 (WSS)...");
    ws.connect(ep);

    bool subscribed = false;

    // 5. Run EventLoop
    while (!finished) {
        loop.step();

        if (ws.state() == net::WsState::Open && !subscribed) {
            LEPTON_LOG_INFO("WSS handshake complete. Sending OKX BTC-USDT subscription JSON...");
            // OKX Public WS Subscription JSON
            std::string sub_json = R"({"op": "subscribe", "args": [{"channel": "tickers", "instId": "BTC-USDT"}]})";
            net::SendStatus status = ws.send({reinterpret_cast<const uint8_t*>(sub_json.data()), sub_json.size()}, /*binary=*/false);
            if (status != net::SendStatus::Ok) {
                LEPTON_LOG_ERROR("Failed to send subscription JSON: {}", static_cast<int>(status));
                finished = true;
            }
            subscribed = true;
        }

        if (ws.state() == net::WsState::Failed) {
            LEPTON_LOG_ERROR("WSS connection failed!");
            finished = true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Clean close
    LEPTON_LOG_INFO("Closing WSS session...");
    ws.close();
    loop.step();

    return 0;
}
