#include "lepton/base/buffer_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

namespace lepton {
namespace {

TEST(BufferPoolTest, ConstructorAndMetadata) {
    // 64-byte alignment test
    BufferPool pool(10, 100, false);
    EXPECT_TRUE(pool.valid());
    EXPECT_EQ(pool.available(), 10u);
    // 100 rounded up to 64-byte multiple is 128
    EXPECT_EQ(pool.buffer_size(), 128u);
}

TEST(BufferPoolTest, AcquireAndReleaseLifecycle) {
    BufferPool pool(5, 128, false);
    EXPECT_EQ(pool.available(), 5u);

    // Acquire one
    IOBuffer buf = pool.acquire();
    EXPECT_TRUE(buf);
    EXPECT_EQ(pool.available(), 4u);
    EXPECT_EQ(buf.capacity(), 128u);
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_EQ(buf.headroom(), 0u);
    EXPECT_EQ(buf.tailroom(), 128u);

    // Write some data (append)
    std::span<uint8_t> spare = buf.spare();
    EXPECT_EQ(spare.size(), 128u);
    
    buf.append(50);
    EXPECT_EQ(buf.size(), 50u);
    EXPECT_EQ(buf.tailroom(), 78u);
    EXPECT_EQ(buf.headroom(), 0u);

    // Release
    pool.release(buf);
    EXPECT_FALSE(buf); // Handle cleared
    EXPECT_EQ(pool.available(), 5u);
}

TEST(BufferPoolTest, AcquireExhaustion) {
    BufferPool pool(3, 64, false);
    
    IOBuffer b1 = pool.acquire();
    IOBuffer b2 = pool.acquire();
    IOBuffer b3 = pool.acquire();
    
    EXPECT_TRUE(b1);
    EXPECT_TRUE(b2);
    EXPECT_TRUE(b3);
    EXPECT_EQ(pool.available(), 0u);

    // 4th acquire should fail
    IOBuffer b4 = pool.acquire();
    EXPECT_FALSE(b4);

    // Release one
    pool.release(b2);
    EXPECT_EQ(pool.available(), 1u);
    
    // Acquire again
    IOBuffer b5 = pool.acquire();
    EXPECT_TRUE(b5);
    EXPECT_EQ(pool.available(), 0u);
    
    pool.release(b1);
    pool.release(b3);
    pool.release(b5);
}

TEST(BufferPoolTest, LIFOCacheLocality) {
    BufferPool pool(3, 64, false);
    
    IOBuffer b1 = pool.acquire();
    IOBuffer b2 = pool.acquire();
    
    void* addr1 = b1.data();
    void* addr2 = b2.data();
    
    pool.release(b1);
    pool.release(b2);
    
    // Since it is LIFO, the next acquire should return b2 (the last released)
    IOBuffer b3 = pool.acquire();
    EXPECT_EQ(b3.data(), addr2);
    
    IOBuffer b4 = pool.acquire();
    EXPECT_EQ(b4.data(), addr1);

    pool.release(b3);
    pool.release(b4);
}

TEST(IOBufferTest, PrependAndHeadroom) {
    BufferPool pool(2, 256, false);
    
    // Acquire with reserve_front headroom
    IOBuffer buf = pool.acquire(64);
    EXPECT_EQ(buf.headroom(), 64u);
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_EQ(buf.tailroom(), 192u); // 256 - 64

    // Append payload
    buf.append(100);
    EXPECT_EQ(buf.size(), 100u);
    
    // Prepend header
    std::span<uint8_t> header = buf.prepend(20);
    EXPECT_EQ(header.size(), 20u);
    EXPECT_EQ(buf.size(), 120u);
    EXPECT_EQ(buf.headroom(), 44u);

    // Consume from front
    buf.consume(10);
    EXPECT_EQ(buf.size(), 110u);
    EXPECT_EQ(buf.headroom(), 54u);

    pool.release(buf);
}

TEST(BufferPoolTest, MultiThreadedConcurrentAcquireRelease) {
    constexpr int kNumThreads = 8;
    constexpr int kNumIterations = 10000;
    constexpr std::size_t kPoolSize = 32;
    BufferPool pool(kPoolSize, 64, false);

    std::atomic<bool> start_gate{false};
    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);

    std::atomic<int> success_count{0};

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&pool, &start_gate, &success_count, t]() {
            // Spin until all threads are ready to maximize contention
            while (!start_gate.load(std::memory_order_relaxed));

            for (int i = 0; i < kNumIterations; ++i) {
                IOBuffer buf = pool.acquire();
                if (buf) {
                    // Write thread ID to the first 4 bytes of payload to assert mutual exclusion
                    std::span<uint8_t> data = buf.append(sizeof(t));
                    std::memcpy(data.data(), &t, sizeof(t));
                    
                    // Simulate minor work
                    std::this_thread::yield();
                    
                    // Read back thread ID: if any other thread obtained this buffer, this will fail
                    int val = -1;
                    std::memcpy(&val, data.data(), sizeof(val));
                    EXPECT_EQ(val, t);

                    pool.release(buf);
                    success_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    // Pool empty, yield and retry
                    std::this_thread::yield();
                    --i; // retry
                }
            }
        });
    }

    // Release all threads simultaneously
    start_gate.store(true, std::memory_order_release);

    for (auto& thread : threads) {
        thread.join();
    }

    // Assert that the final pool count is exactly the initial pool size (no leaks or duplicates)
    EXPECT_EQ(pool.available(), kPoolSize);
    
    // Assert all iterations completed successfully
    EXPECT_EQ(success_count.load(), kNumThreads * kNumIterations);
}

} // namespace
} // namespace lepton
