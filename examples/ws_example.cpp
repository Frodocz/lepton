#include "lepton/base/buffer_pool.h"
#include "lepton/base/logger.h"
#include "lepton/net/event_loop.h"
#include "lepton/net/tcp_socket.h"
#include "lepton/net/ws_session.h"

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

    // 1. Initialize EventLoop and BufferPool
    net::EventLoopConfig loop_cfg{.busy_poll = false};
    net::EventLoop loop(loop_cfg);
    BufferPool pool(16, 4096, false);

    // 2. Resolve endpoint (websockets.chilkat.io on port 80)
    auto ep_opt = net::Endpoint{}.resolve("websockets.chilkat.io", 80);
    if (!ep_opt) {
        LEPTON_LOG_ERROR("Failed to resolve websockets.chilkat.io");
        return 1;
    }
    net::Endpoint ep = *ep_opt;

    // 3. Create WsSession templated on TcpSocket
    net::WsSession<net::TcpSocket> ws(loop, pool);
    ws.set_host("websockets.chilkat.io");
    ws.set_path("/wsChilkatEcho.ashx");
    ws.set_sec_key("dGhlIHNhbXBsZSBub25jZQ=="); // Standard base64 key
    ws.set_ping_interval_ns(10'000'000'000);   // 10s ping intervals
    ws.set_pong_timeout_ns(5'000'000'000);      // 5s pong timeouts

    bool finished = false;

    // Register callback handlers
    ws.on_state([](net::WsState state) {
        LEPTON_LOG_INFO("WebSocket State: {}", static_cast<int>(state));
    });

    ws.on_message([&finished](const net::WsMessageView& msg) {
        LEPTON_LOG_INFO("=== Received WS Message ===");
        LEPTON_LOG_INFO("Opcode: {}", static_cast<int>(msg.opcode));
        LEPTON_LOG_INFO("Payload: {}", std::string_view(reinterpret_cast<const char*>(msg.payload.data()), msg.payload.size()));
        finished = true;
    });

    // 4. Begin connection sequence
    LEPTON_LOG_INFO("Connecting to websockets.chilkat.io:80 (WS)...");
    ws.connect(ep);

    bool message_sent = false;

    // 5. Run EventLoop
    while (!finished) {
        loop.step();

        if (ws.state() == net::WsState::Open && !message_sent) {
            LEPTON_LOG_INFO("WebSocket handshakes complete. Sending text frame...");
            std::string msg = "Hello Lepton WS!";
            net::SendStatus status = ws.send({reinterpret_cast<const uint8_t*>(msg.data()), msg.size()}, /*binary=*/false);
            if (status != net::SendStatus::Ok) {
                LEPTON_LOG_ERROR("Failed to send text frame: {}", static_cast<int>(status));
                finished = true;
            }
            message_sent = true;
        }

        if (ws.state() == net::WsState::Failed) {
            LEPTON_LOG_ERROR("WebSocket connection failed!");
            finished = true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Clean close
    LEPTON_LOG_INFO("Closing WebSocket session...");
    ws.close();
    loop.step();

    return 0;
}
