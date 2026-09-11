#include "bme_core.hpp"

#include <gtest/gtest.h>

#include <cstdint>

// Custom entry so the engine's OS parameters are initialized before any test runs.
// Link `GTest::gtest`, not `gtest_main`.
std::int32_t main(std::int32_t argc, char **argv)
{
    bme::init();
    testing::InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}
