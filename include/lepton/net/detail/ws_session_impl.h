#pragma once

#include "lepton/net/detail/ws_frame.h"
#include "lepton/net/detail/ws_mask.h"

namespace lepton::net {

template <Stream Transport>
WsSession<Transport>::WsSession(EventLoop& loop, BufferPool& pool)
    : loop_{loop}, pool_{pool} {}

template <Stream Transport>
WsSession<Transport>::~WsSession() {
    if (registered_) {
        loop_.remove(*this);
    }
    release_buffers();
}

template <Stream Transport>
void WsSession<Transport>::connect(const Endpoint& ep) {
    ep_ = ep;
    start_connect();
}

template <Stream Transport>
SendStatus WsSession<Transport>::send(std::span<const uint8_t> payload, bool binary) {
    if (state_ != WsState::Open) {
        return SendStatus::NotOpen;
    }
    return enqueue_frame(binary ? WsOpcode::Binary : WsOpcode::Text, payload, /*fin=*/true);
}

template <Stream Transport>
void WsSession<Transport>::close(uint16_t code) {
    auto_reconnect_ = false;  // explicit close: do not reconnect
    if (state_ == WsState::Open) {
        uint8_t body[2] = {static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code & 0xFF)};
        (void)enqueue_frame(WsOpcode::Close, {body, 2}, /*fin=*/true);
        set_state(WsState::Closing);
    } else {
        teardown(WsState::Disconnected);
    }
}

template <Stream Transport>
int WsSession<Transport>::fd() const noexcept {
    return transport_.fd();
}

template <Stream Transport>
bool WsSession<Transport>::wants_write() const noexcept {
    return pending_count_ > 0 || state_ == WsState::Connecting;
}

template <Stream Transport>
void WsSession<Transport>::on_ready(uint32_t /*flags*/) {
    check_timeouts();
    if (state_ == WsState::Disconnected || state_ == WsState::Failed) {
        maybe_reconnect();
        return;
    }

    if (state_ == WsState::Connecting) {
        StreamPhase ph = transport_.poll_open();
        if (ph == StreamPhase::Open) {
            send_upgrade_request();
        } else if (ph == StreamPhase::Failed || ph == StreamPhase::Closed) {
            teardown(WsState::Failed);
        }
        return;
    }

    flush_pending();

    if (state_ == WsState::Upgrading) {
        drive_upgrade_read();
    } else if (state_ == WsState::Open || state_ == WsState::Closing) {
        drive_frame_read();
        check_keepalive();
    }
}

template <Stream Transport>
void WsSession<Transport>::start_connect() {
    release_buffers();
    recv_ = pool_.acquire(0);
    if (!recv_) {
        LEPTON_LOG_ERROR("ws: no recv buffer available");
        set_state(WsState::Failed);
        return;
    }
    recv_.reset(0);

    if constexpr (requires(Transport& t, std::string_view h) { t.set_hostname(h); }) {
        transport_.set_hostname(host_);
    }

    if (connect_timeout_ns_ > 0) {
        connect_deadline_ns_ = TscClock::tscns() + connect_timeout_ns_;
    } else {
        connect_deadline_ns_ = 0;
    }

    if (!transport_.connect(ep_)) {
        teardown(WsState::Failed);
        return;
    }
    set_state(WsState::Connecting);
    if (!registered_) {
        registered_ = loop_.add(*this);
    }
}

template <Stream Transport>
void WsSession<Transport>::maybe_reconnect() {
    if (!auto_reconnect_) {
        return;
    }
    int64_t now = TscClock::tscns();
    if (now < reconnect_at_ns_) {
        return;
    }
    reconnect_delay_ns_ = reconnect_delay_ns_ == 0
        ? kReconnectInitialNs
        : (reconnect_delay_ns_ * 2 > kReconnectMaxNs ? kReconnectMaxNs : reconnect_delay_ns_ * 2);
    start_connect();
}

template <Stream Transport>
void WsSession<Transport>::send_upgrade_request() {
    set_state(WsState::Upgrading);
    recv_.reset(0);

    IOBuffer req = pool_.acquire(0);
    if (!req) {
        teardown(WsState::Failed);
        return;
    }
    std::span<uint8_t> spare = req.spare();
    std::size_t n = build_ws_upgrade(
        {reinterpret_cast<char*>(spare.data()), spare.size()}, host_, path_, sec_key_);
    if (n == 0) {
        pool_.release(req);
        teardown(WsState::Failed);
        return;
    }
    req.append(n);
    push_pending(req);
    flush_pending();
}

