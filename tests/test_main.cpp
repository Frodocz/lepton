#include "lepton/base/logger.h"
#include "lepton/init.h"

#include <gtest/gtest.h>
#include <chrono>
#include <thread>

int main(int argc, char** argv) {
    lepton::init_logger({
        .level = lepton::LogLevel::Debug,
        .to_console = true
    });

    if (lepton::init(argc, argv, "lepton_test") < 0) {
        return 1;
    }

    // Background logger polling thread using C++20 std::jthread and std::stop_token
    std::jthread logger_thread([](std::stop_token stoken) {
        lepton::PollLoggerScope scope;
        while (!stoken.stop_requested()) {
            lepton::poll_logger_for(100);
        }
    });

    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
