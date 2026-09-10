#include "test_helpers.hpp"

#include <cstddef>
#include <string>

TEST(CliParse, SeedFullGpr)
{
    auto cli = parse_cli({"--bytes", "48FFC0", "--seed", "rax=10"});
    ASSERT_TRUE(cli.has_value());
    EXPECT_EQ(cli->seed_gpr[(std::size_t)Reg::RAX].full, "10");
}

TEST(CliParse, SeedSubRegisters)
{
    auto cli = parse_cli({"--seed", "eax=FF,al=1"});
    ASSERT_TRUE(cli.has_value());
    EXPECT_EQ(cli->seed_gpr[(std::size_t)Reg::RAX].dword, "FF");
    EXPECT_EQ(cli->seed_gpr[(std::size_t)Reg::RAX].byte_low, "1");
}

TEST(CliParse, SeedBadHexRejected) { EXPECT_FALSE(parse_cli({"--seed", "rax=ZZ"}).has_value()); }

TEST(CliParse, SeedRspRejected)
{
    auto cli = parse_cli({"--seed", "rsp=1"});
    ASSERT_FALSE(cli.has_value());
    EXPECT_NE(cli.error().find("RSP"), std::string::npos);
}

TEST(CliParse, SeedUnknownRejected) { EXPECT_FALSE(parse_cli({"--seed", "bogus=1"}).has_value()); }

TEST(CliParse, SeedXmm)
{
    auto cli = parse_cli({"--seed", "xmm0=1.0"});
    ASSERT_TRUE(cli.has_value());
    EXPECT_EQ(cli->seed_xmm[0], "1.0");
}

TEST(CliParse, SeedSt)
{
    auto cli = parse_cli({"--seed", "st0=1.5"});
    ASSERT_TRUE(cli.has_value());
    EXPECT_EQ(cli->seed_st[0], "1.5");
}

TEST(CliParse, SeedFlag)
{
    auto cli = parse_cli({"--seed", "cf=1"});
    ASSERT_TRUE(cli.has_value());
    EXPECT_NE(cli->seed_flags & Flag::CF, 0ull);
}

TEST(CliParse, TrackSubset)
{
    auto cli = parse_cli({"--track", "xmm,x87"});
    ASSERT_TRUE(cli.has_value());
    EXPECT_TRUE(cli->track.xmm);
    EXPECT_TRUE(cli->track.x87);
    EXPECT_FALSE(cli->track.gpr);
}

TEST(CliParse, TrackAll)
{
    auto cli = parse_cli({"--track", "all"});
    ASSERT_TRUE(cli.has_value());
    EXPECT_TRUE(cli->track.gpr);
    EXPECT_TRUE(cli->track.rip);
    EXPECT_TRUE(cli->track.rflags);
    EXPECT_TRUE(cli->track.xmm);
    EXPECT_TRUE(cli->track.x87);
}

TEST(CliParse, TrackNone)
{
    auto cli = parse_cli({"--track", "none"});
    ASSERT_TRUE(cli.has_value());
    EXPECT_FALSE(cli->track.gpr);
    EXPECT_FALSE(cli->track.rip);
    EXPECT_FALSE(cli->track.rflags);
    EXPECT_FALSE(cli->track.xmm);
    EXPECT_FALSE(cli->track.x87);
}

TEST(CliParse, TrackBogusRejected) { EXPECT_FALSE(parse_cli({"--track", "bogus"}).has_value()); }

TEST(CliParse, SyntaxAtt)
{
    auto cli = parse_cli({"--syntax", "att"});
    ASSERT_TRUE(cli.has_value());
    EXPECT_EQ(cli->syntax, DisasmSyntax::ATT);
}

TEST(CliParse, BddisasmAttRejected) { EXPECT_FALSE(parse_cli({"--backend", "bddisasm", "--syntax", "att"}).has_value()); }

TEST(CliParse, BackendXed)
{
    auto cli = parse_cli({"--backend", "xed"});
    ASSERT_TRUE(cli.has_value());
    EXPECT_EQ(cli->backend, DisasmBackend::Xed);
}

TEST(CliParse, MaxStepsZeroRejected) { EXPECT_FALSE(parse_cli({"--max-steps", "0"}).has_value()); }

TEST(CliParse, MaxStepsNonNumericRejected) { EXPECT_FALSE(parse_cli({"--max-steps", "abc"}).has_value()); }

TEST(CliParse, MaxStepsValue)
{
    auto cli = parse_cli({"--max-steps", "200000"});
    ASSERT_TRUE(cli.has_value());
    EXPECT_EQ(cli->max_steps, 200'000u);
}

TEST(CliParse, MaxStepsClamped)
{
    auto cli = parse_cli({"--max-steps", "5000000"});
    ASSERT_TRUE(cli.has_value());
    EXPECT_EQ(cli->max_steps, MAX_STEPS_LIMIT);
}
