#include "cpu.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace bme;

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

using CPUIDQuery = std::pair<std::uint32_t, std::uint32_t>;

std::uint32_t text_word(std::string_view text, std::size_t offset)
{
    auto character = [&text](std::size_t index) -> std::uint32_t { return index < text.size() ? (std::uint8_t)text[index] : (std::uint8_t)' '; };

    return character(offset) | character(offset + 1) << 8 | character(offset + 2) << 16 | character(offset + 3) << 24;
}

CPUIDRegisters text_registers(std::string_view text, std::size_t offset)
{
    return {
        .eax = text_word(text, offset),
        .ebx = text_word(text, offset + 4),
        .ecx = text_word(text, offset + 8),
        .edx = text_word(text, offset + 12),
    };
}

class FixtureCPUQuerySource final : public CPUQuerySource
{
public:
    void respond(std::uint32_t leaf, std::uint32_t subleaf, CPUIDRegisters registers)
    {
        responses.insert_or_assign(CPUIDQuery{leaf, subleaf}, registers);
    }

    void respond_with_vendor(std::uint32_t maximum_leaf, std::string_view vendor)
    {
        respond(
            CPUID_VENDOR_LEAF, 0,
            {
                .eax = maximum_leaf,
                .ebx = text_word(vendor, 0),
                .ecx = text_word(vendor, 8),
                .edx = text_word(vendor, 4),
            }
        );
    }

    void respond_with_hypervisor(std::uint32_t maximum_leaf, std::string_view vendor)
    {
        respond(
            CPUID_HYPERVISOR_LEAF, 0,
            {
                .eax = maximum_leaf,
                .ebx = text_word(vendor, 0),
                .ecx = text_word(vendor, 4),
                .edx = text_word(vendor, 8),
            }
        );
    }

    void respond_with_brand(std::string_view brand)
    {
        for (std::uint32_t index = 0; index < 3; ++index)
        {
            respond(CPUID_BRAND_FIRST_LEAF + index, 0, text_registers(brand, (std::size_t)index * 16));
        }
    }

    CPUIDRegisters cpuid(std::uint32_t leaf, std::uint32_t subleaf) override
    {
        queries.emplace_back(leaf, subleaf);

        auto result = responses.find(CPUIDQuery{leaf, subleaf});

        return result == responses.end() ? CPUIDRegisters{} : result->second;
    }

    std::uint64_t xgetbv(std::uint32_t index) override
    {
        xgetbv_queries.emplace_back(index);

        return xcr0;
    }

    std::map<CPUIDQuery, CPUIDRegisters> responses{};
    std::vector<CPUIDQuery>              queries{};
    std::vector<std::uint32_t>           xgetbv_queries{};
    std::uint64_t                        xcr0{};
};

bool queried(const FixtureCPUQuerySource &source, std::uint32_t leaf, std::uint32_t subleaf)
{
    return std::ranges::find(source.queries, CPUIDQuery{leaf, subleaf}) != source.queries.end();
}

void expect_ordered_unique(const std::vector<CPUIDRecord> &records)
{
    auto less = [](const CPUIDRecord &left, const CPUIDRecord &right)
    { return left.leaf < right.leaf || (left.leaf == right.leaf && left.subleaf < right.subleaf); };

    auto equal = [](const CPUIDRecord &left, const CPUIDRecord &right) { return left.leaf == right.leaf && left.subleaf == right.subleaf; };

    EXPECT_TRUE(std::ranges::is_sorted(records, less));
    EXPECT_EQ(std::ranges::adjacent_find(records, equal), records.end());
}

} // namespace

