#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/EnvironmentVariable.hpp"
#include "scene/TextureFit.hpp"

namespace Kataglyphis {

// True iff the six faces share one square size: one layerSize is copied per face, and Vulkan's cube faces are square.
constexpr bool cubemapFacesConsistent(std::span<const int, 6> widths, std::span<const int, 6> heights)
{
    if (widths[0] != heights[0]) { return false; }
    for (size_t i = 0; i < 6; ++i) {
        if (widths[i] <= 0 || heights[i] <= 0) { return false; }
        if (widths[i] != widths[0] || heights[i] != heights[0]) { return false; }
    }
    return true;
}

/// Six square sRGB RGBA8 faces back to back, each halved until it fits `limit` (maxImageDimensionCube).
struct FittedCubemap
{
    uint32_t size = 0;
    uint32_t halvings = 0;
    std::vector<unsigned char> texels;
};

inline auto fitCubemapFacesToLimit(std::span<const unsigned char *const, 6> faces, uint32_t size, uint32_t limit)
  -> FittedCubemap
{
    FittedCubemap cubemap;
    for (const unsigned char *face : faces) {
        const FittedRgba8 fitted = fitRgba8ToLimit(face, size, size, limit, true);
        cubemap.size = fitted.width;
        cubemap.halvings = fitted.halvings;
        cubemap.texels.insert(cubemap.texels.end(), fitted.texels.begin(), fitted.texels.end());
    }
    return cubemap;
}

// KATAGLYPHIS_SKYBOX_DIR names another folder of the six faces; empty or unset keeps the fetched sky.
inline auto skyboxDirectoryOverride() -> std::optional<std::filesystem::path>
{
    const std::optional<std::string> value = environmentVariable("KATAGLYPHIS_SKYBOX_DIR");
    if (!value.has_value() || value->empty()) { return std::nullopt; }
    return std::filesystem::path(*value);
}

}// namespace Kataglyphis
