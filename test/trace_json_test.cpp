#include "bme_core.hpp"
#include "bme_version.hpp"

#include <glaze/json/generic.hpp>
#include <glaze/json/read.hpp>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>

using namespace bme;

namespace
{

using JsonValue = glz::generic_sorted_u64;

auto parse_json(std::string_view json)
{
    auto result = glz::read_json<JsonValue>(json);
    if (!result)
    {
        throw std::runtime_error(glz::format_error(result.error(), json));
    }

    return std::move(*result);
}

auto dump_json(const JsonValue &value)
{
    auto result = value.dump();
    if (!result)
    {
        throw std::runtime_error(glz::format_error(result.error()));
    }

    return std::move(*result);
}

class FailingStreamBuffer final : public std::streambuf
{
protected:
    std::streamsize xsputn(const char *, std::streamsize) override { return 0; }
    int_type        overflow(int_type) override { return traits_type::eof(); }
};

class FlushFailingStreamBuffer final : public std::stringbuf
{
protected:
    int sync() override { return -1; }
};

std::shared_ptr<const CPUFingerprint> make_cpu_fingerprint()
{
    auto result                      = std::make_shared<CPUFingerprint>();
    result->capture_scope            = CPUCaptureScope::HostVisibleProcess;
    result->vendor                   = "AuthenticAMD";
    result->brand                    = "Synthetic CPU";
    result->family                   = 0x1A;
    result->model                    = 0x44;
    result->stepping                 = 0;
    result->maximum_basic_leaf       = 0xDu;
    result->maximum_extended_leaf    = 0x8000'0008u;
    result->physical_address_width   = 52;
    result->linear_address_width     = 57;
    result->hypervisor_present       = true;
    result->hypervisor_vendor        = "TestVendor";
    result->hypervisor_interface     = "Hv#1";
    result->cpuid_features           = {"SSE2", "AVX"};
    result->xcr0_supported           = 0xE7;
    result->xss_supported            = std::nullopt;
    result->xcr0                     = 0x7;
    result->enabled_xstate           = {"x87", "SSE", "AVX"};
    result->execution_cpu_attributed = false;
    result->raw_cpuid.emplace_back(
        CPUIDRecord{
            .leaf    = 1,
            .subleaf = 0,
            .eax     = 0x00A4'0F00,
            .ebx     = 0x1122'3344,
            .ecx     = 0x5566'7788,
            .edx     = 0x99AA'BBCC,
        }
    );

    return result;
}

auto make_registers()
{
    Registers result{};
    result[Reg::RAX]             = 0x0123'4567'89AB'CDEF;
    result[Reg::RBX]             = 0xFEDC'BA98'7654'3210;
    result[Reg::R15]             = 0x0F0E'0D0C'0B0A'0908;
    result[Reg::RIP]             = 0x0000'0000'0000'1000;
    result[Reg::RFLAGS]          = 0x0000'0000'0000'0246;
    result.xmm[0]                = {0x0011'2233'4455'6677, 0x8899'AABB'CCDD'EEFF};
    result.xmm[15]               = {0x1020'3040'5060'7080, 0x90A0'B0C0'D0E0'F000};
    result.mxcsr                 = 0xA1B2'C3D4;
    result.st[0]                 = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    result.st[7]                 = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0};
    result.fpu_control_word      = 0x1234;
    result.fpu_status_word       = 0x5678;
    result.fpu_tag_word_abridged = 0x9A;

    return result;
}

