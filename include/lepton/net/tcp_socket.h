#pragma once

/// @file tcp_socket.h
/// @brief RAII nonblocking TCP transport over the backend seam. Satisfies the
///        `Stream` concept, so it can be the transport for `WsSession` directly
///        (ws://) or the underlying transport for `TlsStream` (wss://).
///
/// Thin by design: all syscalls go through `photon::net::sys::*`, so this class
/// is identical for POSIX and F-Stack. No buffering here — buffering and framing
/// live in the protocol layers.

#include "lepton/base/attributes.h"
#include "lepton/base/logger.h"
#include "lepton/net/endpoint.h"
#include "lepton/net/stream.h"
#include "lepton/net/sys_api.h"

namespace lepton::net {

/// Connection state for the nonblocking connect handshake.
enum class ConnState : uint8_t {
    Idle,
    Connecting,
    Connected,
    Failed,
    Closed,
};

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket() { close(); }

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    TcpSocket(TcpSocket&& o) noexcept : fd_{o.fd_}, state_{o.state_} {
        o.fd_ = -1;
        o.state_ = ConnState::Idle;
    }

    TcpSocket& operator=(TcpSocket&& o) noexcept {
        if (this != &o) {
            close();
            fd_ = o.fd_;
            state_ = o.state_;
            o.fd_ = -1;
            o.state_ = ConnState::Idle;
        }
        return *this;
    }

    /// Create the fd and begin a nonblocking connect.
    /// Returns false only on a hard local error (fd creation / immediate refuse).
    /// EINPROGRESS -> true, state becomes `connecting`;
    /// Completion is observed via poll_connect().
    bool connect(const Endpoint& remote) {
        close();
        int fd = sys::tcp_socket();
        if (fd < 0) [[unlikely]] {
            state_ = ConnState::Failed;
            return false;
        }

        if (sys::set_nonblock(fd) != 0) {
            sys::close(fd);
            state_ = ConnState::Failed;
            return false;
        }
        sys::set_nodelay(fd, true);

        struct sockaddr_in sa {};
        remote.to_sockaddr(&sa);

        int rc = sys::connect(fd, &sa);
        fd_ = fd;
        if (rc == 0) {
            state_ = ConnState::Connected;
            return true;
        }
        if (rc == -EINPROGRESS) {
            state_ = ConnState::Connecting;
            return true;
        }
        close();
        state_ = ConnState::Failed;
        return false;
    }

    /// Called by the event loop when the socket signals writable during connect.
    /// Resolves SO_ERROR and moves state to connected/failed. Returns new state.
    ConnState poll_connect() {
        if (state_ != ConnState::Connecting) {
            return state_;
        }

        // 1) Hard failure? SO_ERROR carries the pending connect errno.
        int err = sys::socket_error(fd_);
        if (err != 0) [[unlikely]] {
            LEPTON_LOG_ERROR("TcpSocket::poll_connect: fd={} connect failed: {}", fd_, err);
            close();
            state_ = ConnState::Failed;
            return state_;
        }
        // 2) SO_ERROR==0 means "connected OR still in progress" — disambiguate
        //    with getpeername (ENOTCONN until the handshake completes). Without
        //    this we'd declare "connected" on the first poll and write into a
        //    SYN_SENT socket: harmless EAGAIN on Linux, fatal ENOTCONN on
        //    FreeBSD/F-Stack.
        if (sys::peer_connected(fd_)) {
            state_ = ConnState::Connected;
        }
        // else: remain connecting; the loop will poll again next cycle.
        return state_;
    }

    /// Stream-concept connection-setup poll. For a plain TCP socket this is just
    /// the connect completion; TLS adds a handshake on top of the same idea.
    StreamPhase poll_open() {
        if (state_ == ConnState::Connecting) {
            poll_connect();
        }
        switch (state_) {
            case ConnState::Connected:  return StreamPhase::Open;
            case ConnState::Connecting: return StreamPhase::Connecting;
            case ConnState::Failed:     return StreamPhase::Failed;
            case ConnState::Closed:     return StreamPhase::Closed;
            default:                    return StreamPhase::Connecting;
        }
    }

    // ── Stream concept ──────────────────────────────────────────────────────
    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] bool closed() const noexcept { return state_ == ConnState::Closed; }

    /// Nonblocking read into `dst`. 0 == EOF, -EAGAIN == nothing yet, <0 == -errno.
    LEPTON_ALWAYS_INLINE sys::io_result read(std::span<std::byte> dst) noexcept {
        return sys::recv(fd_, dst.data(), dst.size());
    }

    /// Nonblocking write of `src`. Returns accepted bytes (may be partial) or
    /// -errno (-EAGAIN if send buffer full). `more` -> MSG_MORE coalescing.
    LEPTON_ALWAYS_INLINE sys::io_result write(std::span<const std::byte> src, bool more) noexcept {
        return sys::send(fd_, src.data(), src.size(), more);
    }

    /// Best-effort writable room hint (for the caller's flow control). The
    /// kernel/F-Stack send buffer size is not cheaply queryable per-call without
    /// a syscall, so this returns a conservative fixed hint; the authoritative
    /// signal is a short/EAGAIN write result.
    [[nodiscard]] std::size_t writable_hint() const noexcept { return 1u << 16; }

    [[nodiscard]] ConnState state() const noexcept { return state_; }

    void set_nodelay(bool on) noexcept { sys::set_nodelay(fd_, on); }
    void set_quickack(bool on) noexcept { sys::set_quickack(fd_, on); }
    void set_recv_buf(int bytes) noexcept { sys::set_recv_buf(fd_, bytes); }
    void set_send_buf(int bytes) noexcept { sys::set_send_buf(fd_, bytes); }

    void close() noexcept {
        if (fd_ >= 0) {
            sys::close(fd_);
            fd_ = -1;
        }
        state_ = ConnState::Closed;
    }
private:
    int fd_{-1};
    ConnState state_{ConnState::Idle};
};

static_assert(Stream<TcpSocket>, "TcpSocket must satisfy the Stream concept");

} // namespace lepton::net
