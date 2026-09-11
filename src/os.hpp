#pragma once

#include "bme_core.hpp"

namespace bme
{

enum class VMProtection : std::uint8_t
{
    ReadWrite = 0,
    ExecuteRead,
};

constexpr std::size_t   SCRATCH_STACK_BYTES       = 0x10000;
constexpr std::size_t   SCRATCH_DATA_BYTES        = 0x10000;
constexpr std::uint64_t SCRATCH_DATA_RESERVE_BASE = 0x1000'0000;

constexpr auto          RFLAGS_STATUS_MASK    = CF | PF | AF | ZF | SF | DF | OF;
constexpr std::uint64_t RFLAGS_TRAP_FLAG      = 0x100;
constexpr std::uint64_t RFLAGS_INTERRUPT_FLAG = 0x200;
constexpr std::uint64_t RFLAGS_RESERVED_BIT1  = 0x2;

void       *vm_alloc(std::size_t size) noexcept;
void       *vm_alloc_at(std::uint64_t address, std::size_t size) noexcept;
bool        vm_commit(void *address, std::size_t size, VMProtection protection) noexcept;
bool        vm_protect(void *address, std::size_t size, VMProtection protection) noexcept;
void        vm_free(void *address, std::size_t size) noexcept;
std::size_t vm_page_size() noexcept;
std::size_t vm_allocation_granularity() noexcept;
bool        vm_flush_instruction_cache(const void *address, std::size_t size) noexcept;

bool        copy_to_clipboard(std::string_view text) noexcept;
std::string environment_string(std::string_view name);
bool        environment_present(std::string_view name);
bool        instrumentation_detected();

extern "C" double st80_to_double(const std::array<std::uint8_t, 10> &bytes, std::uint16_t fpu_control_word) noexcept;
extern "C" void   double_to_st80(double value, std::array<std::uint8_t, 10> &out) noexcept;
extern "C" float  st80_to_float(const std::array<std::uint8_t, 10> &bytes, std::uint16_t fpu_control_word) noexcept;

struct PlatformStep
{
    std::uint64_t rip{};
    Registers     registers{};
    bool          faulted{};
};

struct PlatformRunRequest
{
    // Execution input and limit.
    std::span<const std::uint8_t> code{};
    Registers                     seed{};
    std::size_t                   max_steps{};

    // Scratch data setup.
    std::uint64_t scratch_reserve_base{};
    bool          seed_data_pointers{};
};

struct PlatformRunResult
{
    // Captured execution.
    Registers                 seed{};
    std::vector<PlatformStep> steps{};

    // Outcome.
    Outcome       outcome = Outcome::Finished;
    bool          instrumentation_detected{};
    std::string   fault_name{};
    std::string   error{};
    std::uint64_t stop_address{};
};

bool prepare_platform_run(const PlatformRunRequest &request, PlatformRunResult &result);

PlatformRunResult run_platform_steps(const PlatformRunRequest &request);

} // namespace bme
