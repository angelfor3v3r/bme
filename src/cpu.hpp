#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bme
{

enum class CPUCaptureScope : std::uint8_t
{
    HostVisibleProcess = 0,
};

struct CPUIDRegisters
{
    std::uint32_t eax{};
    std::uint32_t ebx{};
    std::uint32_t ecx{};
    std::uint32_t edx{};
};

// Separates query policy from compiler intrinsics.
// Tests provide fixed results without executing host instructions.
class CPUQuerySource
{
public:
    virtual ~CPUQuerySource() = default;

    virtual CPUIDRegisters cpuid(std::uint32_t leaf, std::uint32_t subleaf) = 0;
    virtual std::uint64_t  xgetbv(std::uint32_t index)                      = 0;
};

struct CPUIDRecord
{
    std::uint32_t leaf{};
    std::uint32_t subleaf{};
    std::uint32_t eax{};
    std::uint32_t ebx{};
    std::uint32_t ecx{};
    std::uint32_t edx{};
};

// Host-visible processor data only.
// Do not add guessed microarchitecture or machine identity fields.
struct CPUFingerprint
{
    CPUCaptureScope capture_scope = CPUCaptureScope::HostVisibleProcess;
    bool            execution_cpu_attributed{};

    std::string                  vendor{};
    std::string                  brand{};
    std::optional<std::uint32_t> family{};
    std::optional<std::uint32_t> model{};
    std::optional<std::uint32_t> stepping{};

    std::uint32_t               maximum_basic_leaf{};
    std::uint32_t               maximum_extended_leaf{};
    std::optional<std::uint8_t> physical_address_width{};
    std::optional<std::uint8_t> linear_address_width{};

    bool        hypervisor_present{};
    std::string hypervisor_vendor{};
    std::string hypervisor_interface{};

    // Keep CPUID advertisement distinct from OS-enabled XSTATE.
    std::vector<std::string>     cpuid_features{};
    std::optional<std::uint64_t> xcr0_supported{};
    std::optional<std::uint64_t> xss_supported{};
    std::optional<std::uint64_t> xcr0{};
    std::vector<std::string>     enabled_xstate{};

    // Raw registers stay sorted by leaf and subleaf and preserve unknown feature bits.
    std::vector<CPUIDRecord> raw_cpuid{};
};

CPUFingerprint                               query_cpu_fingerprint(CPUQuerySource &source);
std::string                                  format_cpu_summary(const CPUFingerprint &fingerprint);
const std::shared_ptr<const CPUFingerprint> &host_cpu_fingerprint();

} // namespace bme
