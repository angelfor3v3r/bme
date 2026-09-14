#include "os.hpp"

namespace bme
{

bool prepare_platform_run(const PlatformRunRequest &request, PlatformRunResult &result)
{
    result.seed = request.seed;

    if (request.code.empty())
    {
        result.outcome = Outcome::Idle;
        result.error   = "No code to run.";

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