template <Stream Transport>
void WsSession<Transport>::drive_upgrade_read() {
    std::span<uint8_t> spare = recv_.spare();
    if (spare.empty()) {
        teardown(WsState::Failed);
        return;
    }
    sys::io_result n = transport_.read(spare);
    if (n == -EAGAIN) {
        return;
    }
    if (n <= 0) {
        teardown(WsState::Failed);
        return;
    }
    recv_.append(static_cast<std::size_t>(n));

    std::span<const uint8_t> got = recv_.bytes();
    WsUpgradeResult r = validate_ws_upgrade(
        {reinterpret_cast<const char*>(got.data()), got.size()}, expected_accept_);
    if (!r.complete) {
        return;
    }
    bool ok = r.accepted || (expected_accept_.empty() && r.headers_len > 0);
    if (!ok) {
        LEPTON_LOG_WARN("ws: upgrade rejected");
        teardown(WsState::Failed);
        return;
    }
    recv_.consume(r.headers_len);
    compact_recv();

    reconnect_delay_ns_ = 0;
    set_state(WsState::Open);
    arm_ping();
    if (recv_.size() > 0) {
        parse_frames();
    }
}

template <Stream Transport>
void WsSession<Transport>::drive_frame_read() {
    std::span<uint8_t> spare = recv_.spare();
    if (spare.empty()) {
        LEPTON_LOG_WARN("ws: recv buffer full, framing exceeds capacity");
        teardown(WsState::Failed);
        return;
    }
    sys::io_result n = transport_.read(spare);
    if (n == -EAGAIN) {
        return;
    }
    if (n == 0) {
        teardown(WsState::Disconnected);
        return;
    }
    if (n < 0) {
        teardown(WsState::Failed);
        return;
    }
    recv_.append(static_cast<std::size_t>(n));
    parse_frames();
}

template <Stream Transport>
void WsSession<Transport>::parse_frames() {
    for (;;) {
        std::span<const uint8_t> buf = recv_.bytes();
        WsFrameHeader hdr;
        std::size_t hlen = ws_parse_header(buf, hdr);
        if (hlen == 0) {
            break;
        }
        if (hlen == kWsParseError) {
            teardown(WsState::Failed);
            return;
        }
        std::size_t total = hlen + static_cast<std::size_t>(hdr.payload_len);
        if (buf.size() < total) {
            break;
        }

        const uint8_t* payload = buf.data() + hlen;
        std::size_t plen = static_cast<std::size_t>(hdr.payload_len);
        dispatch_frame(hdr, {payload, plen});

        recv_.consume(total);
        if (state_ != WsState::Open && state_ != WsState::Closing) {
            return;
        }
    }
    compact_recv();
}

template <Stream Transport>
void WsSession<Transport>::dispatch_frame(const WsFrameHeader& hdr, std::span<const uint8_t> payload) {
    switch (hdr.opcode) {
        case WsOpcode::Text:
        case WsOpcode::Binary:
            if (hdr.fin) {
                deliver({hdr.opcode, payload, false});
            } else {
                begin_fragment(hdr.opcode, payload);
            }
            break;
        case WsOpcode::Continuation:
            append_fragment(payload, hdr.fin);
            break;
        case WsOpcode::Ping:
            (void)enqueue_frame(WsOpcode::Pong, payload, /*fin=*/true);
            break;
        case WsOpcode::Pong:
            awaiting_pong_ = false;
            break;
        case WsOpcode::Close:
            teardown(WsState::Disconnected);
            break;
        default:
            break;
    }
}

template <Stream Transport>
void WsSession<Transport>::deliver(const WsMessageView& msg) {
    if (on_message_) {
        on_message_(msg);
    }
}

template <Stream Transport>
void WsSession<Transport>::begin_fragment(WsOpcode op, std::span<const uint8_t> first) {
    if (!frag_) {
        frag_ = pool_.acquire(0);
        if (!frag_) {
            teardown(WsState::Failed);
            return;
        }
    }
    frag_.reset(0);
    frag_opcode_ = op;
    append_fragment(first, false);
}

template <Stream Transport>
void WsSession<Transport>::append_fragment(std::span<const uint8_t> chunk, bool fin) {
    if (!frag_) {
        return;
    }
    if (chunk.size() > frag_.tailroom()) {
        teardown(WsState::Failed);
        return;
    }
    std::memcpy(frag_.append(chunk.size()).data(), chunk.data(), chunk.size());
    if (fin) {
        deliver({frag_opcode_, frag_.bytes(), false});
        pool_.release(frag_);
    }
}

template <Stream Transport>
SendStatus WsSession<Transport>::enqueue_frame(WsOpcode opcode, std::span<const uint8_t> payload, bool fin) {
    const std::size_t hsize = ws_client_header_size(payload.size());
    IOBuffer b = pool_.acquire(hsize);
    if (!b) {
        return SendStatus::WouldBlock;
    }
    if (payload.size() > b.tailroom()) {
        pool_.release(b);
        return SendStatus::TooLarge;
    }
    std::memcpy(b.append(payload.size()).data(), payload.data(), payload.size());

    uint32_t key = mask_gen_.next();
    uint8_t key_bytes[4];
    std::memcpy(key_bytes, &key, 4);
    ws_mask(b.data(), b.size(), key_bytes, 0);

    std::span<uint8_t> hdr = b.prepend(hsize);
    std::size_t written = ws_encode_client_header(hdr, opcode, payload.size(), key_bytes, fin);
    if (written != hsize) {
        pool_.release(b);
        return SendStatus::TooLarge;
    }

    if (!push_pending(b)) {
        pool_.release(b);
        return SendStatus::WouldBlock;
    }
    flush_pending();
    return SendStatus::Ok;
}

