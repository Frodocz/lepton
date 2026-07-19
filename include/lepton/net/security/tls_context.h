#pragma once

/// @file tls_context.h
/// @brief OpenSSL SSL_CTX wrapper. One per loop/core, shared by all TlsStreams
///        on that core. Configures CA trust, verification, TLS 1.2+ and ALPN.
///
/// Created at startup (cold path); may throw via LEPTON_THROW on misconfig.
/// Holds no per-connection state.

#include "lepton/base/attributes.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Forward-declare the OpenSSL context struct to avoid including <openssl/ssl.h>
// in this header, avoiding namespace pollution and duplicate alias warnings.
struct ssl_ctx_st;

namespace lepton::security {

class TlsContext {
public:
    struct Options {
        bool verify_peer{true};                  ///< require + verify server cert chain
        std::string_view ca_file;                ///< PEM bundle path; empty -> system CAs
        std::vector<std::string_view> alpn;     ///< List of protocols (e.g. {"h2", "http/1.1"}); empty -> none
    };

    explicit TlsContext(const Options& opts);
    ~TlsContext();

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    [[nodiscard]] struct ssl_ctx_st* raw() const noexcept { return ctx_; }
    [[nodiscard]] bool valid() const noexcept { return ctx_ != nullptr; }
    [[nodiscard]] bool verify_peer() const noexcept { return verify_peer_; }

private:
    struct ssl_ctx_st* ctx_{nullptr};
    bool verify_peer_{true};
};

} // namespace lepton::security
