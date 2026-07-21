// A long-running server logging setup, showcasing lepton's server-oriented
// options together:
//   - multi-sink: durable rotating files AND a live console tail
//   - runtime level control while the server runs

#include "lepton/base/logger.h"

#include <chrono>
#include <iostream>
#include <thread>

void logger_worker(std::stop_token stoken) {
    // In production, pin this thread to a non-critical core here.
    lepton::PollLoggerScope scope;
    while (!stoken.stop_requested()) {
        lepton::poll_logger_for(50);
        std::this_thread::sleep_for(std::chrono::microseconds(5));
    }
}

int main() {
    // Durable rotating files (bounded disk) PLUS a live console tail.
    lepton::init_logger({
        .level = lepton::LogLevel::Info, // filter Debug level logs
        .to_console = true,
        .to_rotating_file = true,
        .filename = "server.log",
        .max_file_size_bytes = 16 * 1024 * 1024,  // 16 MiB per file
        .max_backup_files = 5,                    // keep 5 rolls -> bounded disk
        .daily_rotation_time = "00:00",           // also roll at midnight (UTC)
    });

    if (!lepton::is_initialized()) {
        std::cerr << "[Main] logger init failed; aborting.\n";
        return 1;
    }

    std::jthread logger_thread(logger_worker);

    // DEBUG is compiled out
    LEPTON_LOG_DEBUG("compiled out, never appears {}", 42);

    LEPTON_LOG_INFO("server starting up");
    LEPTON_LOG_WARN("cache warm-up slower than expected");
    LEPTON_LOG_ERROR("upstream feed handshake retried");

    // Diagnose a live incident: temporarily raise verbosity. (DEBUG is still gone
    // at compile time, but everything INFO+ obeys this runtime switch.)
    lepton::set_log_level(lepton::LogLevel::Error);
    LEPTON_LOG_INFO("this INFO is now runtime-filtered");
    LEPTON_LOG_ERROR("only errors get through now");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::cout << "[Main] server logging example completed. See server*.log.\n";
    return 0;
}
