#include "bme_core.hpp"
#include "bme_version.hpp"

#include <gtest/gtest.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace bme;

namespace
{

struct JsonValue
{
    using Array  = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;
    using Value  = std::variant<std::nullptr_t, bool, std::uint64_t, std::string, Array, Object>;

    bool        is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(value); }
    bool        boolean() const { return std::get<bool>(value); }
    auto        number() const { return std::get<std::uint64_t>(value); }
    const auto &string() const { return std::get<std::string>(value); }
    const auto &array() const { return std::get<Array>(value); }
    const auto &object() const { return std::get<Object>(value); }
    const auto &at(std::string_view key) const { return object().at(std::string{key}); }

    Value value{};
};

class JsonParser
{
public:
    explicit JsonParser(std::string_view input) : m_input(input) {}

    JsonValue parse()
    {
        skip_whitespace();

        auto result = parse_value();

        skip_whitespace();

        if (m_position != m_input.size())
        {
            fail("Trailing JSON data");
        }

        return result;
    }

private:
    [[noreturn]] void fail(std::string_view message) const
    {
        throw std::runtime_error(std::string{message} + " at byte " + std::to_string(m_position));
    }

    bool consume(char expected) noexcept
    {
        if (m_position == m_input.size() || m_input[m_position] != expected)
        {
            return false;
        }

        ++m_position;

        return true;
    }

    void expect(char expected)
    {
        if (consume(expected))
        {
            return;
        }

        fail("Unexpected JSON token");
    }

    void expect(std::string_view expected)
    {
        if (!m_input.substr(m_position).starts_with(expected))
        {
            fail("Unexpected JSON literal");
        }

        m_position += expected.size();
    }

    void skip_whitespace() noexcept
    {
        while (m_position < m_input.size())
        {
            auto character = m_input[m_position];
            if (character != ' ' && character != '\t' && character != '\r' && character != '\n')
            {
                break;
            }

            ++m_position;
        }
    }

    JsonValue parse_value()
    {
        if (m_position == m_input.size())
        {
            fail("Unexpected end of JSON");
        }

        switch (m_input[m_position])
        {
        case 'n': expect("null"); return JsonValue{.value = nullptr};
        case 't': expect("true"); return JsonValue{.value = true};
        case 'f': expect("false"); return JsonValue{.value = false};
        case '"': return JsonValue{.value = parse_string()};
        case '[': return JsonValue{.value = parse_array()};
        case '{': return JsonValue{.value = parse_object()};
        default:  return JsonValue{.value = parse_number()};
        }
    }

    std::uint32_t parse_hex_quad()
    {
        if (m_input.size() - m_position < 4)
        {
            fail("Incomplete JSON Unicode escape");
        }

        std::uint32_t result{};
        for (std::size_t i{}; i < 4; ++i)
        {
            auto character = m_input[m_position++];

            result <<= 4;

            if (character >= '0' && character <= '9')
            {
                result |= (std::uint32_t)(character - '0');
            }
            else if (character >= 'A' && character <= 'F')
            {
                result |= (std::uint32_t)(character - 'A' + 10);
            }
            else if (character >= 'a' && character <= 'f')
            {
                result |= (std::uint32_t)(character - 'a' + 10);
            }
            else
            {
                fail("Invalid JSON Unicode escape");
            }
        }

        return result;
    }

    void append_utf8(std::string &output, std::uint32_t code_point)
    {
        if (code_point <= 0x7F)
        {
            output += (char)code_point;
        }
        else if (code_point <= 0x7FF)
        {
            output += (char)(0xC0 | code_point >> 6);
            output += (char)(0x80 | (code_point & 0x3F));
        }
        else if (code_point <= 0xFFFF)
        {
            output += (char)(0xE0 | code_point >> 12);
            output += (char)(0x80 | (code_point >> 6 & 0x3F));
            output += (char)(0x80 | (code_point & 0x3F));
        }
        else if (code_point <= 0x10FFFF)
        {
            output += (char)(0xF0 | code_point >> 18);
            output += (char)(0x80 | (code_point >> 12 & 0x3F));
            output += (char)(0x80 | (code_point >> 6 & 0x3F));
            output += (char)(0x80 | (code_point & 0x3F));
        }
        else
        {
            fail("Invalid JSON Unicode code point");
        }
    }

