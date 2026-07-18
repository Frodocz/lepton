#pragma once

/// @file stream.h
/// @brief The transport concept that protocol layers are written against.
///
/// `WsSession`, `HttpClient`, etc. are templated on a `Stream`. This is what
/// lets the SAME WebSocket state machine run over a plain `TcpSocket` (ws://)
/// or a `TlsStream` (wss://) with zero runtime cost and no dependency from
/// `net` on `security` (net_design.md §3).
///
/// A Stream is a connectable, nonblocking, completion-free byte pipe:
///   - `connect`   begins a nonblocking connect to a resolved endpoint.
///   - `poll_open` advances connection setup (TCP connect completion and, for
///     TLS, the handshake) and reports whether the stream is ready.
///   - `read`  pulls available bytes now (never blocks); 0 == EOF, <0 == -errno
///     (with -EAGAIN meaning "nothing available yet").
///   - `write` accepts as many bytes as it can now; returns accepted count or
///     -errno; partial writes are normal and the caller retries later.

#include "lepton/net/endpoint.h"
#include "lepton/net/sys_api.h"

#include <concepts>
#include <cstdint>
#include <span>

namespace lepton::net {

/// Connection-setup phase reported by Stream::poll_open().
enum class StreamPhase : uint8_t { Connecting, Open, Failed, Closed };

template <typename T>
concept Stream = requires(T s, const Endpoint& ep, std::span<uint8_t> wb,
                          std::span<const uint8_t> rb, bool more) {
    { s.connect(ep) } -> std::convertible_to<bool>;
    { s.poll_open() } -> std::convertible_to<StreamPhase>;
    { s.fd() } -> std::convertible_to<int>;
    { s.read(wb) } -> std::convertible_to<sys::io_result>;       ///< bytes, 0=EOF, -errno
    { s.write(rb, more) } -> std::convertible_to<sys::io_result>;///< accepted, -errno
    { s.writable_hint() } -> std::convertible_to<std::size_t>;   ///< best-effort room
    { s.closed() } -> std::convertible_to<bool>;
    { s.close() };
};

} // namespace lepton::net
