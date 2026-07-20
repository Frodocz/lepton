#include "lepton/net/event_loop_thread.h"
#include "lepton/base/logger.h"

#if defined(LEPTON_USE_FSTACK)
#include <ff_api.h>
#endif

namespace lepton::net {

EventLoopThread::EventLoopThread(int argc, char** argv) noexcept
    : cfg_(EventLoopThreadConfig{}), argc_(argc), argv_(argv) {}

EventLoopThread::EventLoopThread(const EventLoopThreadConfig& cfg, int argc, char** argv) noexcept
    : cfg_(cfg), argc_(argc), argv_(argv) {}

EventLoopThread::~EventLoopThread() {
    stop();
}

bool EventLoopThread::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return false; // Already running
    }

    // C++20 atomic state: 0 = initializing, 1 = success, 2 = failed
    std::atomic<int> init_state{0};

    thread_ = std::thread([this, &init_state]() {
#if defined(LEPTON_USE_FSTACK)
        LEPTON_LOG_INFO("[EventLoopThread] Initializing F-Stack inside background thread...");
        if (ff_init(argc_, argv_) < 0) {
            LEPTON_LOG_ERROR("[EventLoopThread] F-Stack initialization failed!");
            init_state.store(2, std::memory_order_release);
            init_state.notify_one();
            running_.store(false, std::memory_order_release);
            return;
        }
#endif

        // Instantiate EventLoop and BufferPool inside the event thread context using configured parameters
        loop_ = std::make_unique<EventLoop>(cfg_.loop_cfg);
        pool_ = std::make_unique<BufferPool>(cfg_.pool_count, cfg_.pool_buf_size, cfg_.pool_hugepage);

        // Track whether the first cycle initialization is done
        struct InitStatus {
            std::atomic<int>& state;
            std::atomic<bool> done{false};
        };
        auto status = std::make_shared<InitStatus>(init_state);

        // Bind step hook wrapper to notify the main thread on the first iteration
        loop_->set_step_hook([this, status]() {
            if (!status->done.load(std::memory_order_relaxed)) {
                status->state.store(1, std::memory_order_release);
                status->state.notify_one(); // Wake up main thread waiting in start()
                status->done.store(true, std::memory_order_relaxed);
            }
            if (step_hook_) {
                step_hook_();
            }
        });

        LEPTON_LOG_INFO("[EventLoopThread] Starting background event loop thread...");
        loop_->run();
        LEPTON_LOG_INFO("[EventLoopThread] Background event loop thread exited.");
        running_.store(false, std::memory_order_release);
    });

    // C++20 atomic wait: blocks the calling thread until the value of init_state changes from 0.
    // On Linux, this is a highly optimized futex call.
    init_state.wait(0, std::memory_order_acquire);

    if (init_state.load(std::memory_order_acquire) != 1) {
        if (thread_.joinable()) {
            thread_.join();
        }
        return false;
    }

    return true;
}

void EventLoopThread::stop() {
    if (loop_) {
        loop_->stop();
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

} // namespace lepton::net
