#pragma once

/// @file endpoint.h
/// @brief IPv4 endpoint value type + cold-path DNS resolution.
///
/// Resolution uses getaddrinfo and BLOCKS — it is a startup/reconnect-path
/// helper only, never called from the busy-poll hot path. Keep the resolved
/// `Endpoint` and reuse it across reconnects.

#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <string_view>

namespace lepton::net {

/// A resolved IPv4 address:port. Trivially copyable; Safe to store.
struct Endpoint {
    uint32_t ip_be{0};    ///< IPv4 in network byte order (0 == invalid)
    uint16_t port_be{0};  ///< port in network byte order

    [[nodiscard]] bool valid() const noexcept { return ip_be != 0; }

    /// Fill a `sockaddr_in` at `out` (must be sockaddr_in-sized). Returns the
    /// byte length written. `out` is void* so callers/backends stay decoupled
    /// from the concrete sockaddr type (see sys_api.h).
    [[nodiscard]] size_t to_sockaddr(void* out) const noexcept {
        struct sockaddr_in sa {};
        sa.sin_family = AF_INET;
        sa.sin_port = port_be;
        sa.sin_addr.s_addr = ip_be;
        std::memcpy(out, &sa, sizeof(sockaddr_in));
        return sizeof(sockaddr_in);
    }

    /// Resolve `host` (name or dotted-quad) + `port` (host order) to an Endpoint.
    /// Returns an invalid Endpoint on failure. BLOCKING — cold path only.
    [[nodiscard]] Endpoint resolve(std::string_view host, uint16_t port);
};

} // namespace lepton::net
