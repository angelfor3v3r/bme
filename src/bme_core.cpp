#include "bme_core.hpp"
#include "common.hpp"

// Version metadata.
// CMake generates `bme_version.hpp` each build, falling back to placeholders outside CMake.
#if __has_include("bme_version.hpp")
#include "bme_version.hpp"
#else
#define BME_GIT_TAG  "unknown"
#define BME_GIT_HASH "unknown"
#define BME_GIT_URL  "https://github.com/angelfor3v3r/bme"
#endif

#include "os.hpp"
#include "util.hpp"

#include <argparse/argparse.hpp>
#include <bddisasm.h>
#include <capstone/capstone.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <ftxui/ftxui.hpp>

// XED's headers have no `extern "C"`, so wrap them for C linkage.
extern "C"
{
#include <xed/xed-interface.h>
}

#include <Zydis/Zydis.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace bme
{

template <class E>
using Error = std::unexpected<E>;

// Shown by `--version` and the TUI About box.
// Keep the copyright in sync with `LICENSE`.
constexpr std::string_view BME_COPYRIGHT = "Copyright (c) 2026 angelfor3v3r (Dexxi) - MIT License";

#if BME_OS_WINDOWS
constexpr std::string_view BME_PLATFORM = "Windows x86_64";
#elif BME_OS_LINUX
constexpr std::string_view BME_PLATFORM = "Linux x86_64";
#else
#error "`bme` Unsupported platform"
#endif

constexpr std::array<std::string_view, REG_COUNT> REG_NAMES{{
    "RAX",
    "RBX",
    "RCX",
    "RDX",
    "RSI",
    "RDI",
    "RBP",
    "RSP",
    "R8",
    "R9",
    "R10",
    "R11",
    "R12",
    "R13",
    "R14",
    "R15",
    "RIP",
    "RFLAGS",
}};

// Sub-register names for the GPR drill-down, indexed like `REG_NAMES[0..15]` (`RAX` -> `EAX` -> `AX` -> `AH`/`AL`).
constexpr std::array<std::string_view, GPR_COUNT> GPR_NAMES_32{{
    "EAX",
    "EBX",
    "ECX",
    "EDX",
    "ESI",
    "EDI",
    "EBP",
    "ESP",
    "R8D",
    "R9D",
    "R10D",
    "R11D",
    "R12D",
    "R13D",
    "R14D",
    "R15D",
}};

constexpr std::array<std::string_view, GPR_COUNT> GPR_NAMES_16{{
    "AX",
    "BX",
    "CX",
    "DX",
    "SI",
    "DI",
    "BP",
    "SP",
    "R8W",
    "R9W",
    "R10W",
    "R11W",
    "R12W",
    "R13W",
    "R14W",
    "R15W",
}};

constexpr std::array<std::string_view, GPR_COUNT> GPR_NAMES_8L{{
    "AL",
    "BL",
    "CL",
    "DL",
    "SIL",
    "DIL",
    "BPL",
    "SPL",
    "R8B",
    "R9B",
    "R10B",
    "R11B",
    "R12B",
    "R13B",
    "R14B",
    "R15B",
}};

// The high-byte aliases (`AH`/`BH`/`CH`/`DH`) only exist for the legacy `A`/`B`/`C`/`D` registers.
constexpr std::array<std::string_view, 4> GPR_NAMES_8H{{"AH", "BH", "CH", "DH"}};

// Register-panel display order.
// `RIP` first (the current instruction), then the GPRs in canonical order, then `RFLAGS`. `RIP`, `RSP`, and `RFLAGS` are engine-controlled and shown
// flat (no seed, no drill-down). The rest are seedable with sub-register drill-down.
constexpr std::array REGISTER_DISPLAY_ORDER{
    Reg::RIP, Reg::RAX, Reg::RBX, Reg::RCX, Reg::RDX, Reg::RSI, Reg::RDI, Reg::RBP, Reg::RSP,
    Reg::R8,  Reg::R9,  Reg::R10, Reg::R11, Reg::R12, Reg::R13, Reg::R14, Reg::R15, Reg::RFLAGS,
};

// The seven user-visible, seedable `RFLAGS` flags (status flags plus `DF`), high-to-low bit order (the conventional debugger layout).
// Shared by the Flags panel and `--quick`.
struct StatusFlag
{
    std::string_view name{};
    std::uint64_t    bit{};
};

constexpr std::array<StatusFlag, 7> STATUS_FLAGS{{
    {.name = "OF", .bit = OF},
    {.name = "DF", .bit = DF},
    {.name = "SF", .bit = SF},
    {.name = "ZF", .bit = ZF},
    {.name = "AF", .bit = AF},
    {.name = "PF", .bit = PF},
    {.name = "CF", .bit = CF},
}};

struct BackendInfo
{
    std::string_view name{};         // Display name (Settings label).
    std::string_view cli{};          // `--backend` CLI token (lowercase).
    bool             intel_syntax{}; // Emits Intel-syntax text.
    bool             att_syntax{};   // Emits AT&T-syntax text.
};

// Per-decoder capability table.
// Adding a decoder = a new `DisasmBackend` value + a row here (plus its `--backend` choice and a `disasm_one` branch).
constexpr std::array<BackendInfo, 4> BACKENDS{{
    {.name = "Zydis", .cli = "zydis", .intel_syntax = true, .att_syntax = true},
    {.name = "bddisasm", .cli = "bddisasm", .intel_syntax = true, .att_syntax = false},
    {.name = "Capstone", .cli = "capstone", .intel_syntax = true, .att_syntax = true},
    {.name = "XED", .cli = "xed", .intel_syntax = true, .att_syntax = true},
}};

constexpr auto BACKEND_COUNT = BACKENDS.size();

const auto &backend_info(DisasmBackend backend) noexcept { return BACKENDS[(std::size_t)backend]; }

// Resolve a `--backend` CLI token to its enum.
// argparse validates the token first, so the Zydis fallback is just a safety net.
auto backend_from_cli(std::string_view name) noexcept
{
    auto found = std::ranges::find(BACKENDS, name, &BackendInfo::cli);

    return found == BACKENDS.end() ? DisasmBackend::Zydis : (DisasmBackend)(found - BACKENDS.begin());
}

// True if `backend` can render `syntax`.
// bddisasm is Intel-only, the others do both.
bool backend_supports(DisasmBackend backend, DisasmSyntax syntax) noexcept
{
    auto &info = backend_info(backend);

    return syntax == DisasmSyntax::ATT ? info.att_syntax : info.intel_syntax;
}

auto format_hex64_string(std::uint64_t value) { return fmt::format("0x{:016X}", value); }

// Format the shortest round-tripping decimal.
// Add a decimal point when needed so finite output remains a decimal seed.
template <std::floating_point T>
auto format_float(T value)
{
    auto text = fmt::format("{}", value);
    if (!text.contains('.'))
    {
        auto exponent_delim = text.find_first_of("eE");
        if (exponent_delim != std::string::npos)
        {
            text.insert(exponent_delim, ".0");
        }
        else if (std::isfinite(value))
        {
            text += ".0";
        }
    }

    return text;
}

// Append `f` to single-precision lanes so copied values preserve their precision.
auto format_float_single(float value) { return format_float(value) + "f"; }

// Classify an x87 register for display.
// `valid` is the `FXSAVE` abridged tag bit. When set, the 80-bit value is inspected for `Zero`/`Special`/`Nonzero`.
std::string_view x87_tag_name(bool valid, const std::array<std::uint8_t, 10> &bytes) noexcept
{
    if (!valid)
    {
        return "Empty";
    }

    // Low 8 bytes = the 64-bit mantissa (little-endian, x86-64 only).
    std::uint64_t mantissa{};
    std::memcpy(&mantissa, bytes.data(), sizeof(mantissa));

    // Exponent 0 means zero when the mantissa is clear, otherwise a (pseudo-)denormal.
    auto exponent = (std::uint32_t)((bytes[9] << 8 | bytes[8]) & 0x7FFF);
    if (exponent == 0)
    {
        return mantissa == 0 ? "Zero" : "Special";
    }

    // Inf / NaN, or unnormal (integer bit clear at a normal exponent).
    if (exponent == 0x7FFF || (mantissa & 0x8000'0000'0000'0000ull) == 0)
    {
        return "Special";
    }

    return "Nonzero";
}

// Space-joined names of the bits set in `word`, or "none".
auto decode_flag_bits(std::uint32_t word, std::span<const std::pair<std::string_view, std::uint32_t>> bits)
{
    std::string result{};
    for (auto &&[name, bit] : bits)
    {
        if ((word & bit) != 0)
        {
            result += fmt::format(" {}", name);
        }
    }

    return !result.empty() ? result.substr(1) : "none";
}

std::vector<std::string> decode_x87_control_word(std::uint16_t control_word)
{
    constexpr std::array<std::string_view, 4>                           PRECISION{"Real4", "Not Used", "Real8", "Real10"};
    constexpr std::array<std::string_view, 4>                           ROUNDING{"Round Near", "Round Down", "Round Up", "Truncate"};
    constexpr std::array<std::pair<std::string_view, std::uint32_t>, 6> MASKS{
        {{"IM", 0x01}, {"DM", 0x02}, {"ZM", 0x04}, {"OM", 0x08}, {"UM", 0x10}, {"PM", 0x20}}
    };

    return {
        fmt::format("{} {}", PRECISION[control_word >> 8 & 3], ROUNDING[control_word >> 10 & 3]),
        "Masks: " + decode_flag_bits(control_word, MASKS),
    };
}

std::vector<std::string> decode_x87_status_word(std::uint16_t status_word)
{
    constexpr std::array<std::pair<std::string_view, std::uint32_t>, 9> FLAGS{
        {{"IE", 0x01}, {"DE", 0x02}, {"ZE", 0x04}, {"OE", 0x08}, {"UE", 0x10}, {"PE", 0x20}, {"SF", 0x40}, {"ES", 0x80}, {"B", 0x8000}}
    };
    constexpr std::array<std::pair<std::string_view, std::uint32_t>, 4> CONDITION{{{"C0", 0x100}, {"C1", 0x200}, {"C2", 0x400}, {"C3", 0x4000}}};

    auto top = status_word >> 11 & 7;

    return {
        fmt::format("TOP={} (ST0=x87r{})", top, top),
        "Flags: " + decode_flag_bits(status_word, FLAGS),
        "CC: " + decode_flag_bits(status_word, CONDITION),
    };
}

std::vector<std::string> decode_mxcsr(std::uint32_t mxcsr)
{
    constexpr std::array<std::string_view, 4>                           ROUNDING{"Round Near", "Toward Negative", "Toward Positive", "Toward Zero"};
    constexpr std::array<std::pair<std::string_view, std::uint32_t>, 6> MASKS{
        {{"IM", 0x80}, {"DM", 0x100}, {"ZM", 0x200}, {"OM", 0x400}, {"UM", 0x800}, {"PM", 0x1000}}
    };
    constexpr std::array<std::pair<std::string_view, std::uint32_t>, 6> FLAGS{
        {{"IE", 0x01}, {"DE", 0x02}, {"ZE", 0x04}, {"OE", 0x08}, {"UE", 0x10}, {"PE", 0x20}}
    };

    // `DAZ` (denormals-are-zero) and `FZ` (flush-to-zero) are mode controls, not exception flags.
    constexpr std::array<std::pair<std::string_view, std::uint32_t>, 2> MODES{{{"DAZ", 0x40}, {"FZ", 0x8000}}};

    return {
        std::string(ROUNDING[mxcsr >> 13 & 3]),
        "Masks: " + decode_flag_bits(mxcsr, MASKS),
        "Flags: " + decode_flag_bits(mxcsr, FLAGS),
        "Modes: " + decode_flag_bits(mxcsr, MODES),
    };
}

// Render a decoded control/status word as stacked lines.
// `prefix` on the first, each group (`Masks`/`Flags`/`CC`/`Modes`) indented on its own line so nothing wraps off-screen.
auto decode_block(const std::string &prefix, const std::vector<std::string> &groups)
{
    ftxui::Elements lines{};
    for (std::size_t i{}; i < groups.size(); ++i)
    {
        lines.emplace_back(ftxui::text(i == 0 ? prefix + groups[i] : "  " + groups[i]) | ftxui::dim);
    }

    return ftxui::vbox(std::move(lines));
}

Result<std::vector<std::uint8_t>, std::string> parse_code_text(std::string_view text)
{
    auto skip_whitespace = [&text](std::size_t &position) noexcept
    {
        while (position < text.size() && std::isspace((std::uint8_t)text[position]) != 0)
        {
            ++position;
        }
    };

    auto parse_byte = [&text](std::size_t position) -> Result<std::uint8_t, std::string>
    {
        if (text.size() - position < 2)
        {
            return Error{fmt::format("Dangling hex nibble at position {}", position)};
        }

        std::uint8_t byte{};
        auto        *pair             = text.data() + position;
        auto [parsed_end, error_code] = std::from_chars(pair, pair + 2, byte, 16);
        if (error_code != std::errc{} || parsed_end != pair + 2)
        {
            return Error{fmt::format("Invalid hex byte {:?} at position {}", text.substr(position, 2), position)};
        }

        return byte;
    };

    std::size_t position{};
    skip_whitespace(position);

    if (position == text.size())
    {
        return Error{"No code to run"};
    }

    std::vector<std::uint8_t> result{};
    result.reserve((text.size() - position) / 2);

    if (text[position] == '{')
    {
        ++position;

        for (;;)
        {
            skip_whitespace(position);

            if (position == text.size())
            {
                return Error{"Missing closing brace in byte array"};
            }

            if (text[position] == '}')
            {
                ++position;

                skip_whitespace(position);

                if (position != text.size())
                {
                    return Error{fmt::format("Unexpected text after byte array at position {}", position)};
                }

                return result.empty() ? Result<std::vector<std::uint8_t>, std::string>{Error{"No code to run"}} : result;
            }

            if (text.size() - position < 3 || text[position] != '0' || (text[position + 1] != 'x' && text[position + 1] != 'X'))
            {
                return Error{fmt::format("Expected a 0x-prefixed byte at position {}", position)};
            }

            position += 2;

            auto digit_start = position;

            while (position < text.size() && std::isxdigit((std::uint8_t)text[position]) != 0)
            {
                ++position;
            }

            auto digit_count = position - digit_start;
            if (digit_count == 0)
            {
                return Error{fmt::format("Expected a hexadecimal byte at position {}", digit_start)};
            }

            if (digit_count > 2)
            {
                return Error{fmt::format("Byte value at position {} exceeds 0xFF", digit_start - 2)};
            }

            std::uint8_t byte{};
            auto [parsed_end, error_code] = std::from_chars(text.data() + digit_start, text.data() + position, byte, 16);
            if (error_code != std::errc{} || parsed_end != text.data() + position)
            {
                return Error{fmt::format("Invalid hexadecimal byte at position {}", digit_start - 2)};
            }

            result.emplace_back(byte);

            skip_whitespace(position);

            if (position < text.size() && text[position] == '}')
            {
                continue;
            }

            if (position == text.size() || text[position] != ',')
            {
                return Error{fmt::format("Expected a comma or closing brace at position {}", position)};
            }

            ++position;
        }
    }

    if (text[position] == '\\')
    {
        while (position < text.size())
        {
            skip_whitespace(position);

            if (position == text.size())
            {
                break;
            }

            if (text.size() - position < 4 || text[position] != '\\' || (text[position + 1] != 'x' && text[position + 1] != 'X'))
            {
                return Error{fmt::format("Expected a \\x-prefixed byte at position {}", position)};
            }

            auto byte = parse_byte(position + 2);
            if (!byte)
            {
                return Error{byte.error()};
            }

            result.emplace_back(*byte);

            position += 4;
        }

        return result;
    }

    while (position < text.size())
    {
        if (std::isspace((std::uint8_t)text[position]) != 0)
        {
            ++position;

            continue;
        }

        if (text.size() - position < 2 || std::isspace((std::uint8_t)text[position + 1]) != 0)
        {
            return Error{fmt::format("Dangling hex nibble at position {}", position)};
        }

        auto byte = parse_byte(position);
        if (!byte)
        {
            return Error{byte.error()};
        }

        result.emplace_back(*byte);

        position += 2;
    }

    return result;
}

namespace
{

// One maximum-length x86 instruction for every permitted step.
constexpr std::size_t MAX_RAW_FILE_BYTES = MAX_STEPS_LIMIT * 15;

Result<std::vector<std::uint8_t>, std::string> read_binary_input(std::string_view path)
{
    std::ifstream input{std::string(path), std::ios::binary | std::ios::ate};
    if (!input)
    {
        return Error{fmt::format("Could not open input file {:?}", path)};
    }

    auto end = input.tellg();
    if (end < 0)
    {
        return Error{fmt::format("Could not determine a usable size for input file {:?}", path)};
    }

    auto end_value = (std::uintmax_t)end;
    if (end_value > std::numeric_limits<std::size_t>::max() || end_value > (std::uintmax_t)std::numeric_limits<std::streamsize>::max())
    {
        return Error{fmt::format("Could not determine a usable size for input file {:?}", path)};
    }

    if (end_value > MAX_RAW_FILE_BYTES)
    {
        return Error{fmt::format("Input file {:?} exceeds the {} byte limit", path, MAX_RAW_FILE_BYTES)};
    }

    std::vector<std::uint8_t> result((std::size_t)end_value);

    input.seekg(0);

    if (!result.empty())
    {
        input.read((char *)result.data(), (std::streamsize)result.size());
    }

    if (!input)
    {
        return Error{fmt::format("Could not read input file {:?}", path)};
    }

    if (result.empty())
    {
        return Error{"No code to run"};
    }

    return result;
}

std::string format_code_text(std::span<const std::uint8_t> code) { return fmt::format("{:02X}", fmt::join(code, " ")); }

} // namespace

// Parses a hex value (optional `0x`).
// Empty text seeds zero.
Result<std::uint64_t, std::string> parse_seed(std::string_view seed)
{
    if (seed.empty())
    {
        return 0;
    }

    std::size_t   offset = seed.size() >= 2 && seed[0] == '0' && (seed[1] == 'x' || seed[1] == 'X') ? 2 : 0;
    std::uint64_t result{};
    auto [parse_end, error_code] = std::from_chars(seed.data() + offset, seed.data() + seed.size(), result, 16);
    if (error_code != std::errc{} || parse_end != seed.data() + seed.size())
    {
        return Error{fmt::format("Invalid hex value {:?}", seed)};
    }

    return result;
}

namespace
{

Result<std::uint32_t, std::string> parse_mxcsr_seed(std::string_view text)
{
    auto value = parse_seed(text);
    if (!value || *value > MXCSR_DEFINED_MASK)
    {
        return Error{fmt::format("Invalid MXCSR seed {:?} (expected defined bits 0 through 15).", text)};
    }

    return (std::uint32_t)*value;
}

Result<std::uint16_t, std::string> parse_fpu_control_word_seed(std::string_view text)
{
    auto value = parse_seed(text);
    if (!value || *value > std::numeric_limits<std::uint16_t>::max())
    {
        return Error{fmt::format("Invalid control_word seed {:?} (expected up to 4 hexadecimal digits).", text)};
    }

    return (std::uint16_t)*value;
}

} // namespace

// Parses a decimal with an optional trailing precision suffix.
// `f`/`F` selects single precision. `l`/`L`/none selects double precision.
Result<DecimalSeed, std::string> parse_decimal_seed(std::string_view text, std::string_view label)
{
    auto original_text      = text;
    auto original_magnitude = text;
    if (!original_magnitude.empty() && (original_magnitude.front() == '+' || original_magnitude.front() == '-'))
    {
        original_magnitude.remove_prefix(1);
    }

    auto infinity_without_suffix =
        ascii_case_insensitive_equal(original_magnitude, "inf") || ascii_case_insensitive_equal(original_magnitude, "infinity");
    bool is_single{};
    if (!infinity_without_suffix && !text.empty() && (text.back() == 'f' || text.back() == 'F'))
    {
        is_single = true;

        text.remove_suffix(1);
    }
    else if (!text.empty() && (text.back() == 'l' || text.back() == 'L'))
    {
        text.remove_suffix(1);
    }

    DecimalSeed result{};
    result.is_single = is_single;

    auto magnitude = text;
    bool negative{};
    if (!magnitude.empty() && (magnitude.front() == '+' || magnitude.front() == '-'))
    {
        negative = magnitude.front() == '-';

        magnitude.remove_prefix(1);
    }

    if (ascii_case_insensitive_equal(magnitude, "inf") || ascii_case_insensitive_equal(magnitude, "infinity"))
    {
        auto infinity = is_single ? (double)std::numeric_limits<float>::infinity() : std::numeric_limits<double>::infinity();
        result.value  = negative ? -infinity : infinity;

        return result;
    }

    if (ascii_case_insensitive_equal(magnitude, "nan"))
    {
        auto nan     = is_single ? (double)std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<double>::quiet_NaN();
        result.value = std::copysign(nan, negative ? -1.0 : 1.0);

        return result;
    }

    if (is_single)
    {
        float parsed{};
        auto [parse_end, error_code] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (error_code != std::errc{} || parse_end != text.data() + text.size())
        {
            return Error{fmt::format("Invalid {} seed {:?} (expected hex or a decimal value).", label, original_text)};
        }

        result.value = (double)parsed;
    }
    else
    {
        auto [parse_end, error_code] = std::from_chars(text.data(), text.data() + text.size(), result.value);
        if (error_code != std::errc{} || parse_end != text.data() + text.size())
        {
            return Error{fmt::format("Invalid {} seed {:?} (expected hex or a decimal value).", label, original_text)};
        }
    }

    return result;
}

// A decimal seed either has a decimal point or a non-finite spelling, which cannot be valid hexadecimal.
bool is_decimal_seed(std::string_view text) noexcept
{
    return text.find('.') != std::string_view::npos || text.find_first_of("iInN") != std::string_view::npos;
}

// Composes a 128-bit XMM seed from hex (up to 32 digits, optional `0x`) or a decimal (optional `f`/`l` suffix).
// Single lands in the low 32 bits, double in the low 64 bits. Empty text seeds zero. Returns `{low, high}`.
Result<std::array<std::uint64_t, 2>, std::string> compose_xmm_seed(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }

    std::array<std::uint64_t, 2> result{};
    if (is_decimal_seed(text))
    {
        auto parsed = parse_decimal_seed(text, "XMM");
        if (!parsed)
        {
            return Error{parsed.error()};
        }

        // Single -> low 32 bits (`f32x4` lane 0).
        // Double -> low 64 bits (`f64x2` lane 0).
        // Upper bits stay zero.
        if (parsed->is_single)
        {
            result[0] = std::bit_cast<std::uint32_t>((float)parsed->value);
        }
        else
        {
            result[0] = std::bit_cast<std::uint64_t>(parsed->value);
        }

        return result;
    }

    std::size_t offset = text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 2 : 0;
    auto        digits = text.substr(offset);
    if (digits.empty() || digits.size() > 32)
    {
        return Error{fmt::format("Invalid XMM seed {:?} (expected up to 32 hex digits).", text)};
    }

    std::size_t low_begin     = digits.size() > 16 ? digits.size() - 16 : 0;
    auto        low           = digits.substr(low_begin);
    auto        high          = digits.substr(0, low_begin);
    auto [low_end, low_error] = std::from_chars(low.data(), low.data() + low.size(), result[0], 16);
    if (low_error != std::errc{} || low_end != low.data() + low.size())
    {
        return Error{fmt::format("Invalid XMM seed {:?} (expected hex).", text)};
    }

    if (!high.empty())
    {
        auto [high_end, high_error] = std::from_chars(high.data(), high.data() + high.size(), result[1], 16);
        if (high_error != std::errc{} || high_end != high.data() + high.size())
        {
            return Error{fmt::format("Invalid XMM seed {:?} (expected hex).", text)};
        }
    }

    return result;
}

// Composes an 80-bit x87 seed from hex (up to 20 digits, optional `0x`) or a decimal (optional `f`/`l` suffix, rounded to 80-bit on the FPU).
Result<std::array<std::uint8_t, 10>, std::string> compose_st_seed(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }

    std::array<std::uint8_t, 10> result{};
    if (is_decimal_seed(text))
    {
        auto parsed = parse_decimal_seed(text, "ST");
        if (!parsed)
        {
            return Error{parsed.error()};
        }

        // Round the value to 80-bit extended on the FPU.
        // The `f`/`l` suffix only chose the parse precision.
        double_to_st80(parsed->value, result);

        return result;
    }

    std::size_t offset = text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 2 : 0;
    auto        digits = text.substr(offset);
    if (digits.empty() || digits.size() > 20)
    {
        return Error{fmt::format("Invalid ST seed {:?} (expected up to 20 hex digits).", text)};
    }

    // Left-pad to the full 20 hex digits, then read big-endian byte pairs into the little-endian array (byte 9 is most significant).
    std::string padded(20 - digits.size(), '0');
    padded += digits;

    for (std::size_t i{}; i < result.size(); ++i)
    {
        auto pair                    = std::string_view(padded).substr(i * 2, 2);
        auto [parse_end, error_code] = std::from_chars(pair.data(), pair.data() + pair.size(), result[9 - i], 16);
        if (error_code != std::errc{} || parse_end != pair.data() + pair.size())
        {
            return Error{fmt::format("Invalid ST seed {:?} (expected hex).", text)};
        }
    }

    return result;
}

namespace
{

Result<std::uint64_t, std::string> parse_cli_seed_value(std::string_view text)
{
    auto value = parse_seed(text);
    if (!value || text.empty())
    {
        return Error{fmt::format("Invalid `--seed` value {:?} (expected hex).", text)};
    }

    return *value;
}

std::optional<std::size_t> parse_xmm_seed_index(std::string_view text) noexcept
{
    if (text.size() < 4 || (text[0] != 'x' && text[0] != 'X') || (text[1] != 'm' && text[1] != 'M') || (text[2] != 'm' && text[2] != 'M'))
    {
        return {};
    }

    std::size_t result{};
    auto [parse_end, error_code] = std::from_chars(text.data() + 3, text.data() + text.size(), result);
    if (error_code != std::errc{} || parse_end != text.data() + text.size() || result >= 16)
    {
        return {};
    }

    return result;
}

std::optional<std::size_t> parse_st_seed_index(std::string_view text) noexcept
{
    if (text.size() != 3 || (text[0] != 's' && text[0] != 'S') || (text[1] != 't' && text[1] != 'T'))
    {
        return {};
    }

    std::size_t result{};
    auto [parse_end, error_code] = std::from_chars(text.data() + 2, text.data() + text.size(), result);
    if (error_code != std::errc{} || parse_end != text.data() + text.size() || result >= 8)
    {
        return {};
    }

    return result;
}

Result<void, std::string> parse_cli_seed_argument(std::string_view seed_text, CLI &cli)
{
    for (auto &&part : seed_text | std::views::split(','))
    {
        std::string_view entry{part};
        if (entry.empty())
        {
            continue;
        }

        auto equals = entry.find('=');
        if (equals == std::string_view::npos)
        {
            return Error{fmt::format("Invalid `--seed` entry {:?} (expected name=value).", entry)};
        }

        auto name       = entry.substr(0, equals);
        auto value_text = entry.substr(equals + 1);
        if (value_text.empty())
        {
            return Error{fmt::format("Invalid `--seed` entry {:?} (expected a non-empty value).", entry)};
        }

        if (ascii_case_insensitive_equal(name, "mxcsr"))
        {
            if (auto value = parse_mxcsr_seed(value_text); !value)
            {
                return Error{value.error()};
            }

            cli.seed_floating_environment.mxcsr = std::string(value_text);

            continue;
        }

        if (ascii_case_insensitive_equal(name, "control_word"))
        {
            if (auto value = parse_fpu_control_word_seed(value_text); !value)
            {
                return Error{value.error()};
            }

            cli.seed_floating_environment.fpu_control_word = std::string(value_text);

            continue;
        }

        if (auto xmm_index = parse_xmm_seed_index(name))
        {
            if (auto value = compose_xmm_seed(value_text); !value)
            {
                return Error{value.error()};
            }

            cli.seed_xmm[*xmm_index] = std::string(value_text);

            continue;
        }

        if (auto st_index = parse_st_seed_index(name))
        {
            if (auto value = compose_st_seed(value_text); !value)
            {
                return Error{value.error()};
            }

            cli.seed_st[*st_index] = std::string(value_text);

            continue;
        }

        std::string *slice{};
        for (std::size_t reg{}; reg < GPR_COUNT && slice == nullptr; ++reg)
        {
            if (ascii_case_insensitive_equal(name, REG_NAMES[reg]))
            {
                slice = &cli.seed_gpr[reg].full;
            }
            else if (ascii_case_insensitive_equal(name, GPR_NAMES_32[reg]))
            {
                slice = &cli.seed_gpr[reg].dword;
            }
            else if (ascii_case_insensitive_equal(name, GPR_NAMES_16[reg]))
            {
                slice = &cli.seed_gpr[reg].word;
            }
            else if (ascii_case_insensitive_equal(name, GPR_NAMES_8L[reg]))
            {
                slice = &cli.seed_gpr[reg].byte_low;
            }
            else if (reg < GPR_NAMES_8H.size() && ascii_case_insensitive_equal(name, GPR_NAMES_8H[reg]))
            {
                slice = &cli.seed_gpr[reg].byte_high;
            }

            if (slice != nullptr && reg == (std::size_t)Reg::RSP)
            {
                return Error{"`--seed` can't set RSP or its sub-registers. RSP is engine-controlled (reset to the scratch-stack top)."};
            }
        }

        if (slice != nullptr)
        {
            auto value = parse_cli_seed_value(value_text);
            if (!value)
            {
                return Error{value.error()};
            }

            *slice = std::string(value_text);

            continue;
        }

        bool matched{};
        for (auto &&flag : STATUS_FLAGS)
        {
            if (ascii_case_insensitive_equal(name, flag.name))
            {
                auto value = parse_cli_seed_value(value_text);
                if (!value)
                {
                    return Error{value.error()};
                }

                cli.seed_flags = *value != 0 ? cli.seed_flags | flag.bit : cli.seed_flags & ~flag.bit;
                matched        = true;

                break;
            }
        }

        if (!matched)
        {
            return Error{fmt::format(
                "Unknown `--seed` name {:?} (expected a GPR slice, XMM0..15, ST0..7, MXCSR, control_word, or a flag CF/PF/AF/ZF/SF/DF/OF).", name
            )};
        }
    }

    return {};
}

} // namespace

Result<CLI, std::string> CLI::parse(std::int32_t argc, char *argv[])
{
    argparse::ArgumentParser program("bme", fmt::format("bme version {} ({} {})\n{}", BME_GIT_TAG, BME_GIT_URL, BME_GIT_HASH, BME_COPYRIGHT));

    auto &input_group = program.add_mutually_exclusive_group();
    input_group.add_argument("--bytes").help(R"(Formatted x86-64 bytes, e.g. 48FFC0, \x48\xFF\xC0, or {0x48, 0xFF, 0xC0}.)");
    input_group.add_argument("--file").help("Read x86-64 machine code from a raw binary file.");
    program.add_argument("--run").flag().help("Run the code immediately after loading.");

    // Keep `.nargs(1)` after `.default_value()` for `--syntax`, `--backend`, and `--format`.
    // `default_value` otherwise resets `nargs` min to 0, parsing invalid values as stray positionals.
    program.add_argument("--syntax")
        .default_value("intel")
        .nargs(1)
        .choices("intel", "att")
        .help("Disassembly syntax: intel or att (default: intel).");
    program.add_argument("--max-steps")
        .help(fmt::format("Max instructions to single-step before aborting (default {}, max {}).", DEFAULT_MAX_STEPS, MAX_STEPS_LIMIT));
    program.add_argument("--quick").flag().help("Print the trace to stdout and exit instead of opening the TUI (needs `--bytes` or `--file`).");
    program.add_argument("--format")
        .default_value("text")
        .nargs(1)
        .choices("text", "json")
        .help("Output format for `--quick`: text or json (default: text).");
    program.add_argument("--pretty").flag().help("Pretty-print JSON output (requires `--quick --format json`).");
    program.add_argument("--track").help(
        "Register classes to show in `--quick`, comma-separated: gpr,rip,rflags,xmm,x87 (or all/none). Default: gpr,rip,rflags."
    );
    program.add_argument("--seed").help(
        "Seed registers, flags, XMM, ST, MXCSR, and control_word as comma-separated name=value pairs. Values are hexadecimal, except XMM/ST also"
        " accept decimal inf/nan with an optional f/l suffix. GPR slices exclude RSP. Flags are CF/PF/AF/ZF/SF/DF/OF."
    );
    program.add_argument("--backend")
        .default_value("zydis")
        .nargs(1)
        .choices("zydis", "bddisasm", "capstone", "xed")
        .help("Disassembler backend: zydis, bddisasm, capstone, or xed (default: zydis).");

    try
    {
        program.parse_args(argc, argv);
    }
    catch (const std::exception &exception)
    {
        return Error{fmt::format("{}\n\t{}", exception.what(), program.usage())};
    }

    auto quick = program.get<bool>("--quick");
    if (program.is_used("--format") && !quick)
    {
        return Error{"`--format` requires `--quick`."};
    }

    auto format_is_json = program.get<std::string>("--format") == "json";
    auto pretty_json    = program.get<bool>("--pretty");
    if (pretty_json && (!quick || !format_is_json))
    {
        return Error{"`--pretty` requires `--quick --format json`."};
    }

    if (format_is_json && program.is_used("--track"))
    {
        return Error{"`--track` cannot be used with `--format json` because JSON always includes full register state."};
    }

    auto max_steps = DEFAULT_MAX_STEPS;
    if (auto max_steps_text = program.present<std::string>("--max-steps"))
    {
        auto [parse_end, error_code] = std::from_chars(max_steps_text->data(), max_steps_text->data() + max_steps_text->size(), max_steps);
        if (error_code != std::errc{} || parse_end != max_steps_text->data() + max_steps_text->size() || max_steps == 0)
        {
            return Error{fmt::format("Invalid `--max-steps` value {:?} (expected a positive integer).", *max_steps_text)};
        }

        max_steps = std::min(max_steps, MAX_STEPS_LIMIT);
    }

    // Default mask, then check each argument.
    TrackMask track{.gpr = true, .rip = true, .rflags = true};
    if (auto track_text = program.present<std::string>("--track"))
    {
        track = {};

        for (auto &&part : *track_text | std::views::split(','))
        {
            std::string_view token{part};
            if (token == "gpr")
            {
                track.gpr = true;
            }
            else if (token == "rip")
            {
                track.rip = true;
            }
            else if (token == "rflags" || token == "flags")
            {
                track.rflags = true;
            }
            else if (token == "xmm" || token == "sse")
            {
                track.xmm = true;
            }
            else if (token == "x87" || token == "fpu")
            {
                track.x87 = true;
            }
            else if (token == "all")
            {
                track = {.gpr = true, .rip = true, .rflags = true, .xmm = true, .x87 = true};
            }
            else if (!token.empty() && token != "none")
            {
                return Error{fmt::format("Unknown `--track` class {:?} (expected gpr, rip, rflags, xmm, x87, all, or none).", token)};
            }
        }
    }

    CLI result{};

    if (auto seed_text = program.present<std::string>("--seed"))
    {
        auto parsed_seed = parse_cli_seed_argument(*seed_text, result);
        if (!parsed_seed)
        {
            return Error{parsed_seed.error()};
        }
    }

    auto syntax  = program.get<std::string>("--syntax") == "att" ? DisasmSyntax::ATT : DisasmSyntax::Intel;
    auto backend = backend_from_cli(program.get<std::string>("--backend"));
    if (!backend_supports(backend, syntax))
    {
        return Error{fmt::format("The {} backend only emits Intel syntax (use `--syntax intel`).", backend_info(backend).name)};
    }

    result.bytes       = program.present<std::string>("--bytes");
    result.input_file  = program.present<std::string>("--file");
    result.run         = program.get<bool>("--run");
    result.quick       = quick;
    result.format      = format_is_json ? OutputFormat::Json : OutputFormat::Text;
    result.pretty_json = pretty_json;
    result.syntax      = syntax;
    result.backend     = backend;
    result.max_steps   = max_steps;
    result.track       = track;

    return result;
}

namespace
{

std::mutex g_engine_mutex{};

struct Decoded
{
    std::string  text{};
    std::uint8_t length{}; // x86-64 instructions are at most 15 bytes.

    [[nodiscard]] bool valid() const noexcept { return !text.empty() && length != 0; }

    explicit operator bool() const noexcept { return valid(); }
};

// Disassemble one x86-64 instruction with the selected backend and syntax.
Decoded disasm_one(DisasmBackend backend, DisasmSyntax syntax, std::uint64_t address, const std::uint8_t *code, std::size_t size)
{
    Decoded result{};

    if (backend == DisasmBackend::Zydis)
    {
        ZydisDisassembledInstruction ix{};
        auto status = syntax == DisasmSyntax::Intel ? ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64, address, code, size, &ix)
                                                    : ZydisDisassembleATT(ZYDIS_MACHINE_MODE_LONG_64, address, code, size, &ix);
        if (ZYAN_SUCCESS(status))
        {
            result.text   = ix.text;
            result.length = (std::uint8_t)ix.info.length;
        }
    }
    else if (backend == DisasmBackend::Bddisasm)
    {
        INSTRUX ix{};
        char    text[ND_MIN_BUF_SIZE]{};
        if (ND_SUCCESS(NdDecodeEx(&ix, code, size, ND_CODE_64, ND_DATA_64)) && ND_SUCCESS(NdToText(&ix, address, sizeof(text), text)))
        {
            result.text   = text;
            result.length = (std::uint8_t)ix.Length;
        }
    }
    else if (backend == DisasmBackend::Capstone)
    {
        csh handle{};
        if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) == CS_ERR_OK)
        {
            cs_option(handle, CS_OPT_SYNTAX, syntax == DisasmSyntax::ATT ? CS_OPT_SYNTAX_ATT : CS_OPT_SYNTAX_INTEL);

            cs_insn *ix{};
            if (cs_disasm(handle, code, size, address, 1, &ix) > 0)
            {
                result.text   = ix->op_str[0] != '\0' ? std::string{ix->mnemonic} + ' ' + ix->op_str : std::string(ix->mnemonic);
                result.length = (std::uint8_t)ix->size;

                cs_free(ix, 1);
            }

            cs_close(&handle);
        }
    }
    else if (backend == DisasmBackend::Xed)
    {
        [[maybe_unused]] static auto xed_ready = (xed_tables_init(), true);

        xed_state_t state{};
        xed_state_init2(&state, XED_MACHINE_MODE_LONG_64, XED_ADDRESS_WIDTH_64b);

        xed_decoded_inst_t ix{};
        xed_decoded_inst_zero_set_mode(&ix, &state);
        if (xed_decode(&ix, code, (std::uint32_t)size) == XED_ERROR_NONE)
        {
            char text[256]{};
            if (xed_format_context(
                    syntax == DisasmSyntax::ATT ? XED_SYNTAX_ATT : XED_SYNTAX_INTEL, &ix, text, sizeof(text), address, nullptr, nullptr
                )
                != 0)
            {
                result.text   = text;
                result.length = (std::uint8_t)xed_decoded_inst_get_length(&ix);
            }
        }
    }

    // Preserve each decoder's exact text.
    // Never normalize casing/spacing/operands (that native formatting is what we diff between backends). Only trim surrounding whitespace so the
    // panels line up.
    trim(result.text);

    return result;
}

