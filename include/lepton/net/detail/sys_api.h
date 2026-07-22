#pragma once

/// @file sys_api.h
/// @brief The single backend seam. POSIX vs F-Stack(DPDK) is chosen
///        at compile time.
///
/// Every function is a thin, always-inline wrapper over one syscall (or its
/// `ff_*` twin). The signatures are backend-agnostic; only the bodies differ.
/// Higher layers (tcp_socket, poller, event_loop) call `lepton::net::sys::*`
/// and stay portable.
///
/// Backend selection macros (set by CMake, see CMakeLists.txt):
///   LEPTON_USE_FSTACK  -> F-Stack + DPDK userspace TCP/IP stack (production)
///                      -> Not set means POSIX kernel sockets (local dev / CI)

#include "lepton/base/attributes.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>

#if defined(LEPTON_USE_FSTACK)
#include <ff_api.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>   // FIONBIO for ff_ioctl-based nonblocking toggle
#include <sys/socket.h>  // SOL_SOCKET / SO_* constants
#else
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

namespace lepton::net::sys {

/// Result of a nonblocking I/O call.
/// >= 0 : bytes transferred (0 on EOF for recv).
///  < 0 : negated errno (e.g. -EAGAIN means "would block, try later").
using io_result = int64_t;

// ── Socket lifecycle ─────────────────────────────────────────────────────────

/// Create a TCP socket. Returns fd or -errno.
LEPTON_ALWAYS_INLINE int tcp_socket() noexcept {
#if defined(LEPTON_USE_FSTACK)
    int fd = ff_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) [[unlikely]] {
        return -errno;
    }
    // FreeBSD-derived stack: suppress SIGPIPE per-socket (no MSG_NOSIGNAL).
    #if defined(SO_NOSIGPIPE)
        int on = 1;
        ff_setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    #endif
    return fd;
#else
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    return fd >= 0 ? fd : -errno;
#endif
}

/// Close a socket fd (idempotent for fd < 0).
LEPTON_ALWAYS_INLINE void close(int fd) noexcept {
    if (fd < 0) {
        return;
    }
#if defined(LEPTON_USE_FSTACK)
    ff_close(fd);
#else
    ::close(fd);
#endif
}

/// Put fd in nonblocking mode. Returns 0 or -errno.
LEPTON_ALWAYS_INLINE int set_nonblock(int fd) noexcept {
#if defined(LEPTON_USE_FSTACK)
    int on = 1;
    return ff_ioctl(fd, FIONBIO, &on) == 0 ? 0 : -errno;
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) [[unlikely]] {
        return -errno;
    }
    if (flags & O_NONBLOCK) {
        return 0;  // already nonblocking
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -errno;
#endif
}

/// TCP_NODELAY on/off. Returns 0 or -errno.
LEPTON_ALWAYS_INLINE int set_nodelay(int fd, bool on) noexcept {
    int v = on ? 1 : 0;
#if defined(LEPTON_USE_FSTACK)
    return ff_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) == 0 ? 0 : -errno;
#else
    return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) == 0 ? 0 : -errno;
#endif
}

/// TCP_QUICKACK on/off. Returns 0 or -errno.
LEPTON_ALWAYS_INLINE int set_quickack(int fd, bool on) noexcept {
#if defined(LEPTON_USE_FSTACK)
    (void)fd;
    (void)on;
    return 0;    
#else
    int v = on ? 1 : 0;
    return ::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &v, sizeof(v)) == 0 ? 0 : -errno;
#endif
}

/// SO_RCVBUF / SO_SNDBUF. Returns 0 or -errno.
LEPTON_ALWAYS_INLINE int set_recv_buf(int fd, int bytes) noexcept {
#if defined(LEPTON_USE_FSTACK)
    return ff_setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) == 0 ? 0 : -errno;
#else
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) == 0 ? 0 : -errno;
#endif
}

LEPTON_ALWAYS_INLINE int set_send_buf(int fd, int bytes) noexcept {
#if defined(LEPTON_USE_FSTACK)
    return ff_setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) == 0 ? 0 : -errno;
#else
    return ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) == 0 ? 0 : -errno;
#endif
}

// ── Connection ───────────────────────────────────────────────────────────────

/// Begin a nonblocking connect to an already-resolved IPv4 endpoint.
LEPTON_ALWAYS_INLINE int connect(int fd, const void* sockaddr_in_ptr) noexcept {
#if defined(LEPTON_USE_FSTACK)
    int ret = ff_connect(fd, reinterpret_cast<const linux_sockaddr*>(sockaddr_in_ptr),
                         sizeof(struct sockaddr_in));
#else
    int ret = ::connect(fd, reinterpret_cast<const struct sockaddr*>(sockaddr_in_ptr),
                        sizeof(struct sockaddr_in));
#endif
    if (ret == 0) {
        return 0;
    }

    int saved_errno = errno;
    return saved_errno == EINPROGRESS ? -EINPROGRESS : -saved_errno;
}

LEPTON_ALWAYS_INLINE bool peer_connected(int fd) noexcept {
    struct sockaddr_in sa {};
    socklen_t len = sizeof(sa);
#if defined(LEPTON_USE_FSTACK)
    return ff_getpeername(fd, reinterpret_cast<linux_sockaddr*>(&sa), &len) == 0;
#else
    return ::getpeername(fd, reinterpret_cast<struct sockaddr*>(&sa), &len) == 0;
#endif
}

LEPTON_ALWAYS_INLINE int socket_error(int fd) noexcept {
    int err = 0;
    socklen_t len = sizeof(err);
#if defined(LEPTON_USE_FSTACK)
    if (ff_getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        return errno;
    }
#else
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        return errno;
    }
#endif
    return err;
}

// ── Data transfer (nonblocking) ──────────────────────────────────────────────

LEPTON_ALWAYS_INLINE io_result recv(int fd, void* buf, std::size_t len) noexcept {
#if defined(LEPTON_USE_FSTACK)
    ssize_t n = ff_recv(fd, buf, len, 0);
#else
    ssize_t n = ::recv(fd, buf, len, MSG_DONTWAIT);
#endif
    if (n < 0) [[unlikely]] {
        int saved_errno = errno;
        return -saved_errno;
    }
    return static_cast<io_result>(n);
}

LEPTON_ALWAYS_INLINE io_result send(int fd, const void* buf, std::size_t len,
                                    [[maybe_unused]] bool more) noexcept {
#if defined(LEPTON_USE_FSTACK)
    ssize_t n = ff_send(fd, buf, len, 0);
#else
    #if defined(MSG_NOSIGNAL)
        constexpr int kNoSig = MSG_NOSIGNAL;
    #else
        constexpr int kNoSig = 0;
    #endif
    #if defined(MSG_MORE)
        const int kMore = more ? MSG_MORE : 0;
    #else
        const int kMore = 0;
    #endif
    ssize_t n = ::send(fd, buf, len, MSG_DONTWAIT | kNoSig | kMore);
#endif

    if (n < 0) {
        int saved_errno = errno;
        return -saved_errno;
    }
    return static_cast<io_result>(n);
}

} // namespace lepton::net::sys
