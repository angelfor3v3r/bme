#include "cpu.hpp"
#include "common.hpp"
#include "util.hpp"
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if BME_OS_WINDOWS
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace bme
{

namespace
{

constexpr auto CPUID_VENDOR_LEAF              = 0x0000'0000u;
constexpr auto CPUID_SIGNATURE_LEAF           = 0x0000'0001u;
constexpr auto CPUID_STRUCTURED_FEATURES_LEAF = 0x0000'0007u;
constexpr auto CPUID_XSTATE_LEAF              = 0x0000'000Du;
constexpr auto CPUID_HYPERVISOR_LEAF          = 0x4000'0000u;
constexpr auto CPUID_HYPERVISOR_INTERFACE     = 0x4000'0001u;
constexpr auto CPUID_EXTENDED_LIMIT_LEAF      = 0x8000'0000u;
constexpr auto CPUID_EXTENDED_FEATURES_LEAF   = 0x8000'0001u;
constexpr auto CPUID_BRAND_FIRST_LEAF         = 0x8000'0002u;
constexpr auto CPUID_BRAND_LAST_LEAF          = 0x8000'0004u;
constexpr auto CPUID_ADDRESS_WIDTH_LEAF       = 0x8000'0008u;

constexpr auto CPUID_MAX_STRUCTURED_FEATURE_SUBLEAF = 128u;

constexpr auto CPUID_XSAVE_MASK      = 1u << 26;
constexpr auto CPUID_OSXSAVE_MASK    = 1u << 27;
constexpr auto CPUID_HYPERVISOR_MASK = 1u << 31;

enum class CPUIDRegister : std::uint8_t
{
    EAX = 0,
    EBX,
    ECX,
    EDX,
};

struct CPUIDFeatureDefinition
{
    std::uint32_t    leaf{};
    std::uint32_t    subleaf{};
    CPUIDRegister    reg{};
    std::uint8_t     bit{};
    std::string_view name{};
};

constexpr auto CPUID_FEATURES = std::to_array<CPUIDFeatureDefinition>({
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::EDX, 0, "fpu"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::EDX, 4, "tsc"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::EDX, 5, "msr"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::EDX, 6, "pae"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::EDX, 8, "cx8"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::EDX, 9, "apic"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::EDX, 11, "sep"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::EDX, 12, "mtrr"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::EDX, 13, "pge"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::EDX, 15, "cmov"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::EDX, 19, "clfsh"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::EDX, 23, "mmx"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::EDX, 24, "fxsr"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::EDX, 25, "sse"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::EDX, 26, "sse2"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::EDX, 28, "htt"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 0, "sse3"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 1, "pclmulqdq"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 3, "monitor"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 5, "vmx"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 6, "smx"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 9, "ssse3"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 12, "fma"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 13, "cx16"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 17, "pcid"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 19, "sse4_1"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 20, "sse4_2"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 21, "x2apic"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 22, "movbe"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 23, "popcnt"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 24, "tsc_deadline"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 25, "aes"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 26, "xsave"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 27, "osxsave"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 28, "avx"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 29, "f16c"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 30, "rdrand"},
    {CPUID_SIGNATURE_LEAF, 0, CPUIDRegister::ECX, 31, "hypervisor"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 0, "fsgsbase"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 2, "sgx"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 3, "bmi1"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 4, "hle"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 5, "avx2"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 7, "smep"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 8, "bmi2"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 9, "erms"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 10, "invpcid"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 11, "rtm"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 14, "mpx"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 16, "avx512f"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 17, "avx512dq"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 18, "rdseed"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 19, "adx"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 20, "smap"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 21, "avx512_ifma"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 23, "clflushopt"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 24, "clwb"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 25, "intel_pt"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 28, "avx512cd"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 29, "sha"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 30, "avx512bw"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EBX, 31, "avx512vl"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 1, "avx512_vbmi"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 2, "umip"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 3, "pku"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 4, "ospke"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 5, "waitpkg"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 6, "avx512_vbmi2"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 7, "cet_ss"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 8, "gfni"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 9, "vaes"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 10, "vpclmulqdq"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 11, "avx512_vnni"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 12, "avx512_bitalg"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 14, "avx512_vpopcntdq"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 16, "la57"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 22, "rdpid"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 25, "cldemote"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 27, "movdiri"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 28, "movdir64b"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 29, "enqcmd"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 30, "sgx_lc"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 31, "pks"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 4, "fsrm"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 5, "uintr"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 8, "avx512_vp2intersect"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 10, "md_clear"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 14, "serialize"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 15, "hybrid"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 16, "tsxldtrk"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 18, "pconfig"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 20, "cet_ibt"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 22, "amx_bf16"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 23, "avx512_fp16"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 24, "amx_tile"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 25, "amx_int8"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 26, "ibrs_ibpb"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 27, "stibp"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 28, "l1d_flush"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 29, "ia32_arch_capabilities"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 30, "ia32_core_capabilities"},
    {CPUID_STRUCTURED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 31, "ssbd"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 0, "lahf_lm"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 2, "svm"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 5, "abm"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 6, "sse4a"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 8, "prefetchw"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 11, "xop"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 16, "fma4"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 21, "tbm"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::ECX, 22, "topoext"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 11, "syscall"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 20, "nx"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 22, "mmxext"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 25, "fxsr_opt"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 26, "pdpe1gb"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 27, "rdtscp"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 29, "long_mode"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 30, "3dnowext"},
    {CPUID_EXTENDED_FEATURES_LEAF, 0, CPUIDRegister::EDX, 31, "3dnow"},
});

struct XStateDefinition
{
    std::uint8_t     bit{};
    std::string_view name{};
};

constexpr auto XSTATE_COMPONENTS = std::to_array<XStateDefinition>({
    {0, "x87"},
    {1, "sse"},
    {2, "avx"},
    {3, "mpx_bndregs"},
    {4, "mpx_bndcsr"},
    {5, "avx512_opmask"},
    {6, "avx512_zmm_hi256"},
    {7, "avx512_hi16_zmm"},
    {8, "processor_trace"},
    {9, "pkru"},
    {10, "pasid"},
    {11, "cet_user"},
    {12, "cet_supervisor"},
    {13, "hdc"},
    {14, "uintr"},
    {15, "lbr"},
    {16, "hwp"},
    {17, "amx_tilecfg"},
    {18, "amx_tiledata"},
    {19, "apx"},
});

class HostCPUQuerySource final : public CPUQuerySource
{
public:
    CPUIDRegisters cpuid(std::uint32_t leaf, std::uint32_t subleaf) noexcept override
    {
#if BME_OS_WINDOWS
        std::array<int, 4> registers{};
        __cpuidex(registers.data(), (int)leaf, (int)subleaf);

        return {
            (std::uint32_t)registers[0],
            (std::uint32_t)registers[1],
            (std::uint32_t)registers[2],
            (std::uint32_t)registers[3],
        };
#else
        CPUIDRegisters registers{};
        __cpuid_count(leaf, subleaf, registers.eax, registers.ebx, registers.ecx, registers.edx);

        return registers;
#endif
    }

    std::uint64_t xgetbv(std::uint32_t index) noexcept override
    {
#if BME_OS_WINDOWS
        return _xgetbv(index);
#else
        std::uint32_t eax{}, edx{};
        __asm__ volatile("xgetbv"
                         : "=a"(eax), "=d"(edx)
                         : "c"(index));

        return (std::uint64_t)edx << 32 | eax;
#endif
    }
};

struct CPUSignature
{
    std::uint32_t family{};
    std::uint32_t model{};
    std::uint32_t stepping{};
};

constexpr auto CPUID_PADDING = [](char character) noexcept { return character == '\0' || character == ' '; };

void append_register_text(std::string &text, std::uint32_t value)
{
    text += (char)(value & 0xFFu);
    text += (char)(value >> 8 & 0xFFu);
    text += (char)(value >> 16 & 0xFFu);
    text += (char)(value >> 24 & 0xFFu);
}

void append_brand_leaf(std::string &text, const CPUIDRegisters &registers)
{
    append_register_text(text, registers.eax);
    append_register_text(text, registers.ebx);
    append_register_text(text, registers.ecx);
    append_register_text(text, registers.edx);
}

CPUSignature decode_intel_signature(std::uint32_t value) noexcept
{
    auto base_family     = value >> 8 & 0xFu;
    auto base_model      = value >> 4 & 0xFu;
    auto extended_model  = value >> 16 & 0xFu;
    auto extended_family = value >> 20 & 0xFFu;
    auto family          = base_family;
    if (base_family == 0xFu)
    {
        family += extended_family;
    }

    auto model = base_model;
    if (base_family == 0x6u || base_family == 0xFu)
    {
        model += extended_model << 4;
    }

    return {.family = family, .model = model, .stepping = value & 0xFu};
}

CPUSignature decode_amd_signature(std::uint32_t value) noexcept
{
    auto base_family     = value >> 8 & 0xFu;
    auto base_model      = value >> 4 & 0xFu;
    auto extended_model  = value >> 16 & 0xFu;
    auto extended_family = value >> 20 & 0xFFu;
    auto family          = base_family;
    auto model           = base_model;
    if (base_family == 0xFu)
    {
        family += extended_family;
        model  += extended_model << 4;
    }

    return {.family = family, .model = model, .stepping = value & 0xFu};
}

void append_record(CPUFingerprint &fingerprint, std::uint32_t leaf, std::uint32_t subleaf, const CPUIDRegisters &registers)
{
    fingerprint.raw_cpuid.emplace_back(
        CPUIDRecord{
            .leaf    = leaf,
            .subleaf = subleaf,
            .eax     = registers.eax,
            .ebx     = registers.ebx,
            .ecx     = registers.ecx,
            .edx     = registers.edx,
        }
    );
}

const CPUIDRecord *find_record(const std::vector<CPUIDRecord> &records, std::uint32_t leaf, std::uint32_t subleaf) noexcept
{
    for (auto &&record : records)
    {
        if (record.leaf == leaf && record.subleaf == subleaf)
        {
            return &record;
        }
    }

    return nullptr;
}

std::uint32_t register_value(const CPUIDRecord &record, CPUIDRegister reg) noexcept
{
    switch (reg)
    {
    case CPUIDRegister::EAX: return record.eax;
    case CPUIDRegister::EBX: return record.ebx;
    case CPUIDRegister::ECX: return record.ecx;
    case CPUIDRegister::EDX: return record.edx;
    }

    return {};
}

void decode_features(CPUFingerprint &fingerprint)
{
    for (auto &&definition : CPUID_FEATURES)
    {
        auto *record = find_record(fingerprint.raw_cpuid, definition.leaf, definition.subleaf);
        if (record != nullptr && (register_value(*record, definition.reg) & 1u << definition.bit) != 0)
        {
            fingerprint.cpuid_features.emplace_back(definition.name);
        }
    }
}

void decode_enabled_xstate(CPUFingerprint &fingerprint)
{
    if (!fingerprint.xcr0_supported || !fingerprint.xcr0)
    {
        return;
    }

    auto enabled_state_components = *fingerprint.xcr0_supported & *fingerprint.xcr0;
    for (auto &&definition : XSTATE_COMPONENTS)
    {
        if ((enabled_state_components & 1ull << definition.bit) != 0)
        {
            fingerprint.enabled_xstate.emplace_back(definition.name);
        }
    }
}

} // namespace

