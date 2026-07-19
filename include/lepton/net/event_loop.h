#pragma once

/// @file event_loop.h
/// @brief Single-thread, busy-polling reactor. One per core. Share-nothing.
///
/// The loop owns a `Poller` (poller mode) or nothing (pure busy-poll mode) and a
/// list of registered `Pollable` sockets. It exposes a per-iteration user hook
/// so the main core can drain its SPSC order queue and call
/// `WsSession::send` — the only place cross-core input enters the net module,
/// and it runs on the loop thread, so nothing here needs a lock
///
/// Backend: under F-Stack, run() delegates to ff_run(&step_trampoline, this);
/// under POSIX it is `while (running_) step();`. `step()` is identical in both.

#include "lepton/base/attributes.h"
#include "lepton/net/detail/poller.h"

#include "third_party/inplace_function.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(LEPTON_USE_FSTACK)
#include <ff_api.h>  // ff_run
#endif

namespace lepton::net {

/// Anything the loop can drive. A session/socket implements this to receive
/// readiness edges (poller mode) or unconditional draining (busy-poll mode).
class Pollable {
public:
    virtual ~Pollable() = default;
    [[nodiscard]] virtual int fd() const noexcept = 0;
    /// Process readiness. In busy-poll mode `flags` is 0 and the callee should
    /// attempt a nonblocking read (and flush any pending writes) regardless.
    virtual void on_ready(uint32_t flags) = 0;
    /// True while the socket has data queued to write (loop arms writable
    /// interest only then, in poller mode).
    [[nodiscard]] virtual bool wants_write() const noexcept = 0;
};

struct EventLoopConfig {
    bool busy_poll{true};       ///< true: drain all each cycle; false: use poller
    int  max_sessions{64};      ///< pre-sized registration table
    int  max_events{64};        ///< poller batch size
    int64_t poll_timeout_ns{0}; ///< poller mode wait; 0 == spin
};

class EventLoop {
public:
    /// Per-iteration hook (e.g. drain the order SPSC queue). Runs on the loop
    /// thread, once per cycle, before draining sockets.
    using StepHook = stdext::inplace_function<void(), 64>;

    explicit EventLoop(const EventLoopConfig& cfg)
        : cfg_{cfg}, poller_{cfg.busy_poll ? 0 : cfg.max_events} {
        sessions_.reserve(static_cast<std::size_t>(cfg.max_sessions));
        events_.resize(static_cast<std::size_t>(cfg.max_events));
    }

    ~EventLoop() = default;

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    /// Register a pollable. Not hot-path; done at session setup.
    bool add(Pollable& p) {
        if (!cfg_.busy_poll) {
            if (!poller_.add(p.fd(), &p, p.wants_write())) {
                return false;
            }
        }
        sessions_.push_back(&p);
        return true;
    }

    bool remove(Pollable& p) {
        if (!cfg_.busy_poll) {
            poller_.del(p.fd());
        }
        
        // Scrub B from the remaining batch to prevent UAF
        if (current_event_idx_ >= 0) {
            for (int j = current_event_idx_ + 1; j < current_event_count_; ++j) {
                if (events_[static_cast<std::size_t>(j)].token == &p) {
                    events_[static_cast<std::size_t>(j)].token = nullptr;
                }
            }
        }

        // Null out session pointer to avoid iterator invalidation during dispatch
        for (auto& s : sessions_) {
            if (s == &p) {
                s = nullptr;
                return true;
            }
        }
        return false;
    }

    /// Called by a session when its write-pending state flips, so the loop can
    /// (poller mode) arm/disarm writable interest. No-op in busy-poll mode.
    void notify_write_interest(Pollable& p, bool wants_write) {
        if (!cfg_.busy_poll) {
            poller_.mod(p.fd(), &p, wants_write);
        }
    }

    void set_step_hook(StepHook hook) { step_hook_ = std::move(hook); }

    /// One reactor iteration: run step hook, then (busy-poll) drain every
    /// pollable or (poller) wait + dispatch ready ones.
    LEPTON_ALWAYS_INLINE void step() {
        if (step_hook_) {
            step_hook_();
        }
        if (cfg_.busy_poll) {
            // Drain every session unconditionally.
            std::size_t size = sessions_.size();
            for (std::size_t i = 0; i < size; ++i) {
                Pollable* p = sessions_[i];
                if (p != nullptr) {
                    p->on_ready(0);
                }
            }
            // Sweep nulled pointers
            sessions_.erase(std::remove(sessions_.begin(), sessions_.end(), nullptr), sessions_.end());
        } else {
            current_event_count_ = poller_.wait(events_.data(), cfg_.max_events, cfg_.poll_timeout_ns);
            for (current_event_idx_ = 0; current_event_idx_ < current_event_count_; ++current_event_idx_) {
                auto* p = static_cast<Pollable*>(events_[static_cast<std::size_t>(current_event_idx_)].token);
                if (p != nullptr) {
                    p->on_ready(events_[static_cast<std::size_t>(current_event_idx_)].flags);
                }
            }
            current_event_idx_ = -1;
            current_event_count_ = 0;
        }
    }

    /// Drive step() until stop(). Under F-Stack this hands control to ff_run.
    void run() {
        running_ = true;
#if defined(LEPTON_USE_FSTACK)
        // ff_run pumps the DPDK PMD and invokes our trampoline each cycle.
        ff_run(&EventLoop::fstack_trampoline, this);
#else
        while (running_) {
            step();
        }
#endif
    }

    void stop() noexcept { running_ = false; }

    [[nodiscard]] bool busy_poll() const noexcept { return cfg_.busy_poll; }
    [[nodiscard]] std::size_t session_count() const noexcept { return sessions_.size(); }

private:
#if defined(LEPTON_USE_FSTACK)
    // ff_run calls this repeatedly with the pointer we passed. Returning 0 keeps
    // the loop alive; the process exits via stop()+external means under F-Stack.
    static int fstack_trampoline(void* arg) {
        auto* self = static_cast<EventLoop*>(arg);
        self->step();
        return 0;
    }
#endif

    EventLoopConfig cfg_;
    Poller poller_;                    ///< unused in busy-poll mode
    std::vector<Pollable*> sessions_;  ///< pre-reserved at construction
    std::vector<ReadyEvent> events_;   ///< poller batch scratch
    StepHook step_hook_{};
    int current_event_idx_{-1};
    int current_event_count_{0};
    bool running_{false};
};

} // namespace lepton::net
