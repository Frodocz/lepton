/// @file endpoint.cpp
/// @brief Blocking DNS resolution (cold path only). See endpoint.h.

#include "lepton/net/endpoint.h"

#include "lepton/base/logger.h"

#include <netdb.h>
#include <arpa/inet.h>

#include <string>

namespace lepton::net {

Endpoint resolve(std::string_view host, uint16_t port) {
    // getaddrinfo needs a NUL-terminated host string.
    std::string host_str{host};

    struct addrinfo hints {};
    hints.ai_family = AF_INET;       // IPv4 only for now
    hints.ai_socktype = SOCK_STREAM; // TCP

    struct addrinfo* result = nullptr;
    int rc = ::getaddrinfo(host_str.c_str(), nullptr, &hints, &result);
    if (rc != 0 || result == nullptr) {
        LEPTON_LOG_WARN("resolve failed for host='{}': {}", host_str,
                        rc != 0 ? ::gai_strerror(rc) : "no result");
        if (result != nullptr) {
            ::freeaddrinfo(result);
        }
        return {};
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