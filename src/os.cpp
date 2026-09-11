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
        result.outcome                  = Outcome::Faulted;
        result.instrumentation_detected = true;
        result.error                    = "Running under an emulator or instrumentation layer. Native single-step tracing is unavailable.";

        return false;
    }

    return true;
}

} // namespace bme
