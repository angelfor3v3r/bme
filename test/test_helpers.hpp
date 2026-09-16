#pragma once

#include "bme_core.hpp"
#include "common.hpp"

#include <gtest/gtest.h>

#if BME_OS_LINUX
#include <cstdlib>
#endif

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if BME_OS_WINDOWS
#include <Windows.h>
#endif

// Pull the library namespace into scope so these helpers and every test that includes this header use the `bme::` API unqualified.
using namespace bme;

class EnvironmentVariableScope
{
public:
    explicit EnvironmentVariableScope(std::string name) : m_name{std::move(name)}
    {
#if BME_OS_WINDOWS
        std::array<char, 32'768> previous{};

        SetLastError(ERROR_SUCCESS);

        auto length = GetEnvironmentVariableA(m_name.c_str(), previous.data(), (DWORD)previous.size());
        if (length == 0)
        {
            auto error = GetLastError();
            if (error == ERROR_ENVVAR_NOT_FOUND)
            {
                m_captured = true;

                return;
            }

            if (error == ERROR_SUCCESS)
            {
                m_had_previous = true;
                m_captured     = true;

                return;
            }

            ADD_FAILURE() << "GetEnvironmentVariableA failed with " << error;

            return;
        }

        if ((std::size_t)length >= previous.size())
        {
            ADD_FAILURE() << "GetEnvironmentVariableA reported an oversized value";

            return;
        }

        m_previous.assign(previous.data(), length);

        m_had_previous = true;
        m_captured     = true;
#else
        auto *previous = std::getenv(m_name.c_str());
        if (previous != nullptr)
        {
            m_previous     = previous;
            m_had_previous = true;
        }

        m_captured = true;
#endif
    }

    EnvironmentVariableScope(const EnvironmentVariableScope &)             = delete;
    EnvironmentVariableScope &operator= (const EnvironmentVariableScope &) = delete;
    EnvironmentVariableScope(EnvironmentVariableScope &&)                  = delete;
    EnvironmentVariableScope &operator= (EnvironmentVariableScope &&)      = delete;

    ~EnvironmentVariableScope()
    {
        if (!m_captured)
        {
            return;
        }

#if BME_OS_WINDOWS
        auto restored =
            m_had_previous ? SetEnvironmentVariableA(m_name.c_str(), m_previous.c_str()) : SetEnvironmentVariableA(m_name.c_str(), nullptr);
        if (restored == FALSE)
        {
            auto error = GetLastError();
            ADD_FAILURE() << "SetEnvironmentVariableA failed while restoring with " << error;
        }
#else
        auto result = m_had_previous ? setenv(m_name.c_str(), m_previous.c_str(), 1) : unsetenv(m_name.c_str());
        if (result != 0)
        {
            ADD_FAILURE() << "Could not restore the environment variable";
        }
#endif
    }

    void set(std::string_view value)
    {
        ASSERT_TRUE(m_captured);

        std::string value_text{value};
#if BME_OS_WINDOWS
        auto result = SetEnvironmentVariableA(m_name.c_str(), value_text.c_str());
        if (result == FALSE)
        {
            auto error = GetLastError();
            ADD_FAILURE() << "SetEnvironmentVariableA failed with " << error;
        }
#else
        auto result = setenv(m_name.c_str(), value_text.c_str(), 1);
        if (result != 0)
        {
            ADD_FAILURE() << "Could not set the environment variable";
        }
#endif
    }

    void clear()
    {
        ASSERT_TRUE(m_captured);

#if BME_OS_WINDOWS
        auto result = SetEnvironmentVariableA(m_name.c_str(), nullptr);
        if (result == FALSE)
        {
            auto error = GetLastError();
            ADD_FAILURE() << "SetEnvironmentVariableA failed while clearing with " << error;
        }
#else
        auto result = unsetenv(m_name.c_str());
        if (result != 0)
        {
            ADD_FAILURE() << "Could not clear the environment variable";
        }
#endif
    }

    [[nodiscard]] std::string_view name() const noexcept { return m_name; }

private:
    std::string m_name{};
    std::string m_previous{};
    bool        m_had_previous{};
    bool        m_captured{};
};

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
// `std::fflush` forces `fmt` output to the captured descriptors before they are read back.
inline auto run_quick_capture(const CLI &cli)
{
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();

    QuickCapture result{};
    result.return_code = run_quick(cli);

    EXPECT_EQ(std::fflush(stdout), 0);
    EXPECT_EQ(std::fflush(stderr), 0);

    result.out = testing::internal::GetCapturedStdout();
    result.err = testing::internal::GetCapturedStderr();

    return result;
}