std::string_view fault_access_action(FaultAccess access) noexcept
{
    switch (access)
    {
    case FaultAccess::Read:    return "reading";
    case FaultAccess::Write:   return "writing";
    case FaultAccess::Execute: return "executing";
    default:                   return "accessing";
    }
}

auto format_address_value(const Trace &trace, std::uint64_t address, bool normalized)
{
    if (normalized)
    {
        static auto page_size = vm_page_size();

        auto code_base = trace.seed[Reg::RIP];
        if (code_base != 0 && !trace.code.empty() && address >= code_base)
        {
            auto code_offset = address - code_base;
            if (code_offset < trace.code.size())
            {
                return fmt::format("code+{:04X}", code_offset);
            }

            if (code_offset == trace.code.size())
            {
                return fmt::format("code+{:04X} (end, guard)", code_offset);
            }

            if (code_offset < trace.code.size() + page_size)
            {
                return fmt::format("code+{:04X} (guard)", code_offset);
            }
        }

        auto stack_pointer = trace.seed[Reg::RSP];
        if (stack_pointer != 0)
        {
            if (address >= stack_pointer)
            {
                auto stack_offset = address - stack_pointer;
                if (stack_offset < SCRATCH_STACK_HEADROOM)
                {
                    return fmt::format("stack+{:04X}", stack_offset);
                }

                if (stack_offset == SCRATCH_STACK_HEADROOM)
                {
                    return fmt::format("stack+{:04X} (end, guard)", stack_offset);
                }

                if (stack_offset < SCRATCH_STACK_HEADROOM + page_size)
                {
                    return fmt::format("stack+{:04X} (guard)", stack_offset);
                }
            }
            else
            {
                auto stack_offset = stack_pointer - address;
                if (stack_offset <= SCRATCH_STACK_BYTES - SCRATCH_STACK_HEADROOM)
                {
                    return fmt::format("stack-{:04X}", stack_offset);
                }

                if (stack_offset <= SCRATCH_STACK_BYTES - SCRATCH_STACK_HEADROOM + page_size)
                {
                    return fmt::format("stack-{:04X} (guard)", stack_offset);
                }
            }
        }

        auto data_base = scratch_data_base();
        if (address >= data_base)
        {
            auto data_offset = address - data_base;
            if (data_offset < SCRATCH_DATA_BYTES)
            {
                return fmt::format("data+{:04X}", data_offset);
            }

            if (data_offset == SCRATCH_DATA_BYTES)
            {
                return fmt::format("data+{:04X} (end, guard)", data_offset);
            }

            if (data_offset < SCRATCH_DATA_BYTES + page_size)
            {
                return fmt::format("data+{:04X} (guard)", data_offset);
            }
        }
        else if (data_base - address <= page_size)
        {
            return fmt::format("data-{:04X} (guard)", data_base - address);
        }
    }

    return format_hex64_string(address);
}

} // namespace

