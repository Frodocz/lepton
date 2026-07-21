#pragma once

/// @file poller.h
/// @brief Readiness poller seam: epoll (POSIX) or ff_epoll (F-Stack).

#include "lepton/base/attributes.h"

#include <cstddef>
#include <cstdint>
#include <limits>

#if defined(LEPTON_USE_FSTACK)
#include <ff_api.h>    // ff_close
#include <ff_epoll.h>  // ff_epoll_create/ctl/wait
#else
#include <unistd.h>
#include <sys/epoll.h>
#endif

#include <cerrno>

namespace lepton::net {

enum ReadyFlags : uint32_t {
    kReadable = 1u << 0,
    kWritable = 1u << 1,
    kError    = 1u << 2,  ///< EPOLLERR/EPOLLHUP or equivalent
};

struct ReadyEvent {
    void* token;    ///< the value passed to add()
    uint32_t flags; ///< OR of ReadyFlags
};

class Poller {
public:
    explicit Poller(int max_events) : max_events_{max_events} {
        if (max_events <= 0) {
            return;
        }
#if defined(LEPTON_USE_FSTACK)
        handle_ = ff_epoll_create(max_events);
#else
        handle_ = ::epoll_create1(EPOLL_CLOEXEC);
#endif
    }

    ~Poller() {
        if (handle_ >= 0) {
#if defined(LEPTON_USE_FSTACK)
            ff_close(handle_);
#else
            ::close(handle_);
#endif
        }
    }

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    [[nodiscard]] bool valid() const noexcept { return handle_ >= 0; }

    bool add(int fd, void* token, bool want_write) {
        return ctl(EPOLL_CTL_ADD, fd, token, want_write);
    }
    bool mod(int fd, void* token, bool want_write) {
        return ctl(EPOLL_CTL_MOD, fd, token, want_write);
    }
    bool del(int fd) {
#if defined(LEPTON_USE_FSTACK)
        return ff_epoll_ctl(handle_, EPOLL_CTL_DEL, fd, nullptr) == 0;
#else
        return ::epoll_ctl(handle_, EPOLL_CTL_DEL, fd, nullptr) == 0;
#endif
    }

    int wait(ReadyEvent* out, int max, int64_t timeout_ns) {
        int timeout_ms;
        if (timeout_ns < 0) {
            timeout_ms = -1;
        } else if (timeout_ns == 0) {
            timeout_ms = 0;
        } else {
            int64_t ms = (timeout_ns + 999'999) / 1'000'000;
            if (ms > std::numeric_limits<int>::max()) {
                timeout_ms = std::numeric_limits<int>::max();
            } else {
                timeout_ms = static_cast<int>(ms);
            }
        }
        if (max > max_events_) {
            max = max_events_;
        }

#if defined(LEPTON_USE_FSTACK)
        alignas(64) struct epoll_event evs[kMaxBatch];
        int n = ff_epoll_wait(handle_, evs, max < kMaxBatch ? max : kMaxBatch, timeout_ms);
#else
        alignas(64) struct epoll_event evs[kMaxBatch];
        int n = ::epoll_wait(handle_, evs, max < kMaxBatch ? max : kMaxBatch, timeout_ms);
#endif
        if (n < 0) {
            return errno == EINTR ? 0 : -1;
        }
        for (int i = 0; i < n; ++i) {
            uint32_t f = 0;
            if (evs[i].events & EPOLLIN) {
                f |= kReadable;
            }
            if (evs[i].events & EPOLLOUT) {
                f |= kWritable;
            }
            if (evs[i].events & (EPOLLERR | EPOLLHUP)) {
                f |= kError;
            }
            out[i].token = evs[i].data.ptr;
            out[i].flags = f;
        }
        return n;
    }

private:
    static constexpr int kMaxBatch = 16;

    bool ctl(int op, int fd, void* token, bool want_write) {
        struct epoll_event ev {};
        ev.events = EPOLLIN | (want_write ? EPOLLOUT : 0u) | EPOLLET;
        ev.data.ptr = token;
#if defined(LEPTON_USE_FSTACK)
        return ff_epoll_ctl(handle_, op, fd, &ev) == 0;
#else
        return ::epoll_ctl(handle_, op, fd, &ev) == 0;
#endif
    }

    int handle_{-1};
    int max_events_{0};
};

} // namespace lepton::net
