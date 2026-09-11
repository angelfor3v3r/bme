#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "os.hpp"

#include <Zydis/Zydis.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

namespace bme
{

namespace
{

enum class ChildError : std::uint8_t
{
    None = 0,
    Trace,
    Allocation,
    Commit,
    Protect,
    Flush,
};

struct ChildReady
{
    std::uint64_t code_base{};
    std::uint64_t initial_rsp{};
    std::uint64_t data_base{};
    ChildError    error{};
};

bool write_all(int descriptor, const void *buffer, std::size_t size) noexcept
{
    auto *bytes = (const std::uint8_t *)buffer;
    while (size != 0)
    {
        auto written = write(descriptor, bytes, size);
        if (written > 0)
        {
            bytes += written;
            size  -= (std::size_t)written;

            continue;
        }

        if (written < 0 && errno == EINTR)
        {
            continue;
        }

        return false;
    }

    return true;
}

bool read_all(int descriptor, void *buffer, std::size_t size) noexcept
{
    auto *bytes = (std::uint8_t *)buffer;
    while (size != 0)
    {
        auto read_size = read(descriptor, bytes, size);
        if (read_size > 0)
        {
            bytes += read_size;
            size  -= (std::size_t)read_size;

            continue;
        }

        if (read_size < 0 && errno == EINTR)
        {
            continue;
        }

        return false;
    }

    return true;
}

bool wait_for_child(pid_t child, int &status) noexcept
{
    for (;;)
    {
        auto waited = waitpid(child, &status, 0);
        if (waited == child)
        {
            return true;
        }

        if (waited == -1 && errno == EINTR)
        {
            continue;
        }

        return false;
    }
}

// WSL2 reports both int3 and completed syscalls as breakpoint-class traps.
// Use Zydis here rather than duplicating x86 instruction decoding.
bool is_software_breakpoint(std::span<const std::uint8_t> code, std::uint64_t code_base, std::uint64_t instruction_address) noexcept
{
    if (instruction_address < code_base)
    {
        return false;
    }

    auto offset = instruction_address - code_base;
    if (offset >= code.size())
    {
        return false;
    }

    ZydisDecoder decoder{};
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
    {
        return false;
    }

    ZydisDecodedInstruction instruction{};
    ZydisDecodedOperand     operands[ZYDIS_MAX_OPERAND_COUNT]{};
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code.data() + offset, code.size() - offset, &instruction, operands)))
    {
        return false;
    }

    if (instruction.mnemonic == ZYDIS_MNEMONIC_INT3)
    {
        return true;
    }

    return instruction.mnemonic
        == ZYDIS_MNEMONIC_INT
        && instruction.operand_count_visible
        == 1
        && operands[0].type
        == ZYDIS_OPERAND_TYPE_IMMEDIATE
        && operands[0].imm.value.u
        == 3;
}

void terminate_child(pid_t child) noexcept
{
    if (child <= 0)
    {
        return;
    }

    kill(child, SIGKILL);

    int status{};
    wait_for_child(child, status);
}

void initialize_fpu() noexcept
{
    std::uint32_t mxcsr = 0x1F80;
    asm volatile("fninit\n\t"
                 "ldmxcsr %0\n\t"
                 "pxor %%xmm0, %%xmm0\n\t"
                 "pxor %%xmm1, %%xmm1\n\t"
                 "pxor %%xmm2, %%xmm2\n\t"
                 "pxor %%xmm3, %%xmm3\n\t"
                 "pxor %%xmm4, %%xmm4\n\t"
                 "pxor %%xmm5, %%xmm5\n\t"
                 "pxor %%xmm6, %%xmm6\n\t"
                 "pxor %%xmm7, %%xmm7\n\t"
                 "pxor %%xmm8, %%xmm8\n\t"
                 "pxor %%xmm9, %%xmm9\n\t"
                 "pxor %%xmm10, %%xmm10\n\t"
                 "pxor %%xmm11, %%xmm11\n\t"
                 "pxor %%xmm12, %%xmm12\n\t"
                 "pxor %%xmm13, %%xmm13\n\t"
                 "pxor %%xmm14, %%xmm14\n\t"
                 "pxor %%xmm15, %%xmm15"
                 :
                 : "m"(mxcsr)
                 : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13",
                   "xmm14", "xmm15");
}

