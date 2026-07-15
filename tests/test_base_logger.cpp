#include "lepton/base/logger.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

// Control flag to cleanly stop the dedicated polling thread
std::atomic<bool> g_keep_polling{true};

void dedicated_logger_worker() noexcept {
    // In production HFT, you would bind this thread to a non-critical core:
    // cpu_set_t cpuset;
    // CPU_ZERO(&cpuset);
    // CPU_SET(0, &cpuset); // Bind to Core 0
    // pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    std::cout << "[Logger Thread] Dedicated background polling worker started.\n";
    while (g_keep_polling.load(std::memory_order_relaxed)) {
        // Continuously consume records from the SPSC ring buffer.
        // poll_one() returns true if it processed a log, false if the queue was empty.
        lepton::poll_logger();
        std::this_thread::sleep_for(std::chrono::microseconds(5));
    }

    // Drain any remaining logs after the keep_polling flag goes false
    std::cout << "[Logger Thread] Draining remaining logs before exit...\n";
    // Keep pulling 1000 times until the queue is empty
    for (int i = 0; i < 1000; ++i) {
        lepton::poll_logger();
    }
    std::cout << "[Logger Thread] Dedicated background polling worker stopped.\n";
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
    lepton::init_logger();
    std::cout << "[Main] Internal Quill system initialized.\n";

    LEPTON_LOG_INFO("Lepton Version: {}.{}.{}",
        lepton::LEPTON_VERSION_MAJOR, lepton::LEPTON_VERSION_MINOR, lepton::LEPTON_VERSION_PATCH);
    LEPTON_LOG_WARN("Lepton is a lightspeed network framework");
    LEPTON_LOG_ERROR("Lepton provides DPDK support via F-Stack");

    // Spawn the DEDICATED background polling thread
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

    // Give the logger thread a quick window to process final flushes
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::cout << "[Main] Signaling dedicated logger thread to stop...\n";
    g_keep_polling.store(false, std::memory_order_relaxed);

    if (logger_thread.joinable()) {
        logger_thread.join();
    }

    std::cout << "[Main] Multi-threaded logging simulation completed successfully.\n";
    return 0;
}