CPUFingerprint query_cpu_fingerprint(CPUQuerySource &source)
{
    CPUFingerprint fingerprint{};
    fingerprint.raw_cpuid.reserve(32);

    auto query_and_record = [&source, &fingerprint](std::uint32_t leaf, std::uint32_t subleaf)
    {
        auto result = source.cpuid(leaf, subleaf);
        append_record(fingerprint, leaf, subleaf, result);

        return result;
    };

    auto vendor                    = query_and_record(CPUID_VENDOR_LEAF, 0);
    fingerprint.maximum_basic_leaf = vendor.eax;

    append_register_text(fingerprint.vendor, vendor.ebx);
    append_register_text(fingerprint.vendor, vendor.edx);
    append_register_text(fingerprint.vendor, vendor.ecx);
    rtrim(fingerprint.vendor, CPUID_PADDING);

    bool xsave_supported{}, osxsave_supported{};
    if (fingerprint.maximum_basic_leaf >= CPUID_SIGNATURE_LEAF)
    {
        auto signature       = query_and_record(CPUID_SIGNATURE_LEAF, 0);
        auto decoded         = fingerprint.vendor == "AuthenticAMD" || fingerprint.vendor == "HygonGenuine" ? decode_amd_signature(signature.eax)
                                                                                                            : decode_intel_signature(signature.eax);
        fingerprint.family   = decoded.family;
        fingerprint.model    = decoded.model;
        fingerprint.stepping = decoded.stepping;
        fingerprint.hypervisor_present = (signature.ecx & CPUID_HYPERVISOR_MASK) != 0;

        xsave_supported   = (signature.ecx & CPUID_XSAVE_MASK) != 0;
        osxsave_supported = (signature.ecx & CPUID_OSXSAVE_MASK) != 0;
    }

    if (fingerprint.maximum_basic_leaf >= CPUID_STRUCTURED_FEATURES_LEAF)
    {
        auto features        = query_and_record(CPUID_STRUCTURED_FEATURES_LEAF, 0);
        auto maximum_subleaf = std::min(features.eax, CPUID_MAX_STRUCTURED_FEATURE_SUBLEAF);
        for (std::uint32_t subleaf = 1; subleaf <= maximum_subleaf; ++subleaf)
        {
            query_and_record(CPUID_STRUCTURED_FEATURES_LEAF, subleaf);
        }
    }

    if (fingerprint.maximum_basic_leaf >= CPUID_XSTATE_LEAF && xsave_supported)
    {
        auto xcr0                  = query_and_record(CPUID_XSTATE_LEAF, 0);
        fingerprint.xcr0_supported = (std::uint64_t)xcr0.edx << 32 | xcr0.eax;

        auto xss                  = query_and_record(CPUID_XSTATE_LEAF, 1);
        fingerprint.xss_supported = (std::uint64_t)xss.edx << 32 | xss.ecx;

        auto supported_state_components = *fingerprint.xcr0_supported | *fingerprint.xss_supported;

        for (std::uint32_t subleaf = 2; subleaf < 64; ++subleaf)
        {
            if ((supported_state_components & 1ull << subleaf) != 0)
            {
                query_and_record(CPUID_XSTATE_LEAF, subleaf);
            }
        }

        if (osxsave_supported)
        {
            fingerprint.xcr0 = source.xgetbv(0);
        }
    }

    if (fingerprint.hypervisor_present)
    {
        auto hypervisor = query_and_record(CPUID_HYPERVISOR_LEAF, 0);
        append_register_text(fingerprint.hypervisor_vendor, hypervisor.ebx);
        append_register_text(fingerprint.hypervisor_vendor, hypervisor.ecx);
        append_register_text(fingerprint.hypervisor_vendor, hypervisor.edx);
        rtrim(fingerprint.hypervisor_vendor, CPUID_PADDING);

        if (hypervisor.eax >= CPUID_HYPERVISOR_INTERFACE)
        {
            auto interface = query_and_record(CPUID_HYPERVISOR_INTERFACE, 0);

            // Leaf 0x40000001 is vendor-specific.
            // Only Hyper-V defines EAX as an ASCII interface signature.
            if (fingerprint.hypervisor_vendor == "Microsoft Hv")
            {
                append_register_text(fingerprint.hypervisor_interface, interface.eax);
                rtrim(fingerprint.hypervisor_interface, CPUID_PADDING);
            }
        }
    }

    auto extended                     = query_and_record(CPUID_EXTENDED_LIMIT_LEAF, 0);
    fingerprint.maximum_extended_leaf = extended.eax;

    if (fingerprint.maximum_extended_leaf >= CPUID_EXTENDED_FEATURES_LEAF)
    {
        query_and_record(CPUID_EXTENDED_FEATURES_LEAF, 0);
    }

    if (fingerprint.maximum_extended_leaf >= CPUID_BRAND_LAST_LEAF)
    {
        std::string brand{};
        brand.reserve(48);

        for (auto leaf = CPUID_BRAND_FIRST_LEAF; leaf <= CPUID_BRAND_LAST_LEAF; ++leaf)
        {
            append_brand_leaf(brand, query_and_record(leaf, 0));
        }

        trim(brand, CPUID_PADDING);

        fingerprint.brand = std::move(brand);
    }

    if (fingerprint.maximum_extended_leaf >= CPUID_ADDRESS_WIDTH_LEAF)
    {
        auto widths                        = query_and_record(CPUID_ADDRESS_WIDTH_LEAF, 0);
        fingerprint.physical_address_width = (std::uint8_t)(widths.eax & 0xFFu);
        fingerprint.linear_address_width   = (std::uint8_t)(widths.eax >> 8 & 0xFFu);
    }

    decode_features(fingerprint);
    decode_enabled_xstate(fingerprint);

    return fingerprint;
}