void child_main(const PlatformRunRequest &request, int ready_descriptor) noexcept
{
    ChildReady ready{};
    if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) == -1)
    {
        ready.error = ChildError::Trace;

        write_all(ready_descriptor, &ready, sizeof(ready));
        _exit(1);
    }

    auto  page_size   = vm_page_size();
    auto  code_region = (request.code.size() + page_size - 1) & ~(page_size - 1);
    auto *buffer      = (std::uint8_t *)vm_alloc(code_region + page_size);
    auto *stack       = (std::uint8_t *)vm_alloc(SCRATCH_STACK_BYTES + page_size);
    auto *data        = (std::uint8_t *)vm_alloc_at(request.scratch_reserve_base, SCRATCH_DATA_BYTES + (page_size * 2));
    if (buffer == nullptr || stack == nullptr || data == nullptr)
    {
        ready.error = ChildError::Allocation;

        write_all(ready_descriptor, &ready, sizeof(ready));
        _exit(1);
    }

    auto *code_base = buffer + code_region - request.code.size();
    if (!vm_commit(buffer, code_region, VMProtection::ReadWrite)
        || !vm_commit(stack + page_size, SCRATCH_STACK_BYTES, VMProtection::ReadWrite)
        || !vm_commit(data + page_size, SCRATCH_DATA_BYTES, VMProtection::ReadWrite))
    {
        ready.error = ChildError::Commit;

        write_all(ready_descriptor, &ready, sizeof(ready));
        _exit(1);
    }

    std::memcpy(code_base, request.code.data(), request.code.size());

    if (!vm_protect(buffer, code_region, VMProtection::ExecuteRead))
    {
        ready.error = ChildError::Protect;

        write_all(ready_descriptor, &ready, sizeof(ready));
        _exit(1);
    }

    if (!vm_flush_instruction_cache(code_base, request.code.size()))
    {
        ready.error = ChildError::Flush;

        write_all(ready_descriptor, &ready, sizeof(ready));
        _exit(1);
    }

    initialize_fpu();

    ready.code_base   = (std::uint64_t)code_base;
    ready.initial_rsp = (std::uint64_t)(stack + page_size + SCRATCH_STACK_BYTES - 0x100) & ~(std::uint64_t)15;
    ready.data_base   = (std::uint64_t)(data + page_size);

    if (!write_all(ready_descriptor, &ready, sizeof(ready)))
    {
        _exit(1);
    }

    raise(SIGSTOP);
    _exit(1);
}

bool get_fpu_registers(pid_t child, user_fpregs_struct &registers) noexcept { return ptrace(PTRACE_GETFPREGS, child, nullptr, &registers) != -1; }

bool set_fpu_registers(pid_t child, const user_fpregs_struct &registers) noexcept
{
    return ptrace(PTRACE_SETFPREGS, child, nullptr, &registers) != -1;
}

void snapshot_fpu(const user_fpregs_struct &source, Registers &target) noexcept
{
    for (std::size_t i{}; i < target.xmm.size(); ++i)
    {
        std::memcpy(target.xmm[i].data(), source.xmm_space + (i * 4), target.xmm[i].size() * sizeof(target.xmm[i][0]));
    }

    auto top = (source.swd >> 11) & 7;
    for (std::size_t i{}; i < target.st.size(); ++i)
    {
        auto physical = ((std::size_t)top + i) & 7;

        std::memcpy(target.st[i].data(), source.st_space + (physical * 4), target.st[i].size());
    }

    target.mxcsr                 = source.mxcsr;
    target.fpu_control_word      = source.cwd;
    target.fpu_status_word       = source.swd;
    target.fpu_tag_word_abridged = (std::uint8_t)source.ftw;
}

bool read_snapshot(pid_t child, Registers &target) noexcept
{
    user_regs_struct   registers{};
    user_fpregs_struct fpu_registers{};
    if (ptrace(PTRACE_GETREGS, child, nullptr, &registers) == -1 || !get_fpu_registers(child, fpu_registers))
    {
        return false;
    }

    target[Reg::RAX] = registers.rax;
    target[Reg::RBX] = registers.rbx;
    target[Reg::RCX] = registers.rcx;
    target[Reg::RDX] = registers.rdx;
    target[Reg::RSI] = registers.rsi;
    target[Reg::RDI] = registers.rdi;
    target[Reg::RBP] = registers.rbp;
    target[Reg::RSP] = registers.rsp;
    target[Reg::R8]  = registers.r8;
    target[Reg::R9]  = registers.r9;
    target[Reg::R10] = registers.r10;
    target[Reg::R11] = registers.r11;
    target[Reg::R12] = registers.r12;
    target[Reg::R13] = registers.r13;
    target[Reg::R14] = registers.r14;
    target[Reg::R15] = registers.r15;
    target.rip       = registers.rip;
    target.rflags    = registers.eflags & ~RFLAGS_TRAP_FLAG;

    snapshot_fpu(fpu_registers, target);

    return true;
}

