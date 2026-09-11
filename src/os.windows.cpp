#include "os.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <ntstatus.h>
#include <Windows.h>
#include <winternl.h>

namespace bme
{

namespace
{

constexpr std::size_t PROCESS_QUERY_INITIAL_BYTES = 0x10000;
constexpr std::size_t PROCESS_QUERY_MAX_BYTES     = 0x4000000;
constexpr std::size_t PROCESS_PARENT_ID_OFFSET    = offsetof(SYSTEM_PROCESS_INFORMATION, UniqueProcessId) + sizeof(HANDLE);
constexpr std::size_t PROCESS_RECORD_MIN_BYTES    = PROCESS_PARENT_ID_OFFSET + sizeof(HANDLE);

struct RawStep
{
    std::uint64_t rip{};
    Registers     registers{};
};

struct Engine
{
    void snapshot(CONTEXT *context, Registers &registers) const noexcept;
    void snapshot_fpu(CONTEXT *context, Registers &registers) const noexcept;

    // Code image and execution limit.
    std::uint64_t base{};
    std::uint64_t code_length{};
    std::size_t   max_steps{};

    // Active VEH state.
    DWORD         thread_id{};
    std::uint64_t previous_rip{};
    CONTEXT       saved_context{};
    bool          active{};

    // Captured execution result.
    std::vector<RawStep> raw_steps{};
    Outcome              outcome = Outcome::Finished;
    DWORD                fault_code{};
    std::uint64_t        fault_address{};
    Registers            fault_registers{};
};

std::unique_ptr<Engine> g_engine{};

DWORD WINAPI sandbox_thread_main(LPVOID) { return 0; }

LONG CALLBACK bme_veh(EXCEPTION_POINTERS *exception_pointers) noexcept;

const char *fault_name(DWORD code) noexcept
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:         return "Access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "Illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION:         return "Privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "Divide by zero";
    case EXCEPTION_INT_OVERFLOW:             return "Integer overflow";
    case EXCEPTION_STACK_OVERFLOW:           return "Stack overflow";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "Misaligned access";
    case EXCEPTION_BREAKPOINT:               return "Breakpoint (int3)";
    case EXCEPTION_IN_PAGE_ERROR:            return "In-page error";
    case EXCEPTION_GUARD_PAGE:               return "Guard page violation";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "Array bounds exceeded";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FP divide by zero";
    case EXCEPTION_FLT_INVALID_OPERATION:    return "FP invalid operation";
    case EXCEPTION_FLT_OVERFLOW:             return "FP overflow";
    case EXCEPTION_FLT_UNDERFLOW:            return "FP underflow";
    case EXCEPTION_FLT_INEXACT_RESULT:       return "FP inexact result";
    case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FP denormal operand";
    case EXCEPTION_FLT_STACK_CHECK:          return "FP stack check";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "Noncontinuable exception";
    case EXCEPTION_INVALID_DISPOSITION:      return "Invalid disposition";
    default:                                 return "Exception";
    }
}

} // namespace

void *vm_alloc(std::size_t size) noexcept { return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS); }

void *vm_alloc_at(std::uint64_t address, std::size_t size) noexcept { return VirtualAlloc((void *)address, size, MEM_RESERVE, PAGE_NOACCESS); }

bool vm_commit(void *address, std::size_t size, VMProtection protection) noexcept
{
    auto native_protection = protection == VMProtection::ReadWrite ? PAGE_READWRITE : PAGE_EXECUTE_READ;

    return VirtualAlloc(address, size, MEM_COMMIT, native_protection) != nullptr;
}

bool vm_protect(void *address, std::size_t size, VMProtection protection) noexcept
{
    DWORD old_protection{};
    auto  native_protection = protection == VMProtection::ReadWrite ? PAGE_READWRITE : PAGE_EXECUTE_READ;

    return VirtualProtect(address, size, native_protection, &old_protection) != FALSE;
}

