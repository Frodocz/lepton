#pragma once

/// @file io_buffer.h
/// @brief Fixed-capacity I/O buffer with a reservable front prefix.
///
/// The "reserve-ahead" design lets a protocol layer write its header *backwards* 
/// into a reserved prefix so the payload never moves on send
/// Buffers are owned by a `BufferPool`; `IOBuffer` is a lightweight handle.

#include "lepton/base/attributes.h"

#include <cassert>
#include <cstddef>
#include <span>

namespace lepton {

class BufferPool;

/// A view over a slice of a pooled buffer. Non-owning; validity is governed by
/// the pool. Layout: [ ...reserved front... | data(len) | ...spare... ]
class IOBuffer
{
public:
    IOBuffer() = default;

    [[nodiscard]] std::byte* data() noexcept { return base_ + head_; }
    [[nodiscard]] const std::byte* data() const noexcept { return base_ + head_; }
    [[nodiscard]] std::size_t size() const noexcept { return len_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }
    [[nodiscard]] std::size_t headroom() const noexcept { return head_; }
    [[nodiscard]] std::size_t tailroom() const noexcept { return cap_ - head_ - len_; }

    [[nodiscard]] std::span<std::byte> bytes() noexcept { return {data(), len_}; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data(), len_}; }

    /// Whole writable region from the current payload end (== tailroom span).
    [[nodiscard]] std::span<std::byte> spare() noexcept { return {data() + len_, tailroom()}; }

    /// Grow the logical payload by `n` (must fit in tailroom). Returns the newly
    /// exposed span for the caller to fill.
    LEPTON_ALWAYS_INLINE std::span<std::byte> append(std::size_t n) noexcept {
        assert(n <= tailroom());
        std::byte* p = data() + len_;
        len_ += n;
        return {p, n};
    }

    /// Move the head backwards by `n` bytes into the reserved prefix and return
    /// that region — used to prepend a protocol header with no payload copy.
    /// Precondition: n <= headroom().
    LEPTON_ALWAYS_INLINE std::span<std::byte> prepend(std::size_t n) noexcept
    {
        assert(n <= head_);
        head_ -= n;
        len_ += n;
        return {base_ + head_, n};
    }

    /// Consume `n` bytes from the front (advance head, shrink len).
    LEPTON_ALWAYS_INLINE void consume(std::size_t n) noexcept {
        assert(n <= len_);
        head_ += n;
        len_ -= n;
    }

    /// Reset to empty with `reserve_front` bytes of headroom available.
    void reset(std::size_t reserve_front = 0) noexcept {
        assert(reserve_front <= cap_);
        head_ = reserve_front;
        len_ = 0;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return base_ != nullptr; }
private:
    friend class BufferPool;
    std::byte* base_{nullptr};  ///< start of the backing storage
    std::size_t cap_{0};      ///< total backing capacity
    std::size_t head_{0};     ///< offset of first payload byte
    std::size_t len_{0};      ///< payload length
};

} // namespace lepton
