#include <gtest/gtest.h>

#include <array>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <span>

#include "common/GuiModelTransform.hpp"

using Kataglyphis::makeGuiModelTransform;

namespace {

constexpr float kEpsilon = 1e-5F;

void expect_mat4_near(const glm::mat4 &actual, const glm::mat4 &expected, float epsilon = kEpsilon)
{
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) { EXPECT_NEAR(actual[col][row], expected[col][row], epsilon); }
    }
}

}// namespace

TEST(GuiModelTransformUnit, ZeroInputIsIdentity)
{
    constexpr std::array<float, 3> position{ 0.0f, 0.0f, 0.0f };
    constexpr std::array<float, 3> rotation{ 0.0f, 0.0f, 0.0f };

    const glm::mat4 result =
      makeGuiModelTransform(std::span<const float, 3>(position), std::span<const float, 3>(rotation));

    expect_mat4_near(result, glm::mat4(1.0f));
}

TEST(GuiModelTransformUnit, PositionLandsInTheTranslationColumn)
{
    constexpr std::array<float, 3> position{ 1.5f, -2.5f, 3.0f };
    constexpr std::array<float, 3> rotation{ 0.0f, 0.0f, 0.0f };

    const glm::mat4 result =
      makeGuiModelTransform(std::span<const float, 3>(position), std::span<const float, 3>(rotation));

    EXPECT_NEAR(result[3][0], 1.5f, kEpsilon);
    EXPECT_NEAR(result[3][1], -2.5f, kEpsilon);
    EXPECT_NEAR(result[3][2], 3.0f, kEpsilon);
    EXPECT_NEAR(result[3][3], 1.0f, kEpsilon);

    // The rest of the matrix stays the identity's linear part - no scale.
    expect_mat4_near(glm::mat4(glm::mat3(result)), glm::mat4(1.0f));
}

TEST(GuiModelTransformUnit, RotationIsAppliedZThenYThenX)
{
    constexpr std::array<float, 3> position{ 0.0f, 0.0f, 0.0f };
    constexpr std::array<float, 3> rotation{ 30.0f, 45.0f, 60.0f };

    const glm::mat4 result =
      makeGuiModelTransform(std::span<const float, 3>(position), std::span<const float, 3>(rotation));

    glm::mat4 expected = glm::mat4(1.0f);
    expected = glm::rotate(expected, glm::radians(rotation[2]), glm::vec3(0.0f, 0.0f, 1.0f));
    expected = glm::rotate(expected, glm::radians(rotation[1]), glm::vec3(0.0f, 1.0f, 0.0f));
    expected = glm::rotate(expected, glm::radians(rotation[0]), glm::vec3(1.0f, 0.0f, 0.0f));

    expect_mat4_near(result, expected);
}
