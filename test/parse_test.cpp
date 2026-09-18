#include "test_helpers.hpp"

#include <cmath>
#include <limits>

TEST(ParseCodeText, ThreeBytes)
{
    auto bytes = parse_code_text("48FFC0");
    ASSERT_TRUE(bytes);
    ASSERT_EQ(bytes->size(), 3u);
    EXPECT_EQ((*bytes)[0], 0x48);
    EXPECT_EQ((*bytes)[1], 0xFF);
    EXPECT_EQ((*bytes)[2], 0xC0);
}

TEST(ParseCodeText, BackslashEscapes)
{
    auto bytes = parse_code_text(R"(\x48\xFF\xC0)");
    ASSERT_TRUE(bytes);
    EXPECT_EQ(*bytes, (std::vector<std::uint8_t>{0x48, 0xFF, 0xC0}));
}

TEST(ParseCodeText, BackslashEscapesAllowInterTokenWhitespace)
{
    auto bytes = parse_code_text(" \t\\x48\n\\xFF\r\\xC0 ");
    ASSERT_TRUE(bytes);
    EXPECT_EQ(*bytes, (std::vector<std::uint8_t>{0x48, 0xFF, 0xC0}));
}

TEST(ParseCodeText, CByteArraySpacingVariants)
{
    std::vector<std::uint8_t> expected{0x48, 0xFF, 0xC0};
    auto                      compact = parse_code_text("{0x48,0xFF,0xC0}");
    auto                      padded  = parse_code_text("{ 0x48, 0xFF, 0xC0 }");
    auto                      mixed   = parse_code_text("{\n\t0x48 , 0xFF,\r\n0xC0 ,\n}");
    ASSERT_TRUE(compact);
    ASSERT_TRUE(padded);
    ASSERT_TRUE(mixed);
    EXPECT_EQ(*compact, expected);
    EXPECT_EQ(*padded, expected);
    EXPECT_EQ(*mixed, expected);
}

TEST(ParseCodeText, CByteArrayRejectsSplitPrefix) { EXPECT_FALSE(parse_code_text("{ 0 x48 }")); }

TEST(ParseCodeText, EmptyByteArrayRejected) { EXPECT_FALSE(parse_code_text("{}")); }

TEST(ParseCodeText, ByteArrayOverflowRejected) { EXPECT_FALSE(parse_code_text("{ 0x148 }")); }

TEST(ParseCodeText, PlainHexRejectsCommas) { EXPECT_FALSE(parse_code_text("48,FF")); }

TEST(ParseCodeText, WhitespaceAroundAndBetweenBytes)
{
    auto bytes = parse_code_text(" \t48\n FF\rC0 ");
    ASSERT_TRUE(bytes);
    ASSERT_EQ(bytes->size(), 3u);
    EXPECT_EQ((*bytes)[0], 0x48);
    EXPECT_EQ((*bytes)[1], 0xFF);
    EXPECT_EQ((*bytes)[2], 0xC0);
}

TEST(ParseCodeText, WhitespaceSplitsNibble) { EXPECT_FALSE(parse_code_text("4 F")); }

TEST(ParseCodeText, DanglingNibble) { EXPECT_FALSE(parse_code_text("4")); }

TEST(ParseCodeText, NonHex) { EXPECT_FALSE(parse_code_text("zz")); }

TEST(ParseCodeText, EmptyRejected)
{
    auto bytes = parse_code_text("");
    ASSERT_FALSE(bytes);
    EXPECT_EQ(bytes.error(), "No code to run");
}

TEST(ParseCodeText, WhitespaceOnlyRejected)
{
    auto bytes = parse_code_text(" \t\n");
    ASSERT_FALSE(bytes);
    EXPECT_EQ(bytes.error(), "No code to run");
}

TEST(ParseSeed, HexPrefix)
{
    auto value = parse_seed("0x1F");
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 0x1Full);
}

TEST(ParseSeed, BareHex)
{
    auto value = parse_seed("1F");
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 0x1Full);
}

TEST(ParseSeed, EmptyIsZero)
{
    auto value = parse_seed("");
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 0ull);
}

TEST(ParseSeed, NonHex) { EXPECT_FALSE(parse_seed("zz")); }

TEST(ParseSeed, Overflow) { EXPECT_FALSE(parse_seed("10000000000000000")); }

TEST(ParseSeed, EmptyHexPrefix) { EXPECT_FALSE(parse_seed("0x")); }

TEST(ParseDecimal, Double)
{
    auto parsed = parse_decimal_seed("1.0", "XMM");
    ASSERT_TRUE(parsed);
    EXPECT_DOUBLE_EQ(parsed->value, 1.0);
    EXPECT_FALSE(parsed->is_single);
}

TEST(ParseDecimal, Single)
{
    auto parsed = parse_decimal_seed("1.1F", "XMM");
    ASSERT_TRUE(parsed);
    EXPECT_DOUBLE_EQ(parsed->value, (double)1.1f);
    EXPECT_TRUE(parsed->is_single);
}

TEST(ParseDecimal, LongSuffixSelectsDoublePrecision)
{
    auto parsed = parse_decimal_seed("1.0l", "XMM");
    ASSERT_TRUE(parsed);
    EXPECT_DOUBLE_EQ(parsed->value, 1.0);
    EXPECT_FALSE(parsed->is_single);
}

TEST(ParseDecimal, Negative)
{
    auto parsed = parse_decimal_seed("-1.5", "XMM");
    ASSERT_TRUE(parsed);
    EXPECT_DOUBLE_EQ(parsed->value, -1.5);
    EXPECT_FALSE(parsed->is_single);
}

TEST(ParseDecimal, SignedNonFinitePrecision)
{
    auto infinity = parse_decimal_seed("-inff", "XMM");
    ASSERT_TRUE(infinity);
    EXPECT_EQ(infinity->value, -(double)std::numeric_limits<float>::infinity());
    EXPECT_TRUE(infinity->is_single);

    auto nan = parse_decimal_seed("-nan", "XMM");
    ASSERT_TRUE(nan);
    EXPECT_TRUE(std::isnan(nan->value));
    EXPECT_TRUE(std::signbit(nan->value));
    EXPECT_FALSE(nan->is_single);
}

TEST(ParseDecimal, BareDotIsError) { EXPECT_FALSE(parse_decimal_seed(".", "XMM")); }

TEST(ParseDecimal, DoubleSuffixIsError) { EXPECT_FALSE(parse_decimal_seed("1.0ff", "XMM")); }
