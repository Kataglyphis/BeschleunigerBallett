// The GUI slider can drag the light direction to zero, which would normalize to NaN in every shader.

#include <gtest/gtest.h>

#include <cmath>

#include <glm/glm.hpp>

#include "common/LightDirection.hpp"

using Kataglyphis::normalizedLightDirection;

TEST(LightDirectionUnit, ZeroVectorReturnsFiniteFallback)
{
    const glm::vec3 result = normalizedLightDirection(glm::vec3(0.0F));

    EXPECT_TRUE(std::isfinite(result.x));
    EXPECT_TRUE(std::isfinite(result.y));
    EXPECT_TRUE(std::isfinite(result.z));
    EXPECT_NEAR(glm::length(result), 1.0F, 1e-5F);
    EXPECT_EQ(result, glm::vec3(0.0F, -1.0F, 0.0F))
      << "fallback must match CascadedShadowMapMath.cpp's rule";
}

TEST(LightDirectionUnit, NonUnitVectorIsNormalizedButKeepsDirection)
{
    const glm::vec3 input(2.0F, -4.0F, 1.0F);
    const glm::vec3 result = normalizedLightDirection(input);

    EXPECT_NEAR(glm::length(result), 1.0F, 1e-5F);
    EXPECT_NEAR(glm::dot(result, glm::normalize(input)), 1.0F, 1e-5F)
      << "normalizing must not change the direction the light travels";
}

TEST(LightDirectionUnit, DefaultGuiDirectionNormalizesWithoutFlippingSign)
{
    const glm::vec3 input(-0.55F, -1.0F, -0.35F);
    const glm::vec3 result = normalizedLightDirection(input);

    EXPECT_NEAR(glm::length(result), 1.0F, 1e-5F);
    EXPECT_LT(result.x, 0.0F);
    EXPECT_LT(result.y, 0.0F);
    EXPECT_LT(result.z, 0.0F);
}
