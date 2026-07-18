#pragma once

/// @file tls_stream.h
/// @brief Async OpenSSL TLS that OWNS a `net::TcpSocket`. Satisfies the `Stream`
///        concept, so `WsSession<TlsStream>` is wss:// with no changes to the
///        WebSocket state machine.
///
/// Design: OpenSSL is driven with a memory BIO pair (rbio/wbio). `pump()` moves
/// ciphertext between the BIOs and the underlying nonblocking TcpSocket. read/
/// write translate SSL_ERROR_WANT_READ/WANT_WRITE into the "-EAGAIN, retry on
/// next readiness" contract the event loop already speaks.
///
/// Usage from WsSession<TlsStream>:
///   ws.transport().set_context(tls_ctx);   // once, before connect()
///   ws.set_host(...)  -> WsSession forwards to TlsStream::set_hostname (SNI)
///   ws.connect(ep)    -> TcpSocket connect, then TLS handshake, then WS upgrade

#include "lepton/base/attributes.h"
#include "lepton/net/stream.h"
#include "lepton/net/tcp_socket.h"
#include "lepton/security/tls_context.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// Forward declarations keep OpenSSL headers out of this (widely included via
// WsSession) header; the impl (tls_stream.cpp) includes <openssl/ssl.h>.
using SSL = struct ssl_st;
using BIO = struct bio_st;

namespace lepton::security {

enum class TlsPhase : uint8_t { Idle, ConnectingTcp, Handshaking, Established, Closed, Error };

class TlsStream {
public:
    TlsStream() = default;
    ~TlsStream();

    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;
    TlsStream(TlsStream&&) noexcept;
    TlsStream& operator=(TlsStream&&) noexcept;

    // ── One-time configuration (cold path, before connect) ────────────────────
    void set_context(TlsContext& ctx) noexcept { ctx_ = &ctx; }
    /// SNI server name + certificate hostname verification target.
    void set_hostname(std::string_view host) { host_.assign(host); }

    // ── Stream concept ────────────────────────────────────────────────────────

    /// Begin the nonblocking TCP connect. TLS handshake starts once TCP is up,
    /// driven by poll_open(). Returns false on a hard local error.
    bool connect(const net::Endpoint& remote);

    /// Advance TCP-connect completion and then the TLS handshake as far as
    /// nonblocking I/O allows. Returns the unified StreamPhase for WsSession.
    net::StreamPhase poll_open();

    [[nodiscard]] int fd() const noexcept { return tcp_.fd(); }
    [[nodiscard]] bool closed() const noexcept { return phase_ == TlsPhase::Closed; }

    /// Decrypt available data into `dst`. 0 == clean close, -EAGAIN == need more
    /// ciphertext (retry on next readiness), <0 == -errno / TLS error.
    net::sys::io_result read(std::span<uint8_t> dst) noexcept;

    /// Encrypt + send `src`. Returns accepted plaintext bytes, -EAGAIN if the
    /// socket is momentarily full, or <0 on error. `more` is advisory only
    /// (TLS record boundaries dominate coalescing).
    net::sys::io_result write(std::span<const uint8_t> src, bool more) noexcept;

    [[nodiscard]] std::size_t writable_hint() const noexcept { return tcp_.writable_hint(); }

    /// Send close_notify (best-effort) then close the socket.
    void close() noexcept;

    [[nodiscard]] TlsPhase phase() const noexcept { return phase_; }

private:
    // Move ciphertext OUT of wbio to the socket and INTO rbio from the socket.
    // Returns false on a fatal transport error (socket dead).
    bool pump() noexcept;

    void begin_handshake();
    void fail() noexcept;

    net::TcpSocket tcp_{};        ///< owned transport
    TlsContext* ctx_{nullptr};
    SSL* ssl_{nullptr};
    BIO* rbio_{nullptr};          ///< network -> SSL (we write ciphertext in)
    BIO* wbio_{nullptr};          ///< SSL -> network (we read ciphertext out)
    std::string host_{};
    TlsPhase phase_{TlsPhase::Idle};
};

static_assert(net::Stream<TlsStream>, "TlsStream must satisfy the Stream concept");

} // namespace lepton::security
