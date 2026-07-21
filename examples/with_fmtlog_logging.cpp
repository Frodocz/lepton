#include "lepton/base/logger.h"

#include <chrono>
#include <iostream>
#include <thread>

#include "fmtlog.h"

// -----------------------------------------------------------------------------
// Scenario: the host uses fmtlog as ITS OWN logging library, and wants a single
// dedicated thread/core to poll BOTH fmtlog and lepton (Quill) logs.
//
// This is the cleanest cross-library case because fmtlog and Quill's
// ManualBackendWorker share the same design: the frontend is lock-free and a
// background thread manually drains the queue. fmtlog exposes fmtlog::poll();
// lepton exposes poll_logger_for(). We interleave both in one loop.
//
// In production you would pin this thread to a non-critical core so the trading
// / media hot path never pays for log formatting or I/O.
// -----------------------------------------------------------------------------

void unified_poll_worker(std::stop_token stoken) {
    // RAII binds lepton's backend to THIS thread and drains+shuts down on exit.
    lepton::PollLoggerScope scope;
    std::cout << "[Poll Thread] Polling lepton + fmtlog on one thread.\n";
    while (!stoken.stop_requested()) {
        lepton::poll_logger_for(50);  // drain lepton (Quill) for up to 50us
        fmtlog::poll();               // drain fmtlog's queue
        std::this_thread::sleep_for(std::chrono::microseconds(5));
    }
    // lepton's remaining records are drained by PollLoggerScope on scope exit; make
    // sure fmtlog's tail is flushed too.
    fmtlog::poll();
    std::cout << "[Poll Thread] Stop signalled; PollLoggerScope will drain lepton on exit.\n";
}

int main() {
    lepton::init_logger({.to_console = true, .to_rotating_file = false});
    fmtlog::setLogFile("with_fmtlog_app.log", /*truncate=*/true);
    // We poll fmtlog ourselves from the unified thread, so DO NOT also call
    // fmtlog::startPollingThread() -- that would spawn a second poller.
    std::cout << "[Main] lepton + fmtlog initialized.\n";

    std::jthread poll_thread(unified_poll_worker);

    for (int i = 0; i < 5; ++i) {
        LEPTON_LOG_INFO("[lepton] packet received, seq={}", i);
        logi("[fmtlog] app processed order {}", i);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::cout << "[Main] fmtlog integration example completed. "
                 "See with_fmtlog_app.log for fmtlog output.\n";
    return 0;
}
