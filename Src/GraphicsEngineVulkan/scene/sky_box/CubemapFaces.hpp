#pragma once

#include <cstddef>
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

// SkyBox's fallback face, shared with the tests so the shipped bytes cannot drift from what they assert.
inline constexpr unsigned char kFallbackCubemapFacePixel[4] = { 0, 0, 0, 255 };

}// namespace Kataglyphis
