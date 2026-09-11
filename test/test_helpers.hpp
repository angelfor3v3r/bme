#pragma once

#include "bme_core.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Pull the library namespace into scope so these helpers and every test that includes this header use the `bme::` API unqualified.
using namespace bme;

// Build a `CLI` the way the real command line would, prepending `argv[0]`.
inline auto parse_cli(std::vector<std::string> args)
{
    args.insert(args.begin(), "bme");

    std::vector<char *> argv{};
    argv.reserve(args.size());
    for (auto &&arg : args)
    {
        argv.emplace_back(arg.data());
    }

    return CLI::parse((std::int32_t)argv.size(), argv.data());
}

// Result of running `run_quick` with both streams captured.
struct QuickCapture
{
    std::int32_t return_code{};
    std::string  out{};
    std::string  err{};
};

// Run `run_quick`, capturing stdout and stderr.
// `std::fflush` forces fmt output to the captured descriptors before they are read back.
inline auto run_quick_capture(const CLI &cli)
{
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();

    QuickCapture result{};
    result.return_code = run_quick(cli);

    std::fflush(stdout);
    std::fflush(stderr);

    result.out = testing::internal::GetCapturedStdout();
    result.err = testing::internal::GetCapturedStderr();

    return result;
}
