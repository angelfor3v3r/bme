#include "common.hpp"
#include "test_helpers.hpp"

#if BME_OS_LINUX
#include <cstdlib>
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

TEST(RunQuick, SeededGprDelta)
{
    auto cli = parse_cli({"--bytes", "48FFC0", "--quick", "--seed", "rax=64"});
    ASSERT_TRUE(cli);

    auto capture = run_quick_capture(*cli);
    EXPECT_EQ(capture.return_code, 0);
    EXPECT_TRUE(capture.out.starts_with("CPU "));
    EXPECT_NE(capture.out.find("\nSeed\n"), std::string::npos);
    EXPECT_NE(capture.out.find("RAX    0x0000000000000064 -> 0x0000000000000065"), std::string::npos);
}

TEST(RunQuick, RawBinaryInput)
{
    auto                        path = std::filesystem::temp_directory_path()
                                     / ("bme-raw-input-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
    std::array<std::uint8_t, 3> code{0x48, 0xFF, 0xC0};
    {
        std::ofstream output{path, std::ios::binary};
        ASSERT_TRUE(output);

        output.write((const char *)code.data(), (std::streamsize)code.size());
        output.close();
        ASSERT_TRUE(output);
    }

    auto cli = parse_cli({"--file", path.string(), "--quick", "--seed", "rax=64"});
    ASSERT_TRUE(cli);

    auto capture = run_quick_capture(*cli);

    std::error_code remove_error{};
    std::filesystem::remove(path, remove_error);

    EXPECT_FALSE(remove_error);
    EXPECT_EQ(capture.return_code, 0);
    EXPECT_NE(capture.out.find("RAX    0x0000000000000064 -> 0x0000000000000065"), std::string::npos);
}

TEST(RunQuick, JsonEmitsOnlyDocument)
{
    auto cli = parse_cli({"--bytes", "48FFC0", "--quick", "--format", "json"});
    ASSERT_TRUE(cli);

    auto capture = run_quick_capture(*cli);
    EXPECT_EQ(capture.return_code, 0);
    EXPECT_TRUE(capture.err.empty());
    EXPECT_TRUE(capture.out.starts_with('{'));
    EXPECT_TRUE(capture.out.ends_with("}\n"));
    EXPECT_NE(capture.out.find("\"schema_version\":1"), std::string::npos);
    EXPECT_NE(capture.out.find("\"outcome\":\"finished\""), std::string::npos);
    EXPECT_NE(capture.out.find("\"execution\""), std::string::npos);
    EXPECT_NE(capture.out.find("\"decoders\""), std::string::npos);
    EXPECT_EQ(capture.out.find("CPU "), std::string::npos);
}

TEST(RunQuick, PrettyJsonUsesMultilineFormatting)
{
    auto cli = parse_cli({"--bytes", "48FFC0", "--quick", "--format", "json", "--pretty"});
    ASSERT_TRUE(cli);

    auto capture = run_quick_capture(*cli);
    EXPECT_EQ(capture.return_code, 0);
    EXPECT_TRUE(capture.err.empty());
    EXPECT_TRUE(capture.out.starts_with("{\n  \"schema_version\": 1,"));
    EXPECT_TRUE(capture.out.ends_with("}\n"));
    EXPECT_NE(capture.out.find("\"outcome\": \"finished\""), std::string::npos);
}

TEST(RunQuick, FaultShowsPartialStateAndRemainingStaticRows)
{
    auto cli = parse_cli({"--bytes", "F490", "--quick"});
    ASSERT_TRUE(cli);

    auto capture = run_quick_capture(*cli);
    EXPECT_EQ(capture.return_code, 0);
    EXPECT_NE(capture.out.find("!0    F4"), std::string::npos);
    EXPECT_NE(capture.out.find("partial state"), std::string::npos);
    EXPECT_NE(capture.out.find("(not reached)"), std::string::npos);
}

TEST(RunQuickErrors, MissingBytes)
{
    auto cli = parse_cli({"--quick"});
    ASSERT_TRUE(cli);

    auto capture = run_quick_capture(*cli);
    EXPECT_EQ(capture.return_code, 1);
    EXPECT_NE(capture.err.find("--quick needs --bytes"), std::string::npos);
}

TEST(RunQuickErrors, EmptyBytes)
{
    auto cli = parse_cli({"--bytes", "", "--quick"});
    ASSERT_TRUE(cli);

    auto capture = run_quick_capture(*cli);
    EXPECT_EQ(capture.return_code, 1);
    EXPECT_NE(capture.err.find("No code to run"), std::string::npos);
}

TEST(RunQuickErrors, BadBytes)
{
    auto cli = parse_cli({"--bytes", "zz", "--quick"});
    ASSERT_TRUE(cli);

    auto capture = run_quick_capture(*cli);
    EXPECT_EQ(capture.return_code, 1);
    EXPECT_NE(capture.err.find("Invalid hex byte"), std::string::npos);
}

#if BME_OS_LINUX
TEST(RunQuickFpuDeathTest, UnmaskedX87ExceptionsDoNotTerminateFormatter)
{
    auto cli = parse_cli({"--bytes", "D92FD9EB", "--quick", "--track", "x87"});
    ASSERT_TRUE(cli);

    // `EXPECT_EXIT` isolates a possible `SIGFPE`.
    // `std::_Exit` ends its child without running inherited cleanup.
    EXPECT_EXIT(std::_Exit(run_quick(*cli)), testing::ExitedWithCode(0), "");
}
#endif
