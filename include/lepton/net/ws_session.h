#pragma once

/// @file ws_session.h
/// @brief WebSocket client state machine, templated on the transport `Stream`.
///
/// `WsSession<TcpSocket>`  == ws://  (local dev / testnet, no TLS)
/// `WsSession<TlsStream>`  == wss:// (production; TlsStream lives in security/)
///
/// Threading: every method runs on the owning EventLoop's thread. `send()` is
/// called from the loop's step hook after draining the  SPSC queue — never
/// from another thread. Hence no locks.
///
/// Callback safety:
///   on_message() runs on the EventLoop thread. To prevent Use-After-Free (UAF),
///   do NOT delete the WsSession object inside this callback; instead, invoke
///   session.close() to safely trigger the disconnection teardown flow.
///
/// Crypto-free by construction: the expected Sec-WebSocket-Accept is supplied by
/// the caller via set_expected_accept() (computed with
/// security/ws_handshake_hash.h), and the Sec-WebSocket-Key is supplied via
/// set_sec_key(). Plain ws:// sessions can pass empty strings to skip accept
/// verification.

#include "lepton/base/attributes.h"
#include "lepton/base/buffer_pool.h"
#include "lepton/base/io_buffer.h"
#include "lepton/base/logger.h"
#include "lepton/base/tsc_clock.h"
#include "lepton/net/endpoint.h"
#include "lepton/net/event_loop.h"
#include "lepton/net/http.h"
#include "lepton/net/stream.h"
#include "lepton/net/detail/ws_frame.h"
#include "lepton/net/detail/ws_mask.h"
#include "third_party/inplace_function.h"

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace lepton::net {

enum class WsState : uint8_t {
    Disconnected, Connecting, Upgrading, Open, Closing, Failed
};

struct WsFrameHeader;

/// Zero-copy view of a received message. `payload` points INTO the session
/// receive buffer and is valid ONLY for the duration of the on_message call.
/// Copy it (e.g. into your SPSC queue) if you need to keep it.
struct WsMessageView {
    WsOpcode opcode;
    std::span<const uint8_t> payload;
    bool is_control;
};

/// Result of send(): the caller (trading core) decides policy on would_block.
enum class SendStatus : uint8_t { Ok, WouldBlock, NotOpen, TooLarge };

template <Stream Transport>
class WsSession : public Pollable {
public:
    using OnMessage = stdext::inplace_function<void(const WsMessageView&), 32>;
    using OnState   = stdext::inplace_function<void(WsState), 32>;

    WsSession(EventLoop& loop, BufferPool& pool);
    ~WsSession() override;

    WsSession(const WsSession&) = delete;
    WsSession& operator=(const WsSession&) = delete;

    // ── Configuration (cold path) ────────────────────────────────────────────
    void on_message(OnMessage cb) { on_message_ = std::move(cb); }
    void on_state(OnState cb) { on_state_ = std::move(cb); }
    void set_ping_interval_ns(int64_t ns) noexcept { ping_interval_ns_ = ns; }
    void set_pong_timeout_ns(int64_t ns) noexcept { pong_timeout_ns_ = ns; }
    void set_auto_reconnect(bool on) noexcept { auto_reconnect_ = on; }

    /// HTTP request target path (e.g. "/ws" or "/stream?streams=...").
    void set_path(std::string_view path) { path_.assign(path); }
    /// Host header + (for TLS) SNI value.
    void set_host(std::string_view host) { host_.assign(host); }
    /// The base64 Sec-WebSocket-Key to send.
    void set_sec_key(std::string_view key) { sec_key_.assign(key); }
    /// Expected Sec-WebSocket-Accept. Empty -> accept header is not verified.
    void set_expected_accept(std::string_view acc) { expected_accept_.assign(acc); }

    /// Begin TCP→[TLS]→HTTP-upgrade to a resolved endpoint.
    void connect(const Endpoint& ep);

    /// Queue an application message. Runs on the loop thread only. Never blocks.
    /// Header is prepended into reserved headroom (zero payload copy) and the
    /// payload is masked in place. Returns would_block if no pending slot/buffer
    /// is available.
    [[nodiscard]] SendStatus send(std::span<const uint8_t> payload, bool binary);

    /// Initiate a clean close (sends Close frame, then tears down).
    void close(uint16_t code = 1000);

    [[nodiscard]] WsState state() const noexcept { return state_; }

    /// Access the underlying transport. Cold path.
    [[nodiscard]] Transport& transport() noexcept { return transport_; }

    // ── Pollable ─────────────────────────────────────────────────────────────
    [[nodiscard]] int fd() const noexcept override;
    [[nodiscard]] bool wants_write() const noexcept override;
    void on_ready(uint32_t flags) override;

private:
    // A pending outbound frame: an owned pool buffer plus how far it's been sent.
    struct Pending {
        IOBuffer buf;
        std::size_t sent{0};
    };
    static constexpr std::size_t kMaxPending = 32;

    // ── Connection lifecycle ──────────────────────────────────────────────────
    void start_connect();
    void maybe_reconnect();

    // ── HTTP upgrade ──────────────────────────────────────────────────────────
    void send_upgrade_request();
    void drive_upgrade_read();

    // ── Frame receive (zero-copy dispatch) ────────────────────────────────────
    void drive_frame_read();
    void parse_frames();
    void dispatch_frame(const WsFrameHeader& hdr, std::span<const uint8_t> payload);
    void deliver(const WsMessageView& msg);

    // Fragmentation reassembly
    void begin_fragment(WsOpcode op, std::span<const uint8_t> first);
    void append_fragment(std::span<const uint8_t> chunk, bool fin);

    // ── Frame send (bounded, non-blocking) ─────────────────────────────────────
    [[nodiscard]] SendStatus enqueue_frame(WsOpcode opcode, std::span<const uint8_t> payload, bool fin);
    bool push_pending(IOBuffer& b);
    void flush_pending();

    // ── Keepalive ──────────────────────────────────────────────────────────────
    void arm_ping() noexcept;
    void check_keepalive() noexcept;

    // ── Buffer / state helpers ─────────────────────────────────────────────────
    void compact_recv() noexcept;
    uint8_t* recv_base() noexcept;
    void set_state(WsState s);
    void teardown(WsState next);
    void drain_pending() noexcept;
    void release_buffers() noexcept;

    // ── Constants ──────────────────────────────────────────────────────────────
    static constexpr int64_t kReconnectInitialNs = 1'000'000'000;   // 1s
    static constexpr int64_t kReconnectMaxNs     = 30'000'000'000;  // 30s

    EventLoop& loop_;
    BufferPool& pool_;
    Transport transport_{};
    WsState state_{WsState::Disconnected};
    bool registered_{false};

    OnMessage on_message_{};
    OnState on_state_{};

    Endpoint ep_{};
    std::string host_{};
    std::string path_{"/"};
    std::string sec_key_{};
    std::string expected_accept_{};

    IOBuffer recv_{};      ///< receive / reassembly-parse buffer
    IOBuffer frag_{};      ///< fragmented-message reassembly
    WsOpcode frag_opcode_{WsOpcode::Text};

    Pending pending_[kMaxPending]{};
    std::size_t pending_head_{0};
    std::size_t pending_count_{0};

    MaskKeyGen mask_gen_{};

    int64_t ping_interval_ns_{0};
    int64_t pong_timeout_ns_{0};
    int64_t next_ping_at_ns_{0};
    int64_t pong_deadline_ns_{0};
    bool awaiting_pong_{false};

    bool auto_reconnect_{false};
    int64_t reconnect_delay_ns_{0};
    int64_t reconnect_at_ns_{0};
};

} // namespace lepton::net

#include "lepton/net/detail/ws_session_impl.h"
