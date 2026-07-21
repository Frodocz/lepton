/// @file event_loop_thread_example.cpp
/// @brief Showcases the EventLoopThread class driving a WSS market data session in a background thread.

#include "lepton/net/event_loop_thread.h"
#include "lepton/net/ws_session.h"
#include "lepton/net/security/tls_context.h"
#include "lepton/net/security/tls_stream.h"
#include "lepton/base/logger.h"
#include "lepton/base/tsc_clock.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace lepton;

int main(int argc, char* argv[]) {
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

    TscClock::calibrate();

    // 1. Create and configure the EventLoopThread (supports both POSIX and F-Stack)
    net::EventLoopThread md_thread(argc, argv);

    // 2. Start the background thread
    LEPTON_LOG_INFO("Starting background EventLoopThread...");
    if (!md_thread.start()) {
        LEPTON_LOG_ERROR("Failed to start EventLoopThread!");
        return 1;
    }

    // Expose underlying loop and buffer pool
    net::EventLoop* loop = md_thread.get_loop();
    BufferPool* pool = md_thread.get_pool();

    // 3. Initialize TLS context and WsSession
    security::TlsContext::Options tls_opts{.verify_peer = false};
    security::TlsContext tls_ctx(tls_opts);

    auto ep_opt = net::Endpoint{}.resolve("ws.okx.com", 8443);
    if (!ep_opt) {
        LEPTON_LOG_ERROR("Failed to resolve ws.okx.com");
        return 1;
    }
    net::Endpoint ep = *ep_opt;

    // Create session registered to the background thread's EventLoop
    net::WsSession<security::TlsStream> ws(*loop, *pool);
    ws.set_host("ws.okx.com");
    ws.set_path("/ws/v5/public");
    ws.set_sec_key("dGhlIHNhbXBsZSBub25jZQ==");
    ws.set_ping_interval_ns(15'000'000'000);   // 15s ping
    ws.set_pong_timeout_ns(5'000'000'000);      // 5s pong

    ws.transport().set_context(tls_ctx);
    ws.transport().set_hostname("ws.okx.com");

    std::atomic<int> message_count{0};
    std::atomic<bool> subscribed{false};

    // Callback handlers
    ws.on_state([](net::WsState state) {
        LEPTON_LOG_INFO("WSS State Changed: {}", static_cast<int>(state));
    });

    ws.on_message([&message_count](const net::WsMessageView& msg) {
        int count = message_count.fetch_add(1) + 1;
        LEPTON_LOG_INFO("=== Received Ticker Message ({}) ===", count);
        LEPTON_LOG_INFO("{}", std::string_view(reinterpret_cast<const char*>(msg.payload.data()), msg.payload.size()));
    });

    // Begin connect
    LEPTON_LOG_INFO("Connecting to ws.okx.com:8443 (WSS)...");
    ws.connect(ep);

    // 4. Configure step-hook
    // The step-hook runs directly inside the background EventLoopThread context
    md_thread.set_step_hook([&]() {
        // Send subscription if connection handshake completed
        if (ws.state() == net::WsState::Open && !subscribed) {
            LEPTON_LOG_INFO("WSS connected. Sending OKX BTC-USDT subscription JSON...");
            std::string sub_json = R"({"op": "subscribe", "args": [{"channel": "tickers", "instId": "BTC-USDT"}]})";
            (void)ws.send({reinterpret_cast<const uint8_t*>(sub_json.data()), sub_json.size()}, /*binary=*/false);
            subscribed = true;
        }
    });

    // 5. Main thread is completely free to perform other duties
    LEPTON_LOG_INFO("Main thread is free to work! Waiting for messages...");

    // Wait in the main thread until we have received 3 messages
    while (message_count.load(std::memory_order_relaxed) < 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    LEPTON_LOG_INFO("Received 3 messages. Stopping EventLoopThread manually from main thread...");
    md_thread.stop();
    
    LEPTON_LOG_INFO("EventLoopThread example finished successfully.");

    return 0;
}
