#include "lepton/base/logger.h"

#include "spdlog/spdlog.h"
#include "spdlog/async.h"
#include "spdlog/sinks/basic_file_sink.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

// -----------------------------------------------------------------------------
// Scenario: the host uses spdlog (async mode) as ITS OWN logging library.
//
// spdlog's async mode owns its OWN backing thread pool -- you do not poll spdlog
// by hand. So lepton contributes ONE dedicated poll thread (which you own and
// drive via PollScope) and spdlog runs its own pool thread; the two coexist.
//
// In production you would pin BOTH background threads to the same non-critical
// core so your hot cores stay clean.
// -----------------------------------------------------------------------------

std::atomic<bool> g_keep_polling{true};

void lepton_poll_worker() {
    // RAII: start_logger() binds lepton's backend to THIS thread; stop_logger()
    // (drain + shutdown) runs when the scope exits.
    lepton::PollScope scope;
    std::cout << "[Poll Thread] lepton poll worker started.\n";
    while (g_keep_polling.load(std::memory_order_relaxed)) {
        lepton::poll_logger_for(50);
        std::this_thread::sleep_for(std::chrono::microseconds(5));
    }
    std::cout << "[Poll Thread] Stop signalled; PollScope will drain on exit.\n";
}

int main() {
    lepton::init_logger({.to_console = true, .to_rotating_file = false});

    // spdlog async: one backing thread, queue size 8192. spdlog owns this thread.
    spdlog::init_thread_pool(8192, 1);
    auto app_logger = spdlog::basic_logger_mt<spdlog::async_factory>(
        "app", "with_spdlog_app.log", /*truncate=*/true);
    app_logger->set_level(spdlog::level::debug);
    std::cout << "[Main] lepton + spdlog(async) initialized.\n";

    // lepton's own poll thread coexists with spdlog's pool thread.
    std::thread poll_thread(lepton_poll_worker);

    // Both loggers are usable from any thread; each has its own background thread
    // doing the formatting and I/O.
    for (int i = 0; i < 5; ++i) {
        LEPTON_LOG_INFO("[lepton] connection accepted, fd={}", 100 + i);
        app_logger->info("[spdlog] handled request {}", i);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::cout << "[Main] Stopping lepton poll thread...\n";
    g_keep_polling.store(false, std::memory_order_relaxed);
    if (poll_thread.joinable()) {
        poll_thread.join();
    }

    // Flush and stop spdlog's own async machinery.
    app_logger->flush();
    spdlog::shutdown();

    std::cout << "[Main] spdlog integration example completed. "
                 "See with_spdlog_app.log for spdlog output.\n";
    return 0;
}
