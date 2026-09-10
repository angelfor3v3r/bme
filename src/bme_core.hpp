#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bme
{

template <class T, class E>
using Result = std::expected<T, E>;

template <class E>
using Error = std::unexpected<E>;

constexpr std::size_t DEFAULT_MAX_STEPS = 50'000;    // Default single-step cap (override via `--max-steps` / Settings).
constexpr std::size_t MAX_STEPS_LIMIT   = 1'000'000; // Hard ceiling. The trap path pre-reserves this many steps, so it bounds memory.

enum class Reg : std::uint8_t
{
    RAX = 0,
    RBX,
    RCX,
    RDX,
    RSI,
    RDI,
    RBP,
    RSP,
    R8,
    R9,
    R10,
    R11,
    R12,
    R13,
    R14,
    R15,
    RIP,
    RFLAGS,
};

constexpr auto        REG_COUNT = (std::size_t)Reg::RFLAGS + 1;
constexpr std::size_t GPR_COUNT = 16; // RAX..R15. RSP is engine-controlled and not user-seedable, see below.

// The x86 RFLAGS bits we expose, the six arithmetic status flags plus DF (the direction flag).
enum Flag : std::uint64_t
{
    CF = 1ull << 0,
    PF = 1ull << 2,
    AF = 1ull << 4,
    ZF = 1ull << 6,
    SF = 1ull << 7,
    DF = 1ull << 10,
    OF = 1ull << 11,
};

enum class Outcome : std::uint8_t
{
    Idle = 0,
    Finished,
    Faulted,
    AbortedCap,
    Stopped,
};

enum class DisasmSyntax : std::uint8_t
{
    Intel = 0,
    ATT,
};

enum class DisasmBackend : std::uint8_t
{
    Zydis = 0,
    Bddisasm,
    Capstone,
    Xed,
};

// Register classes that `--quick` diffs and prints, selected by `--track`.
struct TrackMask
{
    bool gpr{};
    bool rip{};
    bool rflags{};
    bool xmm{};
    bool x87{};
};

struct GPRSeed
{
    std::string full{};      // 64-bit (RAX..R15).
    std::string dword{};     // Low 32 bits (EAX..).
    std::string word{};      // Low 16 bits (AX..).
    std::string byte_high{}; // Bits 8..15 (AH/BH/CH/DH. First four GPRs only).
    std::string byte_low{};  // Low 8 bits (AL..).
};

// A decimal seed value, plus whether it carried an `f`/`F` (single-precision) suffix.
struct DecimalSeed
{
    double value{};
    bool   is_single{};
};

struct Registers
{
    auto operator[] (Reg reg) const noexcept
    {
        switch (reg)
        {
        case Reg::RIP:    return rip;
        case Reg::RFLAGS: return rflags;
        default:          return gpr[(std::size_t)reg];
        }
    }

    auto &operator[] (Reg reg) noexcept
    {
        switch (reg)
        {
        case Reg::RIP:    return rip;
        case Reg::RFLAGS: return rflags;
        default:          return gpr[(std::size_t)reg];
        }
    }

    // Integer state.
    std::array<std::uint64_t, GPR_COUNT> gpr{};    // RAX..R15.
    std::uint64_t                        rip{};    // Instruction pointer.
    std::uint64_t                        rflags{}; // Flags register.

    // SSE state.
    std::array<std::array<std::uint64_t, 2>, 16> xmm{};   // XMM0..15 as {lo, hi}.
    std::uint32_t                                mxcsr{}; // SSE control/status.

    // x87 state.
    std::array<std::array<std::uint8_t, 10>, 8> st{};                    // Stack-relative ST(0)..ST(7), each 80-bit.
    std::uint16_t                               fpu_control_word{};      // x87 control word.
    std::uint16_t                               fpu_status_word{};       // x87 status word.
    std::uint8_t                                fpu_tag_word_abridged{}; // FXSAVE abridged tag (1 bit/reg), not the 16-bit x87 tag word.
};

struct Step
{
    // Instruction.
    std::uint64_t             rip{};   // Instruction or data address.
    std::vector<std::uint8_t> bytes{}; // Machine code.
    std::string               text{};  // Disassembled instruction (decoder mnemonic + operands). "(bad)" if undecodable.

    // Register state after a completed instruction or at an exception.
    Registers registers{};

    // Classification.
    bool reached{}; // Completed instruction.
    bool faulted{}; // Faulting instruction with exception-time partial state.
    bool data{};    // Raw bytes shown as "(data)", not an instruction. Skip on re-disassembly.
};

struct Trace
{
    // Recorded execution.
    std::vector<std::uint8_t> code{}; // Original input bytes.
    Registers                 seed{}; // State before step 0.
    std::vector<Step>         steps{};

    // Outcome.
    Outcome       outcome = Outcome::Idle;
    std::string   message{};
    bool          emulator_detected{}; // Native TF context was not safe to trust.
    std::string   stop_reason{};       // Reason execution halted (labels the stop instruction).
    std::uint64_t stop_address{};      // Address of the stop instruction (int3/fault), 0 otherwise.
};

// A backend-agnostic decode result.
// The disassembly text and the instruction's byte length.
struct Decoded
{
    std::string text{};
    std::size_t length{};

    [[nodiscard]] bool valid() const noexcept { return !text.empty() && length != 0; }

    explicit operator bool() const noexcept { return valid(); }
};

struct CLI
{
    static Result<CLI, std::string> parse(std::int32_t argc, char *argv[]);

    std::optional<std::string>     bytes{};
    bool                           run{};
    bool                           quick{};
    DisasmSyntax                   syntax    = DisasmSyntax::Intel;
    DisasmBackend                  backend   = DisasmBackend::Zydis;
    std::size_t                    max_steps = DEFAULT_MAX_STEPS;
    TrackMask                      track{};
    std::array<GPRSeed, GPR_COUNT> seed_gpr{};
    std::uint64_t                  seed_flags{};
    std::array<std::string, 16>    seed_xmm{};
    std::array<std::string, 8>     seed_st{};
};

// Initialize the process-wide OS parameters the engine needs (page size, allocation granularity).
// Call once before `run_engine`/`run_quick`/`run_tui`.
void init();

Result<std::vector<std::uint8_t>, std::string>    parse_hex(std::string_view text);
Result<std::uint64_t, std::string>                parse_seed(std::string_view seed);
Result<DecimalSeed, std::string>                  parse_decimal_seed(std::string_view text, std::string_view label);
Result<std::array<std::uint64_t, 2>, std::string> compose_xmm_seed(const std::string &text);
Result<std::array<std::uint8_t, 10>, std::string> compose_st_seed(const std::string &text);

std::uint64_t compose_gpr_seed(const GPRSeed &seed, std::string_view label, std::vector<std::string> &errors);
Registers     compose_seed(
    const std::array<GPRSeed, GPR_COUNT> &seed_gpr,
    std::uint64_t                         seed_flags,
    const std::array<std::string, 16>    &seed_xmm,
    const std::array<std::string, 8>     &seed_st,
    std::vector<std::string>             &errors
);

Decoded disasm_one(DisasmBackend backend, DisasmSyntax syntax, std::uint64_t address, const std::uint8_t *code, std::size_t size) noexcept;

// Process-global engine access is serialized.
// Do not call recursively.
Trace run_engine(
    std::span<std::uint8_t> code, const Registers &seed, std::size_t max_steps, DisasmBackend backend, DisasmSyntax syntax, bool seed_data_pointers
);

std::int32_t run_quick(const CLI &cli);
std::int32_t run_tui(const CLI &cli);

} // namespace bme
