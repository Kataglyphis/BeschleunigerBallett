#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "common/SrgbConversion.hpp"
#include "scene/sky_box/CloudAmbient.hpp"

namespace Kataglyphis {

// OxidANT's shaders/common/sky_model.slang constants; BuildIntegrity.AnalyticSkyMatchesOxidantsSkyModel compares them.
inline constexpr glm::vec3 kAnalyticSkyZenith{ 0.09F, 0.16F, 0.35F };
inline constexpr glm::vec3 kAnalyticSkyHorizon{ 0.55F, 0.62F, 0.72F };
inline constexpr glm::vec3 kAnalyticSkyGround{ 0.18F, 0.16F, 0.15F };

// Texels per face edge of the baked sky; the gradient is smooth, and the cloud ambient averages 64 a side.
inline constexpr uint32_t kAnalyticSkyFaceSize = 64;

// sky_model.slang's sky_gradient: horizon to zenith above, horizon to ground below, no sun.
inline auto analyticSkyRadiance(glm::vec3 direction) -> glm::vec3
{
    const float y = glm::normalize(direction).y;
    if (y >= 0.0F) { return glm::mix(kAnalyticSkyHorizon, kAnalyticSkyZenith, std::pow(std::min(y, 1.0F), 0.7F)); }
    return glm::mix(kAnalyticSkyHorizon, kAnalyticSkyGround, std::min(-y * 3.0F, 1.0F));
}

// The direction through texel (column, row) of a face in SkyBox's upload order (+X, -X, +Y, -Y, +Z, -Z), as Vulkan samples it.
inline auto cubemapTexelDirection(std::size_t face, uint32_t column, uint32_t row, uint32_t size) -> glm::vec3
{
    const float s = 2.0F * (static_cast<float>(column) + 0.5F) / static_cast<float>(size) - 1.0F;
    const float t = 2.0F * (static_cast<float>(row) + 0.5F) / static_cast<float>(size) - 1.0F;
    switch (face) {
    case 0:
        return { 1.0F, -t, -s };
    case 1:
        return { -1.0F, -t, s };
    case 2:
        return { s, 1.0F, t };
    case 3:
        return { s, -1.0F, -t };
    case 4:
        return { s, -t, 1.0F };
    default:
        return { -s, -t, -1.0F };
    }
}

// Six RGBA8 sRGB faces back to back: the sky pass is untonemapped, so each texel is the tonemapped gradient post would show.
inline auto analyticSkyFaces(uint32_t size) -> std::vector<unsigned char>
{
    std::vector<unsigned char> texels(static_cast<std::size_t>(size) * size * 4U * 6U);
    std::size_t at = 0;
    for (std::size_t face = 0; face < 6; ++face) {
        for (uint32_t row = 0; row < size; ++row) {
            for (uint32_t column = 0; column < size; ++column) {
                const glm::vec3 radiance = analyticSkyRadiance(cubemapTexelDirection(face, column, row, size));
                for (int c = 0; c < 3; ++c) { texels[at++] = linearToSrgbByte(acesTonemap(radiance[c])); }
                texels[at++] = 255;
            }
        }
    }
    return texels;
}

}// namespace Kataglyphis