TEST(CPUFingerprintQuery, DecodesSyntheticAMDIdentityFeaturesAndXState)
{
    FixtureCPUQuerySource source{};
    source.respond_with_vendor(CPUID_XSTATE_LEAF, "AuthenticAMD");

    auto signature = 0xAu << 20 | 0x4u << 16 | 0xFu << 8 | 0x4u << 4 | 0x1u;
    source.respond(
        CPUID_SIGNATURE_LEAF, 0,
        {
            .eax = signature,
            .ebx = 0,
            .ecx = 1u << 26 | 1u << 27 | 1u << 28,
            .edx = 1u << 26,
        }
    );
    source.respond(CPUID_STRUCTURED_FEATURES_LEAF, 0, {.eax = 2, .ebx = 1u << 5, .ecx = 0, .edx = 0});
    source.respond(CPUID_XSTATE_LEAF, 0, {.eax = 1u << 0 | 1u << 1 | 1u << 2 | 1u << 5, .ebx = 0, .ecx = 0, .edx = 0});
    source.respond(CPUID_XSTATE_LEAF, 1, {.eax = 0, .ebx = 0, .ecx = 1u << 8, .edx = 0});
    source.respond(CPUID_EXTENDED_LIMIT_LEAF, 0, {.eax = CPUID_ADDRESS_WIDTH_LEAF, .ebx = 0, .ecx = 0, .edx = 0});
    source.respond(CPUID_EXTENDED_FEATURES_LEAF, 0, {.eax = 0, .ebx = 0, .ecx = 1u << 2, .edx = 1u << 20});
    source.respond_with_brand("   AMD Fixture Processor");
    source.respond(CPUID_ADDRESS_WIDTH_LEAF, 0, {.eax = 48u | 57u << 8, .ebx = 0, .ecx = 0, .edx = 0});
    source.xcr0      = 1u << 0 | 1u << 1 | 1u << 2;
    auto fingerprint = query_cpu_fingerprint(source);
    EXPECT_EQ(fingerprint.vendor, "AuthenticAMD");
    EXPECT_EQ(fingerprint.brand, "AMD Fixture Processor");
    ASSERT_TRUE(fingerprint.family);
    ASSERT_TRUE(fingerprint.model);
    ASSERT_TRUE(fingerprint.stepping);
    EXPECT_EQ(*fingerprint.family, 0x19u);
    EXPECT_EQ(*fingerprint.model, 0x44u);
    EXPECT_EQ(*fingerprint.stepping, 0x1u);
    ASSERT_TRUE(fingerprint.xcr0_supported);
    ASSERT_TRUE(fingerprint.xss_supported);
    ASSERT_TRUE(fingerprint.xcr0);
    EXPECT_EQ(*fingerprint.xcr0_supported, 0x27u);
    EXPECT_EQ(*fingerprint.xss_supported, 0x100u);
    EXPECT_EQ(*fingerprint.xcr0, 0x7u);
    EXPECT_EQ(fingerprint.enabled_xstate, (std::vector<std::string>{"x87", "sse", "avx"}));
    EXPECT_EQ(fingerprint.cpuid_features, (std::vector<std::string>{"sse2", "xsave", "osxsave", "avx", "avx2", "svm", "nx"}));
    ASSERT_TRUE(fingerprint.physical_address_width);
    ASSERT_TRUE(fingerprint.linear_address_width);
    EXPECT_EQ(*fingerprint.physical_address_width, 48u);
    EXPECT_EQ(*fingerprint.linear_address_width, 57u);
    EXPECT_EQ(source.xgetbv_queries, (std::vector<std::uint32_t>{0}));
    EXPECT_TRUE(queried(source, CPUID_XSTATE_LEAF, 2));
    EXPECT_TRUE(queried(source, CPUID_XSTATE_LEAF, 5));
    EXPECT_TRUE(queried(source, CPUID_XSTATE_LEAF, 8));

    expect_ordered_unique(fingerprint.raw_cpuid);
}

