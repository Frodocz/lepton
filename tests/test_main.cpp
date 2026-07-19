#include "lepton/base/logger.h"

#include <gtest/gtest.h>

int main(int argc, char** argv) {
    lepton::init_logger({.level = lepton::LogLevel::Debug});
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
