#include "lepton/base/logger.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

// This test demonstrates the core lepton integration model: YOU own a dedicated
// poll thread and drive the loop. lepton::PollScope binds the backend to that
// thread on construction and drains + shuts it down on scope exit, so the whole
// start/poll/stop lifecycle stays on one thread with no manual teardown.

std::atomic<bool> g_keep_polling{true};

void dedicated_logger_worker() noexcept {
    // In production HFT, you would bind this thread to a non-critical core:
    // cpu_set_t cpuset;
    // CPU_ZERO(&cpuset);
    // CPU_SET(0, &cpuset); // Bind to Core 0
    // pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    lepton::PollScope scope;  // start_logger() now; stop_logger() on scope exit
    std::cout << "[Logger Thread] Dedicated background polling worker started.\n";
    while (g_keep_polling.load(std::memory_order_relaxed)) {
        lepton::poll_logger_once();
        std::this_thread::sleep_for(std::chrono::microseconds(5));
    }
    std::cout << "[Logger Thread] Stop signalled; PollScope will drain on exit.\n";
}

void critical_path_worker(int thread_id) {
    // Each thread writes a burst of logs simulating market events
    for (int i = 0; i < 5; ++i) {
        uint64_t seq_num = 1000000 + i;
        double price = 65250.50 + thread_id;
        
        // This execution path is purely lock-free and takes ~15-30ns.
        // It does not format the string on this thread.
        LEPTON_LOG_DEBUG("[Thread {}] Order Book Update - Seq: {}, Best Bid: {}", 
                        thread_id, seq_num, price);
        
        // Simulate minor trading work intervals
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int main() {
    // Console sink so the logs are visible inline in this demo. The default is a
    // rotating file sink, which is what a long-running server would use.
    lepton::init_logger({.level = lepton::LogLevel::Debug, .to_console = true, .to_rotating_file = false});
    std::cout << "[Main] Internal Quill system initialized.\n";

    LEPTON_LOG_INFO("Lepton Version: {}.{}.{}",
        lepton::LEPTON_VERSION_MAJOR, lepton::LEPTON_VERSION_MINOR, lepton::LEPTON_VERSION_PATCH);
    LEPTON_LOG_WARN("Lepton is a lightspeed network framework");
    LEPTON_LOG_ERROR("Lepton provides DPDK support via F-Stack");

    // Sleep 10s until the some outside monitors has enough window to start 
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Spawn the dedicated poll thread (user-owned).
    std::thread logger_thread(dedicated_logger_worker);

    // Spawn multiple CRITICAL path execution threads
    std::cout << "[Main] Spawning critical engine threads...\n";
    std::vector<std::thread> engine_threads;
    for (int i = 1; i <= 3; ++i) {
        engine_threads.emplace_back(critical_path_worker, i);
    }

    for (auto& t : engine_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    std::cout << "[Main] All critical engine threads have finished writing.\n";

    lepton::set_log_level(lepton::LogLevel::Warning);
    LEPTON_LOG_INFO("Info level log should not appear");
    LEPTON_LOG_WARN("Warn level log should appear as before");
    LEPTON_LOG_ERROR("Error level log should appear as before");
    LEPTON_LOG_WARN("Change of level should not affect previous critical engine threads Debug level logs");

    // Signal the poll thread to stop, then join it (PollScope drains on exit).
    std::cout << "[Main] Signaling dedicated logger thread to stop...\n";
    g_keep_polling.store(false, std::memory_order_relaxed);
    if (logger_thread.joinable()) {
        logger_thread.join();
    }

    std::cout << "[Main] Multi-threaded logging simulation completed successfully.\n";
    return 0;
}
