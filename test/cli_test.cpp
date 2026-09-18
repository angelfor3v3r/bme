#include "test_helpers.hpp"

#include <cstddef>
#include <string>

TEST(CliParse, RunWithoutBytesIsAllowed)
{
    auto cli = parse_cli({"--run"});
    ASSERT_TRUE(cli);
    EXPECT_TRUE(cli->run);
    EXPECT_FALSE(cli->bytes);
}

TEST(CliParse, File)
{
    auto cli = parse_cli({"--file", "path with spaces.bin"});
    ASSERT_TRUE(cli);
    EXPECT_EQ(cli->input_file.value_or(""), "path with spaces.bin");
}

TEST(CliParse, BytesAndFileAreMutuallyExclusive) { EXPECT_FALSE(parse_cli({"--bytes", "90", "--file", "code.bin"})); }

TEST(CliParse, SeedFullGpr)
{
    auto cli = parse_cli({"--bytes", "48FFC0", "--seed", "rax=10"});
    ASSERT_TRUE(cli);
    EXPECT_EQ(cli->seed_gpr[(std::size_t)Reg::RAX].full, "10");
}

TEST(CliParse, SeedSubRegisters)
{
    auto cli = parse_cli({"--seed", "eax=FF,al=1"});
    ASSERT_TRUE(cli);
    EXPECT_EQ(cli->seed_gpr[(std::size_t)Reg::RAX].dword, "FF");
    EXPECT_EQ(cli->seed_gpr[(std::size_t)Reg::RAX].byte_low, "1");
}

TEST(CliParse, SeedBadHexRejected) { EXPECT_FALSE(parse_cli({"--seed", "rax=ZZ"})); }

TEST(CliParse, SeedEmptyValuesRejected)
{
    EXPECT_FALSE(parse_cli({"--seed", "rax="}));
    EXPECT_FALSE(parse_cli({"--seed", "cf="}));
    EXPECT_FALSE(parse_cli({"--seed", "xmm0="}));
    EXPECT_FALSE(parse_cli({"--seed", "st0="}));
    EXPECT_FALSE(parse_cli({"--seed", "mxcsr="}));
    EXPECT_FALSE(parse_cli({"--seed", "control_word="}));
}

TEST(CliParse, SeedRspRejected)
{
    auto cli = parse_cli({"--seed", "rsp=1"});
    ASSERT_FALSE(cli);
    EXPECT_NE(cli.error().find("RSP"), std::string::npos);
}

TEST(CliParse, SeedUnknownRejected) { EXPECT_FALSE(parse_cli({"--seed", "bogus=1"})); }

TEST(CliParse, SeedXmm)
{
    auto cli = parse_cli({"--seed", "xmm0=1.0"});
    ASSERT_TRUE(cli);
    EXPECT_EQ(cli->seed_xmm[0], "1.0");
}

TEST(CliParse, SeedSt)
{
    auto cli = parse_cli({"--seed", "st0=1.5"});
    ASSERT_TRUE(cli);
    EXPECT_EQ(cli->seed_st[0], "1.5");
}

TEST(CliParse, SeedFloatingEnvironment)
{
    auto cli = parse_cli({"--seed", "mxcsr=5F80,control_word=27F"});
    ASSERT_TRUE(cli);
    EXPECT_EQ(cli->seed_floating_environment.mxcsr, "5F80");
    EXPECT_EQ(cli->seed_floating_environment.fpu_control_word, "27F");
}

TEST(CliParse, SeedMxcsrUndefinedBitsRejected) { EXPECT_FALSE(parse_cli({"--seed", "mxcsr=10000"})); }

TEST(CliParse, SeedControlWordOverflowRejected) { EXPECT_FALSE(parse_cli({"--seed", "control_word=10000"})); }

TEST(CliParse, SeedFlag)
{
    auto cli = parse_cli({"--seed", "cf=1"});
    ASSERT_TRUE(cli);
    EXPECT_NE(cli->seed_flags & Flag::CF, 0ull);
}

TEST(CliParse, TrackSubset)
{
    auto cli = parse_cli({"--track", "xmm,x87"});
    ASSERT_TRUE(cli);
    EXPECT_TRUE(cli->track.xmm);
    EXPECT_TRUE(cli->track.x87);
    EXPECT_FALSE(cli->track.gpr);
    EXPECT_FALSE(cli->track.rip);
    EXPECT_FALSE(cli->track.rflags);
}

