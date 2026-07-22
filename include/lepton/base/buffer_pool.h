#pragma once

/// @file buffer_pool.h
/// @brief Fixed pool of equal-sized backing buffers for IOBuffer handles.
///
/// Features an intrusive free-list to eliminate heap metadata under Linux,
/// static hugepages support via mmap, memory locking, page-by-page pre-faulting
/// (warming) to prevent page faults, and bounds safety checks.
/// Under F-Stack, it uses DPDK's optimized rte_mempool as the backend.
/// Under POSIX, it uses a lock-free, thread-safe versioned-atomic stack (Treiber stack)
/// to prevent ABA problems.
/// Cacheline alignment (alignas(64)) is applied to isolate read-mostly constants
/// from write-heavy atomics, preventing false sharing.

#include "lepton/base/attributes.h"
#include "lepton/base/io_buffer.h"
#include "lepton/base/lepton_error.h"
#include "lepton/base/logger.h"
#include "lepton/init.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

#include <unistd.h>
#include <sys/mman.h>

#if defined(LEPTON_USE_FSTACK)
#include <rte_common.h>
#include <rte_errno.h>
#include <rte_memory.h>
#include <rte_mempool.h>
#endif

namespace lepton {

class BufferPool {
private:
#if !defined(LEPTON_USE_FSTACK)
    struct FreeNode {
        uint32_t next_idx;
    };
#endif

public:
    /// @param count      number of buffers
    /// @param buf_size   backing bytes per buffer (payload + reserved front)
    /// @param hugepage   use MAP_HUGETLB on the arena (POSIX fallback path only)
    BufferPool(std::size_t count, std::size_t buf_size, [[maybe_unused]] bool hugepage = true)
        : buf_size_{(buf_size + 63) & ~63}, // Align buffer size to cacheline (64 bytes)
          count_{count} {
              
#if defined(LEPTON_USE_FSTACK)
        static std::atomic<int> pool_id{0};
        int current_id = pool_id.fetch_add(1, std::memory_order_relaxed);
        LEPTON_REQUIRE(current_id < 64, "Exceeded maximum allowed DPDK mempools (64)");

        char name_buf[32];
        auto format_res = lepton::fmt::format_to_n(name_buf, sizeof(name_buf) - 1, "lepton_mp_{}", current_id);
        *format_res.out = '\0';
        
        // DPDK mempool per-core cache size. Set to 32 if count is reasonably large.
        unsigned cache_size = count_ >= 64 ? 32 : 0;
        
        mp_ = rte_mempool_create(name_buf, 
                                 static_cast<unsigned>(count_), 
                                 static_cast<unsigned>(buf_size_), 
                                 cache_size, 
                                 0,
                                 nullptr, nullptr, nullptr, nullptr,
                                 SOCKET_ID_ANY, 0);
        if (mp_ == nullptr) {
            LEPTON_LOG_ERROR("BufferPool: failed to create DPDK mempool {}: {}", name_buf, rte_strerror(rte_errno));
        } else {
            available_.store(count_, std::memory_order_relaxed);
        }
#else
        head_free_val_.store(0xFFFFFFFFULL, std::memory_order_relaxed); // version 0, index -1
        
        std::size_t total = count_ * buf_size_;
        allocated_size_ = total;
        
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
        if (hugepage) {
            const std::size_t hugepage_size = 2 * 1024 * 1024;
            allocated_size_ = (total + hugepage_size - 1) & ~(hugepage_size - 1);
            
            arena_ = static_cast<uint8_t*>(::mmap(nullptr, allocated_size_, PROT_READ | PROT_WRITE, flags | MAP_HUGETLB, -1, 0));
            if (arena_ == MAP_FAILED) {
                allocated_size_ = total;
                arena_ = static_cast<uint8_t*>(::mmap(nullptr, allocated_size_, PROT_READ | PROT_WRITE, flags, -1, 0));
                if (arena_ != MAP_FAILED) {
                    (void)::madvise(arena_, allocated_size_, MADV_NOHUGEPAGE);
                } else {
                    arena_ = nullptr;
                }
            }
        } else {
            arena_ = static_cast<uint8_t*>(::mmap(nullptr, allocated_size_, PROT_READ | PROT_WRITE, flags, -1, 0));
            if (arena_ == MAP_FAILED) {
                arena_ = nullptr;
            } else {
                (void)::madvise(arena_, allocated_size_, MADV_NOHUGEPAGE);
            }
        }
        
        if (arena_ != nullptr) {
            if (::mlock(arena_, allocated_size_) != 0) {
                LEPTON_LOG_WARN("BufferPool: mlock failed (errno={}): memory pages may be swapped out.", errno);
            }
            (void)::madvise(arena_, allocated_size_, MADV_DONTDUMP);

            for (std::size_t offset = 0; offset < allocated_size_; offset += 4096) {
                arena_[offset] = 0;
            }
            if (allocated_size_ > 0) {
                arena_[allocated_size_ - 1] = 0;
            }

            for (std::size_t i = 0; i < count_ - 1; ++i) {
                auto* node = reinterpret_cast<FreeNode*>(arena_ + i * buf_size_);
                node->next_idx = static_cast<uint32_t>(i + 1);
            }
            auto* last_node = reinterpret_cast<FreeNode*>(arena_ + (count_ - 1) * buf_size_);
            last_node->next_idx = std::numeric_limits<uint32_t>::max();

            head_free_val_.store(0, std::memory_order_relaxed);
            available_.store(count_, std::memory_order_relaxed);
        }
#endif
    }

