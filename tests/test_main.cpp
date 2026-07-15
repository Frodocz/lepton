#include <gtest/gtest.h>
#include "lepton/base/logger.h"

int main(int argc, char** argv) {
    lepton::init_logger(quill::LogLevel::Debug, false);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
