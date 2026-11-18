#pragma once

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>

namespace Kataglyphis {

// True iff all faces match the first's size and none is degenerate: the upload copies one layerSize per face.
constexpr bool cubemapFacesConsistent(std::span<const int, 6> widths, std::span<const int, 6> heights)
{
    for (size_t i = 0; i < 6; ++i) {
        if (widths[i] <= 0 || heights[i] <= 0) { return false; }
        if (widths[i] != widths[0] || heights[i] != heights[0]) { return false; }
    }
    return true;
}

// KATAGLYPHIS_SKYBOX_DIR names another folder of the six faces; empty or unset keeps the fetched sky.
inline auto skyboxDirectoryOverride() -> std::optional<std::filesystem::path>
{
    const char *value = std::getenv("KATAGLYPHIS_SKYBOX_DIR");
    if (value == nullptr || *value == '\0') { return std::nullopt; }
    return std::filesystem::path(value);
}

}// namespace Kataglyphis