TEST(CPUFingerprintQuery, DecodesIntelIdentityAndGatesHypervisorInterface)
{
    FixtureCPUQuerySource source{};
    source.respond_with_vendor(CPUID_SIGNATURE_LEAF, "GenuineIntel");
    source.respond(CPUID_SIGNATURE_LEAF, 0, {.eax = 0x9u << 16 | 0x6u << 8 | 0xAu << 4 | 0x3u, .ebx = 0, .ecx = 1u << 31, .edx = 0});
    source.respond_with_hypervisor(CPUID_HYPERVISOR_INTERFACE, "Microsoft Hv");
    source.respond(CPUID_HYPERVISOR_INTERFACE, 0, {.eax = text_word("Hv#1", 0), .ebx = 0, .ecx = 0, .edx = 0});
    source.respond(CPUID_EXTENDED_LIMIT_LEAF, 0, {});
    auto fingerprint = query_cpu_fingerprint(source);
    EXPECT_EQ(fingerprint.vendor, "GenuineIntel");
    ASSERT_TRUE(fingerprint.family);
    ASSERT_TRUE(fingerprint.model);
    ASSERT_TRUE(fingerprint.stepping);
    EXPECT_EQ(*fingerprint.family, 0x6u);
    EXPECT_EQ(*fingerprint.model, 0x9Au);
    EXPECT_EQ(*fingerprint.stepping, 0x3u);
    EXPECT_TRUE(fingerprint.hypervisor_present);
    EXPECT_EQ(fingerprint.hypervisor_vendor, "Microsoft Hv");
    EXPECT_EQ(fingerprint.hypervisor_interface, "Hv#1");
    EXPECT_TRUE(queried(source, CPUID_HYPERVISOR_INTERFACE, 0));

    FixtureCPUQuerySource gated_source{};
    gated_source.respond_with_vendor(CPUID_SIGNATURE_LEAF, "GenuineIntel");
    gated_source.respond(CPUID_SIGNATURE_LEAF, 0, {.eax = 0, .ebx = 0, .ecx = 1u << 31, .edx = 0});
    gated_source.respond_with_hypervisor(CPUID_HYPERVISOR_INTERFACE, "KVMKVMKVM");
    gated_source.respond(CPUID_HYPERVISOR_INTERFACE, 0, {.eax = 1, .ebx = 0, .ecx = 0, .edx = 0});
    gated_source.respond(CPUID_EXTENDED_LIMIT_LEAF, 0, {});

    auto gated_fingerprint = query_cpu_fingerprint(gated_source);
    EXPECT_TRUE(gated_fingerprint.hypervisor_interface.empty());
    EXPECT_TRUE(queried(gated_source, CPUID_HYPERVISOR_INTERFACE, 0));
}