// Build one decoder's instruction boundaries from the original input bytes.
std::vector<HistoryRow> build_history(const Trace &trace, DisasmBackend backend, DisasmSyntax syntax)
{
    if (trace.code.empty() || trace.instrumentation_detected)
    {
        return {};
    }

    std::vector<HistoryRow> result{};
    auto                    base     = trace.seed[Reg::RIP];
    auto                    code_end = base + trace.code.size();
    std::vector<bool>       event_at(trace.code.size(), false);
    for (auto &&event : trace.execution_events)
    {
        if (event.rip >= base && event.rip < code_end)
        {
            event_at[event.rip - base] = true;
        }
    }

    auto disasm_at = [&trace, backend, syntax, base](std::uint64_t rip)
    {
        HistoryRow row{};
        row.rip    = rip;
        row.offset = rip - base;

        auto  available = trace.code.size() - row.offset;
        auto *bytes     = trace.code.data() + row.offset;
        auto  decoded   = disasm_one(backend, syntax, rip, bytes, available);
        if (decoded && decoded.length <= available)
        {
            row.text   = std::move(decoded.text);
            row.length = decoded.length;
        }
        else
        {
            row.text   = "(bad)";
            row.length = 1;
        }

        return row;
    };

    auto fill_gap = [&](std::uint64_t from, std::uint64_t to)
    {
        while (from < to)
        {
            auto row      = disasm_at(from);
            auto next     = from + row.length;
            auto boundary = std::min(next, to);

            // Split a static decode before any later execution event that starts inside it.
            for (auto address = from + 1; address < boundary; ++address)
            {
                if (event_at[address - base])
                {
                    boundary = address;

                    break;
                }
            }

            if (boundary != next)
            {
                row.text   = "(data)";
                row.kind   = HistoryRowKind::Data;
                row.length = boundary - from;

                next = boundary;
            }

            if (!event_at[from - base])
            {
                result.emplace_back(std::move(row));
            }

            from = next;
        }
    };

    auto linear = base;
    for (std::size_t event_index{}; event_index < trace.execution_events.size(); ++event_index)
    {
        auto &event = trace.execution_events[event_index];

        // Instruction-fetch faults can report an address outside the original input.
        if (event.rip < base || event.rip >= code_end)
        {
            continue;
        }

        if (event.rip > linear)
        {
            fill_gap(linear, event.rip);
        }

        auto row                  = disasm_at(event.rip);
        row.kind                  = event.kind == ExecutionEventKind::Completed ? HistoryRowKind::Reached : HistoryRowKind::Faulted;
        row.execution_event_index = event_index;

        linear = std::max(linear, event.rip + row.length);

        result.emplace_back(std::move(row));
    }

    fill_gap(linear, code_end);

    return result;
}

// TODO Move machine-code execution to a worker process.
//      In-process code can mutate or terminate the host.
//
// A blocking system call can prevent the next single-step trap.
// Execute `code` one instruction at a time, capturing register state after each.
// Single-step faults and instruction-count runaways are contained.
Trace run_engine(
    std::span<std::uint8_t>    code,
    const Registers           &seed,
    std::size_t                max_steps,
    DisasmBackend              backend,
    DisasmSyntax               syntax,
    bool                       seed_data_pointers,
    std::optional<std::size_t> stop_offset
)
{
    Trace trace{};
    trace.cpu_fingerprint     = host_cpu_fingerprint();
    trace.requested_seed      = seed;
    trace.requested_backend   = backend;
    trace.requested_syntax    = syntax;
    trace.effective_max_steps = std::min<std::size_t>(max_steps != 0 ? max_steps : 1, MAX_STEPS_LIMIT);
    trace.seed_data_pointers  = seed_data_pointers;
    trace.code.assign(code.begin(), code.end());

    std::lock_guard engine_lock{g_engine_mutex};

    auto result = run_platform_steps({
        .code               = code,
        .seed               = seed,
        .max_steps          = trace.effective_max_steps,
        .stop_offset        = stop_offset,
        .seed_data_pointers = seed_data_pointers,
    });

    trace.seed                     = result.seed;
    trace.outcome                  = result.outcome;
    trace.instrumentation_detected = result.instrumentation_detected;
    trace.fault_memory_address     = result.fault_memory_address;
    trace.fault_access             = result.fault_access;

    if (result.instrumentation_detected)
    {
        trace.message = result.error + " No trace was recorded.";

        return trace;
    }

    if (!result.error.empty())
    {
        trace.message = std::move(result.error);

        return trace;
    }

    trace.execution_events.reserve(result.steps.size());

    for (auto &&platform_step : result.steps)
    {
        trace.execution_events.emplace_back(
            ExecutionEvent{
                .rip       = platform_step.rip,
                .registers = platform_step.registers,
                .kind      = platform_step.faulted ? ExecutionEventKind::Faulted : ExecutionEventKind::Completed,
            }
        );
    }

    auto executed =
        std::ranges::count_if(trace.execution_events, [](const ExecutionEvent &event) { return event.kind == ExecutionEventKind::Completed; });

    switch (trace.outcome)
    {
    case Outcome::Stopped:
    {
        if (result.stopped_at_target)
        {
            trace.message     = fmt::format("Stopped at selected row (0x{:X}) - {} executed.", result.stop_address, executed);
            trace.stop_reason = "Stopped here - selected row";
        }
        else
        {
            trace.message     = fmt::format("Stopped at int3 (0x{:X}) - {} executed.", result.stop_address, executed);
            trace.stop_reason = "Stopped here - int3 breakpoint";
        }

        trace.stop_address = result.stop_address;

        break;
    }

    case Outcome::Faulted:
    {
        std::string memory_detail{};
        if (result.fault_memory_address)
        {
            auto absolute   = format_hex64_string(*result.fault_memory_address);
            auto normalized = format_address_value(trace, *result.fault_memory_address, true);
            auto memory     = normalized == absolute ? absolute : fmt::format("{} ({})", absolute, normalized);

            memory_detail = fmt::format(" while {} {}", fault_access_action(result.fault_access), memory);
        }

        trace.message      = fmt::format("{} at 0x{:X}{} ({} executed).", result.fault_name, result.stop_address, memory_detail, executed);
        trace.stop_reason  = fmt::format("Faulted here - {}", result.fault_name);
        trace.stop_address = result.stop_address;

        break;
    }

    case Outcome::AbortedCap:
    {
        trace.message     = fmt::format("Hit step cap ({} executed).", trace.effective_max_steps);
        trace.stop_reason = "Not reached - Step cap reached";

        break;
    }

    default:
    {
        trace.message = fmt::format("Finished - {} executed.", executed);

        auto next_rip     = trace.execution_events.empty() ? trace.seed[Reg::RIP] : trace.execution_events.back().registers[Reg::RIP];
        auto code_end     = trace.seed[Reg::RIP] + trace.code.size();
        trace.stop_reason = next_rip == code_end ? "Reached end of input" : "Execution left input range";

        break;
    }
    }

    return trace;
}

