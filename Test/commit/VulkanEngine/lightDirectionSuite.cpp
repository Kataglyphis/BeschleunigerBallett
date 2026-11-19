// The GUI slider can drag the light direction to zero, which would normalize to NaN in every shader.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

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
    EXPECT_EQ(result, glm::vec3(0.0F, -1.0F, 0.0F)) << "fallback must match CascadedShadowMapMath.cpp's rule";
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

TEST(LightDirectionUnit, NonFiniteDirectionsReturnTheFallback)
{
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    constexpr float kInf = std::numeric_limits<float>::infinity();
    // cascades.rs's normalize_or falls back the same way; a NaN light turns every cascade matrix into NaN.
    for (const glm::vec3 raw :
      { glm::vec3(kNaN, -1.0F, 0.0F), glm::vec3(0.0F, kInf, 0.0F), glm::vec3(-kInf, 1.0F, 2.0F) }) {
        EXPECT_EQ(normalizedLightDirection(raw), glm::vec3(0.0F, -1.0F, 0.0F))
          << "(" << raw.x << ", " << raw.y << ", " << raw.z << ")";
    }
}