    std::string parse_string()
    {
        expect('"');

        std::string result{};
        while (m_position < m_input.size())
        {
            auto character = m_input[m_position++];
            if (character == '"')
            {
                return result;
            }

            if ((std::uint8_t)character < 0x20)
            {
                fail("Unescaped JSON control character");
            }

            if (character != '\\')
            {
                result += character;

                continue;
            }

            if (m_position == m_input.size())
            {
                fail("Incomplete JSON escape");
            }

            auto escape = m_input[m_position++];
            switch (escape)
            {
            case '"':  result += '"'; break;
            case '\\': result += '\\'; break;
            case '/':  result += '/'; break;
            case 'b':  result += '\b'; break;
            case 'f':  result += '\f'; break;
            case 'n':  result += '\n'; break;
            case 'r':  result += '\r'; break;
            case 't':  result += '\t'; break;
            case 'u':
            {
                auto code_point = parse_hex_quad();
                if (code_point >= 0xD800 && code_point <= 0xDBFF)
                {
                    if (!consume('\\') || !consume('u'))
                    {
                        fail("Incomplete JSON surrogate pair");
                    }

                    auto low_surrogate = parse_hex_quad();
                    if (low_surrogate < 0xDC00 || low_surrogate > 0xDFFF)
                    {
                        fail("Invalid JSON surrogate pair");
                    }

                    code_point = 0x10000 + ((code_point - 0xD800) << 10) + low_surrogate - 0xDC00;
                }
                else if (code_point >= 0xDC00 && code_point <= 0xDFFF)
                {
                    fail("Unexpected JSON low surrogate");
                }

                append_utf8(result, code_point);

                break;
            }
            default: fail("Invalid JSON escape");
            }
        }

        fail("Unterminated JSON string");
    }

    std::uint64_t parse_number()
    {
        auto start = m_position;
        if (consume('-'))
        {
            fail("Negative JSON number not supported by trace schema");
        }

        if (consume('0'))
        {
            if (m_position < m_input.size() && m_input[m_position] >= '0' && m_input[m_position] <= '9')
            {
                fail("Leading zero in JSON number");
            }
        }
        else
        {
            auto first_digit = m_position;

            while (m_position < m_input.size() && m_input[m_position] >= '0' && m_input[m_position] <= '9')
            {
                ++m_position;
            }

            if (first_digit == m_position)
            {
                fail("Invalid JSON number");
            }
        }

        if (m_position < m_input.size() && (m_input[m_position] == '.' || m_input[m_position] == 'e' || m_input[m_position] == 'E'))
        {
            fail("Fractional JSON number not supported by trace schema");
        }

        std::uint64_t result{};
        auto [end, error] = std::from_chars(m_input.data() + start, m_input.data() + m_position, result);
        if (error != std::errc{} || end != m_input.data() + m_position)
        {
            fail("Invalid JSON integer");
        }

        return result;
    }

    JsonValue::Array parse_array()
    {
        expect('[');
        skip_whitespace();

        if (consume(']'))
        {
            return {};
        }

        JsonValue::Array result{};
        while (true)
        {
            skip_whitespace();

            result.emplace_back(parse_value());

            skip_whitespace();

            if (consume(']'))
            {
                return result;
            }

            expect(',');
        }
    }

    JsonValue::Object parse_object()
    {
        expect('{');
        skip_whitespace();

        if (consume('}'))
        {
            return {};
        }

        JsonValue::Object result{};
        while (true)
        {
            skip_whitespace();

            if (m_position == m_input.size() || m_input[m_position] != '"')
            {
                fail("JSON object key is not a string");
            }

            auto key = parse_string();

            skip_whitespace();
            expect(':');
            skip_whitespace();

            auto inserted = result.emplace(std::move(key), parse_value()).second;
            if (!inserted)
            {
                fail("Duplicate JSON object key");
            }

            skip_whitespace();

            if (consume('}'))
            {
                return result;
            }

            expect(',');
        }
    }

    std::string_view m_input{};
    std::size_t      m_position{};
};

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

Registers make_registers()
{
    Registers result{};
    result[Reg::RAX]             = 0x0123'4567'89AB'CDEF;
    result[Reg::RBX]             = 0xFEDC'BA98'7654'3210;
    result[Reg::RIP]             = 0x0000'0000'0000'1000;
    result[Reg::RFLAGS]          = 0x0000'0000'0000'0246;
    result.xmm[0]                = {0x0011'2233'4455'6677, 0x8899'AABB'CCDD'EEFF};
    result.mxcsr                 = 0xA1B2'C3D4;
    result.st[0]                 = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    result.fpu_control_word      = 0x1234;
    result.fpu_status_word       = 0x5678;
    result.fpu_tag_word_abridged = 0x9A;

    return result;
}

Trace make_trace()
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

    result.outcome      = Outcome::Faulted;
    result.message      = "Synthetic \"fault\"\nmessage";
    result.stop_reason  = "Synthetic fault";
    result.stop_address = 0x2000;

    return result;
}

