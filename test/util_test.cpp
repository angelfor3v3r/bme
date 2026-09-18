#include "util.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace bme;

TEST(AsciiCaseInsensitiveEqual, ComparesAsciiWithoutCase)
{
    EXPECT_TRUE(ascii_case_insensitive_equal("ABC", "abc"));
    EXPECT_FALSE(ascii_case_insensitive_equal("ABC", "abd"));
    EXPECT_FALSE(ascii_case_insensitive_equal("ABC", "AB"));
}

TEST(TrimStringView, AdjustsViewWithoutMutatingStorage)
{
    std::string storage{" \tvalue\r\n"};
    auto        view = trim(std::string_view{storage});
    EXPECT_EQ(view, "value");
    EXPECT_EQ(storage, " \tvalue\r\n");
}

TEST(TrimStringView, HandlesOneSidedAndBoundaryInputs)
{
    EXPECT_EQ(ltrim(std::string_view{" x "}), "x ");
    EXPECT_EQ(rtrim(std::string_view{" x "}), " x");
    EXPECT_EQ(trim(std::string_view{"x"}), "x");
    EXPECT_TRUE(trim(std::string_view{" \t\r\n"}).empty());
    EXPECT_TRUE(trim(std::string_view{}).empty());
}

TEST(TrimStringView, SupportsCustomPredicates)
{
    auto periods = [](char character) noexcept { return character == '.'; };

    EXPECT_EQ(ltrim(std::string_view{"...value..."}, periods), "value...");
    EXPECT_EQ(rtrim(std::string_view{"...value..."}, periods), "...value");
    EXPECT_EQ(trim(std::string_view{"...value..."}, periods), "value");
}