namespace
{

struct UI
{
    // Input.
    std::string                    code{};                           // Formatted byte input.
    DisasmBackend                  backend   = DisasmBackend::Zydis; // Active decode backend (Zydis default).
    DisasmSyntax                   syntax    = DisasmSyntax::Intel;  // Active disassembly syntax (Intel default).
    std::size_t                    max_steps = DEFAULT_MAX_STEPS;    // Single-step cap for the next run.
    std::array<GPRSeed, GPR_COUNT> seed_gpr{};                       // Editable `RAX..R15` seeds (per-slice hex).
    std::uint64_t                  seed_flags{};                     // Seeded status flags (`CF/PF/AF/ZF/SF/DF/OF`). Applied on run.
    std::array<std::string, 16>    seed_xmm{};                       // Seeded `XMM0..15` as hex or decimal with optional precision.
    std::array<std::string, 8>     seed_st{};                        // Seeded `ST0..ST7` as 80-bit hex or decimal with optional precision.
    FloatingEnvironmentSeed        seed_floating_environment{};      // Optional `MXCSR` and x87 control-word seeds.
    bool                           seed_data_pointers   = true;      // Point `RDI`/`RSI` at the scratch data base when left unseeded.
    bool                           normalized_addresses = true;      // Normalize code and scratch data addresses.

    // Execution result and timeline navigation.
    Trace                                               trace{};
    std::int32_t                                        cursor{}; // Timeline position, bound to history menu.
    std::vector<std::string>                            history{};
    std::vector<HistoryRow>                             history_rows{};
    std::int32_t                                        history_tab{}; // Main tab is 0, decoder tabs are `1..BACKEND_COUNT`.
    std::int32_t                                        previous_history_tab{};
    std::array<std::vector<std::string>, BACKEND_COUNT> history_backend{};
    std::array<std::vector<HistoryRow>, BACKEND_COUNT>  history_backend_rows{};

    // Register panel view state.
    std::int32_t                        register_tab{};    // `0=GPR`, `1=SSE`, `2=x87`.
    std::array<std::int32_t, GPR_COUNT> gpr_depth{};       // Sub-register tree depth per GPR (`0=collapsed..3=8-bit`).
    std::array<bool, 16>                xmm_expand{};      // SSE panel per-XMM `f32x4` drill-down (`f64x2` always shows when a trace exists).
    std::array<bool, 8>                 st_expand{};       // x87 panel per-ST narrowed float (`Real4`) drill-down.
    std::array<std::int32_t, 3>         register_scroll{}; // Registers panel scroll offset (row), per tab (`GPR`/`SSE`/`x87`).

    // Status line and modal.
    std::string status = "Idle - Edit bytes, then Run.";
    bool        show_instrumentation_warning{};
    bool        show_about{};    // About modal visible.
    bool        show_settings{}; // Settings modal visible.
};

// A top-aligned scrolling viewport, on the public `ftxui::Node` API (`ftxui` has no top-align frame).
// Plain `ftxui::yframe` centers the focused row, leaving half a viewport of dead travel at each end. That suits following a cursor but not a wheel
// offset, so this lands `offset` on the viewport's first visible row.
class ScrollViewport final : public ftxui::Node
{
private:
    std::int32_t &m_offset;

public:
    ScrollViewport(ftxui::Element child, std::int32_t &offset) : Node{ftxui::Elements{std::move(child)}}, m_offset{offset} {}

    void SetBox(ftxui::Box box) override
    {
        Node::SetBox(box);

        auto external_dimy = box.y_max - box.y_min;
        auto internal_dimy = std::max(requirement_.min_y, external_dimy);
        auto dy            = std::max(0, std::min(internal_dimy - external_dimy - 1, m_offset));

        // Clamp against the real viewport height (known only here) and write it back.
        // The wheel handler grows the offset with no upper bound, so without this it drifts past the end and reversing then stalls.
        m_offset = dy;

        auto children_box  = box;
        children_box.y_min = box.y_min - dy;
        children_box.y_max = box.y_min + internal_dimy - dy;

        children_[0]->SetBox(children_box);
    }

    void Render(ftxui::Screen &screen) override
    {
        // Clip to our box so the taller child does not draw over the toggle above or the panels beside it.
        ftxui::AutoReset stencil(&screen.stencil, ftxui::Box::Intersection(box_, screen.stencil));

        children_[0]->Render(screen);
    }
};

auto scroll_viewport(ftxui::Element child, std::int32_t &offset) { return std::make_shared<ScrollViewport>(std::move(child), offset); }

// Wraps the register tabs, scrolled by a per-tab offset getter (`ui.register_scroll`) driven by the wheel.
// When a seed `Input` is genuinely focused (`has_real_focus`), `ftxui::yframe` follows its cursor so editing scrolls into view. Otherwise
// `ScrollViewport` top-aligns to the offset. The pick is made in C++, not by a competing `ftxui::focus()` marker, which can never win ftxui's
// tie-break (every `Input` marks its own cursor cell focusable and the active tab is the sole focusable child of its `Container::Tab`).
// `focusPositionRelative` was tried instead and clobbered the cursor while typing.
class ScrollerBase final : public ftxui::ComponentBase
{
private:
    std::function<std::int32_t &()> m_offset{};
    std::function<bool()>           m_has_real_focus{};

public:
    ScrollerBase(ftxui::Component child, std::function<std::int32_t &()> offset, std::function<bool()> has_real_focus) :
        m_offset{std::move(offset)}, m_has_real_focus{std::move(has_real_focus)}
    {
        Add(std::move(child));
    }

    ftxui::Element OnRender() override
    {
        auto background = ChildAt(0)->Render();

        if (m_has_real_focus())
        {
            return std::move(background) | ftxui::vscroll_indicator | ftxui::yframe;
        }

        return scroll_viewport(std::move(background) | ftxui::vscroll_indicator, m_offset());
    }
};

auto make_scroller(ftxui::Component child, std::function<std::int32_t &()> offset, std::function<bool()> has_real_focus)
{
    return ftxui::Make<ScrollerBase>(std::move(child), std::move(offset), std::move(has_real_focus));
}

// A non-focusable no-op.
// `TakeFocus()` on it (arrow-key `MoveSelector` skips it, but `TakeFocus` ignores `Focusable()`) steals its container's active-child slot from a real
// seed `Input`, clearing what `ScrollerBase` reads as focus. Targeted by `Escape`, blank-space clicks, and each seed `on_enter`.
class FocusSink final : public ftxui::ComponentBase
{
public:
    bool Focusable() const override { return false; }

    ftxui::Element OnRender() override { return ftxui::emptyElement(); }
};

auto make_focus_sink() { return ftxui::Make<FocusSink>(); }

} // namespace