auto make_trace()
{
    Trace result{};
    result.cpu_fingerprint     = make_cpu_fingerprint();
    result.code                = {0x90};
    result.requested_seed      = make_registers();
    result.requested_backend   = DisasmBackend::Xed;
    result.requested_syntax    = DisasmSyntax::ATT;
    result.effective_max_steps = 123;
    result.seed_data_pointers  = false;
    result.seed                = make_registers();
    result.seed[Reg::RAX]      = 0x1111'1111'1111'1111;

    auto completed_registers      = make_registers();
    completed_registers[Reg::RAX] = 0x2222'2222'2222'2222;

    result.execution_events.emplace_back(
        ExecutionEvent{
            .rip       = result.seed[Reg::RIP],
            .registers = completed_registers,
            .kind      = ExecutionEventKind::Completed,
        }
    );

    auto faulted_registers      = make_registers();
    faulted_registers[Reg::RAX] = 0x3333'3333'3333'3333;
    faulted_registers[Reg::RIP] = 0x2000;

    result.execution_events.emplace_back(
        ExecutionEvent{
            .rip       = 0x2000,
            .registers = faulted_registers,
            .kind      = ExecutionEventKind::Faulted,
        }
    );

    result.outcome              = Outcome::Faulted;
    result.message              = "Synthetic \"fault\"\nmessage";
    result.stop_reason          = "Synthetic fault";
    result.stop_address         = 0x2000;
    result.fault_memory_address = 0xDEAD;
    result.fault_access         = FaultAccess::Write;

    return result;
}

auto serialize(const Trace &trace, bool pretty = false)
{
    std::ostringstream output{};
    auto               result = write_trace_json(trace, output, pretty);
    if (!result)
    {
        throw std::runtime_error(result.error());
    }

    return output.str();
}

struct ExpectedDependency
{
    std::string_view name{};
    std::string_view version{};
    std::string_view revision{};
};

constexpr std::array EXPECTED_DEPENDENCIES{
    ExpectedDependency{.name = "zydis", .version = BME_ZYDIS_VERSION, .revision = BME_ZYDIS_REVISION},
    ExpectedDependency{.name = "bddisasm", .version = BME_BDDISASM_VERSION, .revision = BME_BDDISASM_REVISION},
    ExpectedDependency{.name = "capstone", .version = BME_CAPSTONE_VERSION, .revision = BME_CAPSTONE_REVISION},
    ExpectedDependency{.name = "xed", .version = BME_XED_VERSION, .revision = BME_XED_REVISION},
    ExpectedDependency{.name = "glaze", .version = BME_GLAZE_VERSION, .revision = BME_GLAZE_REVISION},
};

const auto &dependency(const JsonValue::array_t &dependencies, std::string_view name)
{
    for (auto &&entry : dependencies)
    {
        if (entry.at("name").get_string() == name)
        {
            return entry;
        }
    }

    throw std::runtime_error("Missing dependency metadata");
}

