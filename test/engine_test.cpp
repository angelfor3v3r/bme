#include "os.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <array>
#include <barrier>
#include <cmath>
#include <cstdint>
#include <thread>

TEST(RunEngineInput, EmptyCodeRemainsIdle)
{
    std::array<std::uint8_t, 0> code{};
    Registers                   seed{};
    auto                        trace = run_engine(code, seed, DEFAULT_MAX_STEPS, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
    EXPECT_EQ(trace.outcome, Outcome::Idle);
    EXPECT_TRUE(trace.execution_events.empty());
}

TEST(RunEngineFault, PreservesExceptionStateWithoutCompletingInstruction)
{
    std::array<std::uint8_t, 2> code{0x0F, 0x0B};
    Registers                   seed{};
    auto                        trace = run_engine(code, seed, DEFAULT_MAX_STEPS, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
    EXPECT_EQ(trace.outcome, Outcome::Faulted);
    EXPECT_EQ(std::ranges::count(trace.execution_events, ExecutionEventKind::Completed, &ExecutionEvent::kind), 0);

    auto fault = std::ranges::find(trace.execution_events, ExecutionEventKind::Faulted, &ExecutionEvent::kind);
    ASSERT_NE(fault, trace.execution_events.end());
    EXPECT_EQ(fault->rip, trace.stop_address);
    EXPECT_EQ(fault->registers.rip, trace.stop_address);
}

TEST(RunEngineBreakpoint, StopsOnPrefixedInt3WithoutCompletingInstruction)
{
    std::array<std::uint8_t, 2> code{0xF3, 0xCC};
    Registers                   seed{};
    auto                        trace = run_engine(code, seed, DEFAULT_MAX_STEPS, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
    EXPECT_EQ(trace.outcome, Outcome::Stopped);
    EXPECT_EQ(std::ranges::count(trace.execution_events, ExecutionEventKind::Completed, &ExecutionEvent::kind), 0);
    EXPECT_EQ(trace.stop_address, trace.seed[Reg::RIP]);
    EXPECT_EQ(trace.stop_reason, "Stopped here - int3 breakpoint");
}

TEST(RunEngineBreakpoint, StopsOnInterruptThreeWithoutCompletingInstruction)
{
    std::array<std::uint8_t, 2> code{0xCD, 0x03};
    Registers                   seed{};
    auto                        trace = run_engine(code, seed, DEFAULT_MAX_STEPS, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
    EXPECT_EQ(trace.outcome, Outcome::Stopped);
    EXPECT_EQ(std::ranges::count(trace.execution_events, ExecutionEventKind::Completed, &ExecutionEvent::kind), 0);
    EXPECT_EQ(trace.stop_address, trace.seed[Reg::RIP]);
    EXPECT_EQ(trace.stop_reason, "Stopped here - int3 breakpoint");
}

TEST(RunEngineBreakpoint, LockPrefixedInt3Faults)
{
    std::array<std::uint8_t, 2> code{0xF0, 0xCC};
    Registers                   seed{};
    auto                        trace = run_engine(code, seed, DEFAULT_MAX_STEPS, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
    EXPECT_EQ(trace.outcome, Outcome::Faulted);
    EXPECT_EQ(std::ranges::count(trace.execution_events, ExecutionEventKind::Completed, &ExecutionEvent::kind), 0);
    EXPECT_EQ(trace.stop_address, trace.seed[Reg::RIP]);
}

TEST(RunEngineBoundary, IncompleteInstructionFaultsWithoutPadding)
{
    std::array<std::uint8_t, 1> code{0x48};
    Registers                   seed{};
    auto                        trace = run_engine(code, seed, DEFAULT_MAX_STEPS, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
    EXPECT_EQ(trace.outcome, Outcome::Faulted);
    EXPECT_EQ(std::ranges::count(trace.execution_events, ExecutionEventKind::Completed, &ExecutionEvent::kind), 0);

    auto history = build_history(trace, DisasmBackend::Zydis, DisasmSyntax::Intel);
    ASSERT_EQ(history.size(), 1u);
    EXPECT_EQ(history[0].kind, HistoryRowKind::Faulted);
    EXPECT_EQ(history[0].length, 1u);
    EXPECT_EQ(trace.code[history[0].offset], code[0]);
}

TEST(BuildHistoryBoundary, RetFetchFaultRemainsRawAndDoesNotCreateOutOfRangeHistory)
{
    Trace trace{};
    trace.code           = {0xC3};
    trace.seed[Reg::RIP] = 0x1000;
    trace.stop_address   = 0;

    trace.execution_events.emplace_back(ExecutionEvent{.rip = trace.stop_address, .kind = ExecutionEventKind::Faulted});

    auto history = build_history(trace, DisasmBackend::Zydis, DisasmSyntax::Intel);
    ASSERT_EQ(trace.execution_events.size(), 1u);
    EXPECT_EQ(trace.execution_events[0].kind, ExecutionEventKind::Faulted);
    EXPECT_EQ(trace.execution_events[0].rip, trace.stop_address);
    ASSERT_EQ(history.size(), 1u);
    EXPECT_EQ(history[0].rip, trace.seed[Reg::RIP]);
    EXPECT_EQ(history[0].kind, HistoryRowKind::NotReached);
    EXPECT_FALSE(history[0].execution_event_index);
}

TEST(BuildHistoryBoundary, EndGuardFaultRemainsRawAndDoesNotCreateOutOfRangeHistory)
{
    Trace trace{};
    trace.code           = {0x90};
    trace.seed[Reg::RIP] = 0x1000;
    trace.stop_address   = trace.seed[Reg::RIP] + trace.code.size();

    trace.execution_events.emplace_back(ExecutionEvent{.rip = trace.stop_address, .kind = ExecutionEventKind::Faulted});

    auto history = build_history(trace, DisasmBackend::Zydis, DisasmSyntax::Intel);
    ASSERT_EQ(trace.execution_events.size(), 1u);
    EXPECT_EQ(trace.execution_events[0].kind, ExecutionEventKind::Faulted);
    EXPECT_EQ(trace.execution_events[0].rip, trace.stop_address);
    ASSERT_EQ(history.size(), 1u);
    EXPECT_EQ(history[0].rip, trace.seed[Reg::RIP]);
    EXPECT_EQ(history[0].kind, HistoryRowKind::NotReached);
    EXPECT_FALSE(history[0].execution_event_index);
}

TEST(BuildHistoryModel, PreservesRepeatedEventIdentityAcrossBackends)
{
    Trace trace{};
    trace.code           = {0x90};
    trace.seed[Reg::RIP] = 0x1000;
    trace.seed[Reg::RCX] = 3;
    trace.outcome        = Outcome::Faulted;
    trace.stop_address   = trace.seed[Reg::RIP] + trace.code.size();

    auto &first               = trace.execution_events.emplace_back();
    first.rip                 = trace.seed[Reg::RIP];
    first.registers[Reg::RAX] = 1;

    auto &second               = trace.execution_events.emplace_back();
    second.rip                 = trace.seed[Reg::RIP];
    second.registers[Reg::RAX] = 2;

    trace.execution_events.emplace_back(ExecutionEvent{.rip = trace.stop_address, .kind = ExecutionEventKind::Faulted});

    auto zydis_history    = build_history(trace, DisasmBackend::Zydis, DisasmSyntax::Intel);
    auto capstone_history = build_history(trace, DisasmBackend::Capstone, DisasmSyntax::Intel);
    ASSERT_EQ(trace.execution_events.size(), 3u);
    EXPECT_EQ(trace.execution_events[0].registers[Reg::RAX], 1ull);
    EXPECT_EQ(trace.execution_events[1].registers[Reg::RAX], 2ull);
    EXPECT_EQ(trace.execution_events[2].kind, ExecutionEventKind::Faulted);
    EXPECT_EQ(trace.seed[Reg::RCX], 3ull);
    EXPECT_EQ(trace.outcome, Outcome::Faulted);
    EXPECT_EQ(trace.stop_address, trace.seed[Reg::RIP] + trace.code.size());
    EXPECT_EQ(trace.execution_events[2].rip, trace.stop_address);

    for (auto *history : {&zydis_history, &capstone_history})
    {
        ASSERT_EQ(history->size(), 2u);
        EXPECT_EQ((*history)[0].kind, HistoryRowKind::Reached);
        EXPECT_EQ((*history)[0].execution_event_index, 0u);
        EXPECT_EQ((*history)[1].kind, HistoryRowKind::Reached);
        EXPECT_EQ((*history)[1].execution_event_index, 1u);
    }
}

TEST(BuildHistoryModel, DoesNotDecodeStaticRowsAcrossFutureEventStarts)
{
    Trace trace{};
    trace.code           = {0xEB, 0x08, 0x48, 0xFF, 0xC0, 0x90, 0x90, 0x90, 0x90, 0x90, 0xEB, 0xF7};
    trace.seed[Reg::RIP] = 0x1000;

    trace.execution_events.emplace_back(ExecutionEvent{.rip = 0x1000});
    trace.execution_events.emplace_back(ExecutionEvent{.rip = 0x100A});
    trace.execution_events.emplace_back(ExecutionEvent{.rip = 0x1003});

    auto history     = build_history(trace, DisasmBackend::Zydis, DisasmSyntax::Intel);
    auto event_start = trace.execution_events.back().rip;
    auto overlap     = std::ranges::find_if(
        history,
        [event_start](const HistoryRow &row)
        {
            auto static_row = row.kind == HistoryRowKind::NotReached || row.kind == HistoryRowKind::Data;

            return static_row && row.rip < event_start && event_start < row.rip + row.length;
        }
    );
    EXPECT_EQ(overlap, history.end());
}

TEST(RunEngineRequest, RetainsExportProvenanceForIdleTrace)
{
    std::array<std::uint8_t, 0> code{};
    Registers                   requested_seed{};
    requested_seed[Reg::RAX] = 0x1234;

    auto trace = run_engine(code, requested_seed, MAX_STEPS_LIMIT + 1, DisasmBackend::Capstone, DisasmSyntax::ATT, false);
    EXPECT_EQ(trace.outcome, Outcome::Idle);
    EXPECT_TRUE(trace.execution_events.empty());
    EXPECT_EQ(trace.requested_seed[Reg::RAX], requested_seed[Reg::RAX]);
    EXPECT_EQ(trace.requested_backend, DisasmBackend::Capstone);
    EXPECT_EQ(trace.requested_syntax, DisasmSyntax::ATT);
    EXPECT_EQ(trace.effective_max_steps, MAX_STEPS_LIMIT);
    EXPECT_FALSE(trace.seed_data_pointers);
}

TEST(RunEngineLimits, ZeroStillAllowsOneStep)
{
    std::array<std::uint8_t, 2> code{0xEB, 0xFE};
    Registers                   seed{};
    auto                        trace = run_engine(code, seed, 0, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
    EXPECT_EQ(trace.outcome, Outcome::AbortedCap);
    EXPECT_EQ(std::ranges::count(trace.execution_events, ExecutionEventKind::Completed, &ExecutionEvent::kind), 1);
    EXPECT_EQ(std::ranges::count(trace.execution_events, ExecutionEventKind::Faulted, &ExecutionEvent::kind), 0);
}

TEST(RunEngineStopOffset, StopsBeforeSelectedInstruction)
{
    std::array<std::uint8_t, 6> code{0x48, 0xFF, 0xC0, 0x48, 0xFF, 0xC0};
    Registers                   seed{};
    auto                        trace = run_engine(code, seed, DEFAULT_MAX_STEPS, DisasmBackend::Zydis, DisasmSyntax::Intel, true, 3);
    EXPECT_EQ(trace.outcome, Outcome::Stopped);
    ASSERT_EQ(trace.execution_events.size(), 1u);
    EXPECT_EQ(trace.execution_events[0].kind, ExecutionEventKind::Completed);
    EXPECT_EQ(trace.stop_address, trace.seed[Reg::RIP] + 3);
    EXPECT_EQ(trace.stop_reason, "Stopped here - selected row");

    auto history  = build_history(trace, DisasmBackend::Zydis, DisasmSyntax::Intel);
    auto selected = std::ranges::find(history, 3u, &HistoryRow::offset);
    ASSERT_NE(selected, history.end());
    EXPECT_EQ(selected->kind, HistoryRowKind::NotReached);
}

TEST(RunEngineStopOffset, StopsAtEntryWithoutExecuting)
{
    std::array<std::uint8_t, 1> code{0x90};
    Registers                   seed{};
    auto                        trace = run_engine(code, seed, DEFAULT_MAX_STEPS, DisasmBackend::Zydis, DisasmSyntax::Intel, true, 0);
    EXPECT_EQ(trace.outcome, Outcome::Stopped);
    EXPECT_TRUE(trace.execution_events.empty());
    EXPECT_EQ(trace.stop_address, trace.seed[Reg::RIP]);
    EXPECT_EQ(trace.stop_reason, "Stopped here - selected row");
}

TEST(RunEngineFpu, PreservesSeededSseAndX87State)
{
    std::array<std::uint8_t, 1> code{0x90};
    Registers                   seed{};
    seed.xmm[0]                = {0x0123'4567'89AB'CDEF, 0xFEDC'BA98'7654'3210};
    seed.st[0]                 = {0, 0, 0, 0, 0, 0, 0, 0x80, 0xFF, 0x3F};
    seed.fpu_tag_word_abridged = 1;
    seed.mxcsr                 = 0x5F80;
    seed.fpu_control_word      = 0x027F;
    auto trace                 = run_engine(code, seed, DEFAULT_MAX_STEPS, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
    ASSERT_EQ(trace.outcome, Outcome::Finished);
    ASSERT_EQ(trace.execution_events.size(), 1u);
    EXPECT_EQ(trace.execution_events[0].registers.xmm[0], seed.xmm[0]);
    EXPECT_EQ(trace.execution_events[0].registers.st[0], seed.st[0]);
    EXPECT_NE(trace.execution_events[0].registers.fpu_tag_word_abridged & 1, 0);
    EXPECT_EQ(trace.execution_events[0].registers.fpu_control_word, seed.fpu_control_word);
    EXPECT_EQ(trace.execution_events[0].registers.mxcsr, seed.mxcsr);
}

TEST(RunEngineFpu, DecimalStSeedSurvivesFirstStep)
{
    std::array<std::uint8_t, 1>    code{0x90};
    std::array<GPRSeed, GPR_COUNT> gpr{};
    std::array<std::string, 16>    xmm{};
    std::array<std::string, 8>     st{"1.5"};
    std::vector<std::string>       errors{};
    auto                           seed = compose_seed(gpr, 0, xmm, st, FloatingEnvironmentSeed{}, errors);
    std::array<std::uint8_t, 10>   expected{0, 0, 0, 0, 0, 0, 0, 0xC0, 0xFF, 0x3F};
    ASSERT_TRUE(errors.empty());
    EXPECT_EQ(seed.st[0], expected);

    auto trace = run_engine(code, seed, DEFAULT_MAX_STEPS, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
    ASSERT_EQ(trace.outcome, Outcome::Finished);
    ASSERT_EQ(trace.execution_events.size(), 1u);
    EXPECT_EQ(trace.execution_events[0].registers.st[0], expected);
    EXPECT_NE(trace.execution_events[0].registers.fpu_tag_word_abridged & 1, 0);
}

TEST(RunEngineFpu, CapturesLogicalX87StackAfterTopChanges)
{
    std::array<std::uint8_t, 2> code{0xD9, 0xE8};
    Registers                   seed{};
    auto                        expected = compose_st_seed("1.0");
    ASSERT_TRUE(expected);

    auto trace = run_engine(code, seed, DEFAULT_MAX_STEPS, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
    ASSERT_EQ(trace.outcome, Outcome::Finished);
    ASSERT_EQ(trace.execution_events.size(), 1u);

    auto &registers = trace.execution_events[0].registers;
    auto  top       = (registers.fpu_status_word >> 11) & 7;
    auto  physical  = top;
    EXPECT_EQ(registers.st[0], *expected);
    EXPECT_EQ(top, 7);
    EXPECT_NE(registers.fpu_tag_word_abridged >> physical & 1, 0);
}

TEST(FpuConversion, UsesCapturedRoundingMode)
{
    std::array<std::uint8_t, 10> value{};
    double_to_st80(1.0 + std::ldexp(1.0, -24), value);

    auto nearest = st80_to_float(value, 0x003F);
    auto upward  = st80_to_float(value, 0x083F);
    EXPECT_EQ(nearest, 1.0f);
    EXPECT_EQ(upward, std::nextafter(1.0f, 2.0f));
}

TEST(RunEngineSeed, ScratchPointersCanBeDisabled)
{
    std::array<std::uint8_t, 1> code{0x90};
    Registers                   seed{};
    auto                        defaults = run_engine(code, seed, DEFAULT_MAX_STEPS, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
    auto                        disabled = run_engine(code, seed, DEFAULT_MAX_STEPS, DisasmBackend::Zydis, DisasmSyntax::Intel, false);
    ASSERT_EQ(defaults.outcome, Outcome::Finished);
    ASSERT_EQ(disabled.outcome, Outcome::Finished);
    EXPECT_NE(defaults.seed[Reg::RDI], 0ull);
    EXPECT_EQ(defaults.seed[Reg::RDI], defaults.seed[Reg::RSI]);
    EXPECT_EQ(disabled.seed[Reg::RDI], 0ull);
    EXPECT_EQ(disabled.seed[Reg::RSI], 0ull);
}

TEST(RunEngineConcurrency, ConcurrentCallsProduceIndependentTraces)
{
    constexpr auto ITERATIONS = 2000ull;

    std::array<std::uint8_t, 8> code{0x48, 0xFF, 0xC0, 0x48, 0xFF, 0xC9, 0x75, 0xF8};

    Registers first_seed{};
    first_seed[Reg::RAX] = 0x1000;
    first_seed[Reg::RCX] = ITERATIONS;

    Registers second_seed{};
    second_seed[Reg::RAX] = 0x2000;
    second_seed[Reg::RCX] = ITERATIONS;

    Trace        first{};
    Trace        second{};
    std::barrier start{3};

    std::jthread first_thread{[&]
                              {
                                  start.arrive_and_wait();
                                  first = run_engine(code, first_seed, ITERATIONS * 3, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
                              }};
    std::jthread second_thread{[&]
                               {
                                   start.arrive_and_wait();
                                   second = run_engine(code, second_seed, ITERATIONS * 3, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
                               }};

    start.arrive_and_wait();
    first_thread.join();
    second_thread.join();

    ASSERT_EQ(first.outcome, Outcome::Finished);
    ASSERT_EQ(second.outcome, Outcome::Finished);
    ASSERT_FALSE(first.execution_events.empty());
    ASSERT_FALSE(second.execution_events.empty());
    EXPECT_EQ(first.execution_events.back().registers[Reg::RAX], first_seed[Reg::RAX] + ITERATIONS);
    EXPECT_EQ(second.execution_events.back().registers[Reg::RAX], second_seed[Reg::RAX] + ITERATIONS);
    EXPECT_EQ(first.execution_events.back().registers[Reg::RCX], 0ull);
    EXPECT_EQ(second.execution_events.back().registers[Reg::RCX], 0ull);
}
