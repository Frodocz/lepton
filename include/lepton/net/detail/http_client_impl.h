#pragma once

namespace lepton::net {

template <Stream Transport>
HttpClient<Transport>::HttpClient(EventLoop& loop, BufferPool& pool)
    : loop_{loop}, pool_{pool} {}

template <Stream Transport>
HttpClient<Transport>::~HttpClient() {
    if (registered_) {
        loop_.remove(*this);
    }
    release_buffers();
}

template <Stream Transport>
void HttpClient<Transport>::connect(const Endpoint& ep) {
    ep_ = ep;
    if (state_ == HttpState::Idle || state_ == HttpState::Connecting) {
        return;
    }
    start_connect();
}

template <Stream Transport>
HttpSubmit HttpClient<Transport>::request(HttpMethod method, std::string_view path,
                                         std::span<std::string_view> headers,
                                         std::span<const uint8_t> body) {
    if (state_ != HttpState::Idle) {
        return state_ == HttpState::Sending || state_ == HttpState::Receiving
                   ? HttpSubmit::Busy
                   : HttpSubmit::NotReady;
    }
    std::span<uint8_t> spare = send_.spare();
    std::size_t n = build_http_request(
        {reinterpret_cast<char*>(spare.data()), spare.size()}, method, host_, path, headers, body);
    if (n == 0) {
        return HttpSubmit::TooLarge;
    }
    send_.append(n);
    send_off_ = 0;
    recv_.reset(0);
    state_ = HttpState::Sending;
    if (request_timeout_ns_ > 0) {
        request_deadline_ns_ = TscClock::tscns() + request_timeout_ns_;
    } else {
        request_deadline_ns_ = 0;
    }
    
    // Attempt non-blocking write immediately.
    // epoll write interest will only be armed if flush_send() hits -EAGAIN.
    flush_send();
    return HttpSubmit::Ok;
}

template <Stream Transport>
void HttpClient<Transport>::close() {
    transport_.close();
    release_buffers();
    state_ = HttpState::Disconnected;
    write_armed_ = false;
}

template <Stream Transport>
void HttpClient<Transport>::check_timeouts() noexcept {
    if (state_ == HttpState::Connecting && connect_deadline_ns_ > 0) {
        if (TscClock::tscns() >= connect_deadline_ns_) {
            LEPTON_LOG_WARN("http: connection timeout");
            fail();
        }
    } else if ((state_ == HttpState::Sending || state_ == HttpState::Receiving) && request_deadline_ns_ > 0) {
        if (TscClock::tscns() >= request_deadline_ns_) {
            LEPTON_LOG_WARN("http: request timeout");
            fail();
        }
    }
}

template <Stream Transport>
int HttpClient<Transport>::fd() const noexcept {
    return transport_.fd();
}

template <Stream Transport>
bool HttpClient<Transport>::wants_write() const noexcept {
    return state_ == HttpState::Sending || state_ == HttpState::Connecting;
}

template <Stream Transport>
void HttpClient<Transport>::on_ready(uint32_t /*flags*/) {
    check_timeouts();
    if (state_ == HttpState::Failed || state_ == HttpState::Disconnected) {
        return;
    }
    switch (state_) {
        case HttpState::Connecting: {
            StreamPhase ph = transport_.poll_open();
            if (ph == StreamPhase::Open) {
                state_ = HttpState::Idle;
                if (write_armed_) {
                    loop_.notify_write_interest(*this, false);
                    write_armed_ = false;
                }
            } else if (ph == StreamPhase::Failed || ph == StreamPhase::Closed) {
                fail();
            }
            break;
        }
        case HttpState::Sending:
            flush_send();
            break;
        case HttpState::Receiving:
            drive_recv();
            break;
        default:
            break;
    }
}

template <Stream Transport>
void HttpClient<Transport>::start_connect() {
    release_buffers();
    send_ = pool_.acquire(0);
    recv_ = pool_.acquire(0);
    if (!send_ || !recv_) {
        LEPTON_LOG_ERROR("http: no buffers available");
        fail();
        return;
    }
    if constexpr (requires(Transport& t, std::string_view h) { t.set_hostname(h); }) {
        transport_.set_hostname(host_);
    }
    if (connect_timeout_ns_ > 0) {
        connect_deadline_ns_ = TscClock::tscns() + connect_timeout_ns_;
    } else {
        connect_deadline_ns_ = 0;
    }
    
    // Connecting state expects write readiness for nonblocking TCP handshake.
    write_armed_ = true;
    
    if (!transport_.connect(ep_)) {
        fail();
        return;
    }
    state_ = HttpState::Connecting;
    if (!registered_) {
        registered_ = loop_.add(*this);
    }
}

template <Stream Transport>
void HttpClient<Transport>::flush_send() {
    while (send_off_ < send_.size()) {
        std::span<const uint8_t> rest = {send_.data() + send_off_, send_.size() - send_off_};
        sys::io_result n = transport_.write(rest, /*more=*/false);
        if (n == -EAGAIN) {
            if (!write_armed_) {
                loop_.notify_write_interest(*this, true);
                write_armed_ = true;
            }
            return;  // retry on writable
        }
        if (n < 0) {
            fail();
            return;
        }
        send_off_ += static_cast<std::size_t>(n);
    }
    // Fully sent: switch to receiving.
    state_ = HttpState::Receiving;
    if (write_armed_) {
        loop_.notify_write_interest(*this, false);
        write_armed_ = false;
    }
    drive_recv();
}

template <Stream Transport>
void HttpClient<Transport>::drive_recv() {
    for (;;) {
        std::span<uint8_t> spare = recv_.spare();
        if (spare.empty()) {
            LEPTON_LOG_WARN("http: response exceeds buffer capacity");
            fail();
            return;
        }
        sys::io_result n = transport_.read(spare);
        if (n == -EAGAIN) {
            return;  // wait for more
        }
        if (n == 0) {
            // Peer closed. Try to parse what we have (Connection: close); otherwise fail.
            try_complete(/*eof=*/true);
            return;
        }
        if (n < 0) {
            fail();
            return;
        }
        recv_.append(static_cast<std::size_t>(n));
        if (try_complete(/*eof=*/false)) {
            return;
        }
    }
}

template <Stream Transport>
bool HttpClient<Transport>::try_complete(bool eof) {
    HttpResponseView r = parse_http_response(recv_.bytes());
    if (!r.complete) {
        if (eof) {
            fail();
            return true;
        }
        return false;
    }
    if (on_response_) {
        on_response_(r);
    }
    
    // Guard against callback deletion of HttpClient itself:
    // If the state was changed to Failed or Disconnected in the callback,
    // we should return immediately without touching recv_ members.
    if (state_ == HttpState::Failed || state_ == HttpState::Disconnected) {
        return true;
    }

    // Keep-alive: drop the consumed response, stay connected and idle.
    recv_.consume(r.consumed);
    compact_recv();
    send_.reset(0);
    send_off_ = 0;
    state_ = HttpState::Idle;
    return true;
}

template <Stream Transport>
void HttpClient<Transport>::compact_recv() {
    std::size_t sz = recv_.size();
    if (sz == 0) {
        recv_.reset(0);
        return;
    }
    if (recv_.headroom() == 0) {
        return;
    }
    uint8_t* front = recv_.data() - recv_.headroom();
    std::memmove(front, recv_.data(), sz);
    recv_.reset(0);
    recv_.append(sz);
}

template <Stream Transport>
void HttpClient<Transport>::fail() {
    transport_.close();
    state_ = HttpState::Failed;
    write_armed_ = false;
    if (on_error_) {
        on_error_();
    }
}

template <Stream Transport>
void HttpClient<Transport>::release_buffers() {
    if (send_) {
        pool_.release(send_);
    }
    if (recv_) {
        pool_.release(recv_);
    }
}

} // namespace lepton::net