TEST(TraceJson, EmitsVersionedPortableDocument)
{
    auto json = serialize(make_trace());
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(json.back(), '\n');
    EXPECT_EQ(json.find('\n'), json.size() - 1);

    auto document = parse_json(json);
    ASSERT_EQ(document.get_object().size(), 6u);
    EXPECT_EQ(document.at("schema_version").get<std::uint64_t>(), 1u);

    auto &producer = document.at("producer");
    EXPECT_EQ(producer.at("name").get_string(), "bme");
    EXPECT_EQ(producer.at("version").get_string(), BME_GIT_TAG);
    EXPECT_EQ(producer.at("git_hash").get_string(), BME_GIT_HASH);
    EXPECT_EQ(producer.at("repository").get_string(), BME_GIT_URL);

    auto &dependencies = producer.at("dependencies").get_array();
    ASSERT_EQ(dependencies.size(), EXPECTED_DEPENDENCIES.size());

    for (auto &&expected : EXPECTED_DEPENDENCIES)
    {
        auto &actual = dependency(dependencies, expected.name);
        EXPECT_EQ(actual.at("version").get_string(), expected.version);
        EXPECT_EQ(actual.at("revision").get_string(), expected.revision);
    }

    auto &host = document.at("host");

#if BME_OS_WINDOWS
    EXPECT_EQ(host.at("os").get_string(), "windows");
#elif BME_OS_LINUX
    EXPECT_EQ(host.at("os").get_string(), "linux");
#endif

    EXPECT_EQ(host.at("architecture").get_string(), "x86_64");

    auto &cpu = host.at("cpu");
    EXPECT_EQ(cpu.at("capture_scope").get_string(), "host_visible_process");
    EXPECT_FALSE(cpu.at("execution_cpu_attributed").get_boolean());
    EXPECT_EQ(cpu.at("vendor").get_string(), "AuthenticAMD");
    EXPECT_EQ(cpu.at("brand").get_string(), "Synthetic CPU");
    EXPECT_EQ(cpu.at("family").get<std::uint64_t>(), 0x1Au);
    EXPECT_EQ(cpu.at("model").get<std::uint64_t>(), 0x44u);
    EXPECT_EQ(cpu.at("stepping").get<std::uint64_t>(), 0u);
    EXPECT_EQ(cpu.at("maximum_basic_leaf").get_string(), "0x0000000D");
    EXPECT_EQ(cpu.at("maximum_extended_leaf").get_string(), "0x80000008");
    EXPECT_EQ(cpu.at("physical_address_width").get<std::uint64_t>(), 52u);
    EXPECT_EQ(cpu.at("linear_address_width").get<std::uint64_t>(), 57u);
    EXPECT_TRUE(cpu.at("hypervisor_present").get_boolean());
    EXPECT_EQ(cpu.at("hypervisor_vendor").get_string(), "TestVendor");
    EXPECT_EQ(cpu.at("hypervisor_interface").get_string(), "Hv#1");
    EXPECT_EQ(cpu.at("xcr0_supported").get_string(), "0x00000000000000E7");
    EXPECT_TRUE(cpu.at("xss_supported").is_null());
    EXPECT_EQ(cpu.at("xcr0").get_string(), "0x0000000000000007");

    auto &cpuid_features = cpu.at("cpuid_features").get_array();
    ASSERT_EQ(cpuid_features.size(), 2u);
    EXPECT_EQ(cpuid_features[0].get_string(), "SSE2");
    EXPECT_EQ(cpuid_features[1].get_string(), "AVX");

    auto &enabled_xstate = cpu.at("enabled_xstate").get_array();
    ASSERT_EQ(enabled_xstate.size(), 3u);
    EXPECT_EQ(enabled_xstate[0].get_string(), "x87");
    EXPECT_EQ(enabled_xstate[1].get_string(), "SSE");
    EXPECT_EQ(enabled_xstate[2].get_string(), "AVX");

    auto &raw_cpuid = cpu.at("raw_cpuid").get_array();
    ASSERT_EQ(raw_cpuid.size(), 1u);
    EXPECT_EQ(raw_cpuid[0].at("leaf").get_string(), "0x00000001");
    EXPECT_EQ(raw_cpuid[0].at("subleaf").get_string(), "0x00000000");
    EXPECT_EQ(raw_cpuid[0].at("eax").get_string(), "0x00A40F00");
    EXPECT_EQ(raw_cpuid[0].at("ebx").get_string(), "0x11223344");
    EXPECT_EQ(raw_cpuid[0].at("ecx").get_string(), "0x55667788");
    EXPECT_EQ(raw_cpuid[0].at("edx").get_string(), "0x99AABBCC");

    auto &request = document.at("request");
    EXPECT_EQ(request.at("input_bytes").get_string(), "90");
    EXPECT_EQ(request.at("backend").get_string(), "xed");
    EXPECT_EQ(request.at("syntax").get_string(), "att");
    EXPECT_EQ(request.at("effective_max_steps").get<std::uint64_t>(), 123u);
    EXPECT_FALSE(request.at("seed_data_pointers").get_boolean());

    auto &requested_seed = request.at("seed");
    ASSERT_EQ(requested_seed.at("gpr").get_object().size(), 18u);
    ASSERT_EQ(requested_seed.at("sse").get_object().size(), 17u);
    ASSERT_EQ(requested_seed.at("x87").get_object().size(), 11u);
    EXPECT_EQ(requested_seed.at("gpr").at("rax").get_string(), "0x0123456789ABCDEF");
    EXPECT_EQ(requested_seed.at("gpr").at("rbx").get_string(), "0xFEDCBA9876543210");
    EXPECT_EQ(requested_seed.at("gpr").at("r15").get_string(), "0x0F0E0D0C0B0A0908");
    EXPECT_EQ(requested_seed.at("gpr").at("rflags").get_string(), "0x0000000000000246");
    EXPECT_EQ(requested_seed.at("sse").at("xmm0").get_string(), "0x8899AABBCCDDEEFF0011223344556677");
    EXPECT_EQ(requested_seed.at("sse").at("xmm15").get_string(), "0x90A0B0C0D0E0F0001020304050607080");
    EXPECT_EQ(requested_seed.at("sse").at("mxcsr").get_string(), "0xA1B2C3D4");
    EXPECT_EQ(requested_seed.at("x87").at("st0").get_string(), "0x99887766554433221100");
    EXPECT_EQ(requested_seed.at("x87").at("st7").get_string(), "0xA0908070605040302010");
    EXPECT_EQ(requested_seed.at("x87").at("control_word").get_string(), "0x1234");
    EXPECT_EQ(requested_seed.at("x87").at("status_word").get_string(), "0x5678");
    EXPECT_EQ(requested_seed.at("x87").at("tag_word_abridged").get_string(), "0x9A");

    auto &execution = document.at("execution");
    EXPECT_FALSE(execution.at("instrumentation_detected").get_boolean());
    EXPECT_EQ(execution.at("outcome").get_string(), "faulted");
    EXPECT_EQ(execution.at("message").get_string(), "Synthetic \"fault\"\nmessage");
    EXPECT_EQ(execution.at("stop_reason").get_string(), "Synthetic fault");
    EXPECT_EQ(execution.at("stop_address").get_string(), "0x0000000000002000");
    EXPECT_EQ(execution.at("fault_memory_address").get_string(), "0x000000000000DEAD");
    EXPECT_EQ(execution.at("fault_access").get_string(), "write");
    EXPECT_EQ(execution.at("seed").at("gpr").at("rax").get_string(), "0x1111111111111111");

    auto &events = execution.at("events").get_array();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].at("index").get<std::uint64_t>(), 0u);
    EXPECT_EQ(events[0].at("rip").get_string(), "0x0000000000001000");
    EXPECT_EQ(events[0].at("classification").get_string(), "completed");
    EXPECT_EQ(events[0].at("offset").get<std::uint64_t>(), 0u);
    EXPECT_EQ(events[0].at("registers").at("gpr").at("rax").get_string(), "0x2222222222222222");
    EXPECT_EQ(events[1].at("index").get<std::uint64_t>(), 1u);
    EXPECT_EQ(events[1].at("classification").get_string(), "faulted");
    EXPECT_EQ(events[1].at("rip").get_string(), "0x0000000000002000");
    EXPECT_TRUE(events[1].at("offset").is_null());
    EXPECT_EQ(events[1].at("registers").at("gpr").at("rax").get_string(), "0x3333333333333333");
    EXPECT_EQ(events[1].at("registers").at("gpr").at("rip").get_string(), "0x0000000000002000");

    auto &decoders = document.at("decoders").get_array();
    ASSERT_EQ(decoders.size(), 4u);
    EXPECT_EQ(decoders[0].at("backend").get_string(), "zydis");
    EXPECT_EQ(decoders[1].at("backend").get_string(), "bddisasm");
    EXPECT_EQ(decoders[2].at("backend").get_string(), "capstone");
    EXPECT_EQ(decoders[3].at("backend").get_string(), "xed");

    for (auto &&decoder : decoders)
    {
        EXPECT_EQ(decoder.at("requested_syntax").get_string(), "att");

        auto &metadata = dependency(dependencies, decoder.at("backend").get_string());
        EXPECT_EQ(decoder.at("version").get_string(), metadata.at("version").get_string());
        EXPECT_EQ(decoder.at("revision").get_string(), metadata.at("revision").get_string());

        auto &rows = decoder.at("rows").get_array();
        ASSERT_EQ(rows.size(), 1u);

        auto &row = rows.front();
        EXPECT_EQ(row.get_object().size(), 7u);
        EXPECT_EQ(row.at("address").get_string(), "0x0000000000001000");
        EXPECT_EQ(row.at("offset").get<std::uint64_t>(), 0u);
        EXPECT_EQ(row.at("length").get<std::uint64_t>(), 1u);
        EXPECT_EQ(row.at("bytes").get_string(), "90");
        EXPECT_EQ(row.at("classification").get_string(), "reached");
        EXPECT_EQ(row.at("execution_event").get<std::uint64_t>(), 0u);
    }

    EXPECT_EQ(decoders[0].at("effective_syntax").get_string(), "att");
    EXPECT_EQ(decoders[1].at("effective_syntax").get_string(), "intel");
    EXPECT_EQ(decoders[2].at("effective_syntax").get_string(), "att");
    EXPECT_EQ(decoders[3].at("effective_syntax").get_string(), "att");
}

