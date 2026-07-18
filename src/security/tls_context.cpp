/// @file tls_context.cpp
/// @brief OpenSSL SSL_CTX setup (cold path). See tls_context.h.

#include "lepton/security/tls_context.h"

#include "lepton/base/lepton_error.h"
#include "lepton/base/logger.h"

#include <cstring>
#include <string>
#include <vector>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace lepton::security {

namespace {

[[noreturn]] void throw_ssl(const char* what) {
    unsigned long e = ::ERR_get_error();
    char buf[256] = {0};
    if (e != 0) {
        ::ERR_error_string_n(e, buf, sizeof(buf));
    }
    std::string msg = lepton::fmt::format("{}: {}", what, buf[0] ? buf : "unknown OpenSSL error");
    LEPTON_THROW(lepton::LeptonError{std::move(msg)});
}

} // namespace

TlsContext::TlsContext(const Options& opts) : verify_peer_{opts.verify_peer} {
    ctx_ = ::SSL_CTX_new(TLS_client_method());
    if (ctx_ == nullptr) {
        throw_ssl("SSL_CTX_new failed");
    }

    // Modern floor: TLS 1.2 minimum (exchanges require 1.2+; 1.3 negotiated).
    if (::SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION) != 1) {
        throw_ssl("set_min_proto_version failed");
    }

    // CA trust store (Dual-Path stack/heap optimization to prevent stack overflow/truncation)
    if (!opts.ca_file.empty()) {
        if (opts.ca_file.size() < 256) {
            // Path is short: use small 256-byte stack buffer (0 heap allocation)
            char path_buf[256];
            std::memcpy(path_buf, opts.ca_file.data(), opts.ca_file.size());
            path_buf[opts.ca_file.size()] = '\0';

            if (::SSL_CTX_load_verify_locations(ctx_, path_buf, nullptr) != 1) {
                throw_ssl("load_verify_locations failed");
            }
        } else {
            // Abnormally long path: fallback to heap-allocated string safely
            std::string ca{opts.ca_file};
            if (::SSL_CTX_load_verify_locations(ctx_, ca.c_str(), nullptr) != 1) {
                throw_ssl("load_verify_locations failed");
            }
        }
    } else if (::SSL_CTX_set_default_verify_paths(ctx_) != 1) {
        throw_ssl("set_default_verify_paths failed");
    }

    ::SSL_CTX_set_verify(ctx_, opts.verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);

    // ALPN configuration (supports natural std::vector<std::string_view> list)
    if (!opts.alpn.empty()) {
        std::size_t total_size = 0;
        for (const auto& proto : opts.alpn) {
            if (!proto.empty()) {
                if (proto.size() > 255) [[unlikely]] {
                    LEPTON_THROW(lepton::LeptonError{"ALPN individual protocol name exceeds maximum RFC 7301 limit of 255 bytes"});
                }
                total_size += 1 + proto.size();
            }
        }

        if (total_size > 0) {
            constexpr std::size_t kStackThreshold = 128;
            
            if (total_size <= kStackThreshold) {
                // Stack path: zero heap allocations
                unsigned char wire[kStackThreshold];
                unsigned wire_idx = 0;
                for (const auto& proto : opts.alpn) {
                    if (!proto.empty()) {
                        wire[wire_idx++] = static_cast<unsigned char>(proto.size());
                        std::memcpy(wire + wire_idx, proto.data(), proto.size());
                        wire_idx += proto.size();
                    }
                }
                if (::SSL_CTX_set_alpn_protos(ctx_, wire, wire_idx) != 0) {
                    throw_ssl("set_alpn_protos failed");
                }
            } else {
                // Fallback heap path: dynamically sized vector to prevent truncation or overflow
                std::vector<unsigned char> wire(total_size);
                unsigned wire_idx = 0;
                for (const auto& proto : opts.alpn) {
                    if (!proto.empty()) {
                        wire[wire_idx++] = static_cast<unsigned char>(proto.size());
                        std::memcpy(wire.data() + wire_idx, proto.data(), proto.size());
                        wire_idx += proto.size();
                    }
                }
                if (::SSL_CTX_set_alpn_protos(ctx_, wire.data(), wire_idx) != 0) {
                    throw_ssl("set_alpn_protos failed");
                }
            }
        }
    }

    // Let OpenSSL move the write buffer between retries on nonblocking sockets.
    ::SSL_CTX_set_mode(ctx_, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_ENABLE_PARTIAL_WRITE);

    LEPTON_LOG_INFO("TlsContext ready (verify_peer={})", opts.verify_peer);
}

TlsContext::~TlsContext() {
    if (ctx_ != nullptr) {
        ::SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

} // namespace lepton::security
