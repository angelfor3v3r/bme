#include "test_helpers.hpp"

#include <string>

TEST(RunQuickErrors, MissingBytes)
{
    auto cli = parse_cli({"--quick"});
    ASSERT_TRUE(cli.has_value());

    auto capture = run_quick_capture(*cli);
    EXPECT_EQ(capture.return_code, 1);
    EXPECT_NE(capture.err.find("--quick needs --bytes"), std::string::npos);
}

TEST(RunQuickErrors, BadBytes)
{
    auto cli = parse_cli({"--bytes", "zz", "--quick"});
    ASSERT_TRUE(cli.has_value());

    auto capture = run_quick_capture(*cli);
    EXPECT_EQ(capture.return_code, 1);
    EXPECT_NE(capture.err.find("Error"), std::string::npos);
}
