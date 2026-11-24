#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "common/SrgbConversion.hpp"

namespace Kataglyphis {

/// Levels of a full mip chain over the larger side: 1 for a 1x1 image, 12 for 2048.
inline auto mipLevelCount(uint32_t width, uint32_t height) -> uint32_t
{
    uint32_t levels = 1;
    for (uint32_t side = std::max(width, height); side > 1U; side /= 2U) { ++levels; }
    return levels;
}

/// A tightly packed RGBA8 image and how many times it was halved to fit a device's side limit.
struct FittedRgba8
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t halvings = 0;
    std::vector<unsigned char> texels;
};

/// One box-filtered halving, as Rust's generate_mips: each texel averages its 2x2 block (an odd edge drops its last
/// row or column), sRGB colour in linear space and alpha as stored, rounded half up.
inline auto halveRgba8(const unsigned char *rgba, uint32_t width, uint32_t height, bool srgb) -> FittedRgba8
{
    // Four decodes per source channel otherwise, on an image too large for the device.
    static const std::array<float, 256> kLinear = [] {
        std::array<float, 256> table{};
        for (std::size_t encoded = 0; encoded < table.size(); ++encoded) {
            table[encoded] = srgbChannelToLinear(static_cast<uint8_t>(encoded));
        }
        return table;
    }();

    FittedRgba8 half;
    half.width = std::max(width / 2U, 1U);
    half.height = std::max(height / 2U, 1U);
    half.texels.resize(static_cast<std::size_t>(half.width) * half.height * 4U);
    const auto texel = [width](uint32_t x, uint32_t y) { return (static_cast<std::size_t>(y) * width + x) * 4U; };
    for (uint32_t y = 0; y < half.height; ++y) {
        const uint32_t y0 = std::min(2U * y, height - 1U);
        const uint32_t y1 = std::min(2U * y + 1U, height - 1U);
        for (uint32_t x = 0; x < half.width; ++x) {
            const uint32_t x0 = std::min(2U * x, width - 1U);
            const uint32_t x1 = std::min(2U * x + 1U, width - 1U);
            const std::array<std::size_t, 4> taps = { texel(x0, y0), texel(x1, y0), texel(x0, y1), texel(x1, y1) };
            unsigned char *out = half.texels.data() + (static_cast<std::size_t>(y) * half.width + x) * 4U;
            for (std::size_t channel = 0; channel < 4U; ++channel) {
                if (srgb && channel < 3U) {
                    float sum = 0.0F;
                    for (const std::size_t tap : taps) { sum += kLinear[rgba[tap + channel]]; }
                    out[channel] = linearToSrgbByte(sum / 4.0F);
                } else {
                    unsigned int sum = 0;
                    for (const std::size_t tap : taps) { sum += rgba[tap + channel]; }
                    out[channel] = static_cast<unsigned char>((sum + 2U) / 4U);
                }
            }
        }
    }
    return half;
}

/// Halves until neither side exceeds `limit` (VkPhysicalDeviceLimits::maxImageDimension2D or ...Cube), the first
/// level that fits, as Rust's base_candidates picks; an image already within it comes back unchanged.
inline auto fitRgba8ToLimit(const unsigned char *rgba, uint32_t width, uint32_t height, uint32_t limit, bool srgb)
  -> FittedRgba8
{
    const uint32_t side = std::max(limit, 1U);
    if (std::max(width, height) <= side) {
        return {
            width, height, 0, std::vector<unsigned char>(rgba, rgba + static_cast<std::size_t>(width) * height * 4U)
        };
    }
    // The first halving reads the source, so an oversized image is never copied at full size.
    FittedRgba8 fitted = halveRgba8(rgba, width, height, srgb);
    fitted.halvings = 1U;
    while (std::max(fitted.width, fitted.height) > side) {
        FittedRgba8 half = halveRgba8(fitted.texels.data(), fitted.width, fitted.height, srgb);
        half.halvings = fitted.halvings + 1U;
        fitted = std::move(half);
    }
    return fitted;
}

/// One level of a packed RGBA8 chain to upload: its size and where its texels start in the staged bytes.
struct ChainLevel
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t offset = 0;
};

/// The levels of a `levelCount`-level chain over `width` x `height` that fit `limit`, offsets from the first of them,
/// the bytes the whole chain holds and where the kept levels start; no level is kept when none fits.
struct ChainPlan
{
    std::vector<ChainLevel> levels;
    uint64_t firstOffset = 0;
    uint64_t totalBytes = 0;
};

inline auto planChainUpload(uint32_t width, uint32_t height, uint32_t levelCount, uint32_t limit) -> ChainPlan
{
    ChainPlan plan;
    for (uint32_t level = 0; level < levelCount && level < 32U; ++level) {
        const uint32_t level_width = std::max(width >> level, 1U);
        const uint32_t level_height = std::max(height >> level, 1U);
        if (std::max(level_width, level_height) <= limit) {
            if (plan.levels.empty()) { plan.firstOffset = plan.totalBytes; }
            plan.levels.push_back({ level_width, level_height, plan.totalBytes - plan.firstOffset });
        }
        plan.totalBytes += static_cast<uint64_t>(level_width) * level_height * 4U;
    }
    return plan;
}

}// namespace Kataglyphis