void vm_free(void *address, std::size_t) noexcept
{
    if (address != nullptr)
    {
        VirtualFree(address, 0, MEM_RELEASE);
    }
}

std::size_t vm_page_size() noexcept
{
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);

    return system_info.dwPageSize;
}

std::size_t vm_allocation_granularity() noexcept
{
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);

    return system_info.dwAllocationGranularity;
}

bool vm_flush_instruction_cache(const void *address, std::size_t size) noexcept
{
    return FlushInstructionCache(GetCurrentProcess(), address, size) != FALSE;
}

bool copy_to_clipboard(std::string_view text) noexcept
{
    if (text.empty() || OpenClipboard(nullptr) == FALSE)
    {
        return false;
    }

    if (EmptyClipboard() == FALSE)
    {
        CloseClipboard();

        return false;
    }

    bool  copied{};
    auto *handle = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (handle != nullptr)
    {
        auto *buffer = (char *)GlobalLock(handle);
        if (buffer != nullptr)
        {
            auto result = std::ranges::copy(text, buffer);
            *result.out = '\0';

            GlobalUnlock(handle);

            copied = SetClipboardData(CF_TEXT, handle) != nullptr;
            if (!copied)
            {
                GlobalFree(handle);
            }
        }
        else
        {
            GlobalFree(handle);
        }
    }

    return CloseClipboard() != FALSE && copied;
}

std::string environment_string(std::string_view name)
{
    std::string environment_name{name};
    if (environment_name.empty())
    {
        return {};
    }

    auto required = GetEnvironmentVariableA(environment_name.c_str(), nullptr, 0);
    if (required == 0)
    {
        return {};
    }

    auto        capacity = required;
    std::string value(capacity, '\0');
    for (;;)
    {
        auto written = GetEnvironmentVariableA(environment_name.c_str(), value.data(), capacity);
        if (written == 0)
        {
            return {};
        }

        if (written < capacity)
        {
            value.resize(written);

            return value;
        }

        capacity = written;

        value.resize(capacity);
    }
}

bool environment_present(std::string_view name)
{
    std::string environment_name{name};
    if (environment_name.empty())
    {
        return false;
    }

    SetLastError(ERROR_SUCCESS);

    auto required = GetEnvironmentVariableA(environment_name.c_str(), nullptr, 0);

    return required != 0 || GetLastError() == ERROR_SUCCESS;
}

namespace
{

bool parent_process_is(std::initializer_list<std::string_view> executable_names)
{
    using NtQuerySystemInformationFn = NTSTATUS(NTAPI *)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);

    static auto query = [] -> NtQuerySystemInformationFn
    {
        auto ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll == nullptr)
        {
            return nullptr;
        }

