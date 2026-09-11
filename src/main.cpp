#include "bme_core.hpp"

#include <fmt/format.h>

#include <cstdint>
#include <cstdio>

std::int32_t main(std::int32_t argc, char *argv[], [[maybe_unused]] char *envp[])
{
    auto cli_result = bme::CLI::parse(argc, argv);
    if (!cli_result)
    {
        fmt::println(stderr, "{}", cli_result.error());

        return 1;
    }

    bme::init();

    if (cli_result->quick)
    {
        return bme::run_quick(*cli_result);
    }

    return bme::run_tui(*cli_result);
}