// Composes a 64-bit seed from the per-slice text.
// The widest non-empty field is the base, each narrower non-empty field overlays its bits (`EAX` refines `RAX`, `AL` refines `AX`, and so on). Empty
// fields are ignored. A malformed field is skipped (its bits stay whatever the wider field set, never zeroed over) and appended to `errors` as
// `{label}: {reason}`.
std::uint64_t compose_gpr_seed(const GPRSeed &seed, std::string_view label, std::vector<std::string> &errors)
{
    std::uint64_t result{};

    auto overlay = [&result, &errors, &label](std::string_view text, std::uint64_t mask, std::int32_t shift)
    {
        if (text.empty())
        {
            return;
        }

        if (auto value = parse_seed(text))
        {
            result = (result & ~mask) | (*value << shift & mask);
        }
        else
        {
            errors.emplace_back(fmt::format("{}: {}", label, value.error()));
        }
    };

    overlay(seed.full, ~(std::uint64_t)0, 0);
    overlay(seed.dword, 0xFFFF'FFFF, 0);
    overlay(seed.word, 0xFFFF, 0);
    overlay(seed.byte_high, 0xFF00, 8);
    overlay(seed.byte_low, 0xFF, 0);

    return result;
}

// Composes seeded `Registers` from the text fields shared by the TUI's Run and `--quick`.
// A malformed field is left at its default, never blocking the run, and reported in `errors`.
Registers compose_seed(
    const std::array<GPRSeed, GPR_COUNT> &seed_gpr,
    std::uint64_t                         seed_flags,
    const std::array<std::string, 16>    &seed_xmm,
    const std::array<std::string, 8>     &seed_st,
    const FloatingEnvironmentSeed        &seed_floating_environment,
    std::vector<std::string>             &errors
)
{
    Registers seed{};

    if (!seed_floating_environment.mxcsr.empty())
    {
        auto value = parse_mxcsr_seed(seed_floating_environment.mxcsr);
        if (value)
        {
            seed.mxcsr = *value;
        }
        else
        {
            errors.emplace_back(value.error());
        }
    }

    if (!seed_floating_environment.fpu_control_word.empty())
    {
        auto value = parse_fpu_control_word_seed(seed_floating_environment.fpu_control_word);
        if (value)
        {
            seed.fpu_control_word = *value;
        }
        else
        {
            errors.emplace_back(value.error());
        }
    }

    for (std::size_t i{}; i < GPR_COUNT; ++i)
    {
        seed.gpr[i] = compose_gpr_seed(seed_gpr[i], REG_NAMES[i], errors);
    }

    seed[Reg::RFLAGS] = seed_flags;

    for (std::size_t i{}; i < seed.xmm.size(); ++i)
    {
        if (auto value = compose_xmm_seed(seed_xmm[i]))
        {
            seed.xmm[i] = *value;
        }
        else
        {
            errors.emplace_back(fmt::format("XMM{}: {}", i, value.error()));
        }
    }

    for (std::size_t i{}; i < seed.st.size(); ++i)
    {
        if (!seed_st[i].empty())
        {
            if (auto value = compose_st_seed(seed_st[i]))
            {
                seed.st[i]                  = *value;
                seed.fpu_tag_word_abridged |= (std::uint8_t)(1 << i);
            }
            else
            {
                errors.emplace_back(fmt::format("ST{}: {}", i, value.error()));
            }
        }
    }

    return seed;
}

const auto &history_rows_at(const UI &ui, std::int32_t tab) noexcept
{
    return tab == 0 ? ui.history_rows : ui.history_backend_rows[(std::size_t)tab - 1];
}

const auto &history_lines_at(const UI &ui, std::int32_t tab) noexcept { return tab == 0 ? ui.history : ui.history_backend[(std::size_t)tab - 1]; }

bool history_selection_is_static(const UI &ui) noexcept
{
    auto &rows = history_rows_at(ui, ui.history_tab);
    if (ui.cursor <= 0 || (std::size_t)ui.cursor > rows.size())
    {
        return false;
    }

    auto kind = rows[(std::size_t)ui.cursor - 1].kind;

    return kind == HistoryRowKind::NotReached || kind == HistoryRowKind::Data;
}

const auto &history_state_at(const UI &ui, std::size_t position) noexcept
{
    auto &rows = history_rows_at(ui, ui.history_tab);
    position   = std::min(position, rows.size());

    while (position > 0)
    {
        auto &row = rows[--position];
        if (row.execution_event_index && *row.execution_event_index < ui.trace.execution_events.size())
        {
            return ui.trace.execution_events[*row.execution_event_index].registers;
        }
    }

    return ui.trace.seed;
}

struct HistorySelection
{
    std::uint64_t              rip{};
    std::optional<std::size_t> execution_event_index{};
    bool                       has_row{};
};

auto history_selection(const UI &ui, std::int32_t tab) noexcept
{
    HistorySelection result{};
    auto            &rows = history_rows_at(ui, tab);

    if (ui.cursor > 0 && (std::size_t)ui.cursor <= rows.size())
    {
        auto &row                    = rows[(std::size_t)ui.cursor - 1];
        result.rip                   = row.rip;
        result.execution_event_index = row.execution_event_index;
        result.has_row               = true;
    }

    return result;
}

void restore_history_selection(UI &ui, const HistorySelection &selection) noexcept
{
    if (!selection.has_row)
    {
        ui.cursor = 0;

        return;
    }

    auto &rows = history_rows_at(ui, ui.history_tab);
    for (std::size_t i{}; i < rows.size(); ++i)
    {
        auto &row = rows[i];

        if (selection.execution_event_index)
        {
            if (row.execution_event_index == selection.execution_event_index)
            {
                ui.cursor = (std::int32_t)i + 1;

                return;
            }
        }
        else if (!row.execution_event_index && row.rip <= selection.rip && selection.rip - row.rip < row.length)
        {
            ui.cursor = (std::int32_t)i + 1;

            return;
        }
    }

    ui.cursor = std::min(ui.cursor, (std::int32_t)rows.size());
}

auto build_history_lines(const Trace &trace, const std::vector<HistoryRow> &rows, bool relative_addresses)
{
    std::vector<std::string> lines{"- Initial (seed)"};

    std::size_t bytes_width{};
    for (auto &&row : rows)
    {
        bytes_width = std::max(bytes_width, row.length);
    }

    if (bytes_width != 0)
    {
        bytes_width = bytes_width * 3 - 1;
    }

    std::size_t execution_position{};
    for (auto &&row : rows)
    {
        auto bytes   = fmt::format("{:02X}", fmt::join(std::span{trace.code}.subspan(row.offset, row.length), " "));
        auto address = relative_addresses ? fmt::format("code+{:04X}", row.offset) : fmt::format("{:08X}", row.rip);
        if (row.kind == HistoryRowKind::Reached)
        {
            ++execution_position;

            lines.emplace_back(fmt::format("{:>3}  {}  {:<{}}  {}", execution_position, address, bytes, bytes_width, row.text));
        }
        else
        {
            auto marker  = row.kind == HistoryRowKind::Faulted ? "!" : "-";
            auto is_stop = row.rip == trace.stop_address && !trace.stop_reason.empty();
            auto note    = is_stop ? trace.stop_reason : std::string_view{"Not reached"};

            lines.emplace_back(fmt::format("{:>3}  {}  {:<{}}  {} ({})", marker, address, bytes, bytes_width, row.text, note));
        }
    }

    return lines;
}

// Rebuild every history from the immutable execution events with each decoder's own instruction boundaries.
void rebuild_history(UI &ui)
{
    auto selection = history_selection(ui, ui.history_tab);

    ui.history_rows = build_history(ui.trace, ui.backend, ui.syntax);
    ui.history      = build_history_lines(ui.trace, ui.history_rows, ui.normalized_addresses);

    for (std::size_t backend{}; backend < BACKEND_COUNT; ++backend)
    {
        auto syntax                      = backend_supports((DisasmBackend)backend, ui.syntax) ? ui.syntax : DisasmSyntax::Intel;
        ui.history_backend_rows[backend] = build_history(ui.trace, (DisasmBackend)backend, syntax);
        ui.history_backend[backend]      = build_history_lines(ui.trace, ui.history_backend_rows[backend], ui.normalized_addresses);
    }

    restore_history_selection(ui, selection);
}

std::int32_t run_tui(const CLI &cli)
{
    UI ui{};

    // Warn before the first run.
    // The platform preflight checks again immediately before execution.
    ui.show_instrumentation_warning = instrumentation_detected();

    if (cli.bytes)
    {
        ui.code = *cli.bytes;
    }
    else if (cli.input_file)
    {
        auto input = read_binary_input(*cli.input_file);
        if (input)
        {
            ui.code = format_code_text(*input);
        }
        else
        {
            ui.status = fmt::format("Error: {}.", input.error());
        }
    }

    ui.backend   = cli.backend;
    ui.syntax    = cli.syntax;
    ui.max_steps = cli.max_steps;
    ui.history   = {"- (No trace - Press Run)"};

    for (auto &&backend_history : ui.history_backend)
    {
        backend_history = {"- (No trace - Press Run)"};
    }

    // Pre-fill the TUI seed fields from `--seed`.
    // Each `GPRSeed` maps straight onto the panel's per-slice inputs.
    ui.seed_gpr                  = cli.seed_gpr;
    ui.seed_flags                = cli.seed_flags;
    ui.seed_xmm                  = cli.seed_xmm;
    ui.seed_st                   = cli.seed_st;
    ui.seed_floating_environment = cli.seed_floating_environment;

    auto execute = [&ui](std::optional<std::size_t> stop_offset)
    {
        auto decoded = parse_code_text(ui.code);
        if (!decoded)
        {
            ui.status = fmt::format("Error: {}.", decoded.error());

            return;
        }

        if (stop_offset && *decoded != ui.trace.code)
        {
            ui.status = "Run first to refresh History after editing bytes.";

            return;
        }

        // Malformed seed text is lenient here (unlike `--seed`'s hard error at CLI parse time).
        // The run still proceeds with that field left unseeded, and the reason is appended to the status line instead of being silently discarded.
        std::vector<std::string> seed_errors{};
        auto                     seed = compose_seed(ui.seed_gpr, ui.seed_flags, ui.seed_xmm, ui.seed_st, ui.seed_floating_environment, seed_errors);

        ui.trace  = run_engine(*decoded, seed, ui.max_steps, ui.backend, ui.syntax, ui.seed_data_pointers, stop_offset);
        ui.cursor = 0;

        if (ui.trace.instrumentation_detected)
        {
            ui.show_instrumentation_warning = true;
        }

        rebuild_history(ui);

        if (stop_offset && ui.trace.stop_address == ui.trace.seed[Reg::RIP] + *stop_offset)
        {
            restore_history_selection(ui, HistorySelection{.rip = ui.trace.stop_address, .has_row = true});
        }

        ui.status = fmt::format("{} bytes - {}", decoded->size(), ui.trace.message);
        if (!seed_errors.empty())
        {
            ui.status += fmt::format(" (seed errors, left unseeded: {})", fmt::join(seed_errors, "; "));
        }
    };

    auto run = [&execute] { execute({}); };

    // Advance to the next completed instruction or partial fault state.
    // Not-reached rows stay browsable through the history menu.
    auto step = [&ui]() noexcept
    {
        auto &rows = history_rows_at(ui, ui.history_tab);
        for (std::int32_t i = ui.cursor + 1; i <= (std::int32_t)rows.size(); ++i)
        {
            auto kind = rows[(std::size_t)i - 1].kind;
            if (kind == HistoryRowKind::Reached || kind == HistoryRowKind::Faulted)
            {
                ui.cursor = i;

                return;
            }
        }
    };

    // Step back to the previous completed instruction or partial fault state, or the seed.
    auto back = [&ui]() noexcept
    {
        auto &rows = history_rows_at(ui, ui.history_tab);
        for (std::int32_t i = ui.cursor - 1; i >= 1; --i)
        {
            auto kind = rows[(std::size_t)i - 1].kind;
            if (kind == HistoryRowKind::Reached || kind == HistoryRowKind::Faulted)
            {
                ui.cursor = i;

                return;
            }
        }

        ui.cursor = 0;
    };

    auto reset = [&ui]
    {
        ui.trace                = {};
        ui.history              = {"- (No trace - Press Run)"};
        ui.history_rows         = {};
        ui.previous_history_tab = 0;

        for (auto &&backend_history : ui.history_backend)
        {
            backend_history = {"- (No trace - Press Run)"};
        }

        ui.history_backend_rows = {};
        ui.cursor               = 0;
        ui.history_tab          = 0;
        ui.register_tab         = 0;
        ui.gpr_depth            = {};
        ui.xmm_expand           = {};
        ui.st_expand            = {};
        ui.register_scroll      = {};
        ui.status               = "Idle - Edit bytes, then Run.";
    };

    std::string syntax_label  = ui.syntax == DisasmSyntax::ATT ? "Syntax: AT&T" : "Syntax: Intel";
    auto        backend_label = fmt::format("Backend: {}", backend_info(ui.backend).name);

    auto toggle_syntax = [&ui, &syntax_label]
    {
        auto next = ui.syntax == DisasmSyntax::Intel ? DisasmSyntax::ATT : DisasmSyntax::Intel;
        if (!backend_supports(ui.backend, next))
        {
            ui.status = fmt::format("AT&T syntax is unavailable on the {} backend.", backend_info(ui.backend).name);

            return;
        }

        ui.syntax = next;

        syntax_label = ui.syntax == DisasmSyntax::ATT ? "Syntax: AT&T" : "Syntax: Intel";

        if (!ui.history_rows.empty())
        {
            rebuild_history(ui);
        }
    };

    // `direction` uses +1 to cycle forward and -1 to cycle back.
    // Fall back to Intel if the selected backend cannot render the active syntax.
    auto cycle_backend = [&ui, &syntax_label, &backend_label](std::int32_t direction)
    {
        auto count = (std::int32_t)BACKENDS.size();

        ui.backend = (DisasmBackend)(((std::int32_t)ui.backend + direction + count) % count);

        backend_label = fmt::format("Backend: {}", backend_info(ui.backend).name);

        if (!backend_supports(ui.backend, ui.syntax))
        {
            ui.syntax = DisasmSyntax::Intel;

            syntax_label = "Syntax: Intel";
        }

        if (!ui.history_rows.empty())
        {
            rebuild_history(ui);
        }
    };

    ftxui::InputOption code_option{};
    code_option.multiline   = false;
    code_option.placeholder = "Hex bytes, e.g. 48 FF C0";
    code_option.on_enter    = run;
    code_option.transform   = [](ftxui::InputState state)
    {
        if (state.is_placeholder)
        {
            state.element |= ftxui::dim;
        }

        state.element |= ftxui::inverted;
        if (state.focused)
        {
            state.element |= ftxui::bold;
        }
        else if (state.hovered)
        {
            state.element |= ftxui::underlined;
        }

        return state.element;
    };

    auto code_input = ftxui::Input(&ui.code, code_option);

    struct GPRSeedInputs
    {
        ftxui::Component full{};
        ftxui::Component dword{};
        ftxui::Component word{};
        ftxui::Component byte_high{};
        ftxui::Component byte_low{};
    };

    // One seed `ftxui::Input` per editable GPR slice, shown with the drill-down depth (`ftxui::Maybe`-gated) and composed back into `ui.seed_gpr[i]`
    // at run.
    // `on_enter` drops focus like Escape, so Enter also leaves the field.
    std::array<GPRSeedInputs, GPR_COUNT> seed_inputs{};
    ftxui::Components                    seed_components{};
    auto                                 gpr_focus_sink = make_focus_sink();

    ftxui::InputOption seed_option{};
    seed_option.multiline   = false;
    seed_option.placeholder = "0";
    seed_option.on_enter    = [gpr_focus_sink]() noexcept { gpr_focus_sink->TakeFocus(); };

    for (std::size_t i{}; i < GPR_COUNT; ++i)
    {
        // `RSP` is engine-controlled (always reset to the scratch-stack top).
        // No seed input.
        if (i == (std::size_t)Reg::RSP)
        {
            continue;
        }

        seed_inputs[i].full     = ftxui::Input(&ui.seed_gpr[i].full, seed_option);
        seed_inputs[i].dword    = ftxui::Input(&ui.seed_gpr[i].dword, seed_option);
        seed_inputs[i].word     = ftxui::Input(&ui.seed_gpr[i].word, seed_option);
        seed_inputs[i].byte_low = ftxui::Input(&ui.seed_gpr[i].byte_low, seed_option);

        seed_components.emplace_back(seed_inputs[i].full);
        seed_components.emplace_back(ftxui::Maybe(seed_inputs[i].dword, [&ui, i]() noexcept { return ui.gpr_depth[i] >= 1; }));
        seed_components.emplace_back(ftxui::Maybe(seed_inputs[i].word, [&ui, i]() noexcept { return ui.gpr_depth[i] >= 2; }));

        // The high-byte aliases (`AH`/`BH`/`CH`/`DH`) only exist for the legacy `A`/`B`/`C`/`D` registers.
        if (i < 4)
        {
            seed_inputs[i].byte_high = ftxui::Input(&ui.seed_gpr[i].byte_high, seed_option);

            seed_components.emplace_back(ftxui::Maybe(seed_inputs[i].byte_high, [&ui, i]() noexcept { return ui.gpr_depth[i] >= 3; }));
        }

        seed_components.emplace_back(ftxui::Maybe(seed_inputs[i].byte_low, [&ui, i]() noexcept { return ui.gpr_depth[i] >= 3; }));
    }

    seed_components.emplace_back(gpr_focus_sink);

    auto seed_selected  = (std::int32_t)seed_components.size() - 1;
    auto seed_container = ftxui::Container::Vertical(seed_components, &seed_selected);

    // `SSE`/`x87` seed inputs, one per register, are composed on run.
    // Separate `InputOption`s let `on_enter` target each panel's `FocusSink`.
    auto xmm_focus_sink = make_focus_sink();

    ftxui::InputOption xmm_seed_option{};
    xmm_seed_option.multiline   = false;
    xmm_seed_option.placeholder = "0";
    xmm_seed_option.on_enter    = [xmm_focus_sink]() noexcept { xmm_focus_sink->TakeFocus(); };

    std::array<ftxui::Component, 16> xmm_inputs{};
    ftxui::Components                xmm_seed_components{};
    for (std::size_t i{}; i < xmm_inputs.size(); ++i)
    {
        xmm_inputs[i] = ftxui::Input(&ui.seed_xmm[i], xmm_seed_option);

        xmm_seed_components.emplace_back(xmm_inputs[i]);
    }

    auto mxcsr_seed_option        = xmm_seed_option;
    mxcsr_seed_option.placeholder = "1F80";

    auto mxcsr_input = ftxui::Input(&ui.seed_floating_environment.mxcsr, mxcsr_seed_option);
    xmm_seed_components.emplace_back(mxcsr_input);

    xmm_seed_components.emplace_back(xmm_focus_sink);

    auto xmm_seed_selected  = (std::int32_t)xmm_seed_components.size() - 1;
    auto xmm_seed_container = ftxui::Container::Vertical(xmm_seed_components, &xmm_seed_selected);
    auto st_focus_sink      = make_focus_sink();

    ftxui::InputOption st_seed_option{};
    st_seed_option.multiline   = false;
    st_seed_option.placeholder = "0";
    st_seed_option.on_enter    = [st_focus_sink]() noexcept { st_focus_sink->TakeFocus(); };

    std::array<ftxui::Component, 8> st_inputs{};
    ftxui::Components               st_seed_components{};
    for (std::size_t i{}; i < st_inputs.size(); ++i)
    {
        st_inputs[i] = ftxui::Input(&ui.seed_st[i], st_seed_option);

        st_seed_components.emplace_back(st_inputs[i]);
    }

    auto fpu_control_word_seed_option        = st_seed_option;
    fpu_control_word_seed_option.placeholder = "037F";

    auto fpu_control_word_input = ftxui::Input(&ui.seed_floating_environment.fpu_control_word, fpu_control_word_seed_option);
    st_seed_components.emplace_back(fpu_control_word_input);

    st_seed_components.emplace_back(st_focus_sink);

    auto st_seed_selected  = (std::int32_t)st_seed_components.size() - 1;
    auto st_seed_container = ftxui::Container::Vertical(st_seed_components, &st_seed_selected);

    auto run_to_selected_row = [&ui, &execute]
    {
        auto &rows = history_rows_at(ui, ui.history_tab);
        if (ui.cursor <= 0 || (std::size_t)ui.cursor > rows.size())
        {
            ui.status = "Select a History row to run to.";

            return;
        }

        execute(rows[(std::size_t)ui.cursor - 1].offset);
    };

    auto copy_history_row = [&ui]
    {
        auto &rows = history_rows_at(ui, ui.history_tab);
        if (ui.cursor <= 0 || (std::size_t)ui.cursor > rows.size())
        {
            ui.status = "Select a History row to copy.";

            return;
        }

        auto &lines = history_lines_at(ui, ui.history_tab);
        auto  line  = trim(std::string_view{lines[(std::size_t)ui.cursor]});

        ui.status = copy_to_clipboard(line) ? "Copied selected History row to clipboard." : "Clipboard copy failed.";
    };

    auto run_button          = ftxui::Button("Run", run, ftxui::ButtonOption::Ascii());
    auto run_to_button       = ftxui::Button("Run to row", run_to_selected_row, ftxui::ButtonOption::Ascii());
    auto step_button         = ftxui::Button("Step", step, ftxui::ButtonOption::Ascii());
    auto back_button         = ftxui::Button("Back", back, ftxui::ButtonOption::Ascii());
    auto copy_history_button = ftxui::Button("Copy row", copy_history_row, ftxui::ButtonOption::Ascii());
    auto reset_button        = ftxui::Button("Reset", reset, ftxui::ButtonOption::Ascii());

    // Cycling buttons.
    // Left-click advances, right-click (within the button's reflected box) steps back.
    ftxui::Box syntax_button_box{};
    ftxui::Box backend_button_box{};

    auto rightclick_back = [](ftxui::Component button, ftxui::Box *box, auto on_back) noexcept
    {
        return ftxui::CatchEvent(
            std::move(button),
            [box, on_back = std::move(on_back)](ftxui::Event event)
            {
                if (event.is_mouse()
                    && event.mouse().button
                    == ftxui::Mouse::Right
                    && event.mouse().motion
                    == ftxui::Mouse::Pressed
                    && box->Contain(event.mouse().x, event.mouse().y))
                {
                    on_back();

                    return true;
                }

                return false;
            }
        );
    };

    auto syntax_button =
        rightclick_back(ftxui::Button(&syntax_label, toggle_syntax, ftxui::ButtonOption::Ascii()), &syntax_button_box, toggle_syntax);
    auto backend_button = rightclick_back(
        ftxui::Button(
            &backend_label, [&cycle_backend] { cycle_backend(1); }, ftxui::ButtonOption::Ascii()
        ),
        &backend_button_box, [&cycle_backend] { cycle_backend(-1); }
    );

    auto settings_button = ftxui::Button("Settings", [&ui] noexcept { ui.show_settings = true; }, ftxui::ButtonOption::Ascii());
    auto about_button    = ftxui::Button("About", [&ui] noexcept { ui.show_about = true; }, ftxui::ButtonOption::Ascii());

    auto screen      = ftxui::ScreenInteractive::Fullscreen();
    auto quit_button = ftxui::Button("Quit", screen.ExitLoopClosure(), ftxui::ButtonOption::Ascii());
    auto buttons     = ftxui::Container::Horizontal(
        {run_button, run_to_button, step_button, back_button, copy_history_button, reset_button, settings_button, about_button, quit_button}
    );

    std::vector<std::string> history_tab_labels{"Main"};
    for (auto &&info : BACKENDS)
    {
        history_tab_labels.emplace_back(info.name);
    }

    auto history_menu_option = [&ui](std::int32_t tab)
    {
        auto option                     = ftxui::MenuOption::Vertical();
        option.entries_option.transform = [&ui, tab](const ftxui::EntryState &state)
        {
            auto element = ftxui::text((state.active ? "> " : "  ") + state.label);
            if (state.index > 0)
            {
                auto &rows  = history_rows_at(ui, tab);
                auto  index = (std::size_t)state.index - 1;
                if (index < rows.size())
                {
                    auto &row     = rows[index];
                    auto  is_stop = row.rip == ui.trace.stop_address && !ui.trace.stop_reason.empty();

                    if (row.kind == HistoryRowKind::Faulted)
                    {
                        element |= ftxui::color(ftxui::Color::LightCoral);
                    }
                    else if (is_stop)
                    {
                        element |= ftxui::color(ftxui::Color::Yellow);
                    }

                    if ((row.kind == HistoryRowKind::NotReached || row.kind == HistoryRowKind::Data) && !is_stop)
                    {
                        element |= ftxui::dim;
                    }

                    if (row.kind == HistoryRowKind::Faulted || is_stop)
                    {
                        element |= ftxui::bold;
                    }
                }
            }

            if (state.focused)
            {
                element |= ftxui::inverted;
            }

            if (state.active)
            {
                element |= ftxui::bold;
            }

            return element;
        };

        return option;
    };

    std::vector history_menus{ftxui::Menu(&ui.history, &ui.cursor, history_menu_option(0))};
    for (std::size_t i{}; i < BACKEND_COUNT; ++i)
    {
        history_menus.emplace_back(ftxui::Menu(&ui.history_backend[i], &ui.cursor, history_menu_option((std::int32_t)i + 1)));
    }

    auto history_tabs = ftxui::Container::Tab(history_menus, &ui.history_tab);

    auto history_toggle_option      = ftxui::MenuOption::Toggle();
    history_toggle_option.on_change = [&ui]() noexcept
    {
        auto selection = history_selection(ui, ui.previous_history_tab);
        restore_history_selection(ui, selection);

        ui.previous_history_tab = ui.history_tab;
    };

    auto history_toggle = ftxui::Menu(history_tab_labels, &ui.history_tab, history_toggle_option);
    auto history_pane   = ftxui::Container::Vertical({history_toggle, history_tabs});

    // Wheel anywhere in the History box (not just over an entry) steps the timeline cursor.
    // The menu's own frame then follows the selection. A reflected box scopes the wheel to this panel.
    ftxui::Box history_box{};
    auto       history_view = ftxui::CatchEvent(
        history_pane,
        [&ui, &history_box](ftxui::Event event) noexcept
        {
            if (!event.is_mouse() || !history_box.Contain(event.mouse().x, event.mouse().y))
            {
                return false;
            }

            if (event.mouse().button == ftxui::Mouse::WheelUp)
            {
                ui.cursor = std::max(0, ui.cursor - 1);

                return true;
            }

            if (event.mouse().button == ftxui::Mouse::WheelDown)
            {
                ui.cursor = std::min((std::int32_t)history_rows_at(ui, ui.history_tab).size(), ui.cursor + 1);

                return true;
            }

            return false;
        }
    );

    // Clickable name-cell regions for the `GPR` sub-register tree.
    // Filled during render, hit-tested on click.
    struct GPRHit
    {
        ftxui::Box   box{};
        std::int32_t reg{};
        std::int32_t level{};
    };

    std::vector<GPRHit> gpr_hits{};

    // Clickable `XMM` name cells for the `SSE` lane drill-down.
    // Filled during render, hit-tested on click to toggle the lane view.
    struct XMMHit
    {
        ftxui::Box   box{};
        std::int32_t reg{};
    };

    std::vector<XMMHit> xmm_hits{};

    // Clickable `ST` name cells for the x87 narrowed-float drill-down.
    // Filled during render, hit-tested on click to toggle the row.
    struct STHit
    {
        ftxui::Box   box{};
        std::int32_t reg{};
    };

    std::vector<STHit> st_hits{};

    // Clickable register-value cells across all three panels.
    // A left-click copies the raw value. Ctrl-left-click copies a normalized address when the full value points into a sandbox region.
    struct CopyHit
    {
        ftxui::Box                 box{};
        std::string                text{};
        std::optional<std::string> normalized{};
    };

    std::vector<CopyHit> copy_hits{};

    auto copy_cell = [&copy_hits](const ftxui::Element &cell, std::string text, std::optional<std::string> normalized = {})
    {
        copy_hits.emplace_back(CopyHit{.text = std::move(text), .normalized = std::move(normalized)});

        return cell | ftxui::reflect(copy_hits.back().box);
    };

    auto render_registers = [&ui, &seed_inputs, &gpr_hits, &copy_hits, &copy_cell]
    {
        std::vector<ftxui::Elements> rows{ftxui::Elements{ftxui::text("Seed"), ftxui::text("Register"), ftxui::text("Value")}};

        auto  position  = (std::size_t)ui.cursor;
        auto  has_trace = !ui.history_rows.empty();
        auto &current   = history_state_at(ui, position);
        auto &previous  = history_state_at(ui, position == 0 ? 0 : position - 1);

        gpr_hits.clear();

        // One per seedable `GPR` (all but `RSP`, which is flat), up to 3 clickable levels each.
        // Reserve so `ftxui::reflect()` box refs stay stable.
        gpr_hits.reserve((GPR_COUNT - 1) * 3);

        copy_hits.clear();

        // Every value cell is a copy target.
        // Up to 5 rows per `GPR` (`full`/`dword`/`word`/`AH`/`AL`) plus the 3 flat rows. `GPR_COUNT * 5` is a safe upper bound so `ftxui::reflect()`
        // box refs stay stable.
        copy_hits.reserve(GPR_COUNT * 5);

        auto make_value = [has_trace](std::string_view text, bool changed) noexcept
        {
            auto cell = ftxui::text(text);

            return has_trace && changed ? cell | ftxui::color(ftxui::Color::Yellow) | ftxui::bold : cell | ftxui::dim;
        };

        for (auto &&reg : REGISTER_DISPLAY_ORDER)
        {
            // Engine-controlled, flat registers.
            // `RIP` (entry point), `RSP` (scratch-stack top), `RFLAGS` (live flags). No seed, no drill-down.
            if (reg == Reg::RIP || reg == Reg::RSP || reg == Reg::RFLAGS)
            {
                auto name      = ftxui::text(std::string("  ") + std::string(REG_NAMES[(std::size_t)reg])) | ftxui::bold;
                auto copy_text = has_trace ? format_hex64_string(current[reg]) : std::string("-");
                auto text      = has_trace ? (reg == Reg::RFLAGS ? copy_text : format_address_value(ui.trace, current[reg], ui.normalized_addresses))
                                           : std::string("-");
                auto val       = make_value(text, current[reg] != previous[reg]);
                std::optional<std::string> normalized_copy{};
                if (has_trace && reg != Reg::RFLAGS)
                {
                    normalized_copy = format_address_value(ui.trace, current[reg], true);
                    if (*normalized_copy == copy_text)
                    {
                        normalized_copy.reset();
                    }
                }

                rows.emplace_back(
                    ftxui::Elements{ftxui::emptyElement(), name, has_trace ? copy_cell(val, copy_text, std::move(normalized_copy)) : val}
                );

                continue;
            }

            // Seedable `GPR` with sub-register drill-down.
            auto i     = (std::size_t)reg;
            auto depth = ui.gpr_depth[i];
            auto full  = current[reg];
            auto prev  = previous[reg];

            // Emit one tree node.
            // Levels 0..2 expand on click, level 3 is a leaf.
            auto emit = [&](std::int32_t level, std::string_view sub_name, std::uint64_t value, std::uint64_t prev_value, std::int32_t hex_digits,
                            const ftxui::Component &seed_component)
            {
                std::string marker = level < 3 ? (depth > level ? "v " : "> ") : "  ";
                auto        name   = ftxui::text(std::string((std::size_t)level * 2, ' ') + marker + std::string(sub_name)) | ftxui::bold;
                if (level < 3)
                {
                    gpr_hits.emplace_back(GPRHit{.reg = (std::int32_t)i, .level = level});

                    name = name | ftxui::reflect(gpr_hits.back().box);
                }

                auto seed      = seed_component ? seed_component->Render() | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 18) : ftxui::emptyElement();
                auto copy_text = has_trace ? fmt::format("0x{:0{}X}", value, hex_digits) : std::string("-");
                auto text = has_trace ? (level == 0 ? format_address_value(ui.trace, value, ui.normalized_addresses) : copy_text) : std::string("-");
                auto val  = make_value(text, value != prev_value);
                std::optional<std::string> normalized_copy{};
                if (has_trace && level == 0)
                {
                    normalized_copy = format_address_value(ui.trace, value, true);
                    if (*normalized_copy == copy_text)
                    {
                        normalized_copy.reset();
                    }
                }

                rows.emplace_back(ftxui::Elements{seed, name, has_trace ? copy_cell(val, copy_text, std::move(normalized_copy)) : val});
            };

            emit(0, REG_NAMES[i], full, prev, 16, seed_inputs[i].full);

            if (depth >= 1)
            {
                emit(1, GPR_NAMES_32[i], full & 0xFFFF'FFFF, prev & 0xFFFF'FFFF, 8, seed_inputs[i].dword);
            }

            if (depth >= 2)
            {
                emit(2, GPR_NAMES_16[i], full & 0xFFFF, prev & 0xFFFF, 4, seed_inputs[i].word);
            }

            if (depth >= 3)
            {
                if (i < 4)
                {
                    emit(3, GPR_NAMES_8H[i], full >> 8 & 0xFF, prev >> 8 & 0xFF, 2, seed_inputs[i].byte_high);
                }

                emit(3, GPR_NAMES_8L[i], full & 0xFF, prev & 0xFF, 2, seed_inputs[i].byte_low);
            }
        }

        auto table = ftxui::Table(std::move(rows));
        table.SelectAll().SeparatorVertical(ftxui::LIGHT);
        table.SelectRow(0).Decorate(ftxui::dim);

        return table.Render();
    };

    // Clickable flag tokens.
    // Filled during render, hit-tested on click to toggle that seed flag.
    struct FlagHit
    {
        ftxui::Box    box{};
        std::uint64_t bit{};
    };

    std::vector<FlagHit> flag_hits{};

    auto render_flags = [&ui, &flag_hits]
    {
        flag_hits.clear();
        flag_hits.reserve(STATUS_FLAGS.size());

        // No trace yet.
        // Show the seeded flags so clicking gives feedback. Otherwise, show the step's real flags.
        auto            position = (std::size_t)ui.cursor;
        auto            flags    = ui.history_rows.empty() ? ui.seed_flags : history_state_at(ui, position)[Reg::RFLAGS];
        ftxui::Elements tokens{};
        for (auto &&flag : STATUS_FLAGS)
        {
            flag_hits.emplace_back(FlagHit{.bit = flag.bit});

            auto set   = (flags & flag.bit) != 0;
            auto token = (set ? ftxui::text(flag.name) | ftxui::color(ftxui::Color::Green) | ftxui::bold : ftxui::text(flag.name) | ftxui::dim)
                       | ftxui::reflect(flag_hits.back().box);

            tokens.emplace_back(token);
            tokens.emplace_back(ftxui::text("  "));
        }

        auto &rows = history_rows_at(ui, ui.history_tab);
        if (position > 0 && position <= rows.size() && rows[position - 1].kind == HistoryRowKind::Faulted && (flags & RFLAGS_RESUME_FLAG) != 0)
        {
            tokens.emplace_back(ftxui::text("|  "));
            tokens.emplace_back(ftxui::text("RF") | ftxui::color(ftxui::Color::Yellow) | ftxui::bold);
        }

        return ftxui::hbox(std::move(tokens));
    };

    // The Flags panel is interactive.
    // Clicking a flag toggles its seed bit (applied on the next Run).
    auto flags_view = ftxui::Renderer(render_flags);
    flags_view      = ftxui::CatchEvent(
        flags_view,
        [&ui, &flag_hits](ftxui::Event event) noexcept
        {
            if (!event.is_mouse() || event.mouse().button != ftxui::Mouse::Left || event.mouse().motion != ftxui::Mouse::Pressed)
            {
                return false;
            }

            for (auto &&hit : flag_hits)
            {
                if (hit.box.Contain(event.mouse().x, event.mouse().y))
                {
                    ui.seed_flags ^= hit.bit;

                    return true;
                }
            }

            return false;
        }
    );

    auto render_xmm = [&ui, &xmm_inputs, &mxcsr_input, &xmm_hits, &copy_hits, &copy_cell]
    {
        std::vector<ftxui::Elements> rows{ftxui::Elements{ftxui::text("Seed"), ftxui::text("Register"), ftxui::text("Value (hi : lo)")}};

        auto  position  = (std::size_t)ui.cursor;
        auto  has_trace = !ui.history_rows.empty();
        auto &current   = history_state_at(ui, position);
        auto &previous  = history_state_at(ui, position == 0 ? 0 : position - 1);

        xmm_hits.clear();
        xmm_hits.reserve(current.xmm.size());

        // Every value cell is a copy target, including each lane in the `f64x2`/`f32x4` breakdown.
        // Up to 7 per `XMM` register (hex plus 2 `f64x2` lanes plus 4 `f32x4` lanes) plus `MXCSR`, so `ftxui::reflect()` box refs stay stable.
        copy_hits.clear();
        copy_hits.reserve(current.xmm.size() * 7 + 1);

        for (std::size_t i{}; i < current.xmm.size(); ++i)
        {
            auto changed = has_trace && current.xmm[i] != previous.xmm[i];

            auto colored = [changed](const ftxui::Element &cell) noexcept
            { return changed ? cell | ftxui::color(ftxui::Color::Yellow) | ftxui::bold : cell | ftxui::dim; };

            auto display  = has_trace ? fmt::format("{:016X}:{:016X}", current.xmm[i][1], current.xmm[i][0]) : std::string("-");
            auto copy     = has_trace ? fmt::format("{:016X}{:016X}", current.xmm[i][1], current.xmm[i][0]) : std::string();
            auto value    = colored(ftxui::text(display));
            auto seed     = xmm_inputs[i]->Render() | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 20);
            auto expanded = ui.xmm_expand[i];
            auto marker   = expanded ? "v " : "> ";

            xmm_hits.emplace_back(XMMHit{.reg = (std::int32_t)i});

            auto name = ftxui::text(marker + fmt::format("XMM{}", i)) | ftxui::bold | ftxui::reflect(xmm_hits.back().box);

            rows.emplace_back(ftxui::Elements{seed, name, has_trace ? copy_cell(value, copy) : value});

            // Lane breakdown.
            // The same 128 bits as two doubles (shown by default) or four floats (behind the expand toggle), high lane first to match the hex's hi/lo
            // order. Each lane value is its own copy target so a click grabs just that number, not the whole row.
            if (has_trace)
            {
                auto lo     = current.xmm[i][0];
                auto hi     = current.xmm[i][1];
                auto f64_hi = format_float(std::bit_cast<double>(hi));
                auto f64_lo = format_float(std::bit_cast<double>(lo));

                rows.emplace_back(
                    ftxui::Elements{
                        ftxui::emptyElement(), colored(ftxui::text("    f64x2")),
                        ftxui::hbox({
                            copy_cell(colored(ftxui::text(f64_hi)), f64_hi),
                            colored(ftxui::text(", ")),
                            copy_cell(colored(ftxui::text(f64_lo)), f64_lo),
                        })
                    }
                );

                if (expanded)
                {
                    auto f32_a = format_float_single(std::bit_cast<float>((std::uint32_t)(hi >> 32)));
                    auto f32_b = format_float_single(std::bit_cast<float>((std::uint32_t)hi));
                    auto f32_c = format_float_single(std::bit_cast<float>((std::uint32_t)(lo >> 32)));
                    auto f32_d = format_float_single(std::bit_cast<float>((std::uint32_t)lo));

                    rows.emplace_back(
                        ftxui::Elements{
                            ftxui::emptyElement(), colored(ftxui::text("    f32x4")),
                            ftxui::hbox({
                                copy_cell(colored(ftxui::text(f32_a)), f32_a),
                                colored(ftxui::text(", ")),
                                copy_cell(colored(ftxui::text(f32_b)), f32_b),
                                colored(ftxui::text(", ")),
                                copy_cell(colored(ftxui::text(f32_c)), f32_c),
                                colored(ftxui::text(", ")),
                                copy_cell(colored(ftxui::text(f32_d)), f32_d),
                            })
                        }
                    );
                }
            }
        }

        // `MXCSR` as a table row so its value lines up under the `XMM` values.
        auto mxcsr_text  = has_trace ? fmt::format("0x{:08X}", current.mxcsr) : std::string("-");
        auto mxcsr_value = ftxui::text(mxcsr_text);
        mxcsr_value =
            has_trace && current.mxcsr != previous.mxcsr ? mxcsr_value | ftxui::color(ftxui::Color::Yellow) | ftxui::bold : mxcsr_value | ftxui::dim;

        rows.emplace_back(
            ftxui::Elements{
                mxcsr_input->Render() | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 20), ftxui::text("MXCSR") | ftxui::bold,
                has_trace ? copy_cell(mxcsr_value, mxcsr_text) : mxcsr_value
            }
        );

        auto table = ftxui::Table(std::move(rows));
        table.SelectAll().SeparatorVertical(ftxui::LIGHT);
        table.SelectRow(0).Decorate(ftxui::dim);

        if (!has_trace)
        {
            return table.Render();
        }

        // `MXCSR` breakdown below the table.
        // Each group (rounding, masks, flags, modes) on its own line.
        return ftxui::vbox({table.Render(), decode_block("", decode_mxcsr(current.mxcsr))});
    };

    auto render_x87 = [&ui, &st_inputs, &fpu_control_word_input, &st_hits, &copy_hits, &copy_cell]
    {
        std::vector<ftxui::Elements> rows{};
        rows.emplace_back(
            ftxui::Elements{
                ftxui::text("Seed"), ftxui::text("ST"), ftxui::text("x87r"), ftxui::text("Tag"), ftxui::text("Raw (80-bit)"), ftxui::text("Value")
            }
        );

        auto  position     = (std::size_t)ui.cursor;
        auto  has_trace    = !ui.history_rows.empty();
        auto &current      = history_state_at(ui, position);
        auto &previous     = history_state_at(ui, position == 0 ? 0 : position - 1);
        auto  top_current  = current.fpu_status_word >> 11 & 7;
        auto  top_previous = previous.fpu_status_word >> 11 & 7;

        st_hits.clear();
        st_hits.reserve(current.st.size());

        // Raw storage and the control word are copyable, decimal values require an occupied x87 tag.
        // Reserve 3 targets per `ST` plus the control word so `ftxui::reflect()` box refs stay stable.
        copy_hits.clear();
        copy_hits.reserve(current.st.size() * 3 + 1);

        for (std::int32_t i{}; i < (std::int32_t)current.st.size(); ++i)
        {
            // `st[i]` is already stack-relative (slot 0 = `ST0`).
            // `TOP` only maps `ST(i)` to its physical x87 register and (physical-ordered) tag bit.
            auto       &register_bytes    = current.st[(std::size_t)i];
            auto        physical          = (top_current + i) & 7;
            auto        previous_physical = (top_previous + i) & 7;
            auto        occupied          = has_trace && (current.fpu_tag_word_abridged >> physical & 1) != 0;
            auto        previous_occupied = has_trace && (previous.fpu_tag_word_abridged >> previous_physical & 1) != 0;
            std::string tag               = "-";
            std::string raw               = "-";
            std::string value             = "-";
            if (has_trace)
            {
                tag = x87_tag_name(occupied, register_bytes);
                raw = fmt::format("0x{:02X}", fmt::join(register_bytes | std::views::reverse, ""));
                if (occupied)
                {
                    value = format_float(st80_to_double(register_bytes, current.fpu_control_word));
                }
            }

            auto changed = has_trace && (register_bytes != previous.st[(std::size_t)i] || occupied != previous_occupied);

            auto style = [changed](std::string_view text) noexcept
            { return changed ? ftxui::text(text) | ftxui::color(ftxui::Color::Yellow) | ftxui::bold : ftxui::text(text) | ftxui::dim; };

            auto seed     = st_inputs[(std::size_t)i]->Render() | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 20);
            auto expanded = ui.st_expand[i];
            auto marker   = expanded ? "v " : "> ";

            st_hits.emplace_back(STHit{.reg = i});

            auto name       = ftxui::text(marker + fmt::format("ST{}", i)) | ftxui::bold | ftxui::reflect(st_hits.back().box);
            auto value_cell = occupied ? copy_cell(style(value), value) : style(value);

            rows.emplace_back(
                ftxui::Elements{
                    seed,
                    name,
                    ftxui::text(has_trace ? fmt::format("x87r{}", physical) : "-") | ftxui::dim,
                    style(tag),
                    has_trace ? copy_cell(style(raw), raw) : style(raw),
                    value_cell | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 16),
                }
            );

            // Narrowed drill-down.
            // The same value rounded directly to `Real4` storage on the FPU, not double-rounded through a double. Raw is display-only because it is
            // not the register's 80-bit encoding. Value is a copy target, its `f`-suffixed text round-trips through the seed field.
            if (occupied && expanded)
            {
                auto narrowed_float = st80_to_float(register_bytes, current.fpu_control_word);
                auto narrowed_bits  = std::bit_cast<std::uint32_t>(narrowed_float);
                auto narrowed_raw   = fmt::format("0x{:08X}", narrowed_bits);
                auto narrowed_value = format_float_single(narrowed_float);

                rows.emplace_back(
                    ftxui::Elements{
                        ftxui::emptyElement(),
                        style("  f32"),
                        ftxui::emptyElement(),
                        ftxui::emptyElement(),
                        style(narrowed_raw),
                        copy_cell(style(narrowed_value), narrowed_value) | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 16),
                    }
                );
            }
        }

        auto control_word_text  = has_trace ? fmt::format("0x{:04X}", current.fpu_control_word) : std::string("-");
        auto control_word_value = ftxui::text(control_word_text);
        control_word_value      = has_trace && current.fpu_control_word != previous.fpu_control_word
                                    ? control_word_value | ftxui::color(ftxui::Color::Yellow) | ftxui::bold
                                    : control_word_value | ftxui::dim;

        rows.emplace_back(
            ftxui::Elements{
                fpu_control_word_input->Render() | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 20),
                ftxui::text("Control word") | ftxui::bold,
                ftxui::emptyElement(),
                ftxui::emptyElement(),
                has_trace ? copy_cell(control_word_value, control_word_text) : control_word_value,
                ftxui::emptyElement(),
            }
        );

        auto table = ftxui::Table(std::move(rows));
        table.SelectAll().SeparatorVertical(ftxui::LIGHT);
        table.SelectRow(0).Decorate(ftxui::dim);

        if (!has_trace)
        {
            return table.Render();
        }

        return ftxui::vbox({
            table.Render(),
            decode_block(fmt::format("CW 0x{:04X}  ", current.fpu_control_word), decode_x87_control_word(current.fpu_control_word)),
            decode_block(fmt::format("SW 0x{:04X}  ", current.fpu_status_word), decode_x87_status_word(current.fpu_status_word)),
            ftxui::text(fmt::format("TW 0x{:02X}", current.fpu_tag_word_abridged)) | ftxui::dim,
        });
    };

    auto register_view_gpr = ftxui::Renderer(seed_container, render_registers);

    register_view_gpr = ftxui::CatchEvent(
        register_view_gpr,
        [&ui, &gpr_hits](ftxui::Event event) noexcept
        {
            if (!event.is_mouse() || event.mouse().button != ftxui::Mouse::Left || event.mouse().motion != ftxui::Mouse::Pressed)
            {
                return false;
            }

            for (auto &&hit : gpr_hits)
            {
                if (hit.box.Contain(event.mouse().x, event.mouse().y))
                {
                    auto &depth = ui.gpr_depth[hit.reg];
                    depth       = depth > hit.level ? hit.level : hit.level + 1;

                    return true;
                }
            }

            return false;
        }
    );

    auto register_view_xmm = ftxui::Renderer(xmm_seed_container, render_xmm);
    register_view_xmm      = ftxui::CatchEvent(
        register_view_xmm,
        [&ui, &xmm_hits](ftxui::Event event) noexcept
        {
            if (!event.is_mouse() || event.mouse().button != ftxui::Mouse::Left || event.mouse().motion != ftxui::Mouse::Pressed)
            {
                return false;
            }

            for (auto &&hit : xmm_hits)
            {
                if (hit.box.Contain(event.mouse().x, event.mouse().y))
                {
                    ui.xmm_expand[hit.reg] = !ui.xmm_expand[hit.reg];

                    return true;
                }
            }

            return false;
        }
    );

    auto register_view_x87 = ftxui::Renderer(st_seed_container, render_x87);
    register_view_x87      = ftxui::CatchEvent(
        register_view_x87,
        [&ui, &st_hits](ftxui::Event event) noexcept
        {
            if (!event.is_mouse() || event.mouse().button != ftxui::Mouse::Left || event.mouse().motion != ftxui::Mouse::Pressed)
            {
                return false;
            }

            for (auto &&hit : st_hits)
            {
                if (hit.box.Contain(event.mouse().x, event.mouse().y))
                {
                    ui.st_expand[hit.reg] = !ui.st_expand[hit.reg];

                    return true;
                }
            }

            return false;
        }
    );

    auto register_tabs = ftxui::Container::Tab({register_view_gpr, register_view_xmm, register_view_x87}, &ui.register_tab);

    // True when a real seed `Input` holds focus on the visible tab (its `FocusSink` is no longer the active child).
    // `ScrollerBase` reads it to pick yframe vs offset, the wheel handler to skip scrolling while typing.
    auto register_has_real_focus = [&ui, gpr_focus_sink, xmm_focus_sink, st_focus_sink]() noexcept
    {
        switch (ui.register_tab)
        {
        case 0:  return !gpr_focus_sink->Active();
        case 1:  return !xmm_focus_sink->Active();
        default: return !st_focus_sink->Active();
        }
    };

    // Hands the visible tab's `FocusSink` the active-child slot, dropping any real seed `Input` focus.
    // Shared by `Escape`, blank-space clicks, and each seed `on_enter`.
    auto defocus_current_register_tab = [&ui, gpr_focus_sink, xmm_focus_sink, st_focus_sink]() noexcept
    {
        switch (ui.register_tab)
        {
        case 0:  gpr_focus_sink->TakeFocus(); break;
        case 1:  xmm_focus_sink->TakeFocus(); break;
        default: st_focus_sink->TakeFocus(); break;
        }
    };

    auto register_scroller = make_scroller(
        register_tabs, [&ui]() noexcept -> std::int32_t & { return ui.register_scroll[(std::size_t)ui.register_tab]; }, register_has_real_focus
    );

    // On tab switch, hand focus back to the scroller.
    // Otherwise the toggle keeps it and arrow keys can't reach the new tab's seed `Input`s until one is clicked.
    auto register_toggle_option      = ftxui::MenuOption::Toggle();
    register_toggle_option.on_change = [register_scroller]() noexcept { register_scroller->TakeFocus(); };

    auto register_toggle = ftxui::Menu(std::vector<std::string>{"GPR", "SSE", "x87"}, &ui.register_tab, register_toggle_option);
    auto registers_pane  = ftxui::Container::Vertical({register_toggle, register_scroller});

    // The wheel is consumed without moving the offset while a seed field controls the viewport.
    // A left click drops seed focus like Escape, then falls through so a real control can reclaim it.
    ftxui::Box registers_box{};
    auto       registers_view = ftxui::CatchEvent(
        registers_pane,
        [&ui, &registers_box, &defocus_current_register_tab, &register_has_real_focus](ftxui::Event event) noexcept
        {
            if (!event.is_mouse() || !registers_box.Contain(event.mouse().x, event.mouse().y))
            {
                return false;
            }

            if (event.mouse().button == ftxui::Mouse::Left && event.mouse().motion == ftxui::Mouse::Pressed)
            {
                defocus_current_register_tab();

                return false;
            }

            if (event.mouse().button == ftxui::Mouse::WheelUp)
            {
                if (!register_has_real_focus())
                {
                    auto &scroll = ui.register_scroll[(std::size_t)ui.register_tab];
                    scroll       = std::max(0, scroll - 1);
                }

                return true;
            }

            if (event.mouse().button == ftxui::Mouse::WheelDown)
            {
                if (!register_has_real_focus())
                {
                    ui.register_scroll[(std::size_t)ui.register_tab] += 1;
                }

                return true;
            }

            return false;
        }
    );
    auto        root      = ftxui::Container::Vertical({
        code_input,
        buttons,
        flags_view,
        ftxui::Container::Horizontal({registers_view, history_view}),
    });
    std::string code_addr = "-";
    std::string code_size{};
    std::string stack_addr = "-";
    auto        stack_size = fmt::format(" (Stack {} KiB)", SCRATCH_STACK_BYTES / 1024);
    auto        data_addr  = fmt::format("0x{:016X}", scratch_data_base());
    auto        data_size  = fmt::format(" ({} KiB)", SCRATCH_DATA_BYTES / 1024);

    // Clickable code, stack, and data address regions.
    // A left-click copies the absolute address. Ctrl-left-click copies its normalized form.
    ftxui::Box code_box{};
    ftxui::Box stack_box{};
    ftxui::Box data_box{};

    auto layout = ftxui::Renderer(
        root,
        [&]
        {
            code_addr  = ui.trace.seed[Reg::RIP] != 0 ? format_hex64_string(ui.trace.seed[Reg::RIP]) : "-";
            stack_addr = ui.trace.seed[Reg::RSP] != 0 ? format_hex64_string(ui.trace.seed[Reg::RSP]) : "-";
            code_size  = ui.trace.code.empty() ? "" : fmt::format(" ({} B)", ui.trace.code.size());

            auto code_element  = ftxui::text(code_addr) | ftxui::bold | ftxui::reflect(code_box);
            auto stack_element = ftxui::text(stack_addr) | ftxui::bold | ftxui::reflect(stack_box);
            if (code_addr != "-")
            {
                code_element |= ftxui::underlined;
            }

            if (stack_addr != "-")
            {
                stack_element |= ftxui::underlined;
            }

            auto bytes_row   = ftxui::hbox({
                ftxui::text(" Bytes ") | ftxui::bold,
                code_input->Render() | ftxui::flex,
            });
            auto address_row = ftxui::hbox({
                ftxui::text(" Code @ ") | ftxui::dim,
                code_element,
                ftxui::text(code_size) | ftxui::dim,
                ftxui::separatorEmpty(),
                ftxui::separatorLight(),
                ftxui::separatorEmpty(),
                ftxui::text("RSP @ ") | ftxui::dim,
                stack_element,
                ftxui::text(stack_size) | ftxui::dim,
                ftxui::separatorEmpty(),
                ftxui::separatorLight(),
                ftxui::separatorEmpty(),
                ftxui::text("Data @ ") | ftxui::dim,
                ftxui::text(data_addr) | ftxui::bold | ftxui::underlined | ftxui::reflect(data_box),
                ftxui::text(data_size) | ftxui::dim,
            });
            auto controls    = ftxui::hbox({
                run_button->Render(),
                ftxui::separatorEmpty(),
                run_to_button->Render(),
                ftxui::separatorEmpty(),
                step_button->Render(),
                ftxui::separatorEmpty(),
                back_button->Render(),
                ftxui::separatorEmpty(),
                copy_history_button->Render(),
                ftxui::separatorEmpty(),
                reset_button->Render(),
                ftxui::separatorEmpty(),
                settings_button->Render(),
                ftxui::separatorEmpty(),
                about_button->Render(),
                ftxui::separatorEmpty(),
                quit_button->Render(),
            });

            ftxui::Elements left_elements{};
            left_elements.emplace_back(
                ftxui::window(
                    ftxui::text(" Registers "),
                    ftxui::vbox({register_toggle->Render(), ftxui::separator(), register_scroller->Render() | ftxui::flex})
                        | ftxui::reflect(registers_box)
                )
                | ftxui::flex
            );

            if (history_selection_is_static(ui))
            {
                left_elements.emplace_back(
                    ftxui::text(" Static row - no execution state. Showing prior state. ") | ftxui::color(ftxui::Color::Yellow) | ftxui::bold
                );
            }

            left_elements.emplace_back(ftxui::window(ftxui::text(" Flags "), flags_view->Render()));

            auto left          = ftxui::vbox(std::move(left_elements));
            auto history_title = fmt::format(" History ({}) ", history_tab_labels[(std::size_t)ui.history_tab]);
            auto right         = ftxui::window(
                ftxui::text(history_title),
                ftxui::vbox(
                    {history_toggle->Render(), ftxui::separator(), history_tabs->Render() | ftxui::vscroll_indicator | ftxui::yframe | ftxui::flex}
                ) | ftxui::reflect(history_box)
            );
            auto body = ftxui::hbox({left | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 84), right | ftxui::flex}) | ftxui::flex;
            auto instrumentation_warning =
                ui.show_instrumentation_warning
                    ? ftxui::text(fmt::format(" WARNING: {}", INSTRUMENTATION_REFUSAL)) | ftxui::color(ftxui::Color::LightCoral) | ftxui::bold
                    : ftxui::emptyElement();

            return ftxui::vbox({
                ftxui::window(
                    ftxui::text(" BME - Bare metal machine code viewer "), ftxui::vbox({bytes_row, address_row, ftxui::separator(), controls})
                ),
                body,
                instrumentation_warning,
                ftxui::text(' ' + ui.status) | ftxui::dim,
            });
        }
    );

    // Layout-level events.
    // Function-key shortcuts (`F5`/`F8`/`F7`) and address copy clicks. All skipped while a modal is open.
    layout = ftxui::CatchEvent(
        layout,
        [&](ftxui::Event event)
        {
            if (ui.show_about || ui.show_settings)
            {
                return false;
            }

            auto copy_value = [&ui](std::string text, bool normalized)
            {
                auto copied = copy_to_clipboard(text);
                auto kind   = normalized ? "normalized address" : "value";
                ui.status   = copied ? fmt::format("Copied {} {} to clipboard.", kind, text) : "Clipboard copy failed.";
            };

            if (event.is_mouse()
                && event.mouse().button
                == ftxui::Mouse::Left
                && event.mouse().motion
                == ftxui::Mouse::Pressed
                && code_addr
                != "-"
                && code_box.Contain(event.mouse().x, event.mouse().y))
            {
                auto normalized = event.mouse().control;
                auto text       = normalized ? format_address_value(ui.trace, ui.trace.seed[Reg::RIP], true) : code_addr;
                copy_value(std::move(text), normalized);

                return true;
            }

            if (event.is_mouse()
                && event.mouse().button
                == ftxui::Mouse::Left
                && event.mouse().motion
                == ftxui::Mouse::Pressed
                && stack_addr
                != "-"
                && stack_box.Contain(event.mouse().x, event.mouse().y))
            {
                auto normalized = event.mouse().control;
                auto text       = normalized ? format_address_value(ui.trace, ui.trace.seed[Reg::RSP], true) : stack_addr;
                copy_value(std::move(text), normalized);

                return true;
            }

            if (event.is_mouse()
                && event.mouse().button
                == ftxui::Mouse::Left
                && event.mouse().motion
                == ftxui::Mouse::Pressed
                && data_box.Contain(event.mouse().x, event.mouse().y))
            {
                auto normalized = event.mouse().control;
                auto text       = normalized ? format_address_value(ui.trace, scratch_data_base(), true) : data_addr;
                copy_value(std::move(text), normalized);

                return true;
            }

            if (event.is_mouse() && event.mouse().button == ftxui::Mouse::Left && event.mouse().motion == ftxui::Mouse::Pressed)
            {
                for (auto &&hit : copy_hits)
                {
                    if (hit.box.Contain(event.mouse().x, event.mouse().y))
                    {
                        auto normalized = event.mouse().control && hit.normalized.has_value();
                        auto text       = normalized ? *hit.normalized : hit.text;
                        copy_value(std::move(text), normalized);

                        return true;
                    }
                }
            }

            // `Escape` drops a stuck seed `Input`'s focus (clicking one leaves it active forever, per `ftxui`) so the wheel can scroll again.
            // Visible tab only, so it can't flip `ui.register_tab`.
            if (event == ftxui::Event::Escape)
            {
                defocus_current_register_tab();

                return true;
            }

            if (event == ftxui::Event::F5)
            {
                run();

                return true;
            }

            if (event == ftxui::Event::F8)
            {
                step();

                return true;
            }

            if (event == ftxui::Event::F7)
            {
                back();

                return true;
            }

            return false;
        }
    );

    auto host_cpu_summary = format_cpu_summary(*host_cpu_fingerprint());

    auto about_close = [&ui] noexcept { ui.show_about = false; };

    auto about_ok    = ftxui::Button("Close", about_close, ftxui::ButtonOption::Ascii());
    auto about_modal = ftxui::Renderer(
        about_ok,
        [&about_ok, &host_cpu_summary]
        {
            auto field = [](std::string_view label, std::string_view value)
            {
                return ftxui::hbox({
                    ftxui::text(label) | ftxui::dim | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 11),
                    ftxui::text(value),
                });
            };

            return ftxui::vbox({
                       ftxui::text(" BME - Bare metal machine code viewer ") | ftxui::bold,
                       ftxui::separator(),
                       field("Version", fmt::format("{} ({})", BME_GIT_TAG, BME_GIT_HASH)),
                       field("Repository", BME_GIT_URL),
                       ftxui::text(BME_COPYRIGHT),
                       field("CPU", host_cpu_summary),
                       field("Platform", BME_PLATFORM),
                       ftxui::separator(),
                       field("Decoders", "Zydis (MIT), bddisasm (Apache-2.0), Capstone (BSD-3-Clause), XED (Apache-2.0)"),
                       field("Built with", "FTXUI, fmt, argparse (All MIT)"),
                       field("Licenses", "THIRD_PARTY_LICENSES.md"),
                       ftxui::separator(),
                       about_ok->Render() | ftxui::center,
                   })
                 | ftxui::border;
        }
    );

    about_modal = ftxui::CatchEvent(
        about_modal,
        [&ui](const ftxui::Event &event) noexcept
        {
            if (event == ftxui::Event::Escape)
            {
                ui.show_about = false;

                return true;
            }

            return false;
        }
    );

    layout |= ftxui::Modal(about_modal, &ui.show_about);

    // Settings modal.
    // Disassembly syntax plus decode backend (reuse the toggle buttons), single-step cap, and the `RDI`/`RSI`-to-scratch toggle. The cap parses into
    // `ui.max_steps` live (last valid value kept), applied on the next `Run`.
    auto max_steps_text = std::to_string(ui.max_steps);

    ftxui::InputOption max_steps_option{};
    max_steps_option.multiline = false;
    max_steps_option.on_change = [&ui, &max_steps_text]() noexcept
    {
        std::size_t parsed{};
        auto [parse_end, error_code] = std::from_chars(max_steps_text.data(), max_steps_text.data() + max_steps_text.size(), parsed);
        if (error_code == std::errc{} && parse_end == max_steps_text.data() + max_steps_text.size() && parsed > 0)
        {
            ui.max_steps = std::min(parsed, MAX_STEPS_LIMIT);
        }
    };

    auto max_steps_input = ftxui::Input(&max_steps_text, max_steps_option);

    ftxui::CheckboxOption address_option{};
    address_option.on_change = [&ui]
    {
        if (!ui.trace.code.empty())
        {
            rebuild_history(ui);
        }
    };

    auto data_pointers_checkbox = ftxui::Checkbox("Seed RDI/RSI to scratch data", &ui.seed_data_pointers);
    auto address_checkbox       = ftxui::Checkbox("Show normalized addresses", &ui.normalized_addresses, address_option);
    auto settings_close         = ftxui::Button("Close", [&ui] noexcept { ui.show_settings = false; }, ftxui::ButtonOption::Ascii());
    auto settings_container =
        ftxui::Container::Vertical({syntax_button, backend_button, max_steps_input, data_pointers_checkbox, address_checkbox, settings_close});
    auto settings_modal = ftxui::Renderer(
        settings_container,
        [&]
        {
            return ftxui::vbox({
                       ftxui::text(" Settings ") | ftxui::bold,
                       ftxui::separator(),
                       ftxui::hbox(
                           {ftxui::text("Syntax") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 11) | ftxui::dim,
                            syntax_button->Render() | ftxui::reflect(syntax_button_box)}
                       ),
                       ftxui::hbox(
                           {ftxui::text("Backend") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 11) | ftxui::dim,
                            backend_button->Render() | ftxui::reflect(backend_button_box)}
                       ),
                       ftxui::hbox({
                           ftxui::text("Max steps") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 11) | ftxui::dim,
                           max_steps_input->Render() | ftxui::inverted | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 12),
                       }),
                       data_pointers_checkbox->Render(),
                       address_checkbox->Render(),
                       ftxui::separator(),
                       settings_close->Render() | ftxui::center,
                   })
                 | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 34)
                 | ftxui::border;
        }
    );

    settings_modal = ftxui::CatchEvent(
        settings_modal,
        [&ui](const ftxui::Event &event) noexcept
        {
            if (event == ftxui::Event::Escape)
            {
                ui.show_settings = false;

                return true;
            }

            return false;
        }
    );

    layout |= ftxui::Modal(settings_modal, &ui.show_settings);

    if (cli.run && (cli.bytes || cli.input_file) && !ui.code.empty())
    {
        run();
    }

    screen.Loop(layout);

    return 0;
}