        return (NtQuerySystemInformationFn)GetProcAddress(ntdll, "NtQuerySystemInformation");
    }();
    if (query == nullptr)
    {
        return false;
    }

    std::vector<std::uint8_t> process_buffer(PROCESS_QUERY_INITIAL_BYTES);
    ULONG                     process_buffer_length{};
    for (;;)
    {
        auto status = query(SystemProcessInformation, process_buffer.data(), (ULONG)process_buffer.size(), &process_buffer_length);
        if (NT_SUCCESS(status))
        {
            if ((std::size_t)process_buffer_length > process_buffer.size())
            {
                return false;
            }

            break;
        }

        if (status != STATUS_INFO_LENGTH_MISMATCH && status != STATUS_BUFFER_TOO_SMALL)
        {
            return false;
        }

        auto next_size = process_buffer.size();
        if ((std::size_t)process_buffer_length > next_size)
        {
            next_size = (std::size_t)process_buffer_length;
        }
        else if (next_size <= PROCESS_QUERY_MAX_BYTES / 2)
        {
            next_size *= 2;
        }
        else
        {
            next_size = PROCESS_QUERY_MAX_BYTES;
        }

        if (next_size <= process_buffer.size() || next_size > PROCESS_QUERY_MAX_BYTES)
        {
            return false;
        }

        process_buffer.resize(next_size);
    }

    auto process_id_from_handle = [](HANDLE native_process_id) noexcept { return (DWORD)(ULONG_PTR)native_process_id; };

    auto find_process = [&process_buffer, process_buffer_length,
                         &process_id_from_handle](DWORD target_process_id) noexcept -> PSYSTEM_PROCESS_INFORMATION
    {
        auto       *buffer_begin = process_buffer.data();
        auto        buffer_size  = (std::size_t)process_buffer_length;
        std::size_t process_offset{};
        for (;;)
        {
            if (process_offset > buffer_size || buffer_size - process_offset < PROCESS_RECORD_MIN_BYTES)
            {
                return nullptr;
            }

            auto *process = (PSYSTEM_PROCESS_INFORMATION)(buffer_begin + process_offset);
            if (process_id_from_handle(process->UniqueProcessId) == target_process_id)
            {
                return process;
            }

            auto next_offset = (std::size_t)process->NextEntryOffset;
            if (next_offset == 0 || next_offset < PROCESS_RECORD_MIN_BYTES || next_offset > buffer_size - process_offset)
            {
                return nullptr;
            }

            process_offset += next_offset;
        }
    };

    auto *current_process = find_process(GetCurrentProcessId());
    if (current_process == nullptr)
    {
        return false;
    }

    HANDLE native_parent_id{};
    std::memcpy(&native_parent_id, (std::uint8_t *)current_process + PROCESS_PARENT_ID_OFFSET, sizeof(native_parent_id));

    auto  parent_id = process_id_from_handle(native_parent_id);
    auto *parent    = find_process(parent_id);
    if (parent == nullptr || parent->ImageName.Buffer == nullptr)
    {
        return false;
    }

    auto image_length_bytes = (std::size_t)parent->ImageName.Length;
    if (image_length_bytes % sizeof(wchar_t) != 0)
    {
        return false;
    }

    auto buffer_begin = (std::uintptr_t)process_buffer.data();
    auto buffer_end   = buffer_begin + (std::size_t)process_buffer_length;
    auto image_begin  = (std::uintptr_t)parent->ImageName.Buffer;
    if (image_begin < buffer_begin || image_begin > buffer_end)
    {
        return false;
    }

    if (image_length_bytes > buffer_end - image_begin)
    {
        return false;
    }

    std::wstring_view image_path{parent->ImageName.Buffer, image_length_bytes / sizeof(wchar_t)};
    auto              delim           = image_path.find_last_of(L"\\/");
    auto              executable_name = delim == std::wstring_view::npos ? image_path : image_path.substr(delim + 1);

    for (auto &&expected_name : executable_names)
    {
        if (ascii_case_insensitive_equal(executable_name, expected_name))
        {
            return true;
        }
    }

    return false;
}

} // namespace

bool instrumentation_detected()
{
    return environment_present("SDE_COMMAND_LINE")
        || parent_process_is({"sde.exe"})
        || GetModuleHandleA("sde.dll")
        != nullptr
        || environment_present("PIN_COMMAND_LINE")
        || parent_process_is({"pin.exe", "pinbin.exe"})
        || GetModuleHandleA("pinvm.dll")
        != nullptr;
}

void Engine::snapshot(CONTEXT *context, Registers &registers) const noexcept
{
    registers[Reg::RAX] = context->Rax;
    registers[Reg::RBX] = context->Rbx;
    registers[Reg::RCX] = context->Rcx;
    registers[Reg::RDX] = context->Rdx;
    registers[Reg::RSI] = context->Rsi;
    registers[Reg::RDI] = context->Rdi;
    registers[Reg::RBP] = context->Rbp;
    registers[Reg::RSP] = context->Rsp;
    registers[Reg::R8]  = context->R8;
    registers[Reg::R9]  = context->R9;
    registers[Reg::R10] = context->R10;
    registers[Reg::R11] = context->R11;
    registers[Reg::R12] = context->R12;
    registers[Reg::R13] = context->R13;
    registers[Reg::R14] = context->R14;
    registers[Reg::R15] = context->R15;
    registers.rip       = context->Rip;
    registers.rflags    = context->EFlags & ~RFLAGS_TRAP_FLAG;

    snapshot_fpu(context, registers);
}

