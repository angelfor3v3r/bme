#include "bme_core.hpp"
#include "common.hpp"

#if __has_include("bme_version.hpp")
#include "bme_version.hpp"
#else
#define BME_GIT_TAG           "unknown"
#define BME_GIT_HASH          "unknown"
#define BME_GIT_URL           "https://github.com/angelfor3v3r/bme"
#define BME_GLAZE_VERSION     "unknown"
#define BME_GLAZE_REVISION    "unknown"
#define BME_ZYDIS_VERSION     "unknown"
#define BME_ZYDIS_REVISION    "unknown"
#define BME_BDDISASM_VERSION  "unknown"
#define BME_BDDISASM_REVISION "unknown"
#define BME_CAPSTONE_VERSION  "unknown"
#define BME_CAPSTONE_REVISION "unknown"
#define BME_XED_VERSION       "unknown"
#define BME_XED_REVISION      "unknown"
#endif

#include <glaze/core/ostream_buffer.hpp>
#include <glaze/json/write.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <ios>
#include <optional>
#include <ostream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bme
{

namespace
{

// Every compiled decoder is serialized.
// Keep this list synchronized with `DisasmBackend`.
constexpr std::uint32_t TRACE_SCHEMA_VERSION = 1;
constexpr std::array    SERIALIZED_BACKENDS{
    DisasmBackend::Zydis,
    DisasmBackend::Bddisasm,
    DisasmBackend::Capstone,
    DisasmBackend::Xed,
};

struct PrettyJsonOptions : glz::opts
{
    std::uint8_t indentation_width = 2;
};

constexpr auto PRETTY_JSON_OPTIONS = []
{
    PrettyJsonOptions options{};
    options.skip_null_members = false;
    options.prettify          = true;

    return options;
}();

constexpr char hex_digit(std::uint8_t value) noexcept
{
    constexpr std::string_view HEX_DIGITS = "0123456789ABCDEF";

    return HEX_DIGITS[value & 0xF];
}

template <std::size_t digits>
std::string format_hex(std::uint64_t value)
{
    static_assert(digits <= 16);

    std::string result(2 + digits, '0');
    result[0] = '0';
    result[1] = 'x';

    for (std::size_t i{}; i < digits; ++i)
    {
        auto shift    = (digits - i - 1) * 4;
        result[i + 2] = hex_digit((std::uint8_t)(value >> shift));
    }

    return result;
}

std::string format_bytes(std::span<const std::uint8_t> bytes)
{
    std::string result(bytes.size() * 2, '0');
    for (std::size_t i{}; i < bytes.size(); ++i)
    {
        result[i * 2]     = hex_digit(bytes[i] >> 4);
        result[i * 2 + 1] = hex_digit(bytes[i]);
    }

    return result;
}

std::string format_xmm(const std::array<std::uint64_t, 2> &value)
{
    std::string result(34, '0');
    result[0] = '0';
    result[1] = 'x';

    for (std::size_t i{}; i < 16; ++i)
    {
        auto shift     = (15 - i) * 4;
        result[i + 2]  = hex_digit((std::uint8_t)(value[1] >> shift));
        result[i + 18] = hex_digit((std::uint8_t)(value[0] >> shift));
    }

    return result;
}

std::string format_st(const std::array<std::uint8_t, 10> &value)
{
    std::string result(22, '0');
    result[0] = '0';
    result[1] = 'x';

    for (std::size_t i{}; i < value.size(); ++i)
    {
        auto byte             = value[value.size() - i - 1];
        result[2 + i * 2]     = hex_digit(byte >> 4);
        result[2 + i * 2 + 1] = hex_digit(byte);
    }

    return result;
}

std::string_view backend_name(DisasmBackend backend) noexcept
{
    switch (backend)
    {
    case DisasmBackend::Zydis:    return "zydis";
    case DisasmBackend::Bddisasm: return "bddisasm";
    case DisasmBackend::Capstone: return "capstone";
    case DisasmBackend::Xed:      return "xed";
    }

    return "unknown";
}

std::string_view backend_version(DisasmBackend backend) noexcept
{
    switch (backend)
    {
    case DisasmBackend::Zydis:    return BME_ZYDIS_VERSION;
    case DisasmBackend::Bddisasm: return BME_BDDISASM_VERSION;
    case DisasmBackend::Capstone: return BME_CAPSTONE_VERSION;
    case DisasmBackend::Xed:      return BME_XED_VERSION;
    }

    return "unknown";
}

std::string_view backend_revision(DisasmBackend backend) noexcept
{
    switch (backend)
    {
    case DisasmBackend::Zydis:    return BME_ZYDIS_REVISION;
    case DisasmBackend::Bddisasm: return BME_BDDISASM_REVISION;
    case DisasmBackend::Capstone: return BME_CAPSTONE_REVISION;
    case DisasmBackend::Xed:      return BME_XED_REVISION;
    }

    return "unknown";
}

std::string_view syntax_name(DisasmSyntax syntax) noexcept { return syntax == DisasmSyntax::ATT ? "att" : "intel"; }

DisasmSyntax effective_syntax(DisasmBackend backend, DisasmSyntax requested) noexcept
{
    return backend_supports(backend, requested) ? requested : DisasmSyntax::Intel;
}

std::string_view capture_scope_name(CPUCaptureScope scope) noexcept
{
    switch (scope)
    {
    case CPUCaptureScope::HostVisibleProcess: return "host_visible_process";
    }

    return "unknown";
}

std::string_view outcome_name(Outcome outcome) noexcept
{
    switch (outcome)
    {
    case Outcome::Idle:       return "idle";
    case Outcome::Error:      return "error";
    case Outcome::Finished:   return "finished";
    case Outcome::Faulted:    return "faulted";
    case Outcome::AbortedCap: return "aborted_cap";
    case Outcome::Stopped:    return "stopped";
    }

    return "unknown";
}

std::string_view event_kind_name(ExecutionEventKind kind) noexcept { return kind == ExecutionEventKind::Faulted ? "faulted" : "completed"; }

std::optional<std::string_view> fault_access_name(FaultAccess access) noexcept
{
    switch (access)
    {
    case FaultAccess::Read:    return "read";
    case FaultAccess::Write:   return "write";
    case FaultAccess::Execute: return "execute";
    case FaultAccess::Unknown: return "unknown";
    default:                   return {};
    }
}

std::string_view history_kind_name(HistoryRowKind kind) noexcept
{
    switch (kind)
    {
    case HistoryRowKind::Reached:    return "reached";
    case HistoryRowKind::Faulted:    return "faulted";
    case HistoryRowKind::NotReached: return "not_reached";
    case HistoryRowKind::Data:       return "data";
    }

    return "unknown";
}

std::optional<std::size_t> relative_offset(const Trace &trace, std::uint64_t rip) noexcept
{
    auto base = trace.seed[Reg::RIP];
    if (rip < base)
    {
        return {};
    }

    auto offset = rip - base;
    if (offset >= trace.code.size())
    {
        return {};
    }

    return offset;
}

std::optional<std::string_view> nullable_text(const std::string &value) noexcept
{
    return value.empty() ? std::nullopt : std::optional<std::string_view>{value};
}

struct DependencyDocument
{
    std::string_view name{};
    std::string_view version{};
    std::string_view revision{};

    struct glaze
    {
        using T = DependencyDocument;

        static constexpr auto value = glz::object("name", &T::name, "version", &T::version, "revision", &T::revision);
    };
};

struct ProducerDocument
{
    std::string_view                  name       = "bme";
    std::string_view                  version    = BME_GIT_TAG;
    std::string_view                  git_hash   = BME_GIT_HASH;
    std::string_view                  repository = BME_GIT_URL;
    std::array<DependencyDocument, 5> dependencies{{
        {.name = "zydis", .version = BME_ZYDIS_VERSION, .revision = BME_ZYDIS_REVISION},
        {.name = "bddisasm", .version = BME_BDDISASM_VERSION, .revision = BME_BDDISASM_REVISION},
        {.name = "capstone", .version = BME_CAPSTONE_VERSION, .revision = BME_CAPSTONE_REVISION},
        {.name = "xed", .version = BME_XED_VERSION, .revision = BME_XED_REVISION},
        {.name = "glaze", .version = BME_GLAZE_VERSION, .revision = BME_GLAZE_REVISION},
    }};

    struct glaze
    {
        using T = ProducerDocument;

        static constexpr auto value = glz::object(
            "name", &T::name, "version", &T::version, "git_hash", &T::git_hash, "repository", &T::repository, "dependencies", &T::dependencies
        );
    };
};

struct GPRDocument
{
    std::string rax{};
    std::string rbx{};
    std::string rcx{};
    std::string rdx{};
    std::string rsi{};
    std::string rdi{};
    std::string rbp{};
    std::string rsp{};
    std::string r8{};
    std::string r9{};
    std::string r10{};
    std::string r11{};
    std::string r12{};
    std::string r13{};
    std::string r14{};
    std::string r15{};
    std::string rip{};
    std::string rflags{};

    struct glaze
    {
        using T = GPRDocument;

        static constexpr auto value = glz::object(
            "rax",
            &T::rax,
            "rbx",
            &T::rbx,
            "rcx",
            &T::rcx,
            "rdx",
            &T::rdx,
            "rsi",
            &T::rsi,
            "rdi",
            &T::rdi,
            "rbp",
            &T::rbp,
            "rsp",
            &T::rsp,
            "r8",
            &T::r8,
            "r9",
            &T::r9,
            "r10",
            &T::r10,
            "r11",
            &T::r11,
            "r12",
            &T::r12,
            "r13",
            &T::r13,
            "r14",
            &T::r14,
            "r15",
            &T::r15,
            "rip",
            &T::rip,
            "rflags",
            &T::rflags
        );
    };
};

struct SSEDocument
{
    std::array<std::string, 16> xmm{};
    std::string                 mxcsr{};

    struct glaze;
};

struct SSEDocument::glaze
{
    using T = SSEDocument;

    static constexpr auto value = glz::object(
        "xmm0",
        [](const T &self) -> const std::string & { return self.xmm[0]; },
        "xmm1",
        [](const T &self) -> const std::string & { return self.xmm[1]; },
        "xmm2",
        [](const T &self) -> const std::string & { return self.xmm[2]; },
        "xmm3",
        [](const T &self) -> const std::string & { return self.xmm[3]; },
        "xmm4",
        [](const T &self) -> const std::string & { return self.xmm[4]; },
        "xmm5",
        [](const T &self) -> const std::string & { return self.xmm[5]; },
        "xmm6",
        [](const T &self) -> const std::string & { return self.xmm[6]; },
        "xmm7",
        [](const T &self) -> const std::string & { return self.xmm[7]; },
        "xmm8",
        [](const T &self) -> const std::string & { return self.xmm[8]; },
        "xmm9",
        [](const T &self) -> const std::string & { return self.xmm[9]; },
        "xmm10",
        [](const T &self) -> const std::string & { return self.xmm[10]; },
        "xmm11",
        [](const T &self) -> const std::string & { return self.xmm[11]; },
        "xmm12",
        [](const T &self) -> const std::string & { return self.xmm[12]; },
        "xmm13",
        [](const T &self) -> const std::string & { return self.xmm[13]; },
        "xmm14",
        [](const T &self) -> const std::string & { return self.xmm[14]; },
        "xmm15",
        [](const T &self) -> const std::string & { return self.xmm[15]; },
        "mxcsr",
        &T::mxcsr
    );
};

struct X87Document
{
    std::array<std::string, 8> st{};
    std::string                control_word{};
    std::string                status_word{};
    std::string                tag_word_abridged{};

    struct glaze;
};

struct X87Document::glaze
{
    using T = X87Document;

    static constexpr auto value = glz::object(
        "st0",
        [](const T &self) -> const std::string & { return self.st[0]; },
        "st1",
        [](const T &self) -> const std::string & { return self.st[1]; },
        "st2",
        [](const T &self) -> const std::string & { return self.st[2]; },
        "st3",
        [](const T &self) -> const std::string & { return self.st[3]; },
        "st4",
        [](const T &self) -> const std::string & { return self.st[4]; },
        "st5",
        [](const T &self) -> const std::string & { return self.st[5]; },
        "st6",
        [](const T &self) -> const std::string & { return self.st[6]; },
        "st7",
        [](const T &self) -> const std::string & { return self.st[7]; },
        "control_word",
        &T::control_word,
        "status_word",
        &T::status_word,
        "tag_word_abridged",
        &T::tag_word_abridged
    );
};

struct RegistersDocument
{
    GPRDocument gpr{};
    SSEDocument sse{};
    X87Document x87{};

    struct glaze
    {
        using T = RegistersDocument;

        static constexpr auto value = glz::object("gpr", &T::gpr, "sse", &T::sse, "x87", &T::x87);
    };
};

RegistersDocument make_registers_document(const Registers &registers)
{
    RegistersDocument result{};
    result.gpr = {
        .rax    = format_hex<16>(registers[Reg::RAX]),
        .rbx    = format_hex<16>(registers[Reg::RBX]),
        .rcx    = format_hex<16>(registers[Reg::RCX]),
        .rdx    = format_hex<16>(registers[Reg::RDX]),
        .rsi    = format_hex<16>(registers[Reg::RSI]),
        .rdi    = format_hex<16>(registers[Reg::RDI]),
        .rbp    = format_hex<16>(registers[Reg::RBP]),
        .rsp    = format_hex<16>(registers[Reg::RSP]),
        .r8     = format_hex<16>(registers[Reg::R8]),
        .r9     = format_hex<16>(registers[Reg::R9]),
        .r10    = format_hex<16>(registers[Reg::R10]),
        .r11    = format_hex<16>(registers[Reg::R11]),
        .r12    = format_hex<16>(registers[Reg::R12]),
        .r13    = format_hex<16>(registers[Reg::R13]),
        .r14    = format_hex<16>(registers[Reg::R14]),
        .r15    = format_hex<16>(registers[Reg::R15]),
        .rip    = format_hex<16>(registers[Reg::RIP]),
        .rflags = format_hex<16>(registers[Reg::RFLAGS]),
    };

    for (std::size_t i{}; i < result.sse.xmm.size(); ++i)
    {
        result.sse.xmm[i] = format_xmm(registers.xmm[i]);
    }

    result.sse.mxcsr = format_hex<8>(registers.mxcsr);

    for (std::size_t i{}; i < result.x87.st.size(); ++i)
    {
        result.x87.st[i] = format_st(registers.st[i]);
    }

    result.x87.control_word      = format_hex<4>(registers.fpu_control_word);
    result.x87.status_word       = format_hex<4>(registers.fpu_status_word);
    result.x87.tag_word_abridged = format_hex<2>(registers.fpu_tag_word_abridged);

    return result;
}

struct CPUIDRecordDocument
{
    std::string leaf{};
    std::string subleaf{};
    std::string eax{};
    std::string ebx{};
    std::string ecx{};
    std::string edx{};

    struct glaze
    {
        using T = CPUIDRecordDocument;

        static constexpr auto value =
            glz::object("leaf", &T::leaf, "subleaf", &T::subleaf, "eax", &T::eax, "ebx", &T::ebx, "ecx", &T::ecx, "edx", &T::edx);
    };
};

CPUIDRecordDocument make_cpuid_record_document(const CPUIDRecord &record)
{
    return {
        .leaf    = format_hex<8>(record.leaf),
        .subleaf = format_hex<8>(record.subleaf),
        .eax     = format_hex<8>(record.eax),
        .ebx     = format_hex<8>(record.ebx),
        .ecx     = format_hex<8>(record.ecx),
        .edx     = format_hex<8>(record.edx),
    };
}

struct CPUView
{
    const CPUFingerprint *fingerprint{};

    struct glaze;
};

struct CPUView::glaze
{
    using T = CPUView;

    static constexpr auto value = glz::object(
        "capture_scope",
        [](const T &self) { return capture_scope_name(self.fingerprint->capture_scope); },
        "execution_cpu_attributed",
        [](const T &self) { return self.fingerprint->execution_cpu_attributed; },
        "vendor",
        [](const T &self) -> const std::string & { return self.fingerprint->vendor; },
        "brand",
        [](const T &self) { return nullable_text(self.fingerprint->brand); },
        "family",
        [](const T &self) { return self.fingerprint->family; },
        "model",
        [](const T &self) { return self.fingerprint->model; },
        "stepping",
        [](const T &self) { return self.fingerprint->stepping; },
        "maximum_basic_leaf",
        [](const T &self) { return format_hex<8>(self.fingerprint->maximum_basic_leaf); },
        "maximum_extended_leaf",
        [](const T &self) { return format_hex<8>(self.fingerprint->maximum_extended_leaf); },
        "physical_address_width",
        [](const T &self) { return self.fingerprint->physical_address_width; },
        "linear_address_width",
        [](const T &self) { return self.fingerprint->linear_address_width; },
        "hypervisor_present",
        [](const T &self) { return self.fingerprint->hypervisor_present; },
        "hypervisor_vendor",
        [](const T &self) { return nullable_text(self.fingerprint->hypervisor_vendor); },
        "hypervisor_interface",
        [](const T &self) { return nullable_text(self.fingerprint->hypervisor_interface); },
        "cpuid_features",
        [](const T &self) -> const std::vector<std::string> & { return self.fingerprint->cpuid_features; },
        "xcr0_supported",
        [](const T &self) -> std::optional<std::string>
        {
            if (!self.fingerprint->xcr0_supported)
            {
                return {};
            }

            return format_hex<16>(*self.fingerprint->xcr0_supported);
        },
        "xss_supported",
        [](const T &self) -> std::optional<std::string>
        {
            if (!self.fingerprint->xss_supported)
            {
                return {};
            }

            return format_hex<16>(*self.fingerprint->xss_supported);
        },
        "xcr0",
        [](const T &self) -> std::optional<std::string>
        {
            if (!self.fingerprint->xcr0)
            {
                return {};
            }

            return format_hex<16>(*self.fingerprint->xcr0);
        },
        "enabled_xstate",
        [](const T &self) -> const std::vector<std::string> & { return self.fingerprint->enabled_xstate; },
        "raw_cpuid",
        [](const T &self)
        { return self.fingerprint->raw_cpuid | std::views::transform([](const CPUIDRecord &record) { return make_cpuid_record_document(record); }); }
    );
};

struct HostView
{
    const CPUFingerprint *fingerprint{};

    struct glaze;
};

struct HostView::glaze
{
    using T                     = HostView;
    static constexpr auto value = glz::object(
        "os",
        [](const T &)
        {
#if BME_OS_WINDOWS
            return std::string_view{"windows"};
#elif BME_OS_LINUX
            return std::string_view{"linux"};
#else
#error "Unsupported platform"
#endif
        },
        "architecture",
        [](const T &) { return std::string_view{"x86_64"}; },
        "cpu",
        [](const T &self) { return CPUView{self.fingerprint}; }
    );
};

struct RequestDocument
{
    std::string       input_bytes{};
    std::string_view  backend{};
    std::string_view  syntax{};
    std::size_t       effective_max_steps{};
    bool              seed_data_pointers{};
    RegistersDocument seed{};

    struct glaze
    {
        using T = RequestDocument;

        static constexpr auto value = glz::object(
            "input_bytes",
            &T::input_bytes,
            "backend",
            &T::backend,
            "syntax",
            &T::syntax,
            "effective_max_steps",
            &T::effective_max_steps,
            "seed_data_pointers",
            &T::seed_data_pointers,
            "seed",
            &T::seed
        );
    };
};

RequestDocument make_request_document(const Trace &trace)
{
    return {
        .input_bytes         = format_bytes(trace.code),
        .backend             = backend_name(trace.requested_backend),
        .syntax              = syntax_name(trace.requested_syntax),
        .effective_max_steps = trace.effective_max_steps,
        .seed_data_pointers  = trace.seed_data_pointers,
        .seed                = make_registers_document(trace.requested_seed),
    };
}

struct ExecutionEventDocument
{
    std::size_t                index{};
    std::string_view           classification{};
    std::string                rip{};
    std::optional<std::size_t> offset{};
    RegistersDocument          registers{};

    struct glaze
    {
        using T = ExecutionEventDocument;

        static constexpr auto value =
            glz::object("index", &T::index, "classification", &T::classification, "rip", &T::rip, "offset", &T::offset, "registers", &T::registers);
    };
};

ExecutionEventDocument make_execution_event_document(const Trace &trace, std::size_t index)
{
    auto &event = trace.execution_events[index];

    return {
        .index          = index,
        .classification = event_kind_name(event.kind),
        .rip            = format_hex<16>(event.rip),
        .offset         = relative_offset(trace, event.rip),
        .registers      = make_registers_document(event.registers),
    };
}

struct ExecutionView
{
    const Trace *trace{};

    struct glaze;
};

struct ExecutionView::glaze
{
    using T = ExecutionView;

    static constexpr auto value = glz::object(
        "seed",
        [](const T &self) { return make_registers_document(self.trace->seed); },
        "events",
        [](const T &self)
        {
            return std::views::iota((std::size_t)0, self.trace->execution_events.size())
                 | std::views::transform([trace = self.trace](std::size_t index) { return make_execution_event_document(*trace, index); });
        },
        "outcome",
        [](const T &self) { return outcome_name(self.trace->outcome); },
        "message",
        [](const T &self) -> const std::string & { return self.trace->message; },
        "instrumentation_detected",
        [](const T &self) { return self.trace->instrumentation_detected; },
        "stop_reason",
        [](const T &self) -> const std::string & { return self.trace->stop_reason; },
        "stop_address",
        [](const T &self) -> std::optional<std::string>
        {
            if (self.trace->stop_address == 0)
            {
                return {};
            }

            return format_hex<16>(self.trace->stop_address);
        },
        "fault_memory_address",
        [](const T &self) -> std::optional<std::string>
        {
            if (!self.trace->fault_memory_address)
            {
                return {};
            }

            return format_hex<16>(*self.trace->fault_memory_address);
        },
        "fault_access",
        [](const T &self) { return fault_access_name(self.trace->fault_access); }
    );
};

struct HistoryRowDocument
{
    std::string                address{};
    std::size_t                offset{};
    std::size_t                length{};
    std::string                bytes{};
    std::string_view           text{};
    std::string_view           classification{};
    std::optional<std::size_t> execution_event{};

    struct glaze
    {
        using T = HistoryRowDocument;

        static constexpr auto value = glz::object(
            "address",
            &T::address,
            "offset",
            &T::offset,
            "length",
            &T::length,
            "bytes",
            &T::bytes,
            "text",
            &T::text,
            "classification",
            &T::classification,
            "execution_event",
            &T::execution_event
        );
    };
};

HistoryRowDocument make_history_row_document(const Trace &trace, const HistoryRow &row)
{
    return {
        .address         = format_hex<16>(row.rip),
        .offset          = row.offset,
        .length          = row.length,
        .bytes           = format_bytes(std::span{trace.code}.subspan(row.offset, row.length)),
        .text            = row.text,
        .classification  = history_kind_name(row.kind),
        .execution_event = row.execution_event_index,
    };
}

struct DecoderDocument
{
    const Trace            *trace{};
    DisasmBackend           backend{};
    DisasmSyntax            requested_syntax{};
    DisasmSyntax            rendered_syntax{};
    std::vector<HistoryRow> rows{};

    struct glaze;
};

struct DecoderDocument::glaze
{
    using T                     = DecoderDocument;
    static constexpr auto value = glz::object(
        "backend",
        [](const T &self) { return backend_name(self.backend); },
        "version",
        [](const T &self) { return backend_version(self.backend); },
        "revision",
        [](const T &self) { return backend_revision(self.backend); },
        "requested_syntax",
        [](const T &self) { return syntax_name(self.requested_syntax); },
        "effective_syntax",
        [](const T &self) { return syntax_name(self.rendered_syntax); },
        "rows",
        [](const T &self)
        { return self.rows | std::views::transform([trace = self.trace](const HistoryRow &row) { return make_history_row_document(*trace, row); }); }
    );
};

DecoderDocument make_decoder_document(const Trace &trace, DisasmBackend backend)
{
    auto rendered_syntax = effective_syntax(backend, trace.requested_syntax);

    return {
        .trace            = &trace,
        .backend          = backend,
        .requested_syntax = trace.requested_syntax,
        .rendered_syntax  = rendered_syntax,
        .rows             = build_history(trace, backend, rendered_syntax),
    };
}

struct TraceDocumentView
{
    const Trace *trace{};

    struct glaze;
};

struct TraceDocumentView::glaze
{
    using T = TraceDocumentView;

    static constexpr auto value = glz::object(
        "schema_version",
        [](const T &) { return TRACE_SCHEMA_VERSION; },
        "producer",
        [](const T &) { return ProducerDocument{}; },
        "host",
        [](const T &self) { return HostView{self.trace->cpu_fingerprint.get()}; },
        "request",
        [](const T &self) { return make_request_document(*self.trace); },
        "execution",
        [](const T &self) { return ExecutionView{self.trace}; },
        "decoders",
        [](const T &self)
        {
            return SERIALIZED_BACKENDS
                 | std::views::transform([trace = self.trace](DisasmBackend backend) { return make_decoder_document(*trace, backend); });
        }
    );
};

} // namespace

Result<void, std::string> write_trace_json(const Trace &trace, std::ostream &output, bool pretty)
{
    if (trace.cpu_fingerprint == nullptr)
    {
        return std::unexpected{std::string{"Trace has no CPU fingerprint."}};
    }

    try
    {
        TraceDocumentView     document{&trace};
        glz::ostream_buffer<> buffer{output};
        auto error = pretty ? glz::write<PRETTY_JSON_OPTIONS>(document, buffer) : glz::write<glz::opts{.skip_null_members = false}>(document, buffer);
        if (error)
        {
            return std::unexpected{std::string{"Trace JSON serialization failed. "} + glz::format_error(error)};
        }

        if (!buffer.good())
        {
            return std::unexpected{std::string{"Trace JSON stream write failed."}};
        }

        output.put('\n');
        output.flush();

        if (!output.good())
        {
            return std::unexpected{std::string{"Trace JSON stream write failed."}};
        }
    }
    catch (const std::ios_base::failure &exception)
    {
        return std::unexpected{std::string{"Trace JSON stream write failed. "} + exception.what()};
    }
    catch (const std::exception &exception)
    {
        return std::unexpected{std::string{"Trace JSON serialization failed. "} + exception.what()};
    }

    return {};
}

} // namespace bme
