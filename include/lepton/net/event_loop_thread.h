#pragma once

#include "lepton/net/event_loop.h"
#include "lepton/base/buffer_pool.h"

#include <atomic>
#include <memory>
#include <thread>

namespace lepton::net {

struct EventLoopThreadConfig {
    /// EventLoop configuration
    EventLoopConfig loop_cfg{.busy_poll = true};

    /// BufferPool buffer count
    std::size_t pool_count{32};

    /// BufferPool single buffer size in bytes
    std::size_t pool_buf_size{4096};

    /// Enable hugepages mapping for BufferPool (POSIX fallback path only)
    bool pool_hugepage{false};
};

class LEPTON_API EventLoopThread {
public:
    /// Constructor with default configuration, optionally accepting F-Stack parameters.
    EventLoopThread(int argc = 0, char** argv = nullptr) noexcept;

    /// Constructor with custom configuration, optionally accepting F-Stack parameters.
    EventLoopThread(const EventLoopThreadConfig& cfg, int argc = 0, char** argv = nullptr) noexcept;
    
    ~EventLoopThread();

    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;

    /// Spawns a background thread, initializes F-Stack/POSIX socket environments,
    /// and launches loop.run() blocking. Returns true on success.
    bool start();

    /// Stops the running event loop and joins the background thread.
    void stop();

    /// Expose underlying EventLoop
    [[nodiscard]] EventLoop* get_loop() const noexcept { return loop_.get(); }

    /// Expose underlying BufferPool
    [[nodiscard]] BufferPool* get_pool() const noexcept { return pool_.get(); }

    /// Register a callback to run once per cycle inside the event loop thread
    void set_step_hook(EventLoop::StepHook hook) noexcept { step_hook_ = std::move(hook); }

    /// Returns true if the thread is currently running the event loop
    [[nodiscard]] bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }

private:
    EventLoopThreadConfig cfg_;
    int argc_;
    char** argv_;
    std::atomic<bool> running_{false};
    std::thread thread_;

    std::unique_ptr<EventLoop> loop_;
    std::unique_ptr<BufferPool> pool_;

    EventLoop::StepHook step_hook_;
};

} // namespace lepton::net