TEST(TraceJson, SerializesInRangeFaultHistory)
{
    auto trace = make_trace();

    trace.execution_events.clear();

    trace.execution_events.emplace_back(
        ExecutionEvent{
            .rip       = trace.seed[Reg::RIP],
            .registers = make_registers(),
            .kind      = ExecutionEventKind::Faulted,
        }
    );

    trace.stop_address = trace.seed[Reg::RIP];

    auto  document  = parse_json(serialize(trace));
    auto &execution = document.at("execution");
    EXPECT_EQ(execution.at("stop_address").get_string(), "0x0000000000001000");

    auto &events = execution.at("events").get_array();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].at("classification").get_string(), "faulted");
    EXPECT_EQ(events[0].at("offset").get<std::uint64_t>(), 0u);

    for (auto &&decoder : document.at("decoders").get_array())
    {
        auto &rows = decoder.at("rows").get_array();
        ASSERT_EQ(rows.size(), 1u);

        auto &row = rows.front();
        EXPECT_EQ(row.at("address").get_string(), "0x0000000000001000");
        EXPECT_EQ(row.at("offset").get<std::uint64_t>(), 0u);
        EXPECT_EQ(row.at("length").get<std::uint64_t>(), 1u);
        EXPECT_EQ(row.at("bytes").get_string(), "90");
        EXPECT_EQ(row.at("classification").get_string(), "faulted");
        EXPECT_EQ(row.at("execution_event").get<std::uint64_t>(), 0u);
    }
}

