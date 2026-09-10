#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <Windows.h>

#include "test_helpers.hpp"

namespace bme
{

// White-box declarations for the internal instrumentation helpers.
std::string environment_string(std::string_view name) noexcept;
bool        environment_present(std::string_view name) noexcept;

} // namespace bme

namespace
{

class EnvironmentVariableScope
{
public:
    explicit EnvironmentVariableScope(std::string name) : m_name{std::move(name)}
    {
        // Windows caps one environment variable at 32,767 characters including the terminator.
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

        m_previous.assign(previous.data(), (std::size_t)length);

        m_had_previous = true;
        m_captured     = true;
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

        auto restored =
            m_had_previous ? SetEnvironmentVariableA(m_name.c_str(), m_previous.c_str()) : SetEnvironmentVariableA(m_name.c_str(), nullptr);
        if (restored == FALSE)
        {
            auto error = GetLastError();
            ADD_FAILURE() << "SetEnvironmentVariableA failed while restoring with " << error;
        }
    }

    void set(std::string_view value)
    {
        ASSERT_TRUE(m_captured);

        std::string value_text{value};
        auto        result = SetEnvironmentVariableA(m_name.c_str(), value_text.c_str());
        if (result == FALSE)
        {
            auto error = GetLastError();
            ADD_FAILURE() << "SetEnvironmentVariableA failed with " << error;
        }
    }

    void clear()
    {
        ASSERT_TRUE(m_captured);

        auto result = SetEnvironmentVariableA(m_name.c_str(), nullptr);
        if (result == FALSE)
        {
            auto error = GetLastError();
            ADD_FAILURE() << "SetEnvironmentVariableA failed while clearing with " << error;
        }
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

    auto required = GetEnvironmentVariableA(variable_name.data(), nullptr, 0);
    EXPECT_EQ(required, (DWORD)(expected.size() + 1));

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
