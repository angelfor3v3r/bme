#include "test_helpers.hpp"

#include <algorithm>
#include <array>
#include <barrier>
#include <cstdint>
#include <thread>

namespace bme { void redisasm(Trace &trace, DisasmBackend backend, DisasmSyntax syntax) noexcept; } // namespace bme

TEST(RunEngineFault, PreservesExceptionStateWithoutCompletingInstruction)
{
    std::array<std::uint8_t, 2> code{0x0F, 0x0B};
    Registers                   seed{};
    auto                        trace = run_engine(code, seed, DEFAULT_MAX_STEPS, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
    EXPECT_EQ(trace.outcome, Outcome::Faulted);
    EXPECT_EQ(std::ranges::count_if(trace.steps, [](const Step &step) { return step.reached; }), 0);

    auto fault = std::ranges::find_if(trace.steps, [](const Step &step) { return step.faulted; });
    ASSERT_NE(fault, trace.steps.end());
    EXPECT_FALSE(fault->reached);
    EXPECT_EQ(fault->rip, trace.stop_address);
    EXPECT_EQ(fault->registers.rip, trace.stop_address);
}

TEST(RunEngineBoundary, IncompleteInstructionFaultsWithoutPadding)
{
    std::array<std::uint8_t, 1> code{0x48};
    Registers                   seed{};
    auto                        trace = run_engine(code, seed, DEFAULT_MAX_STEPS, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
    EXPECT_EQ(trace.outcome, Outcome::Faulted);
    EXPECT_EQ(std::ranges::count_if(trace.steps, [](const Step &step) { return step.reached; }), 0);

    auto fault = std::ranges::find_if(trace.steps, [](const Step &step) { return step.faulted; });
    ASSERT_NE(fault, trace.steps.end());
    ASSERT_EQ(fault->bytes.size(), 1u);
    EXPECT_EQ(fault->bytes[0], code[0]);
}

TEST(RedisasmBoundary, RetFetchFaultDoesNotCreateOutOfRangeHistory)
{
    Trace trace{};
    trace.code           = {0xC3};
    trace.seed[Reg::RIP] = 0x1000;
    trace.stop_address   = 0;

    auto &fault   = trace.steps.emplace_back();
    fault.rip     = trace.stop_address;
    fault.faulted = true;

    redisasm(trace, DisasmBackend::Zydis, DisasmSyntax::Intel);

    ASSERT_EQ(trace.steps.size(), 1u);
    EXPECT_EQ(trace.steps[0].rip, trace.seed[Reg::RIP]);
    EXPECT_FALSE(trace.steps[0].faulted);
}

TEST(RedisasmBoundary, GuardJumpFaultDoesNotCreateOutOfRangeHistory)
{
    Trace trace{};
    trace.code           = {0xE9, 0x00, 0x00, 0x00, 0x00};
    trace.seed[Reg::RIP] = 0x1000;
    trace.stop_address   = trace.seed[Reg::RIP] + trace.code.size();

    auto &fault   = trace.steps.emplace_back();
    fault.rip     = trace.stop_address;
    fault.faulted = true;

    redisasm(trace, DisasmBackend::Zydis, DisasmSyntax::Intel);

    ASSERT_EQ(trace.steps.size(), 1u);
    EXPECT_EQ(trace.steps[0].rip, trace.seed[Reg::RIP]);
    EXPECT_FALSE(trace.steps[0].faulted);
}

TEST(RunEngineLimits, ZeroStillAllowsOneStep)
{
    std::array<std::uint8_t, 2> code{0xEB, 0xFE};
    Registers                   seed{};
    auto                        trace = run_engine(code, seed, 0, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
    EXPECT_EQ(trace.outcome, Outcome::AbortedCap);
    EXPECT_EQ(std::ranges::count_if(trace.steps, [](const Step &step) { return step.reached; }), 1);
    EXPECT_EQ(std::ranges::count_if(trace.steps, [](const Step &step) { return step.faulted; }), 0);
}

TEST(RunEngineConcurrency, ConcurrentCallsProduceIndependentTraces)
{
    constexpr std::uint64_t     iterations = 2000;
    std::array<std::uint8_t, 8> code{0x48, 0xFF, 0xC0, 0x48, 0xFF, 0xC9, 0x75, 0xF8};

    Registers first_seed{};
    first_seed[Reg::RAX] = 0x1000;
    first_seed[Reg::RCX] = iterations;

    Registers second_seed{};
    second_seed[Reg::RAX] = 0x2000;
    second_seed[Reg::RCX] = iterations;

    Trace        first{};
    Trace        second{};
    std::barrier start{3};

    std::jthread first_thread{[&]
                              {
                                  start.arrive_and_wait();
                                  first = run_engine(code, first_seed, iterations * 3, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
                              }};
    std::jthread second_thread{[&]
                               {
                                   start.arrive_and_wait();
                                   second = run_engine(code, second_seed, iterations * 3, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
                               }};

    start.arrive_and_wait();
    first_thread.join();
    second_thread.join();

    ASSERT_EQ(first.outcome, Outcome::Finished);
    ASSERT_EQ(second.outcome, Outcome::Finished);
    ASSERT_FALSE(first.steps.empty());
    ASSERT_FALSE(second.steps.empty());
    EXPECT_EQ(first.steps.back().registers[Reg::RAX], first_seed[Reg::RAX] + iterations);
    EXPECT_EQ(second.steps.back().registers[Reg::RAX], second_seed[Reg::RAX] + iterations);
    EXPECT_EQ(first.steps.back().registers[Reg::RCX], 0ull);
    EXPECT_EQ(second.steps.back().registers[Reg::RCX], 0ull);
}
