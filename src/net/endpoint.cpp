/// @file endpoint.cpp
/// @brief Blocking DNS resolution (cold path only). See endpoint.h.

#include "lepton/net/endpoint.h"

#include "lepton/base/logger.h"

#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <netdb.h>

namespace lepton::net {

std::optional<Endpoint> Endpoint::resolve(std::string_view host, uint16_t port) {
    if (host.size() >= 256) [[unlikely]] {
        LEPTON_LOG_WARN("host='{}' is too long, pass a reduced version", host);
        return std::nullopt;
    }

    // Avoid heap allocation, getaddrinfo needs a NUL-terminated host string.
    char host_buffer[256];
    std::memcpy(host_buffer, host.data(), host.size());
    host_buffer[host.size()] = '\0';

    struct addrinfo hints {};
    hints.ai_family = AF_INET;       // IPv4 only for now
    hints.ai_socktype = SOCK_STREAM; // TCP

    struct addrinfo* result = nullptr;
    int rc = ::getaddrinfo(host_buffer, nullptr, &hints, &result);
    if (rc != 0 || result == nullptr) {
        LEPTON_LOG_WARN("resolve failed for host='{}': {}", host_buffer,
                        rc != 0 ? ::gai_strerror(rc) : "no result");
        if (result != nullptr) {
            ::freeaddrinfo(result);
        }
        return std::nullopt;
    }

    // Take the first IPv4 result.
    const auto* sa = reinterpret_cast<const struct sockaddr_in*>(result->ai_addr);
    Endpoint ep{};
    ep.ip_be = sa->sin_addr.s_addr;
    ep.port_be = ::htons(port);

    ::freeaddrinfo(result);
    return ep;
}

} // namespace lepton::net