    ~BufferPool() {
#if defined(LEPTON_USE_FSTACK)
        // Under F-Stack mode, we deliberately do NOT call rte_mempool_free(mp_).
        // Since F-Stack globally intercepts malloc/free, invoking DPDK's mempool free
        // can lead to boundary mixing between F-Stack's jemalloc and DPDK EAL memory,
        // corrupting glibc's fastbins and causing a crash on exit.
        // DPDK's internal mempool memory will be cleaned up by rte_eal_cleanup / process termination.
        (void)mp_;
#else
        if (arena_ != nullptr) {
            (void)::munlock(arena_, allocated_size_);
            (void)::munmap(arena_, allocated_size_);
        }
#endif
    }

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    [[nodiscard]] LEPTON_ALWAYS_INLINE IOBuffer acquire(std::size_t reserve_front = 0) noexcept {
#if defined(LEPTON_USE_FSTACK)
        if (mp_ == nullptr) [[unlikely]] {
            return {};
        }
        void* obj = nullptr;
        if (rte_mempool_get(mp_, &obj) < 0) [[unlikely]] {
            return {};
        }
        available_.fetch_sub(1, std::memory_order_relaxed);
        IOBuffer buf;
        buf.base_ = static_cast<uint8_t*>(obj);
        buf.cap_ = buf_size_;
        buf.head_ = reserve_front;
        buf.len_ = 0;
        return buf;
#else
        uint64_t old_val = head_free_val_.load(std::memory_order_acquire);
        while (true) {
            int32_t idx = static_cast<int32_t>(old_val & 0xFFFFFFFFULL);
            if (idx < 0) {
                return {}; // Empty pool
            }
            
            auto* node = reinterpret_cast<FreeNode*>(arena_ + idx * buf_size_);
            int32_t next_idx = static_cast<int32_t>(node->next_idx);
            
            uint32_t new_version = static_cast<uint32_t>((old_val >> 32) + 1);
            uint64_t new_val = (static_cast<uint64_t>(new_version) << 32) | (static_cast<uint32_t>(next_idx) & 0xFFFFFFFFULL);
            
            if (head_free_val_.compare_exchange_weak(old_val, new_val,
                                                     std::memory_order_release,
                                                     std::memory_order_acquire)) {
                available_.fetch_sub(1, std::memory_order_relaxed);
                IOBuffer buf;
                buf.base_ = arena_ + idx * buf_size_;
                buf.cap_ = buf_size_;
                buf.head_ = reserve_front;
                buf.len_ = 0;
                return buf;
            }
        }
#endif
    }

    LEPTON_ALWAYS_INLINE void release(IOBuffer& buf) noexcept {
        if (buf.base_ == nullptr) {
            return;
        }
        
#if defined(LEPTON_USE_FSTACK)
        rte_mempool_put(mp_, buf.base_);
        available_.fetch_add(1, std::memory_order_relaxed);
        buf = IOBuffer{}; // Clear handle to protect against double release
#else
#ifndef NDEBUG
        if (arena_ != nullptr) {
            const std::size_t offset = static_cast<std::size_t>(buf.base_ - arena_);
            assert(buf.base_ >= arena_ && offset < count_ * buf_size_);
            assert(offset % buf_size_ == 0);
        }
#endif

        std::size_t idx = static_cast<std::size_t>(buf.base_ - arena_) / buf_size_;
        auto* node = reinterpret_cast<FreeNode*>(buf.base_);
        
        uint64_t old_val = head_free_val_.load(std::memory_order_acquire);
        while (true) {
            int32_t old_idx = static_cast<int32_t>(old_val & 0xFFFFFFFFULL);
            node->next_idx = old_idx < 0 ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(old_idx);
            
            uint32_t new_version = static_cast<uint32_t>((old_val >> 32) + 1);
            uint64_t new_val = (static_cast<uint64_t>(new_version) << 32) | (static_cast<uint32_t>(idx) & 0xFFFFFFFFULL);
            
            if (head_free_val_.compare_exchange_weak(old_val, new_val,
                                                     std::memory_order_release,
                                                     std::memory_order_acquire)) {
                available_.fetch_add(1, std::memory_order_relaxed);
                buf = IOBuffer{}; // Clear handle to protect against double release
                return;
            }
        }
#endif
    }

    [[nodiscard]] std::size_t available() const noexcept {
#if defined(LEPTON_USE_FSTACK)
        return mp_ ? rte_mempool_avail_count(mp_) : 0;
#else
        return available_.load(std::memory_order_relaxed);
#endif
    }

    [[nodiscard]] std::size_t buffer_size() const noexcept { return buf_size_; }
    
    [[nodiscard]] bool valid() const noexcept {
#if defined(LEPTON_USE_FSTACK)
        return mp_ != nullptr;
#else
        return arena_ != nullptr;
#endif
    }

private:
    // ── Cacheline 1: Read-mostly configuration & pointers ──
    alignas(64)
#if defined(LEPTON_USE_FSTACK)
    struct rte_mempool* mp_{nullptr};
#else
    uint8_t* arena_{nullptr};
#endif
    std::size_t buf_size_{0};
    std::size_t count_{0};
    std::size_t allocated_size_{0};

    // ── Cacheline 2: Write-heavy runtime atomics ──
    alignas(64)
#if !defined(LEPTON_USE_FSTACK)
    std::atomic<uint64_t> head_free_val_{0xFFFFFFFFULL}; 
#endif
    std::atomic<std::size_t> available_{0};
};

} // namespace lepton