void Engine::snapshot_fpu(CONTEXT *context, Registers &registers) const noexcept
{
    for (std::size_t i{}; i < registers.xmm.size(); ++i)
    {
        registers.xmm[i][0] = context->FltSave.XmmRegisters[i].Low;
        registers.xmm[i][1] = (std::uint64_t)context->FltSave.XmmRegisters[i].High;
    }

    for (std::size_t i{}; i < registers.st.size(); ++i)
    {
        std::memcpy(registers.st[i].data(), &context->FltSave.FloatRegisters[i], 10);
    }

    registers.mxcsr                 = context->MxCsr;
    registers.fpu_control_word      = context->FltSave.ControlWord;
    registers.fpu_status_word       = context->FltSave.StatusWord;
    registers.fpu_tag_word_abridged = context->FltSave.TagWord;
}

namespace
{
LONG CALLBACK bme_veh(EXCEPTION_POINTERS *exception_pointers) noexcept
{
    if (g_engine == nullptr || !g_engine->active || GetCurrentThreadId() != g_engine->thread_id)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    auto *context  = exception_pointers->ContextRecord;
    auto  exc_code = exception_pointers->ExceptionRecord->ExceptionCode;
    if (exc_code == EXCEPTION_SINGLE_STEP)
    {
        auto next_rip   = context->Rip;
        auto code_begin = g_engine->base;

        auto &raw_step = g_engine->raw_steps.emplace_back();
        raw_step.rip   = g_engine->previous_rip;
        g_engine->snapshot(context, raw_step.registers);

        auto code_end = code_begin + g_engine->code_length;
        if (next_rip < code_begin || next_rip >= code_end)
        {
            g_engine->outcome = Outcome::Finished;
            *context          = g_engine->saved_context;

            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (g_engine->raw_steps.size() >= g_engine->max_steps)
        {
            g_engine->outcome = Outcome::AbortedCap;
            *context          = g_engine->saved_context;

            return EXCEPTION_CONTINUE_EXECUTION;
        }

        g_engine->previous_rip  = next_rip;
        context->EFlags        |= (DWORD)RFLAGS_TRAP_FLAG;

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (exc_code == EXCEPTION_BREAKPOINT)
    {
        g_engine->outcome       = Outcome::Stopped;
        g_engine->fault_address = g_engine->previous_rip;
        *context                = g_engine->saved_context;

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    g_engine->outcome       = Outcome::Faulted;
    g_engine->fault_code    = exc_code;
    g_engine->fault_address = (std::uint64_t)exception_pointers->ExceptionRecord->ExceptionAddress;
    g_engine->snapshot(context, g_engine->fault_registers);
    *context = g_engine->saved_context;

    return EXCEPTION_CONTINUE_EXECUTION;
}
} // namespace

PlatformRunResult run_platform_steps(const PlatformRunRequest &request)
{
    PlatformRunResult result{};
    if (!prepare_platform_run(request, result))
    {
        return result;
    }

    auto  page_size   = vm_page_size();
    auto  code_region = request.code.size() + page_size - 1 & ~(page_size - 1);
    auto *buffer      = (std::uint8_t *)vm_alloc(code_region + page_size);
    auto *stack       = (std::uint8_t *)vm_alloc(SCRATCH_STACK_BYTES + page_size);
    auto *data        = (std::uint8_t *)vm_alloc_at(request.scratch_reserve_base, SCRATCH_DATA_BYTES + (page_size * 2));

    auto release_regions = [&buffer, &stack, &data, code_region, page_size]() noexcept
    {
        vm_free(buffer, code_region + page_size);
        vm_free(stack, SCRATCH_STACK_BYTES + page_size);
        vm_free(data, SCRATCH_DATA_BYTES + (page_size * 2));
    };

    auto fail = [&result, &release_regions](std::string error)
    {
        release_regions();

        result.outcome = Outcome::Faulted;
        result.error   = std::move(error);

        return result;
    };

    if (buffer == nullptr || stack == nullptr || data == nullptr)
    {
        return fail("VirtualAlloc failed.");
    }

    auto *code_base = buffer + code_region - request.code.size();

    if (!vm_commit(buffer, code_region, VMProtection::ReadWrite)
        || !vm_commit(stack + page_size, SCRATCH_STACK_BYTES, VMProtection::ReadWrite)
        || !vm_commit(data + page_size, SCRATCH_DATA_BYTES, VMProtection::ReadWrite))
    {
        return fail("VirtualAlloc commit failed.");
    }

    std::memcpy(code_base, request.code.data(), request.code.size());

    if (!vm_protect(buffer, code_region, VMProtection::ExecuteRead))
    {
        return fail("VirtualProtect failed.");
    }

    if (!vm_flush_instruction_cache(code_base, request.code.size()))
    {
        return fail("FlushInstructionCache failed.");
    }

    auto initial_rsp   = (std::uint64_t)(stack + page_size + SCRATCH_STACK_BYTES - 0x100) & ~(std::uint64_t)15;
    auto data_base     = (std::uint64_t)(data + page_size);
    auto effective_rdi = request.seed_data_pointers && request.seed[Reg::RDI] == 0 ? data_base : request.seed[Reg::RDI];
    auto effective_rsi = request.seed_data_pointers && request.seed[Reg::RSI] == 0 ? data_base : request.seed[Reg::RSI];

    g_engine               = std::make_unique<Engine>();
    g_engine->base         = (std::uint64_t)code_base;
    g_engine->code_length  = request.code.size();
    g_engine->max_steps    = std::min(request.max_steps != 0 ? request.max_steps : (std::size_t)1, MAX_STEPS_LIMIT);
    g_engine->previous_rip = g_engine->base;
    g_engine->raw_steps.reserve(g_engine->max_steps);

    result.seed[Reg::RSP]    = initial_rsp;
    result.seed[Reg::RDI]    = effective_rdi;
    result.seed[Reg::RSI]    = effective_rsi;
    result.seed[Reg::RIP]    = g_engine->base;
    result.seed[Reg::RFLAGS] = (request.seed[Reg::RFLAGS] & RFLAGS_STATUS_MASK) | RFLAGS_RESERVED_BIT1 | RFLAGS_INTERRUPT_FLAG;

    auto thread = CreateThread(nullptr, 0, sandbox_thread_main, nullptr, CREATE_SUSPENDED, nullptr);
    if (thread == nullptr)
    {
        g_engine = nullptr;

        return fail("CreateThread failed.");
    }

    auto terminate_thread = [thread]() noexcept
    {
        if (TerminateThread(thread, 0) != FALSE)
        {
            WaitForSingleObject(thread, INFINITE);
        }

        CloseHandle(thread);
    };

    auto fail_thread = [&result, &release_regions, &terminate_thread](std::string error)
    {
        terminate_thread();
        release_regions();

        g_engine = nullptr;

        result.outcome = Outcome::Faulted;
        result.error   = std::move(error);

        return result;
    };

    g_engine->thread_id = GetThreadId(thread);
    if (g_engine->thread_id == 0)
    {
        return fail_thread("GetThreadId failed.");
    }

    CONTEXT context{};
    context.ContextFlags = CONTEXT_ALL;
    if (GetThreadContext(thread, &context) == FALSE)
    {
        return fail_thread("GetThreadContext failed.");
    }

    g_engine->saved_context = context;
    g_engine->snapshot_fpu(&context, result.seed);

    for (std::size_t i{}; i < result.seed.xmm.size(); ++i)
    {
        result.seed.xmm[i] = request.seed.xmm[i];

        context.FltSave.XmmRegisters[i].Low  = request.seed.xmm[i][0];
        context.FltSave.XmmRegisters[i].High = (LONGLONG)request.seed.xmm[i][1];
    }

    for (std::size_t i{}; i < result.seed.st.size(); ++i)
    {
        if ((request.seed.fpu_tag_word_abridged >> i & 1) != 0)
        {
            result.seed.st[i] = request.seed.st[i];

            std::memcpy(&context.FltSave.FloatRegisters[i], request.seed.st[i].data(), request.seed.st[i].size());

            context.FltSave.TagWord |= (std::uint8_t)(1 << i);
        }
    }

    result.seed.fpu_tag_word_abridged = context.FltSave.TagWord;

    context.Rax    = request.seed[Reg::RAX];
    context.Rbx    = request.seed[Reg::RBX];
    context.Rcx    = request.seed[Reg::RCX];
    context.Rdx    = request.seed[Reg::RDX];
    context.Rsi    = effective_rsi;
    context.Rdi    = effective_rdi;
    context.Rbp    = request.seed[Reg::RBP];
    context.R8     = request.seed[Reg::R8];
    context.R9     = request.seed[Reg::R9];
    context.R10    = request.seed[Reg::R10];
    context.R11    = request.seed[Reg::R11];
    context.R12    = request.seed[Reg::R12];
    context.R13    = request.seed[Reg::R13];
    context.R14    = request.seed[Reg::R14];
    context.R15    = request.seed[Reg::R15];
    context.Rsp    = initial_rsp;
    context.Rip    = g_engine->base;
    context.EFlags = (DWORD)((request.seed[Reg::RFLAGS] & RFLAGS_STATUS_MASK) | RFLAGS_RESERVED_BIT1 | RFLAGS_INTERRUPT_FLAG | RFLAGS_TRAP_FLAG);

    if (SetThreadContext(thread, &context) == FALSE)
    {
        return fail_thread("SetThreadContext failed.");
    }

    auto *veh_handle = AddVectoredExceptionHandler(1, bme_veh);
    if (veh_handle == nullptr)
    {
        return fail_thread("Failed to install exception handler.");
    }

    g_engine->active = true;
    if (ResumeThread(thread) == (DWORD)-1)
    {
        g_engine->active = false;

        RemoveVectoredExceptionHandler(veh_handle);

        return fail_thread("ResumeThread failed.");
    }

    auto wait_result = WaitForSingleObject(thread, INFINITE);

    g_engine->active = false;

    RemoveVectoredExceptionHandler(veh_handle);

    if (wait_result != WAIT_OBJECT_0)
    {
        return fail_thread("WaitForSingleObject failed.");
    }

    CloseHandle(thread);

    result.steps.reserve(g_engine->raw_steps.size() + (g_engine->outcome == Outcome::Faulted ? 1 : 0));

    for (auto &&raw_step : g_engine->raw_steps)
    {
        auto &step     = result.steps.emplace_back();
        step.rip       = raw_step.rip;
        step.registers = raw_step.registers;
    }

    if (g_engine->outcome == Outcome::Faulted)
    {
        auto &step     = result.steps.emplace_back();
        step.rip       = g_engine->fault_address;
        step.registers = g_engine->fault_registers;
        step.faulted   = true;
    }

    result.outcome      = g_engine->outcome;
    result.stop_address = g_engine->fault_address;

    if (g_engine->outcome == Outcome::Faulted)
    {
        result.fault_name = fault_name(g_engine->fault_code);
    }

    release_regions();

    g_engine = nullptr;

    return result;
}

} // namespace bme
