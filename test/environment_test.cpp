#include "common.hpp"
#include "os.hpp"
#include "test_helpers.hpp"

#if BME_OS_WINDOWS
#include <array>
#else
#include <cstdlib>
#endif

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#if BME_OS_WINDOWS
#include <Windows.h>
#endif

namespace
{

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

    ~EnvironmentVariableScope() noexcept
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

} // namespace

TEST(Environment, StringHasExactLengthAndPresenceTracksEmptyValues)
{
    EnvironmentVariableScope variable{"BME_TUI_ENVIRONMENT_TEST_VALUE"};
    variable.set("value with spaces");

    auto        variable_name = variable.name();
    std::string expected{"value with spaces"};
    auto        actual = environment_string(variable_name);
    EXPECT_EQ(actual, expected);
    EXPECT_TRUE(environment_present(variable_name));

    variable.set("");

    auto empty_value = environment_string(variable_name);
    EXPECT_TRUE(empty_value.empty());
    EXPECT_TRUE(environment_present(variable_name));

    variable.clear();

    EXPECT_EQ(environment_string(variable_name), "");
    EXPECT_FALSE(environment_present(variable_name));
}

TEST(Environment, ScopeRestoresPreviousValue)
{
    EnvironmentVariableScope outer{"BME_TUI_ENVIRONMENT_TEST_VALUE"};
    outer.set("original");

    {
        EnvironmentVariableScope inner{"BME_TUI_ENVIRONMENT_TEST_VALUE"};

        inner.set("temporary");

        EXPECT_EQ(environment_string(inner.name()), "temporary");
    }

    EXPECT_EQ(environment_string(outer.name()), "original");
}
