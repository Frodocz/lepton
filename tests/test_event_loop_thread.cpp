#include "lepton/net/event_loop_thread.h"
#include "lepton/base/tsc_clock.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace lepton;
using namespace lepton::net;

#if defined(LEPTON_USE_FSTACK)
TEST(EventLoopThreadTest, SkippedUnderFStack) {
    GTEST_SKIP() << "Skipping EventLoopThreadTest under F-Stack mode due to multi-threading and single-run EAL constraints.";
}
#else
TEST(EventLoopThreadTest, BasicLifecycleAndStepHook) {
    TscClock::calibrate();

    EventLoopThread loop_thread;
    std::atomic<int> counter{0};

    // Set a step hook to increment a counter
    loop_thread.set_step_hook([&]() {
        counter.fetch_add(1, std::memory_order_relaxed);
    });

    // Start the thread (non-blocking)
    ASSERT_TRUE(loop_thread.start());

    // Wait until the loop thread runs at least 10 cycles
    int retries = 0;
    while (counter.load(std::memory_order_relaxed) < 10 && retries < 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        retries++;
    }

    // Stop it manually from the main thread
    loop_thread.stop();

    // Verify it stopped
    EXPECT_FALSE(loop_thread.is_running());
    EXPECT_GE(counter.load(), 10);
}

TEST(EventLoopThreadTest, ManualStop) {
    EventLoopThread loop_thread;
    std::atomic<int> counter{0};

    loop_thread.set_step_hook([&]() {
        counter.fetch_add(1, std::memory_order_relaxed);
    });

    ASSERT_TRUE(loop_thread.start());
    EXPECT_TRUE(loop_thread.is_running());

    // Sleep a bit to let it run
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Manually stop it from the main thread
    loop_thread.stop();
    EXPECT_FALSE(loop_thread.is_running());
    EXPECT_GT(counter.load(), 0);
}

TEST(EventLoopThreadTest, CustomConfiguration) {
    EventLoopThreadConfig cfg;
    cfg.loop_cfg.busy_poll = true;
    cfg.pool_count = 64;
    cfg.pool_buf_size = 8192;
    cfg.pool_hugepage = false;

    EventLoopThread loop_thread(cfg);
    ASSERT_TRUE(loop_thread.start());
    EXPECT_TRUE(loop_thread.is_running());

    BufferPool* pool = loop_thread.get_pool();
    ASSERT_NE(pool, nullptr);

    loop_thread.stop();
    EXPECT_FALSE(loop_thread.is_running());
}
#endif
