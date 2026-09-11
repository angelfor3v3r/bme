#include "test_helpers.hpp"

#include <string>

TEST(RunQuick, SeededGprDelta)
{
    auto cli = parse_cli({"--bytes", "48FFC0", "--quick", "--seed", "rax=64"});
    ASSERT_TRUE(cli.has_value());

    auto capture = run_quick_capture(*cli);
    EXPECT_EQ(capture.return_code, 0);
    EXPECT_NE(capture.out.find("RAX    0x0000000000000064 -> 0x0000000000000065"), std::string::npos);
}

TEST(RunQuickErrors, MissingBytes)
{
    auto cli = parse_cli({"--quick"});
    ASSERT_TRUE(cli.has_value());

    auto capture = run_quick_capture(*cli);
    EXPECT_EQ(capture.return_code, 1);
    EXPECT_NE(capture.err.find("--quick needs --bytes"), std::string::npos);
}

TEST(RunQuickErrors, EmptyBytes)
{
    auto cli = parse_cli({"--bytes", "", "--quick"});
    ASSERT_TRUE(cli.has_value());

    auto capture = run_quick_capture(*cli);
    EXPECT_EQ(capture.return_code, 1);
    EXPECT_NE(capture.err.find("No code to run"), std::string::npos);
}

TEST(RunQuickErrors, WhitespaceOnlyBytes)
{
    auto cli = parse_cli({"--bytes", " \t\n", "--quick"});
    ASSERT_TRUE(cli.has_value());

    auto capture = run_quick_capture(*cli);
    EXPECT_EQ(capture.return_code, 1);
    EXPECT_NE(capture.err.find("No code to run"), std::string::npos);
}

TEST(RunQuickErrors, BadBytes)
{
    auto cli = parse_cli({"--bytes", "zz", "--quick"});
    ASSERT_TRUE(cli.has_value());

    auto capture = run_quick_capture(*cli);
    EXPECT_EQ(capture.return_code, 1);
    EXPECT_NE(capture.err.find("Error"), std::string::npos);
}
