#include "test_helpers.hpp"

#include <cmath>
#include <limits>

TEST(ParseHex, ThreeBytes)
{
    auto bytes = parse_hex("48FFC0");
    ASSERT_TRUE(bytes.has_value());
    ASSERT_EQ(bytes->size(), 3u);
    EXPECT_EQ((*bytes)[0], 0x48);
    EXPECT_EQ((*bytes)[1], 0xFF);
    EXPECT_EQ((*bytes)[2], 0xC0);
}

TEST(ParseHex, SpacesAllowed)
{
    auto bytes = parse_hex("48 FF C0");
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes->size(), 3u);
}

TEST(ParseHex, WhitespaceAroundAndBetweenBytes)
{
    auto bytes = parse_hex(" \t48\n FF\rC0 ");
    ASSERT_TRUE(bytes.has_value());
    ASSERT_EQ(bytes->size(), 3u);
    EXPECT_EQ((*bytes)[0], 0x48);
    EXPECT_EQ((*bytes)[1], 0xFF);
    EXPECT_EQ((*bytes)[2], 0xC0);
}

TEST(ParseHex, WhitespaceSplitsNibble) { EXPECT_FALSE(parse_hex("4 F").has_value()); }

TEST(ParseHex, DanglingNibble) { EXPECT_FALSE(parse_hex("4").has_value()); }

TEST(ParseHex, NonHex) { EXPECT_FALSE(parse_hex("zz").has_value()); }

TEST(ParseHex, Empty)
{
    auto bytes = parse_hex("");
    ASSERT_TRUE(bytes.has_value());
    EXPECT_TRUE(bytes->empty());
}

TEST(ParseSeed, HexPrefix)
{
    auto value = parse_seed("0x1F");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x1Full);
}

TEST(ParseSeed, BareHex)
{
    auto value = parse_seed("1F");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x1Full);
}

TEST(ParseSeed, EmptyIsZero)
{
    auto value = parse_seed("");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0ull);
}

TEST(ParseSeed, NonHex) { EXPECT_FALSE(parse_seed("zz").has_value()); }

TEST(ParseSeed, Overflow) { EXPECT_FALSE(parse_seed("10000000000000000").has_value()); }

TEST(ParseSeed, EmptyHexPrefix) { EXPECT_FALSE(parse_seed("0x").has_value()); }

TEST(ParseDecimal, Double)
{
    auto parsed = parse_decimal_seed("1.0", "XMM");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_DOUBLE_EQ(parsed->value, 1.0);
    EXPECT_FALSE(parsed->is_single);
}

TEST(ParseDecimal, Single)
{
    auto parsed = parse_decimal_seed("1.1F", "XMM");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_DOUBLE_EQ(parsed->value, (double)1.1f);
    EXPECT_TRUE(parsed->is_single);
}

TEST(ParseDecimal, LongDoubleSuffix)
{
    auto parsed = parse_decimal_seed("1.0l", "XMM");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_DOUBLE_EQ(parsed->value, 1.0);
    EXPECT_FALSE(parsed->is_single);
}

TEST(ParseDecimal, Negative)
{
    auto parsed = parse_decimal_seed("-1.5", "XMM");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_DOUBLE_EQ(parsed->value, -1.5);
    EXPECT_FALSE(parsed->is_single);
}

TEST(ParseDecimal, SignedNonFinitePrecision)
{
    auto infinity = parse_decimal_seed("-inff", "XMM");
    ASSERT_TRUE(infinity.has_value());
    EXPECT_EQ(infinity->value, -(double)std::numeric_limits<float>::infinity());
    EXPECT_TRUE(infinity->is_single);

    auto nan = parse_decimal_seed("-nan", "XMM");
    ASSERT_TRUE(nan.has_value());
    EXPECT_TRUE(std::isnan(nan->value));
    EXPECT_TRUE(std::signbit(nan->value));
    EXPECT_FALSE(nan->is_single);
}

TEST(ParseDecimal, BareDotIsError) { EXPECT_FALSE(parse_decimal_seed(".", "XMM").has_value()); }

TEST(ParseDecimal, DoubleSuffixIsError) { EXPECT_FALSE(parse_decimal_seed("1.0ff", "XMM").has_value()); }