const char *fault_name(int signal) noexcept
{
    switch (signal)
    {
    case SIGILL:  return "Illegal instruction";
    case SIGFPE:  return "Floating-point exception";
    case SIGSEGV: return "Access violation";
    case SIGBUS:  return "Bus error";
    case SIGTRAP: return "Trap signal";
    default:      return "Signal";
    }
}

const char *child_error_name(ChildError error) noexcept
{
    switch (error)
    {
    case ChildError::Trace:      return "ptrace worker setup failed.";
    case ChildError::Allocation: return "Memory allocation failed.";
    case ChildError::Commit:     return "Memory commit failed.";
    case ChildError::Protect:    return "Memory protection failed.";
    case ChildError::Flush:      return "Instruction-cache flush failed.";
    default:                     return "Unknown worker setup failure.";
    }
}

} // namespace

void *vm_alloc(std::size_t size) noexcept
{
    auto *result = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    return result == MAP_FAILED ? nullptr : result;
}

void *vm_alloc_at(std::uint64_t address, std::size_t size) noexcept
{
    auto *result = mmap((void *)address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

    return result == MAP_FAILED ? nullptr : result;
}

bool vm_commit(void *address, std::size_t size, VMProtection protection) noexcept
{
    auto native_protection = protection == VMProtection::ReadWrite ? PROT_READ | PROT_WRITE : PROT_READ | PROT_EXEC;

    return mprotect(address, size, native_protection) == 0;
}

bool vm_protect(void *address, std::size_t size, VMProtection protection) noexcept { return vm_commit(address, size, protection); }

void vm_free(void *address, std::size_t size) noexcept
{
    if (address != nullptr)
    {
        munmap(address, size);
    }
}

std::size_t vm_page_size() noexcept
{
    auto page_size = sysconf(_SC_PAGESIZE);

    return page_size > 0 ? (std::size_t)page_size : 4096;
}

std::size_t vm_allocation_granularity() noexcept { return vm_page_size(); }

bool vm_flush_instruction_cache(const void *address, std::size_t size) noexcept
{
    auto *begin = (char *)address;
    __builtin___clear_cache(begin, begin + size);

    return true;
}

bool copy_to_clipboard(std::string_view text) noexcept
{
    if (text.empty() || isatty(STDOUT_FILENO) == 0)
    {
        return false;
    }

    auto encode = [](std::uint8_t value) noexcept
    {
        return value < 26  ? (char)('A' + value)
             : value < 52  ? (char)('a' + value - 26)
             : value < 62  ? (char)('0' + value - 52)
             : value == 62 ? '+'
                           : '/';
    };
    if (std::fputs("\x1b]52;c;", stdout) < 0)
    {
        return false;
    }

    for (std::size_t offset{}; offset < text.size(); offset += 3)
    {
        auto                first  = (std::uint8_t)text[offset];
        auto                second = offset + 1 < text.size() ? (std::uint8_t)text[offset + 1] : 0;
        auto                third  = offset + 2 < text.size() ? (std::uint8_t)text[offset + 2] : 0;
        std::array<char, 4> encoded{
            encode(first >> 2),
            encode((first & 0x3) << 4 | second >> 4),
            offset + 1 < text.size() ? encode((second & 0xF) << 2 | third >> 6) : '=',
            offset + 2 < text.size() ? encode(third & 0x3F) : '=',
        };
        if (std::fwrite(encoded.data(), 1, encoded.size(), stdout) != encoded.size())
        {
            return false;
        }
    }

    return std::fputs("\a", stdout) >= 0 && std::fflush(stdout) == 0;
}

std::string environment_string(std::string_view name)
{
    std::string environment_name{name};
    if (environment_name.empty())
    {
        return {};
    }

    auto *result = std::getenv(environment_name.c_str());

    return result == nullptr ? std::string{} : std::string{result};
}

bool environment_present(std::string_view name)
{
    std::string environment_name{name};

    return !environment_name.empty() && std::getenv(environment_name.c_str()) != nullptr;
}

bool instrumentation_detected() { return environment_present("SDE_COMMAND_LINE") || environment_present("PIN_COMMAND_LINE"); }

PlatformRunResult run_platform_steps(const PlatformRunRequest &request)
{
    PlatformRunResult result{};
    if (!prepare_platform_run(request, result))
    {
        return result;
    }

    int pipe_descriptors[2]{};
    if (pipe(pipe_descriptors) != 0)
    {
        result.outcome = Outcome::Faulted;
        result.error   = "ptrace worker pipe failed.";

        return result;
    }

    auto child = fork();
    if (child == -1)
    {
        close(pipe_descriptors[0]);
        close(pipe_descriptors[1]);

        result.outcome = Outcome::Faulted;
        result.error   = "ptrace worker fork failed.";

        return result;
    }

    if (child == 0)
    {
        close(pipe_descriptors[0]);
        child_main(request, pipe_descriptors[1]);
    }

    close(pipe_descriptors[1]);

    ChildReady ready{};
    auto       ready_received = read_all(pipe_descriptors[0], &ready, sizeof(ready));

    close(pipe_descriptors[0]);

    if (!ready_received)
    {
        terminate_child(child);

        result.outcome = Outcome::Faulted;
        result.error   = "ptrace worker startup failed.";

        return result;
    }

    int status{};
    if (!wait_for_child(child, status))
    {
        terminate_child(child);

        result.outcome = Outcome::Faulted;
        result.error   = "ptrace worker wait failed.";

        return result;
    }

    if (ready.error != ChildError::None)
    {
        result.outcome = Outcome::Faulted;
        result.error   = child_error_name(ready.error);

        return result;
    }

    if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP)
    {
        terminate_child(child);

        result.outcome = Outcome::Faulted;
        result.error   = "ptrace worker did not stop.";

        return result;
    }

    auto fail = [&result, child](std::string error)
    {
        terminate_child(child);

        result.outcome = Outcome::Faulted;
        result.error   = std::move(error);

        return result;
    };

    user_regs_struct   registers{};
    user_fpregs_struct fpu_registers{};
    if (ptrace(PTRACE_GETREGS, child, nullptr, &registers) == -1 || !get_fpu_registers(child, fpu_registers))
    {
        return fail("ptrace register read failed.");
    }

    snapshot_fpu(fpu_registers, result.seed);

    for (std::size_t i{}; i < result.seed.xmm.size(); ++i)
    {
        result.seed.xmm[i] = request.seed.xmm[i];

        std::memcpy(fpu_registers.xmm_space + (i * 4), request.seed.xmm[i].data(), request.seed.xmm[i].size() * sizeof(request.seed.xmm[i][0]));
    }

    for (std::size_t i{}; i < result.seed.st.size(); ++i)
    {
        if ((request.seed.fpu_tag_word_abridged >> i & 1) != 0)
        {
            result.seed.st[i] = request.seed.st[i];

            std::memcpy(fpu_registers.st_space + (i * 4), request.seed.st[i].data(), request.seed.st[i].size());

            fpu_registers.ftw |= (std::uint16_t)(1 << i);
        }
    }

    fpu_registers.swd &= (std::uint16_t)~(7 << 11);

    result.seed.fpu_tag_word_abridged = (std::uint8_t)fpu_registers.ftw;

    auto effective_rdi = request.seed_data_pointers && request.seed[Reg::RDI] == 0 ? ready.data_base : request.seed[Reg::RDI];
    auto effective_rsi = request.seed_data_pointers && request.seed[Reg::RSI] == 0 ? ready.data_base : request.seed[Reg::RSI];
    auto max_steps     = std::min(request.max_steps != 0 ? request.max_steps : (std::size_t)1, MAX_STEPS_LIMIT);

    result.seed[Reg::RSP]    = ready.initial_rsp;
    result.seed[Reg::RDI]    = effective_rdi;
    result.seed[Reg::RSI]    = effective_rsi;
    result.seed[Reg::RIP]    = ready.code_base;
    result.seed[Reg::RFLAGS] = (request.seed[Reg::RFLAGS] & RFLAGS_STATUS_MASK) | RFLAGS_RESERVED_BIT1 | RFLAGS_INTERRUPT_FLAG;

    registers.rax    = request.seed[Reg::RAX];
    registers.rbx    = request.seed[Reg::RBX];
    registers.rcx    = request.seed[Reg::RCX];
    registers.rdx    = request.seed[Reg::RDX];
    registers.rsi    = effective_rsi;
    registers.rdi    = effective_rdi;
    registers.rbp    = request.seed[Reg::RBP];
    registers.r8     = request.seed[Reg::R8];
    registers.r9     = request.seed[Reg::R9];
    registers.r10    = request.seed[Reg::R10];
    registers.r11    = request.seed[Reg::R11];
    registers.r12    = request.seed[Reg::R12];
    registers.r13    = request.seed[Reg::R13];
    registers.r14    = request.seed[Reg::R14];
    registers.r15    = request.seed[Reg::R15];
    registers.rsp    = ready.initial_rsp;
    registers.rip    = ready.code_base;
    registers.eflags = (request.seed[Reg::RFLAGS] & RFLAGS_STATUS_MASK) | RFLAGS_RESERVED_BIT1 | RFLAGS_INTERRUPT_FLAG;

    if (ptrace(PTRACE_SETREGS, child, nullptr, &registers) == -1 || !set_fpu_registers(child, fpu_registers))
    {
        return fail("ptrace register set failed.");
    }

    result.steps.reserve(max_steps + 1);

    auto previous_rip = ready.code_base;
    auto code_end     = ready.code_base + request.code.size();
    for (;;)
    {
        if (ptrace(PTRACE_SINGLESTEP, child, nullptr, nullptr) == -1)
        {
            return fail("ptrace single-step failed.");
        }

        if (!wait_for_child(child, status))
        {
            return fail("ptrace worker wait failed.");
        }

        if (!WIFSTOPPED(status))
        {
            result.outcome      = Outcome::Faulted;
            result.error        = "ptrace worker exited unexpectedly.";
            result.stop_address = previous_rip;

            terminate_child(child);

            return result;
        }

        auto signal = WSTOPSIG(status);
        bool single_step_trap{};
        bool breakpoint_signal{};
        bool breakpoint_trap{};
        if (signal == SIGTRAP)
        {
            siginfo_t signal_info{};
            if (ptrace(PTRACE_GETSIGINFO, child, nullptr, &signal_info) == -1)
            {
                return fail("ptrace signal info failed.");
            }

            breakpoint_signal = signal_info.si_code == TRAP_BRKPT || signal_info.si_code == SI_KERNEL;
            breakpoint_trap   = breakpoint_signal && is_software_breakpoint(request.code, ready.code_base, previous_rip);
            single_step_trap  = signal_info.si_code == TRAP_TRACE;
        }

        Registers snapshot{};
        if (!read_snapshot(child, snapshot))
        {
            return fail("ptrace register read failed.");
        }

        if (breakpoint_trap)
        {
            result.outcome      = Outcome::Stopped;
            result.stop_address = previous_rip;

            terminate_child(child);

            return result;
        }

        bool completed_trap = single_step_trap;

        // A completed syscall can use a breakpoint-class stop on WSL2.
        if (breakpoint_signal && snapshot.rip != previous_rip)
        {
            completed_trap = true;
        }

        if (signal != SIGTRAP || !completed_trap)
        {
            auto &step     = result.steps.emplace_back();
            step.rip       = snapshot.rip;
            step.registers = snapshot;
            step.faulted   = true;

            result.outcome      = Outcome::Faulted;
            result.fault_name   = fault_name(signal);
            result.stop_address = result.steps.back().rip;

            terminate_child(child);

            return result;
        }

        auto  next_rip = snapshot.rip;
        auto &step     = result.steps.emplace_back();
        step.rip       = previous_rip;
        step.registers = snapshot;

        if (next_rip < ready.code_base || next_rip >= code_end)
        {
            result.outcome = Outcome::Finished;

            terminate_child(child);

            return result;
        }

        if (result.steps.size() >= max_steps)
        {
            result.outcome = Outcome::AbortedCap;

            terminate_child(child);

            return result;
        }

        previous_rip = next_rip;
    }
}

} // namespace bme