std::string serialize(const Trace &trace)
{
    std::ostringstream output{};
    auto               result = write_trace_json(trace, output);
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

const JsonValue &dependency(const JsonValue::Array &dependencies, std::string_view name)
{
    for (auto &&entry : dependencies)
    {
        if (entry.at("name").string() == name)
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

    auto document = JsonParser{json}.parse();
    ASSERT_EQ(document.object().size(), 6u);
    EXPECT_EQ(document.at("schema_version").number(), 1u);

    auto &producer = document.at("producer");
    EXPECT_EQ(producer.at("name").string(), "bme");
    EXPECT_EQ(producer.at("repository").string(), "https://github.com/angelfor3v3r/bme");

    auto &dependencies = producer.at("dependencies").array();
    ASSERT_EQ(dependencies.size(), EXPECTED_DEPENDENCIES.size());
    for (auto &&expected : EXPECTED_DEPENDENCIES)
    {
        auto &actual = dependency(dependencies, expected.name);
        EXPECT_EQ(actual.at("version").string(), expected.version);
        EXPECT_EQ(actual.at("revision").string(), expected.revision);
    }

    auto &host = document.at("host");

#if BME_OS_WINDOWS
    EXPECT_EQ(host.at("os").string(), "windows");
#elif BME_OS_LINUX
    EXPECT_EQ(host.at("os").string(), "linux");
#endif

    EXPECT_EQ(host.at("architecture").string(), "x86_64");

    auto &cpu = host.at("cpu");
    EXPECT_EQ(cpu.at("capture_scope").string(), "host_visible_process");
    EXPECT_FALSE(cpu.at("execution_cpu_attributed").boolean());
    EXPECT_EQ(cpu.at("vendor").string(), "AuthenticAMD");
    EXPECT_EQ(cpu.at("maximum_extended_leaf").string(), "0x80000008");
    EXPECT_TRUE(cpu.at("xss_supported").is_null());
    EXPECT_EQ(cpu.at("xcr0").string(), "0x0000000000000007");

    auto &raw_cpuid = cpu.at("raw_cpuid").array();
    ASSERT_EQ(raw_cpuid.size(), 1u);
    EXPECT_EQ(raw_cpuid[0].at("eax").string(), "0x00A40F00");
    EXPECT_EQ(raw_cpuid[0].at("edx").string(), "0x99AABBCC");

    auto &request = document.at("request");
    EXPECT_EQ(request.at("input_bytes").string(), "90");
    EXPECT_EQ(request.at("backend").string(), "xed");
    EXPECT_EQ(request.at("syntax").string(), "att");
    EXPECT_EQ(request.at("effective_max_steps").number(), 123u);
    EXPECT_FALSE(request.at("seed_data_pointers").boolean());

    auto &requested_seed = request.at("seed");
    EXPECT_EQ(requested_seed.at("gpr").at("rax").string(), "0x0123456789ABCDEF");
    EXPECT_EQ(requested_seed.at("gpr").at("rflags").string(), "0x0000000000000246");
    EXPECT_EQ(requested_seed.at("sse").at("xmm0").string(), "0x8899AABBCCDDEEFF0011223344556677");
    EXPECT_EQ(requested_seed.at("sse").at("mxcsr").string(), "0xA1B2C3D4");
    EXPECT_EQ(requested_seed.at("x87").at("st0").string(), "0x99887766554433221100");
    EXPECT_EQ(requested_seed.at("x87").at("control_word").string(), "0x1234");
    EXPECT_EQ(requested_seed.at("x87").at("status_word").string(), "0x5678");
    EXPECT_EQ(requested_seed.at("x87").at("tag_word_abridged").string(), "0x9A");

    auto &execution = document.at("execution");
    EXPECT_EQ(execution.at("outcome").string(), "faulted");
    EXPECT_EQ(execution.at("message").string(), "Synthetic \"fault\"\nmessage");
    EXPECT_EQ(execution.at("stop_address").string(), "0x0000000000002000");
    EXPECT_EQ(execution.at("seed").at("gpr").at("rax").string(), "0x1111111111111111");

    auto &events = execution.at("events").array();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].at("index").number(), 0u);
    EXPECT_EQ(events[0].at("classification").string(), "completed");
    EXPECT_EQ(events[0].at("offset").number(), 0u);
    EXPECT_EQ(events[0].at("registers").at("gpr").at("rax").string(), "0x2222222222222222");
    EXPECT_EQ(events[1].at("classification").string(), "faulted");
    EXPECT_TRUE(events[1].at("offset").is_null());
    EXPECT_EQ(events[1].at("registers").at("gpr").at("rax").string(), "0x3333333333333333");
    EXPECT_EQ(events[1].at("registers").at("gpr").at("rip").string(), "0x0000000000002000");

    auto &decoders = document.at("decoders").array();
    ASSERT_EQ(decoders.size(), 4u);
    EXPECT_EQ(decoders[0].at("backend").string(), "zydis");
    EXPECT_EQ(decoders[1].at("backend").string(), "bddisasm");
    EXPECT_EQ(decoders[2].at("backend").string(), "capstone");
    EXPECT_EQ(decoders[3].at("backend").string(), "xed");

    for (auto &&decoder : decoders)
    {
        EXPECT_EQ(decoder.at("requested_syntax").string(), "att");
        EXPECT_NE(decoder.at("version").string(), "unknown");
        EXPECT_EQ(decoder.at("revision").string().size(), 40u);

        auto &rows = decoder.at("rows").array();
        ASSERT_FALSE(rows.empty());

        auto &row = rows.front();
        EXPECT_EQ(row.object().size(), 7u);
        EXPECT_FALSE(row.at("address").string().empty());
        EXPECT_GT(row.at("length").number(), 0u);
    }

    EXPECT_EQ(decoders[0].at("effective_syntax").string(), "att");
    EXPECT_EQ(decoders[1].at("effective_syntax").string(), "intel");
    EXPECT_EQ(decoders[2].at("effective_syntax").string(), "att");
    EXPECT_EQ(decoders[3].at("effective_syntax").string(), "att");
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