// Non-interactive `--quick` output.
// JSON writes the full trace. Text prints the initial state, selected register deltas, not-reached rows, and final outcome.
std::int32_t run_quick(const CLI &cli)
{
    if (!cli.bytes && !cli.input_file)
    {
        fmt::println(stderr, "--quick needs --bytes or --file.");

        return 1;
    }

    auto decoded = cli.bytes ? parse_code_text(*cli.bytes) : read_binary_input(*cli.input_file);
    if (!decoded)
    {
        fmt::println(stderr, "Error: {}", decoded.error());

        return 1;
    }

    // Seed from `--seed`.
    // `CLI::parse` already validated every field strictly, so a composition failure here should be unreachable. If it happens anyway, fail loudly
    // rather than silently run with a wrong seed. `RDI`/`RSI` still default to the scratch data base when left unseeded.
    std::vector<std::string> seed_errors{};
    auto                     seed = compose_seed(cli.seed_gpr, cli.seed_flags, cli.seed_xmm, cli.seed_st, cli.seed_floating_environment, seed_errors);
    if (!seed_errors.empty())
    {
        for (auto &&error : seed_errors)
        {
            fmt::println(stderr, "Error: {}", error);
        }

        return 1;
    }

    auto trace = run_engine(*decoded, seed, cli.max_steps, cli.backend, cli.syntax, true);
    if (cli.format == OutputFormat::Json)
    {
        auto result = write_trace_json(trace, std::cout, cli.pretty_json);
        if (!result)
        {
            fmt::println(stderr, "Error: {}", result.error());

            return 1;
        }

        return trace.outcome == Outcome::Error ? 1 : 0;
    }

    if (trace.outcome == Outcome::Error)
    {
        fmt::println(stderr, "Error: {}", trace.message);

        return 1;
    }

    auto &track = cli.track;

    // Print the tracked registers that changed between `before` and `after`, one per line.
    auto print_deltas = [&track](const Registers &before, const Registers &after)
    {
        // Indented, `fmt`-padded delta line.
        // `<label> <before> -> <after><extra>`.
        auto row = [](std::string_view label, std::string_view before_text, std::string_view after_text, std::string_view extra = "")
        { fmt::println("{:6}{:<6} {} -> {}{}", "", label, before_text, after_text, extra); };

        if (track.gpr)
        {
            for (std::size_t i{}; i < GPR_COUNT; ++i)
            {
                if (before.gpr[i] != after.gpr[i])
                {
                    row(REG_NAMES[i], format_hex64_string(before.gpr[i]), format_hex64_string(after.gpr[i]));
                }
            }
        }

        if (track.rip && before.rip != after.rip)
        {
            row("RIP", format_hex64_string(before.rip), format_hex64_string(after.rip));
        }

        if (track.rflags && before.rflags != after.rflags)
        {
            std::string changed{};
            for (auto &&[name, bit] : STATUS_FLAGS)
            {
                if (((before.rflags ^ after.rflags) & bit) != 0)
                {
                    changed += fmt::format(" {}{}", (after.rflags & bit) != 0 ? '+' : '-', name);
                }
            }

            if (((before.rflags ^ after.rflags) & RFLAGS_RESUME_FLAG) != 0)
            {
                changed += fmt::format(" {}RF", (after.rflags & RFLAGS_RESUME_FLAG) != 0 ? '+' : '-');
            }

            row("RFLAGS", format_hex64_string(before.rflags), format_hex64_string(after.rflags),
                changed.empty() ? std::string{} : fmt::format(" [{}]", changed.substr(1)));
        }

        if (track.xmm)
        {
            for (std::size_t i{}; i < before.xmm.size(); ++i)
            {
                if (before.xmm[i] != after.xmm[i])
                {
                    row(fmt::format("XMM{}", i), fmt::format("{:016X}:{:016X}", before.xmm[i][1], before.xmm[i][0]),
                        fmt::format("{:016X}:{:016X}", after.xmm[i][1], after.xmm[i][0]));
                }
            }

            if (before.mxcsr != after.mxcsr)
            {
                row("MXCSR", fmt::format("0x{:08X}", before.mxcsr), fmt::format("0x{:08X}", after.mxcsr),
                    fmt::format("  ({})", fmt::join(decode_mxcsr(after.mxcsr), " | ")));
            }
        }

        if (track.x87)
        {
            for (std::size_t i{}; i < before.st.size(); ++i)
            {
                if (before.st[i] != after.st[i])
                {
                    row(fmt::format("ST{}", i), fmt::format("0x{:02X}", fmt::join(before.st[i] | std::views::reverse, "")),
                        fmt::format("0x{:02X}", fmt::join(after.st[i] | std::views::reverse, "")),
                        fmt::format(
                            "  ({} -> {})", format_float(st80_to_double(before.st[i], before.fpu_control_word)),
                            format_float(st80_to_double(after.st[i], after.fpu_control_word))
                        ));
                }
            }

            if (before.fpu_control_word != after.fpu_control_word)
            {
                row("x87CW", fmt::format("0x{:04X}", before.fpu_control_word), fmt::format("0x{:04X}", after.fpu_control_word),
                    fmt::format("  ({})", fmt::join(decode_x87_control_word(after.fpu_control_word), " | ")));
            }

            if (before.fpu_status_word != after.fpu_status_word)
            {
                row("x87SW", fmt::format("0x{:04X}", before.fpu_status_word), fmt::format("0x{:04X}", after.fpu_status_word),
                    fmt::format("  ({})", fmt::join(decode_x87_status_word(after.fpu_status_word), " | ")));
            }

            if (before.fpu_tag_word_abridged != after.fpu_tag_word_abridged)
            {
                row("x87TW", fmt::format("0x{:02X}", before.fpu_tag_word_abridged), fmt::format("0x{:02X}", after.fpu_tag_word_abridged));
            }
        }
    };

    fmt::println("CPU {}", format_cpu_summary(*trace.cpu_fingerprint));

    // Seed baseline.
    // Diff against a zeroed set so only the non-zero seed registers print.
    fmt::println("Seed");
    print_deltas({}, trace.seed);

    auto *previous = &trace.seed;
    auto  history  = build_history(trace, cli.backend, cli.syntax);
    for (auto &&row : history)
    {
        auto bytes = fmt::format("{:02X}", fmt::join(std::span{trace.code}.subspan(row.offset, row.length), " "));
        if (row.kind == HistoryRowKind::Reached)
        {
            auto &event = trace.execution_events[*row.execution_event_index];

            fmt::println("+{:<4X} {:<24} {}", row.offset, bytes, row.text);

            print_deltas(*previous, event.registers);

            previous = &event.registers;
        }
        else if (row.kind == HistoryRowKind::Faulted)
        {
            auto &event = trace.execution_events[*row.execution_event_index];

            fmt::println("!{:<4X} {:<24} {}  ({}, partial state)", row.offset, bytes, row.text, trace.stop_reason);

            print_deltas(*previous, event.registers);
        }
        else
        {
            auto note = row.rip == trace.stop_address && !trace.stop_reason.empty() ? trace.stop_reason : std::string_view{"not reached"};

            fmt::println(" {:<4X} {:<24} {}  ({})", row.offset, bytes, row.text, note);
        }
    }

    fmt::println("{}", trace.message);

    return 0;
}

} // namespace bme
