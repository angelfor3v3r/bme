#include "os.hpp"

namespace bme
{

// Round the fixed scratch reservation down to the platform allocation granularity.
// Usable data starts one guard page above this address.
std::uint64_t scratch_reserve_base() noexcept
{
    static auto result = SCRATCH_DATA_RESERVE_BASE & ~(vm_allocation_granularity() - 1);

    return result;
}

std::uint64_t scratch_data_base() noexcept
{
    static auto result = scratch_reserve_base() + vm_page_size();

    return result;
}

bool prepare_platform_run(const PlatformRunRequest &request, PlatformRunResult &result)
{
    result.seed = request.seed;

    if (request.code.empty())
    {
        result.outcome = Outcome::Idle;
        result.error   = "No code to run.";

        return false;
    }

    if (request.stop_offset && *request.stop_offset >= request.code.size())
    {
        result.outcome = Outcome::Error;
        result.error   = "Stop offset is outside the input.";

        return false;
    }

    if (instrumentation_detected())
    {
        result.outcome                  = Outcome::Error;
        result.instrumentation_detected = true;
        result.error                    = INSTRUMENTATION_REFUSAL;

        return false;
    }

    return true;
}

} // namespace bme
