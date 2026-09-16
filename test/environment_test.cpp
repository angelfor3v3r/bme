#include "common.hpp"
#include "os.hpp"
#include "test_helpers.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

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

TEST(Environment, InstrumentationRefusalIsExecutionError)
{
    EnvironmentVariableScope variable{"SDE_COMMAND_LINE"};
    variable.set("test");

    std::array<std::uint8_t, 1> code{0x90};
    Registers                   seed{};
    auto                        trace = run_engine(code, seed, DEFAULT_MAX_STEPS, DisasmBackend::Zydis, DisasmSyntax::Intel, true);
    EXPECT_EQ(trace.outcome, Outcome::Error);
    EXPECT_TRUE(trace.instrumentation_detected);
    EXPECT_TRUE(trace.execution_events.empty());
    EXPECT_NE(trace.message.find("No trace was recorded"), std::string::npos);

    auto cli = parse_cli({"--bytes", "90", "--quick"});
    ASSERT_TRUE(cli);

    auto capture = run_quick_capture(*cli);
    EXPECT_EQ(capture.return_code, 1);
    EXPECT_TRUE(capture.out.empty());
    EXPECT_NE(capture.err.find("Error: Running under an emulator or instrumentation layer."), std::string::npos);

    auto json_cli = parse_cli({"--bytes", "90", "--quick", "--format", "json"});
    ASSERT_TRUE(json_cli);

    auto json_capture = run_quick_capture(*json_cli);
    EXPECT_EQ(json_capture.return_code, 1);
    EXPECT_TRUE(json_capture.err.empty());
    EXPECT_TRUE(json_capture.out.starts_with('{'));
    EXPECT_TRUE(json_capture.out.ends_with("}\n"));
    EXPECT_NE(json_capture.out.find("\"outcome\":\"error\""), std::string::npos);
    EXPECT_NE(json_capture.out.find("\"instrumentation_detected\":true"), std::string::npos);
}
