/// @file ws_mask.cpp
/// @brief OS-entropy seed for the WebSocket mask-key PRNG (cold path). See
///        ws_mask.h — kept out of the header to avoid pulling <sys/random.h>
///        into every translation unit that frames WebSocket messages.

#include "lepton/net/detail/ws_mask.h"

#include <ctime>

#include <sys/random.h>

namespace lepton::net {

uint64_t MaskKeyGen::seed_from_os() noexcept {
    uint64_t s = 0;
    ssize_t n = ::getrandom(&s, sizeof(s), 0);
    if (n != static_cast<ssize_t>(sizeof(s)) || s == 0) {
        // Extremely unlikely fallback: mix a couple of coarse clocks so we never
        // seed with a constant. Still only the *mask* key, not a security token.
        s = static_cast<uint64_t>(::time(nullptr));
        s ^= (s << 32) | 0x9E3779B97F4A7C15ULL;
    }
    return s;
}

} // namespace lepton::net
