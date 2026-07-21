/// @file tls_stream.cpp
/// @brief Async OpenSSL TLS over an owned nonblocking TcpSocket.

#include "lepton/net/security/tls_stream.h"

#include "lepton/base/logger.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cerrno>
#include <utility>

namespace lepton::security {

namespace {
constexpr std::size_t kCipherChunk = 16384 + 64;  // one TLS record + slack
}

TlsStream::~TlsStream() { close(); }

TlsStream::TlsStream(TlsStream&& o) noexcept
    : tcp_{std::move(o.tcp_)}, ctx_{o.ctx_}, ssl_{o.ssl_}, rbio_{o.rbio_},
      wbio_{o.wbio_}, host_{std::move(o.host_)}, phase_{o.phase_} {
    o.ssl_ = nullptr;
    o.rbio_ = nullptr;
    o.wbio_ = nullptr;
    o.phase_ = TlsPhase::Idle;
}

TlsStream& TlsStream::operator=(TlsStream&& o) noexcept {
    if (this != &o) {
        close();
        tcp_ = std::move(o.tcp_);
        ctx_ = o.ctx_;
        ssl_ = o.ssl_;
        rbio_ = o.rbio_;
        wbio_ = o.wbio_;
        host_ = std::move(o.host_);
        phase_ = o.phase_;
        o.ssl_ = nullptr;
        o.rbio_ = nullptr;
        o.wbio_ = nullptr;
        o.phase_ = TlsPhase::Idle;
    }
    return *this;
}

bool TlsStream::connect(const net::Endpoint& remote) {
    if (ctx_ == nullptr || !ctx_->valid()) {
        LEPTON_LOG_ERROR("TlsStream::connect without a valid TlsContext");
        phase_ = TlsPhase::Error;
        return false;
    }
    if (!tcp_.connect(remote)) {
        phase_ = TlsPhase::Error;
        return false;
    }
    phase_ = TlsPhase::ConnectingTcp;
    return true;
}

void TlsStream::begin_handshake() {
    ssl_ = ::SSL_new(ctx_->raw());
    if (ssl_ == nullptr) {
        fail();
        return;
    }
    // Memory BIOs: we shuttle ciphertext through them ourselves.
    rbio_ = ::BIO_new(BIO_s_mem());
    wbio_ = ::BIO_new(BIO_s_mem());
    if (rbio_ == nullptr || wbio_ == nullptr) {
        fail();
        return;
    }
    ::SSL_set_bio(ssl_, rbio_, wbio_);  // SSL takes ownership of both BIOs
    ::SSL_set_connect_state(ssl_);

    if (!host_.empty()) {
        ::SSL_set_tlsext_host_name(ssl_, host_.c_str());  // SNI
        if (ctx_->verify_peer()) {
            // Enable built-in hostname verification against the cert.
            ::SSL_set_hostflags(ssl_, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            ::SSL_set1_host(ssl_, host_.c_str());
        }
    }
    phase_ = TlsPhase::Handshaking;
}

net::StreamPhase TlsStream::poll_open() {
    switch (phase_) {
        case TlsPhase::ConnectingTcp: {
            net::StreamPhase tp = tcp_.poll_open();
            if (tp == net::StreamPhase::Connecting) {
                return net::StreamPhase::Connecting;
            }
            if (tp != net::StreamPhase::Open) {
                fail();
                return net::StreamPhase::Failed;
            }
            begin_handshake();  // TCP up -> start TLS
            [[fallthrough]];
        }
        case TlsPhase::Handshaking: {
            for (;;) {
                int r = ::SSL_do_handshake(ssl_);
                if (r == 1) {
                    if (!pump()) {  // flush final handshake flight
                        return net::StreamPhase::Failed;
                    }
                    phase_ = TlsPhase::Established;
                    LEPTON_LOG_INFO("TLS handshake complete ({})", ::SSL_get_version(ssl_));
                    return net::StreamPhase::Open;
                }
                int err = ::SSL_get_error(ssl_, r);
                if (err == SSL_ERROR_WANT_READ) {
                    bool read_any = false;
                    if (!pump_in(read_any)) {
                        return net::StreamPhase::Failed;
                    }
                    if (!pump_out()) {
                        return net::StreamPhase::Failed;
                    }
                    if (read_any) {
                        continue; // Got new bytes in rbio; rerun handshake immediately
                    }
                    return net::StreamPhase::Connecting; // Wait for socket readability
                } else if (err == SSL_ERROR_WANT_WRITE) {
                    if (!pump_out()) {
                        return net::StreamPhase::Failed;
                    }
                    continue; // Outbound space cleared; rerun handshake immediately
                } else {
                    LEPTON_LOG_WARN("TLS handshake error: ssl_err={}", err);
                    unsigned long ossl_err;
                    while ((ossl_err = ::ERR_get_error()) != 0) {
                        char err_buf[256];
                        ::ERR_error_string_n(ossl_err, err_buf, sizeof(err_buf));
                        LEPTON_LOG_WARN("  OpenSSL details: {}", err_buf);
                    }
                    fail();
                    return net::StreamPhase::Failed;
                }
            }
        }
        case TlsPhase::Established:
            return net::StreamPhase::Open;
        case TlsPhase::Closed:
            return net::StreamPhase::Closed;
        default:
            return net::StreamPhase::Failed;
    }
}

bool TlsStream::pump() noexcept {
    bool dummy = false;
    return pump_out() && pump_in(dummy);
}

bool TlsStream::pump_out() noexcept {
    alignas(64) thread_local uint8_t buf[kCipherChunk];

    // 1) Drain ciphertext OpenSSL produced (wbio) out to the socket.
    for (;;) {
        int n = ::BIO_read(wbio_, buf, static_cast<int>(sizeof(buf)));
        if (n <= 0) {
            break;  // nothing pending (memory BIO: <=0 means empty)
        }
        std::size_t off = 0;
        while (off < static_cast<std::size_t>(n)) {
            net::sys::io_result w =
                tcp_.write({buf + off, static_cast<std::size_t>(n) - off}, /*more=*/false);
            if (w == -EAGAIN) {
                // Socket full: push the unsent tail back into wbio so we retry
                // it on the next pump. BIO_write on a mem BIO cannot fail here.
                ::BIO_write(wbio_, buf + off, static_cast<int>(static_cast<std::size_t>(n) - off));
                return true;
            }
            if (w < 0) {
                return false;  // transport dead
            }
            off += static_cast<std::size_t>(w);
        }
    }
    return true;
}

bool TlsStream::pump_in(bool& read_any) noexcept {
    alignas(64) thread_local uint8_t buf[kCipherChunk];
    read_any = false;

    // 2) Pull ciphertext from the socket INTO rbio for OpenSSL to consume.
    for (;;) {
        if (::BIO_ctrl_pending(rbio_) >= 65536) {
            break;  // backpressure: stop reading from socket to prevent memory exhaustion
        }
        net::sys::io_result r = tcp_.read(buf);
        if (r > 0) {
            read_any = true;
            ::BIO_write(rbio_, buf, static_cast<int>(r));
        } else if (r == -EAGAIN) {
            break;  // no more ciphertext right now
        } else if (r == 0) {
            // Peer closed the TCP connection.
            return true;  // let SSL_read surface it as EOF / close_notify
        } else {
            return false;
        }
    }
    return true;
}

net::sys::io_result TlsStream::read(std::span<uint8_t> dst) noexcept {
    if (phase_ != TlsPhase::Established) {
        return -EAGAIN;
    }
    // Only pump from the network if OpenSSL doesn't already have pending data in memory.
    // This avoids redundant read() syscalls when draining multiple TLS records.
    if (::SSL_pending(ssl_) == 0 && ::BIO_ctrl_pending(rbio_) == 0) {
        if (!pump()) {
            fail();
            return -EIO;
        }
    } else {
        // Just pump outbound data in case we have write-buffered TLS control frames
        if (!pump_out()) {
            fail();
            return -EIO;
        }
    }
    int n = ::SSL_read(ssl_, dst.data(), static_cast<int>(dst.size()));
    if (n > 0) {
        return n;
    }
    int err = ::SSL_get_error(ssl_, n);
    switch (err) {
        case SSL_ERROR_WANT_READ:
            // EPOLLET + TLS fix: OpenSSL may have already buffered decrypted
            // plaintext from a previous TLS record that was read from the socket
            // in one go.  Edge-triggered epoll won't fire again for data already
            // consumed from the fd, so we must check SSL_pending() and retry.
            if (::SSL_pending(ssl_) > 0) {
                n = ::SSL_read(ssl_, dst.data(), static_cast<int>(dst.size()));
                if (n > 0) {
                    return n;
                }
            }
            [[fallthrough]];
        case SSL_ERROR_WANT_WRITE:
            // A renegotiation/handshake record may need to go out.
            if (::SSL_pending(ssl_) == 0 && ::BIO_ctrl_pending(rbio_) == 0) {
                if (!pump()) {
                    fail();
                    return -EIO;
                }
            } else {
                if (!pump_out()) {
                    fail();
                    return -EIO;
                }
            }
            return -EAGAIN;
        case SSL_ERROR_ZERO_RETURN:
            return 0;  // clean TLS close_notify
        default:
            return -EIO;
    }
}

net::sys::io_result TlsStream::write(std::span<const uint8_t> src, bool /*more*/) noexcept {
    if (phase_ != TlsPhase::Established) {
        return -EAGAIN;
    }
    if (src.empty()) {
        return 0;
    }
    int n = ::SSL_write(ssl_, src.data(), static_cast<int>(src.size()));
    if (n > 0) {
        if (!pump()) {  // push the produced ciphertext to the socket
            fail();
            return -EIO;
        }
        return n;
    }
    int err = ::SSL_get_error(ssl_, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        if (!pump()) {
            fail();
            return -EIO;
        }
        return -EAGAIN;
    }
    return -EIO;
}

void TlsStream::close() noexcept {
    if (ssl_ != nullptr) {
        if (phase_ == TlsPhase::Established) {
            ::SSL_shutdown(ssl_);  // best-effort close_notify into wbio
            pump();                // try to flush it
        }
        ::SSL_free(ssl_);  // frees the attached rbio_/wbio_ too
        ssl_ = nullptr;
        rbio_ = nullptr;
        wbio_ = nullptr;
    }
    tcp_.close();
    phase_ = TlsPhase::Closed;
}

void TlsStream::fail() noexcept {
    ::ERR_clear_error();
    if (ssl_ != nullptr) {
        ::SSL_free(ssl_);
        ssl_ = nullptr;
        rbio_ = nullptr;
        wbio_ = nullptr;
    }
    tcp_.close();
    phase_ = TlsPhase::Error;
}

} // namespace lepton::security