std::string format_cpu_summary(const CPUFingerprint &fingerprint)
{
    auto summary = fingerprint.brand.empty() ? std::string_view{fingerprint.vendor} : std::string_view{fingerprint.brand};
    if (summary.empty())
    {
        summary = "Unknown";
    }

    std::string details{};
    if (!fingerprint.brand.empty() && !fingerprint.vendor.empty())
    {
        details = fingerprint.vendor;
    }

    auto append_identity = [&details](std::string_view name, std::uint32_t value)
    {
        if (!details.empty())
        {
            details += ' ';
        }

        details += fmt::format("{} 0x{:X}", name, value);
    };

    if (fingerprint.family)
    {
        append_identity("family", *fingerprint.family);
    }

    if (fingerprint.model)
    {
        append_identity("model", *fingerprint.model);
    }

    if (fingerprint.stepping)
    {
        append_identity("stepping", *fingerprint.stepping);
    }

    if (fingerprint.xcr0)
    {
        if (!details.empty())
        {
            details += ", ";
        }

        details += fmt::format("XCR0 0x{:016X}", *fingerprint.xcr0);
    }

    if (details.empty())
    {
        return std::string{summary};
    }

    return fingerprint.brand.empty() ? fmt::format("{} {}", summary, details) : fmt::format("{} ({})", summary, details);
}

const std::shared_ptr<const CPUFingerprint> &host_cpu_fingerprint()
{
    static auto result = []
    {
        HostCPUQuerySource source{};

        return std::make_shared<const CPUFingerprint>(query_cpu_fingerprint(source));
    }();

    return result;
}

} // namespace bme
