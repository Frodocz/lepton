#pragma once

/// @file buffer_pool.h
/// @brief Fixed pool of equal-sized backing buffers for IOBuffer handles.
///
/// Features an intrusive free-list to eliminate heap metadata, static hugepages
/// support via mmap, memory locking, pre-faulting (warming) to prevent page faults,
/// and bounds safety checks.

#include "lepton/base/attributes.h"
#include "lepton/base/io_buffer.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

namespace lepton {

class BufferPool {
private:
    struct FreeNode {
        uint32_t next_idx;
    };

public:
    /// @param count      number of buffers
    /// @param buf_size   backing bytes per buffer (payload + reserved front)
    /// @param hugepage   use MAP_HUGETLB / MADV_HUGEPAGE on the arena
    BufferPool(std::size_t count, std::size_t buf_size, bool hugepage = true)
        : buf_size_{(buf_size + 63) & ~63}, // Align buffer size to cacheline (64 bytes)
          count_{count} {
              
        const std::size_t total = count_ * buf_size_;
        
#if defined(__linux__)
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
        if (hugepage) {
            // 1. Try allocating with explicit huge pages (MAP_HUGETLB)
            arena_ = static_cast<std::byte*>(::mmap(nullptr, total, PROT_READ | PROT_WRITE, flags | MAP_HUGETLB, -1, 0));
            if (arena_ == MAP_FAILED) {
                // 2. Fallback to standard anonymous mmap if MAP_HUGETLB is not configured
                arena_ = static_cast<std::byte*>(::mmap(nullptr, total, PROT_READ | PROT_WRITE, flags, -1, 0));
                if (arena_ != MAP_FAILED) {
                    ::madvise(arena_, total, MADV_HUGEPAGE); // Advise Transparent Huge Pages (THP)
                } else {
                    arena_ = nullptr;
                }
            }
        } else {
            arena_ = static_cast<std::byte*>(::mmap(nullptr, total, PROT_READ | PROT_WRITE, flags, -1, 0));
            if (arena_ == MAP_FAILED) {
                arena_ = nullptr;
            }
        }
        
        if (arena_ != nullptr) {
            // Lock pages in physical memory (ignore errors if system memory-lock limits prevent it)
            (void)::mlock(arena_, total);
            // Disable core-dumping this pool to prevent slow, blocking writes on crashes
            (void)::madvise(arena_, total, MADV_DONTDUMP);
        }
#else
        constexpr std::size_t kAlign = 64;
        arena_ = static_cast<std::byte*>(::aligned_alloc(kAlign, total));
        if (arena_ == nullptr) {
            return;
        }
#endif

        if (arena_ != nullptr) {
            // Pre-fault (warm) the memory pool to avoid page faults on the hot path
            for (std::size_t i = 0; i < count_; ++i) {
                arena_[i * buf_size_] = std::byte{0};
            }

            // Intrusively link all free buffers: index 0 -> 1 -> 2 -> ... -> count-1
            for (std::size_t i = 0; i < count_ - 1; ++i) {
                auto* node = reinterpret_cast<FreeNode*>(arena_ + i * buf_size_);
                node->next_idx = static_cast<uint32_t>(i + 1);
            }
            // Sentinel value for the tail of the list
            auto* last_node = reinterpret_cast<FreeNode*>(arena_ + (count_ - 1) * buf_size_);
            last_node->next_idx = std::numeric_limits<uint32_t>::max();

            head_free_idx_ = 0;
            available_ = count_;
        }
    }

    ~BufferPool() {
        if (arena_ != nullptr) {
#if defined(__linux__)
            const std::size_t total = count_ * buf_size_;
            (void)::munlock(arena_, total);
            (void)::munmap(arena_, total);
#else
            ::free(arena_);
#endif
        }
    }

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    [[nodiscard]] LEPTON_ALWAYS_INLINE IOBuffer acquire(std::size_t reserve_front = 0) noexcept {
        if (head_free_idx_ < 0) [[unlikely]] {
            return {};
        }
        
        std::size_t idx = static_cast<std::size_t>(head_free_idx_);
        auto* node = reinterpret_cast<FreeNode*>(arena_ + idx * buf_size_);
        
        // Update head to next link
        if (node->next_idx == std::numeric_limits<uint32_t>::max()) {
            head_free_idx_ = -1;
        } else {
            head_free_idx_ = static_cast<int32_t>(node->next_idx);
        }
        
        --available_;

        IOBuffer buf;
        buf.base_ = arena_ + idx * buf_size_;
        buf.cap_ = buf_size_;
        buf.head_ = reserve_front;
        buf.len_ = 0;
        return buf;
    }

    LEPTON_ALWAYS_INLINE void release(IOBuffer& buf) noexcept {
        if (buf.base_ == nullptr) {
            return;
        }
        
#ifndef NDEBUG
        // Verify buffer belongs to this pool (bounds and alignment safety check)
        const std::size_t offset = static_cast<std::size_t>(buf.base_ - arena_);
        assert(buf.base_ >= arena_ && offset < count_ * buf_size_);
        assert(offset % buf_size_ == 0);
#endif

        std::size_t idx = static_cast<std::size_t>(buf.base_ - arena_) / buf_size_;
        auto* node = reinterpret_cast<FreeNode*>(buf.base_);
        
        // Push back onto head of free-list (LIFO cache locality)
        node->next_idx = head_free_idx_ < 0 ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(head_free_idx_);
        head_free_idx_ = static_cast<int32_t>(idx);
        
        ++available_;
        buf = IOBuffer{}; // Clear handle to protect against double release
    }

    [[nodiscard]] std::size_t available() const noexcept { return available_; }
    [[nodiscard]] std::size_t buffer_size() const noexcept { return buf_size_; }
    [[nodiscard]] bool valid() const noexcept { return arena_ != nullptr; }

private:
    std::byte* arena_{nullptr};
    std::size_t buf_size_{0};
    std::size_t count_{0};
    int32_t head_free_idx_{-1};
    std::size_t available_{0};
};

} // namespace lepton
