#include "test_helpers.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

TEST(ComposeGpr, FullOnly)
{
    std::vector<std::string> errors{};
    GPRSeed                  seed{.full = "0x1122334455667788"};
    EXPECT_EQ(compose_gpr_seed(seed, "RAX", errors), 0x1122'3344'5566'7788ull);
    EXPECT_TRUE(errors.empty());
}

TEST(ComposeGpr, DwordOverlaysFull)
{
    std::vector<std::string> errors{};
    GPRSeed                  seed{.full = "0x1122334455667788", .dword = "0xAABBCCDD"};
    EXPECT_EQ(compose_gpr_seed(seed, "RAX", errors), 0x1122'3344'AABB'CCDDull);
}

TEST(ComposeGpr, ByteLowOverlay)
{
    std::vector<std::string> errors{};
    GPRSeed                  seed{.full = "0", .byte_low = "0xFF"};
    EXPECT_EQ(compose_gpr_seed(seed, "RAX", errors), 0xFFull);
}

TEST(ComposeGpr, ByteHighOverlay)
{
    std::vector<std::string> errors{};
    GPRSeed                  seed{.full = "0", .byte_high = "0xFF"};
    EXPECT_EQ(compose_gpr_seed(seed, "RAX", errors), 0xFF00ull);
}

TEST(ComposeGpr, MalformedSliceReported)
{
    std::vector<std::string> errors{};
    GPRSeed                  seed{.full = "0x1", .dword = "zz"};
    EXPECT_EQ(compose_gpr_seed(seed, "RAX", errors), 0x1ull);
    EXPECT_EQ(errors.size(), 1u);
}

TEST(ComposeXmm, Double)
{
    auto value = compose_xmm_seed("1.0");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ((*value)[0], 0x3FF0'0000'0000'0000ull);
    EXPECT_EQ((*value)[1], 0ull);
}

TEST(ComposeXmm, Single)
{
    auto value = compose_xmm_seed("1.1f");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ((*value)[0], (std::uint64_t)std::bit_cast<std::uint32_t>(1.1f));
    EXPECT_EQ((*value)[1], 0ull);
}

TEST(ComposeXmm, DoubleInfinity)
{
    auto value = compose_xmm_seed("inf");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ((*value)[0], std::bit_cast<std::uint64_t>(std::numeric_limits<double>::infinity()));
    EXPECT_EQ((*value)[1], 0ull);
}

TEST(ComposeXmm, SingleInfinity)
{
    auto value = compose_xmm_seed("inff");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ((*value)[0], (std::uint64_t)std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity()));
    EXPECT_EQ((*value)[1], 0ull);
}

TEST(ComposeXmm, DoubleNaN)
{
    auto value = compose_xmm_seed("nan");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ((*value)[0], std::bit_cast<std::uint64_t>(std::numeric_limits<double>::quiet_NaN()));
    EXPECT_EQ((*value)[1], 0ull);
}

TEST(ComposeXmm, SingleNaN)
{
    auto value = compose_xmm_seed("nanf");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ((*value)[0], (std::uint64_t)std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN()));
    EXPECT_EQ((*value)[1], 0ull);
}

TEST(ComposeXmm, HexSplit)
{
    auto value = compose_xmm_seed("0x" + std::string(32, 'A'));
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ((*value)[0], 0xAAAA'AAAA'AAAA'AAAAull);
    EXPECT_EQ((*value)[1], 0xAAAA'AAAA'AAAA'AAAAull);
}

TEST(ComposeXmm, TooManyDigitsIsError) { EXPECT_FALSE(compose_xmm_seed(std::string(33, 'A')).has_value()); }

TEST(ComposeXmm, EmptyIsZero)
{
    auto value = compose_xmm_seed("");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ((*value)[0], 0ull);
    EXPECT_EQ((*value)[1], 0ull);
}

TEST(ComposeSt, HexBytePlacement)
{
    // 20 hex digits, big-endian text.
    // Byte 9 holds the most-significant pair, byte 0 the least.
    auto value = compose_st_seed("0x0102030405060708090A");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ((*value)[9], 0x01);
    EXPECT_EQ((*value)[0], 0x0A);
}

TEST(ComposeSt, EmptyIsZero)
{
    auto value = compose_st_seed("");
    ASSERT_TRUE(value.has_value());

    for (auto &&byte : *value)
    {
        EXPECT_EQ(byte, 0);
    }
}

TEST(ComposeSeed, GprXmmAndFlags)
{
    std::array<GPRSeed, GPR_COUNT> gpr{};
    gpr[(std::size_t)Reg::RAX].full = "0x64";

    std::array<std::string, 16> xmm{};
    xmm[0] = "1.0";

    std::array<std::string, 8> st{};
    std::vector<std::string>   errors{};
    auto                       seed = compose_seed(gpr, Flag::CF, xmm, st, errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(seed[Reg::RAX], 0x64ull);
    EXPECT_EQ(seed[Reg::RFLAGS], (std::uint64_t)Flag::CF);
    EXPECT_EQ(seed.xmm[0][0], 0x3FF0'0000'0000'0000ull);
    EXPECT_EQ(seed.fpu_tag_word_abridged, 0);
}

TEST(ComposeSeed, MalformedFieldReported)
{
    std::array<GPRSeed, GPR_COUNT> gpr{};
    std::array<std::string, 16>    xmm{};

    // The `n` routes this through decimal parsing, where it is rejected.
    xmm[0] = "nonsense";

    std::array<std::string, 8> st{};
    std::vector<std::string>   errors{};
    auto                       seed = compose_seed(gpr, 0, xmm, st, errors);
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(seed.xmm[0][0], 0ull);
    EXPECT_EQ(seed.xmm[0][1], 0ull);
}