TEST(TraceJson, PrettyDocumentMatchesCompactDocument)
{
    auto  trace       = make_trace();
    auto  compact     = parse_json(serialize(trace));
    auto  pretty_json = serialize(trace, true);
    auto  pretty      = parse_json(pretty_json);
    auto &pretty_cpu  = pretty.at("host").at("cpu");
    EXPECT_TRUE(pretty_json.starts_with("{\n  \"schema_version\": 1,"));
    EXPECT_TRUE(pretty_cpu.contains("xss_supported"));
    EXPECT_TRUE(pretty_cpu.at("xss_supported").is_null());
    EXPECT_EQ(dump_json(pretty), dump_json(compact));
}

TEST(TraceJson, RejectsMissingFingerprint)
{
    Trace              trace{};
    std::ostringstream output{};
    auto               result = write_trace_json(trace, output);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().find("CPU fingerprint"), std::string::npos);
    EXPECT_TRUE(output.str().empty());
}

TEST(TraceJson, ReportsStreamFailure)
{
    auto                trace = make_trace();
    FailingStreamBuffer buffer{};
    std::ostream        output{&buffer};
    auto                result = write_trace_json(trace, output);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().find("stream write failed"), std::string::npos);
}

TEST(TraceJson, ReportsFlushFailure)
{
    auto                     trace = make_trace();
    FlushFailingStreamBuffer buffer{};
    std::ostream             output{&buffer};
    auto                     result = write_trace_json(trace, output);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().find("stream write failed"), std::string::npos);
}

} // namespace
