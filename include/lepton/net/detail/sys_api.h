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

#include <cstddef>
#include <cstdint>
#include <cerrno>

#if defined(LEPTON_USE_FSTACK)
#include <ff_api.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>   // FIONBIO for ff_ioctl-based nonblocking toggle
#include <sys/socket.h>  // SOL_SOCKET / SO_* constants
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
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
    // tcp_socket() already sets SOCK_NONBLOCK
    return 0;
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

/// Begin a nonblocking connect to an already-resolved IPv4 endpoint. `arg`
/// points to a filled `sockaddr_in` (opaque here to keep this header free of
/// the F-Stack linux_sockaddr vs POSIX sockaddr divergence — see endpoint.h).
/// Returns 0 if connected immediately, -EINPROGRESS if in progress, else -errno.
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

/// True once the nonblocking connect has actually completed. Uses getpeername:
/// it fails with ENOTCONN while the TCP handshake is still in progress and
/// succeeds once the peer is connected. This is the portable completion probe —
/// SO_ERROR alone is 0 *both* while connecting and when connected, so relying on
/// it declares "connected" prematurely (harmless EAGAIN on Linux, but ENOTCONN
/// on FreeBSD/F-Stack when the app then writes).
LEPTON_ALWAYS_INLINE bool peer_connected(int fd) noexcept {
    struct sockaddr_in sa {};
    socklen_t len = sizeof(sa);
#if defined(LEPTON_USE_FSTACK)
    return ff_getpeername(fd, reinterpret_cast<linux_sockaddr*>(&sa), &len) == 0;
#else
    return ::getpeername(fd, reinterpret_cast<struct sockaddr*>(&sa), &len) == 0;
#endif
}

/// Read SO_ERROR to resolve the outcome of an in-progress connect.
/// Returns 0 (connected or still in progress) or the pending errno on failure.
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

/// Nonblocking recv into buf[0..len). 0 == EOF, -EAGAIN == nothing yet, <0 == -errno.
/// The fd is already nonblocking (set_nonblock), so no per-call MSG flag is
/// needed — this keeps the F-Stack (FreeBSD) path clean of Linux-only flags.
LEPTON_ALWAYS_INLINE io_result recv(int fd, void* buf, std::size_t len) noexcept {
#if defined(LEPTON_USE_FSTACK)
    ssize_t n = ff_recv(fd, buf, len, MSG_DONTWAIT);
#else
    ssize_t n = ::recv(fd, buf, len, MSG_DONTWAIT);
#endif
    if (n < 0) [[unlikely]] {
        int saved_errno = errno;
        return -saved_errno;
    }
    return static_cast<io_result>(n);
}

/// Nonblocking send of buf[0..len). Returns bytes accepted (may be partial) or
/// -errno (-EAGAIN if the send buffer is full). Never blocks.
///
/// `more` requests MSG_MORE (coalesce a following payload into one segment) on
/// Linux/POSIX. F-Stack (FreeBSD-derived) has no MSG_MORE and no MSG_NOSIGNAL;
/// SIGPIPE there is suppressed via SO_NOSIGPIPE at socket creation instead. On
/// the F-Stack path `more` is a no-op — TCP_NODELAY is already set, and the WS
/// layer builds each frame as a single write, so segment coalescing is minor.
LEPTON_ALWAYS_INLINE io_result send(int fd, const void* buf, std::size_t len,
                                    [[maybe_unused]] bool more) noexcept {
#if defined(LEPTON_USE_FSTACK)
    int flags = 0;
    #if defined(MSG_DONTWAIT)
        flags |= MSG_DONTWAIT;
    #endif
    ssize_t n = ff_send(fd, buf, len, flags);
#else
    // Linux/POSIX
    constexpr int kNoSig = MSG_NOSIGNAL;
    const int kMore = more ? MSG_MORE : 0;
    ssize_t n = ::send(fd, buf, len, MSG_DONTWAIT | kNoSig | kMore);
#endif

    if (n < 0) {
        int saved_errno = errno;
        return -saved_errno;
    }
    return static_cast<io_result>(n);
}

} // namespace lepton::net::sys
