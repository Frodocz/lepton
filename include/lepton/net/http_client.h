#pragma once

/// @file http_client.h
/// @brief Minimal keep-alive HTTP/1.1 client with timeout handling, templated on the transport
///        `Stream`. `HttpClient<TlsStream>` is HTTPS (what exchanges use for
///        REST); `HttpClient<TcpSocket>` is plaintext for local testing.
///
/// Threading: every method runs on the owning EventLoop's thread. `request()` is
/// called from the loop's step hook after draining the order SPSC queue — never
/// from another thread. Hence no locks.
///
/// Callback safety:
///   on_response() runs on the EventLoop thread. To prevent Use-After-Free (UAF),
///   do NOT delete the HttpClient object inside this callback; instead, invoke
///   client.close() to safely trigger the disconnection flow.
///
/// One in-flight request at a time per client (HTTP/1.1 without pipelining).
/// For concurrency, hold several HttpClient instances (a small pool).

#include "lepton/base/attributes.h"
#include "lepton/base/buffer_pool.h"
#include "lepton/base/io_buffer.h"
#include "lepton/base/logger.h"
#include "lepton/base/tsc_clock.h"
#include "lepton/net/endpoint.h"
#include "lepton/net/event_loop.h"
#include "lepton/net/http.h"
#include "lepton/net/stream.h"

#include "third_party/inplace_function.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace lepton::net {

enum class HttpState : uint8_t { Disconnected, Connecting, Idle, Sending, Receiving, Failed };

/// Result of request(): whether the request was accepted for sending now.
enum class HttpSubmit : uint8_t { Ok, Busy, NotReady, TooLarge };

template <Stream Transport>
class HttpClient : public Pollable {
public:
    /// Delivered once per response, on the loop thread. `resp.body` is a view
    /// into the receive buffer — copy it if you need to keep it.
    using OnResponse = stdext::inplace_function<void(const HttpResponseView&), 32>;
    /// Called when the in-flight request fails (transport/parse error).
    using OnError = stdext::inplace_function<void(), 32>;

    HttpClient(EventLoop& loop, BufferPool& pool);
    ~HttpClient() override;

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // ── Configuration (cold path) ─────────────────────────────────────────────
    void on_response(OnResponse cb) { on_response_ = std::move(cb); }
    void on_error(OnError cb) { on_error_ = std::move(cb); }
    void set_host(std::string_view host) { host_.assign(host); }
    [[nodiscard]] Transport& transport() noexcept { return transport_; }

    // Timeouts
    void set_connect_timeout_ns(int64_t ns) noexcept { connect_timeout_ns_ = ns; }
    void set_request_timeout_ns(int64_t ns) noexcept { request_timeout_ns_ = ns; }

    /// Open (or re-open) the connection to `ep`. Idempotent while connected.
    void connect(const Endpoint& ep);

    [[nodiscard]] HttpState state() const noexcept { return state_; }
    [[nodiscard]] bool ready() const noexcept { return state_ == HttpState::Idle; }

    /// Queue a request. Only valid when ready().
    /// Returns ok / busy (a request is in flight) / not_ready / too_large.
    [[nodiscard]] HttpSubmit request(HttpMethod method, std::string_view path,
                                     std::span<std::string_view> headers,
                                     std::span<const uint8_t> body);

    void close();

    // ── Timeout check ─────────────────────────────────────────────────────────
    void check_timeouts() noexcept;

    // ── Pollable ──────────────────────────────────────────────────────────────
    [[nodiscard]] int fd() const noexcept override;
    [[nodiscard]] bool wants_write() const noexcept override;
    void on_ready(uint32_t flags) override;

private:
    void start_connect();
    void flush_send();
    void drive_recv();
    bool try_complete(bool eof);
    void compact_recv();
    void fail();
    void release_buffers();

    EventLoop& loop_;
    BufferPool& pool_;
    Transport transport_{};
    HttpState state_{HttpState::Disconnected};
    bool registered_{false};
    bool write_armed_{false}; // Epoll MOD write interest tracking optimization

    OnResponse on_response_{};
    OnError on_error_{};

    Endpoint ep_{};
    std::string host_{};

    IOBuffer send_{};
    std::size_t send_off_{0};
    IOBuffer recv_{};

    // Timeout configuration (cold path, set once)
    int64_t request_timeout_ns_{0};   ///< 0 = no per-request timeout
    int64_t connect_timeout_ns_{0};   ///< 0 = no connect timeout

    // Timeout deadlines (armed per request/connect)
    int64_t request_deadline_ns_{0};  ///< absolute TSC ns; 0 = inactive
    int64_t connect_deadline_ns_{0};  ///< absolute TSC ns; 0 = inactive
};

} // namespace lepton::net

#include "lepton/net/detail/http_client_impl.h"