template <Stream Transport>
bool WsSession<Transport>::push_pending(IOBuffer& b) {
    if (pending_count_ >= kMaxPending) {
        return false;
    }
    std::size_t idx = (pending_head_ + pending_count_) % kMaxPending;
    pending_[idx].buf = b;
    pending_[idx].sent = 0;
    
    bool first_write = (pending_count_ == 0);
    ++pending_count_;
    b = IOBuffer{};
    
    if (first_write) {
        loop_.notify_write_interest(*this, true);
    }
    return true;
}

template <Stream Transport>
void WsSession<Transport>::flush_pending() {
    while (pending_count_ > 0) {
        Pending& p = pending_[pending_head_];
        std::span<const uint8_t> rest = {p.buf.data() + p.sent, p.buf.size() - p.sent};
        sys::io_result n = transport_.write(rest, /*more=*/false);
        if (n == -EAGAIN) {
            return;
        }
        if (n < 0) {
            teardown(WsState::Failed);
            return;
        }
        p.sent += static_cast<std::size_t>(n);
        if (p.sent >= p.buf.size()) {
            pool_.release(p.buf);
            pending_head_ = (pending_head_ + 1) % kMaxPending;
            --pending_count_;
            if (state_ == WsState::Closing && pending_count_ == 0) {
                teardown(WsState::Disconnected);
                return;
            }
        } else {
            return;
        }
    }
    loop_.notify_write_interest(*this, false);
}

template <Stream Transport>
void WsSession<Transport>::arm_ping() noexcept {
    awaiting_pong_ = false;
    if (ping_interval_ns_ > 0) {
        next_ping_at_ns_ = TscClock::tscns() + ping_interval_ns_;
    }
}

template <Stream Transport>
void WsSession<Transport>::check_keepalive() noexcept {
    if (ping_interval_ns_ <= 0) {
        return;
    }
    int64_t now = TscClock::tscns();
    if (awaiting_pong_) {
        if (pong_timeout_ns_ > 0 && now >= pong_deadline_ns_) {
            LEPTON_LOG_WARN("ws: pong timeout, connection stale");
            teardown(WsState::Failed);
        }
        return;
    }
    if (now >= next_ping_at_ns_) {
        (void)enqueue_frame(WsOpcode::Ping, {}, /*fin=*/true);
        awaiting_pong_ = true;
        pong_deadline_ns_ = now + pong_timeout_ns_;
        next_ping_at_ns_ = now + ping_interval_ns_;
    }
}

template <Stream Transport>
void WsSession<Transport>::compact_recv() noexcept {
    std::size_t sz = recv_.size();
    if (sz == 0) {
        recv_.reset(0);
        return;
    }
    if (recv_.headroom() == 0) {
        return;
    }
    std::memmove(recv_base(), recv_.data(), sz);
    recv_.reset(0);
    recv_.append(sz);
}

template <Stream Transport>
uint8_t* WsSession<Transport>::recv_base() noexcept {
    return recv_.data() - recv_.headroom();
}

template <Stream Transport>
void WsSession<Transport>::set_state(WsState s) {
    if (s == state_) {
        return;
    }
    state_ = s;
    if (on_state_) {
        on_state_(s);
    }
}

template <Stream Transport>
void WsSession<Transport>::teardown(WsState next) {
    transport_.close();
    drain_pending();
    if (frag_) {
        pool_.release(frag_);
    }
    if (next == WsState::Disconnected || next == WsState::Failed) {
        if (auto_reconnect_) {
            reconnect_at_ns_ = TscClock::tscns() + reconnect_delay_ns_;
        }
    }
    set_state(next);
}

template <Stream Transport>
void WsSession<Transport>::drain_pending() noexcept {
    while (pending_count_ > 0) {
        pool_.release(pending_[pending_head_].buf);
        pending_head_ = (pending_head_ + 1) % kMaxPending;
        --pending_count_;
    }
}

template <Stream Transport>
void WsSession<Transport>::release_buffers() noexcept {
    drain_pending();
    if (recv_) {
        pool_.release(recv_);
    }
    if (frag_) {
        pool_.release(frag_);
    }
}

template <Stream Transport>
void WsSession<Transport>::check_timeouts() noexcept {
    if ((state_ == WsState::Connecting || state_ == WsState::Upgrading) && connect_deadline_ns_ > 0) {
        if (TscClock::tscns() >= connect_deadline_ns_) {
            LEPTON_LOG_WARN("ws: connection timeout");
            teardown(WsState::Failed);
        }
    }
}

} // namespace lepton::net