TEST(CliParse, TrackAll)
{
    auto cli = parse_cli({"--track", "all"});
    ASSERT_TRUE(cli);
    EXPECT_TRUE(cli->track.gpr);
    EXPECT_TRUE(cli->track.rip);
    EXPECT_TRUE(cli->track.rflags);
    EXPECT_TRUE(cli->track.xmm);
    EXPECT_TRUE(cli->track.x87);
}

TEST(CliParse, TrackNone)
{
    auto cli = parse_cli({"--track", "none"});
    ASSERT_TRUE(cli);
    EXPECT_FALSE(cli->track.gpr);
    EXPECT_FALSE(cli->track.rip);
    EXPECT_FALSE(cli->track.rflags);
    EXPECT_FALSE(cli->track.xmm);
    EXPECT_FALSE(cli->track.x87);
}

TEST(CliParse, TrackBogusRejected) { EXPECT_FALSE(parse_cli({"--track", "bogus"})); }

TEST(CliParse, SyntaxAtt)
{
    auto cli = parse_cli({"--syntax", "att"});
    ASSERT_TRUE(cli);
    EXPECT_EQ(cli->syntax, DisasmSyntax::ATT);
}

TEST(CliParse, SyntaxBogusRejected)
{
    auto cli = parse_cli({"--syntax", "bogus"});
    ASSERT_FALSE(cli);
    EXPECT_NE(cli.error().find("allowed options"), std::string::npos);
}

TEST(CliParse, BddisasmAttRejected) { EXPECT_FALSE(parse_cli({"--backend", "bddisasm", "--syntax", "att"})); }

TEST(CliParse, BackendXed)
{
    auto cli = parse_cli({"--backend", "xed"});
    ASSERT_TRUE(cli);
    EXPECT_EQ(cli->backend, DisasmBackend::Xed);
}

TEST(CliParse, BackendBogusRejected)
{
    auto cli = parse_cli({"--backend", "xml"});
    ASSERT_FALSE(cli);
    EXPECT_NE(cli.error().find("allowed options"), std::string::npos);
}

TEST(CliParse, FormatDefaultsToText)
{
    auto cli = parse_cli({});
    ASSERT_TRUE(cli);
    EXPECT_EQ(cli->format, OutputFormat::Text);
}

TEST(CliParse, FormatJson)
{
    auto cli = parse_cli({"--quick", "--format", "json"});
    ASSERT_TRUE(cli);
    EXPECT_EQ(cli->format, OutputFormat::Json);
}

TEST(CliParse, PrettyJson)
{
    auto cli = parse_cli({"--quick", "--format", "json", "--pretty"});
    ASSERT_TRUE(cli);
    EXPECT_TRUE(cli->pretty_json);
}

TEST(CliParse, FormatWithoutQuickRejected) { EXPECT_FALSE(parse_cli({"--format", "text"})); }

TEST(CliParse, FormatBogusRejected)
{
    auto cli = parse_cli({"--quick", "--format", "xml"});
    ASSERT_FALSE(cli);
    EXPECT_NE(cli.error().find("allowed options"), std::string::npos);
}

TEST(CliParse, TrackWithJsonRejected) { EXPECT_FALSE(parse_cli({"--quick", "--format", "json", "--track", "all"})); }

TEST(CliParse, PrettyWithoutJsonRejected) { EXPECT_FALSE(parse_cli({"--quick", "--pretty"})); }

TEST(CliParse, MaxStepsZeroRejected) { EXPECT_FALSE(parse_cli({"--max-steps", "0"})); }

TEST(CliParse, MaxStepsNonNumericRejected) { EXPECT_FALSE(parse_cli({"--max-steps", "abc"})); }

TEST(CliParse, MaxStepsValue)
{
    auto cli = parse_cli({"--max-steps", "200000"});
    ASSERT_TRUE(cli);
    EXPECT_EQ(cli->max_steps, 200'000u);
}

TEST(CliParse, MaxStepsClamped)
{
    auto cli = parse_cli({"--max-steps", "5000000"});
    ASSERT_TRUE(cli);
    EXPECT_EQ(cli->max_steps, MAX_STEPS_LIMIT);
}
