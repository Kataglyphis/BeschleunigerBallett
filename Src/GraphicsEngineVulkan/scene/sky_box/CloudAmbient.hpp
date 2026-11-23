#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include <glm/glm.hpp>

#include "common/SrgbConversion.hpp"

namespace Kataglyphis {

// Mean sky radiance over the upper and the lower hemisphere: volumetric_clouds.slang's CloudAmbient.
struct CloudAmbient
{
    glm::vec3 top{ 0.0F };
    glm::vec3 bottom{ 0.0F };
};

// At most this many texels per face edge are averaged; OxidANT's CloudAmbient::of_cubemap takes the same.
inline constexpr uint32_t kCloudAmbientMaxEdgeTexels = 64;

// skybox_radiance.slang's SKYBOX_DISABLED_RADIANCE, the linear navy the switched-off sky shows.
inline constexpr glm::vec3 kSkyboxDisabledRadiance{ 0.0F, 0.0F, 0.5F };

// aces.slang's ACES_A to ACES_E, x (a x + b) / (x (c x + d) + e); BuildIntegrity pins them to the shader's.
inline constexpr std::array<float, 5> kAcesCoefficients{ 2.51F, 0.03F, 2.43F, 0.59F, 0.14F };

// aces.slang's aces_tonemap, the display value post gives a radiance.
inline auto acesTonemap(float radiance) -> float
{
    const auto [a, b, c, d, e] = kAcesCoefficients;
    const float x = std::max(radiance, 0.0F);
    return std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0F, 1.0F);
}

// aces.slang's aces_tonemap_inverse: the photo sky enters the HDR target as the radiance whose tonemap shows its pixel.
inline auto acesTonemapInverse(float display) -> float
{
    const auto [a, b, c, d, e] = kAcesCoefficients;
    const float y = std::clamp(display, 0.0F, 1.0F);
    const float qa = a - c * y;
    const float qb = b - d * y;
    return (-qb + std::sqrt(qb * qb + 4.0F * qa * e * y)) / (2.0F * qa);
}

inline auto acesTonemapInverse(glm::vec3 display) -> glm::vec3
{ return { acesTonemapInverse(display.x), acesTonemapInverse(display.y), acesTonemapInverse(display.z) }; }

// The switched-off sky: the navy, as post composites the clouds over it.
inline auto cloudAmbientOfDisabledSky() -> CloudAmbient
{
    const glm::vec3 navy = acesTonemapInverse(kSkyboxDisabledRadiance);
    return { navy, navy };
}

// RGBA8 sRGB faces in SkyBox's upload order (+X, -X, +Y, -Y, +Z, -Z); a side face's upper rows look up.
inline auto cloudAmbientOfCubemap(std::span<const unsigned char *const, 6> faces, uint32_t width, uint32_t height)
  -> CloudAmbient
{
    if (width == 0 || height == 0) { return {}; }
    const uint32_t columnStride = std::max(width / kCloudAmbientMaxEdgeTexels, 1U);
    const uint32_t rowStride = std::max(height / kCloudAmbientMaxEdgeTexels, 1U);
    std::array<glm::dvec3, 2> sums{ glm::dvec3(0.0), glm::dvec3(0.0) };
    std::array<double, 2> counts{ 0.0, 0.0 };
    for (std::size_t face = 0; face < faces.size(); ++face) {
        for (uint32_t row = 0; row < height; row += rowStride) {
            const float v = (static_cast<float>(row) + 0.5F) / static_cast<float>(height);
            std::array<double, 2> weights{ 0.5, 0.5 };
            if (face == 2 || (face != 3 && v < 0.5F)) {
                weights = { 1.0, 0.0 };
            } else if (face == 3 || v > 0.5F) {
                weights = { 0.0, 1.0 };
            }
            for (uint32_t column = 0; column < width; column += columnStride) {
                const unsigned char *texel = faces[face] + (static_cast<std::size_t>(row) * width + column) * 4U;
                const glm::dvec3 radiance(acesTonemapInverse(glm::vec3(
                  srgbChannelToLinear(texel[0]), srgbChannelToLinear(texel[1]), srgbChannelToLinear(texel[2]))));
                for (std::size_t half = 0; half < 2; ++half) {
                    sums[half] += radiance * weights[half];
                    counts[half] += weights[half];
                }
            }
        }
    }
    return { glm::vec3(sums[0] / std::max(counts[0], 1.0)), glm::vec3(sums[1] / std::max(counts[1], 1.0)) };
}

}// namespace Kataglyphis