TEST(CPUFingerprintQuery, OmitsUnsupportedLeavesAndIncompleteBrand)
{
    FixtureCPUQuerySource source{};
    source.respond_with_vendor(CPUID_VENDOR_LEAF, "MinimalCPU  ");
    source.respond(CPUID_EXTENDED_LIMIT_LEAF, 0, {.eax = 0x8000'0003u, .ebx = 0, .ecx = 0, .edx = 0});
    source.respond(CPUID_EXTENDED_FEATURES_LEAF, 0, {});

    auto fingerprint = query_cpu_fingerprint(source);
    EXPECT_EQ(fingerprint.vendor, "MinimalCPU");
    EXPECT_TRUE(fingerprint.brand.empty());
    EXPECT_FALSE(fingerprint.family);
    EXPECT_FALSE(fingerprint.xcr0_supported);
    EXPECT_FALSE(fingerprint.xss_supported);
    EXPECT_FALSE(fingerprint.xcr0);
    EXPECT_TRUE(source.xgetbv_queries.empty());

    for (auto leaf = CPUID_BRAND_FIRST_LEAF; leaf <= CPUID_BRAND_LAST_LEAF; ++leaf)
    {
        EXPECT_FALSE(queried(source, leaf, 0));
    }

    EXPECT_EQ(source.queries, (std::vector<CPUIDQuery>{{CPUID_VENDOR_LEAF, 0}, {CPUID_EXTENDED_LIMIT_LEAF, 0}, {CPUID_EXTENDED_FEATURES_LEAF, 0}}));
}

TEST(CPUFingerprintQuery, RequiresXsaveForStateLeavesAndOSXSaveForXgetbv)
{
    FixtureCPUQuerySource source{};
    source.respond_with_vendor(CPUID_XSTATE_LEAF, "GenuineIntel");
    source.respond(CPUID_SIGNATURE_LEAF, 0, {.eax = 0, .ebx = 0, .ecx = 1u << 26, .edx = 0});
    source.respond(CPUID_XSTATE_LEAF, 0, {.eax = 0x3u, .ebx = 0, .ecx = 0, .edx = 0});
    source.respond(CPUID_XSTATE_LEAF, 1, {});
    source.respond(CPUID_EXTENDED_LIMIT_LEAF, 0, {});
    auto fingerprint = query_cpu_fingerprint(source);
    EXPECT_TRUE(fingerprint.xcr0_supported);
    EXPECT_TRUE(fingerprint.xss_supported);
    EXPECT_FALSE(fingerprint.xcr0);
    EXPECT_TRUE(source.xgetbv_queries.empty());
    EXPECT_TRUE(queried(source, CPUID_XSTATE_LEAF, 0));
    EXPECT_TRUE(queried(source, CPUID_XSTATE_LEAF, 1));

    FixtureCPUQuerySource unsupported_source{};
    unsupported_source.respond_with_vendor(CPUID_XSTATE_LEAF, "GenuineIntel");
    unsupported_source.respond(CPUID_SIGNATURE_LEAF, 0, {});
    unsupported_source.respond(CPUID_EXTENDED_LIMIT_LEAF, 0, {});

    auto unsupported_fingerprint = query_cpu_fingerprint(unsupported_source);
    EXPECT_FALSE(unsupported_fingerprint.xcr0_supported);
    EXPECT_FALSE(queried(unsupported_source, CPUID_XSTATE_LEAF, 0));
}

TEST(CPUFingerprintQuery, CapsStructuredFeatureSubleafEnumeration)
{
    FixtureCPUQuerySource source{};
    source.respond_with_vendor(CPUID_STRUCTURED_FEATURES_LEAF, "GenuineIntel");
    source.respond(CPUID_SIGNATURE_LEAF, 0, {});
    source.respond(CPUID_STRUCTURED_FEATURES_LEAF, 0, {.eax = std::numeric_limits<std::uint32_t>::max(), .ebx = 0, .ecx = 0, .edx = 0});
    source.respond(CPUID_EXTENDED_LIMIT_LEAF, 0, {});
    auto fingerprint = query_cpu_fingerprint(source);
    auto structured_queries =
        std::ranges::count_if(source.queries, [](const CPUIDQuery &query) { return query.first == CPUID_STRUCTURED_FEATURES_LEAF; });
    EXPECT_EQ(structured_queries, 129);
    EXPECT_TRUE(queried(source, CPUID_STRUCTURED_FEATURES_LEAF, 128));
    EXPECT_FALSE(queried(source, CPUID_STRUCTURED_FEATURES_LEAF, 129));

    expect_ordered_unique(fingerprint.raw_cpuid);
}

TEST(CPUFingerprintFormat, IncludesAvailableIdentityAndXcr0)
{
    CPUFingerprint fingerprint{};
    fingerprint.vendor   = "AuthenticAMD";
    fingerprint.brand    = "AMD Fixture Processor";
    fingerprint.family   = 0x1Au;
    fingerprint.model    = 0x44u;
    fingerprint.stepping = 0x0u;
    fingerprint.xcr0     = 0x602E7u;
    EXPECT_EQ(format_cpu_summary(fingerprint), "AMD Fixture Processor (AuthenticAMD family 0x1A model 0x44 stepping 0x0, XCR0 0x00000000000602E7)");
}

TEST(CPUFingerprintFormat, FallsBackToVendorAndOmitsUnavailableFields)
{
    CPUFingerprint fingerprint{};
    fingerprint.vendor = "MinimalCPU";
    fingerprint.family = 0x6u;
    EXPECT_EQ(format_cpu_summary(fingerprint), "MinimalCPU family 0x6");

    fingerprint.family.reset();
    EXPECT_EQ(format_cpu_summary(fingerprint), "MinimalCPU");

    fingerprint.vendor.clear();
    EXPECT_EQ(format_cpu_summary(fingerprint), "Unknown");
}

TEST(CPUFingerprintHost, ReturnsOneCachedHostSnapshot)
{
    auto &first  = host_cpu_fingerprint();
    auto &second = host_cpu_fingerprint();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first.get(), second.get());
    EXPECT_FALSE(first->vendor.empty());
    EXPECT_NE(
        std::ranges::find_if(first->raw_cpuid, [](const CPUIDRecord &record) { return record.leaf == CPUID_VENDOR_LEAF && record.subleaf == 0; }),
        first->raw_cpuid.end()
    );
}
