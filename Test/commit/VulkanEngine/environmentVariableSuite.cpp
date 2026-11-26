// The engine reads its knobs through environmentVariable, which takes getenv_s on Windows and getenv elsewhere.

#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "ScopedEnvVar.hpp"
#include "common/EnvironmentVariable.hpp"

namespace {

using Kataglyphis::TestSupport::ScopedEnvVar;

constexpr const char *kName = "KATAGLYPHIS_ENVIRONMENT_VARIABLE_SUITE_PROBE";

TEST(EnvironmentVariableUnit, AnUnsetVariableReadsAsNullopt)
{
    const ScopedEnvVar unset(kName, "");
    EXPECT_EQ(Kataglyphis::environmentVariable(kName), std::nullopt);
    EXPECT_FALSE(Kataglyphis::environmentFlag(kName));
}

TEST(EnvironmentVariableUnit, ASetVariableReadsBackWhole)
{
    // Long enough to need the second getenv_s call's buffer, with a space and a separator in it.
    const std::string value = "C:/some dir/with a long enough path;and=more " + std::string(300, 'x');
    const ScopedEnvVar set(kName, value.c_str());
    EXPECT_EQ(Kataglyphis::environmentVariable(kName), value);
    EXPECT_TRUE(Kataglyphis::environmentFlag(kName));
}

TEST(EnvironmentVariableUnit, AOneCharacterValueKeepsItsCharacter)
{
    const ScopedEnvVar set(kName, "1");
    EXPECT_EQ(Kataglyphis::environmentVariable(kName), std::string("1"));
    EXPECT_TRUE(Kataglyphis::environmentFlag(kName));
}

}// namespace
