#include "lepton/base/logger.h"

#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/ConsoleSink.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

// -----------------------------------------------------------------------------
// Scenario: the host application ALSO uses Quill as its own logging system.
//
// Key fact: Quill has a SINGLE backend per process. lepton acquires the one and
// only ManualBackendWorker in init_logger(); the host must NOT call
// acquire_manual_backend_worker() again (it throws "can only be called once").
//
// Instead, the host simply creates its own Quill loggers/sinks via the Frontend.
// Because the backend polls ALL thread-local SPSC queues, a SINGLE poll loop --
// lepton's -- drains both lepton's logs and the host's own logs on one thread.
//
// The host drives the poll loop itself, using lepton::PollScope (RAII) to bind
// the backend to this thread and drain+shutdown on scope exit, and
// poll_logger_for() to bound the time spent per iteration -- the natural fit
// when one thread/core services every logger in the process.
// -----------------------------------------------------------------------------

std::atomic<bool> g_keep_polling{true};

// The host application's own Quill logger (separate sink, separate name).
quill::Logger* g_app_logger = nullptr;

void unified_poll_worker() {
    // RAII: start_logger() binds the backend to THIS thread now; stop_logger()
    // (drain + shutdown) runs automatically when the scope exits. Everything on
    // one thread, satisfying Quill's same-thread contract.
    lepton::PollScope scope;
    std::cout << "[Poll Thread] Unified backend poll worker started.\n";
    while (g_keep_polling.load(std::memory_order_relaxed)) {
        // Bounded drain: process for up to 50us, then yield to whatever else this
        // shared logging thread is responsible for.
        lepton::poll_logger_for(50);
        std::this_thread::sleep_for(std::chrono::microseconds(5));
    }
    std::cout << "[Poll Thread] Draining remaining logs before exit...\n";
}

int main() {
    // 1. lepton initialises the frontend AND acquires the process-wide backend worker.
    lepton::init_logger({.to_console = true, .to_rotating_file = false});
    std::cout << "[Main] Lepton logger initialized (owns the Quill backend).\n";

    // 2. The host creates ITS OWN Quill logger. Note: it does NOT and must not
    //    call acquire_manual_backend_worker() -- it just uses the Frontend.
    auto app_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("app_console");
    g_app_logger = quill::Frontend::create_or_get_logger("app", std::move(app_sink));
    g_app_logger->set_log_level(quill::LogLevel::Debug);
    std::cout << "[Main] Host application Quill logger created.\n";

    // 3. Start the single unified poll thread (host-owned).
    std::thread poll_thread(unified_poll_worker);

    // 4. Interleave lepton logs and host-app logs from the main thread. Both flow
    //    through the same backend and are drained by the same poll loop.
    // NOTE: lepton's header defines QUILL_DISABLE_NON_PREFIXED_MACROS, so the bare
    // LOG_INFO macro is unavailable. Host code that includes lepton uses the
    // QUILL_-prefixed macros (or its own wrappers) for its own loggers.
    for (int i = 0; i < 5; ++i) {
        LEPTON_LOG_INFO("[lepton] network event #{}", i);
        QUILL_LOG_INFO(g_app_logger, "[app] business event #{}", i);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::cout << "[Main] Finished emitting logs; stopping poll thread...\n";
    g_keep_polling.store(false, std::memory_order_relaxed);
    if (poll_thread.joinable()) {
        poll_thread.join();
    }

    std::cout << "[Main] Shared-backend logging scenario completed successfully.\n";
    return 0;
}
