#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "common/SrgbConversion.hpp"

namespace Kataglyphis {

/// What a texture chain's bytes hold, in OxidANT's bridge order: RGBA8 texels, or one block format's 4x4 blocks.
enum class ChainFormat : uint8_t { Rgba8 = 0, Bc1 = 1, Bc3 = 2, Bc5 = 3, Bc7 = 4, Etc2Rgba8 = 5, Astc4x4 = 6 };

/// The block families a device samples and its longest image side, by which the loader picks a KTX2's format once; the
/// default samples none, so every texture arrives as RGBA8.
struct BlockSupport
{
    bool bc = false;
    bool etc2 = false;
    bool astc = false;
    uint32_t maxSide = 0;
    friend bool operator==(const BlockSupport &, const BlockSupport &) = default;
};

/// A chain as the loader hands it over: what its bytes hold, its base's size and how many levels it has.
struct ChainShape
{
    ChainFormat format = ChainFormat::Rgba8;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levelCount = 0;
};

/// Bytes per 4x4 block, 0 for RGBA8.
constexpr auto chainBlockBytes(ChainFormat format) -> uint32_t
{
    switch (format) {
    case ChainFormat::Rgba8:
        return 0;
    case ChainFormat::Bc1:
        return 8;
    default:
        return 16;
    }
}

/// One `width` x `height` level's bytes: four per texel, or its whole blocks.
constexpr auto chainLevelBytes(ChainFormat format, uint32_t width, uint32_t height) -> uint64_t
{
    const uint32_t block_bytes = chainBlockBytes(format);
    if (block_bytes == 0) { return static_cast<uint64_t>(width) * height * 4U; }
    return static_cast<uint64_t>((width + 3U) / 4U) * ((height + 3U) / 4U) * block_bytes;
}

/// The format a chain uploads as: a colour slot samples the sRGB variant, a data slot UNORM, BC5 its only one, as the
/// WebGPU renderer picks.
constexpr auto chainVkFormat(ChainFormat format, bool srgb) -> vk::Format
{
    switch (format) {
    case ChainFormat::Bc1:
        return srgb ? vk::Format::eBc1RgbaSrgbBlock : vk::Format::eBc1RgbaUnormBlock;
    case ChainFormat::Bc3:
        return srgb ? vk::Format::eBc3SrgbBlock : vk::Format::eBc3UnormBlock;
    case ChainFormat::Bc5:
        return vk::Format::eBc5UnormBlock;
    case ChainFormat::Bc7:
        return srgb ? vk::Format::eBc7SrgbBlock : vk::Format::eBc7UnormBlock;
    case ChainFormat::Etc2Rgba8:
        return srgb ? vk::Format::eEtc2R8G8B8A8SrgbBlock : vk::Format::eEtc2R8G8B8A8UnormBlock;
    case ChainFormat::Astc4x4:
        return srgb ? vk::Format::eAstc4x4SrgbBlock : vk::Format::eAstc4x4UnormBlock;
    case ChainFormat::Rgba8:
    default:
        return srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
    }
}

/// Whether `support` samples `format`'s family; RGBA8 always.
constexpr auto samplesChainFormat(const BlockSupport &support, ChainFormat format) -> bool
{
    switch (format) {
    case ChainFormat::Bc1:
    case ChainFormat::Bc3:
    case ChainFormat::Bc5:
    case ChainFormat::Bc7:
        return support.bc;
    case ChainFormat::Etc2Rgba8:
        return support.etc2;
    case ChainFormat::Astc4x4:
        return support.astc;
    case ChainFormat::Rgba8:
    default:
        return true;
    }
}

/// Each family's formats in both colour spaces they have, which a device must sample for the loader to hand them over.
inline constexpr std::array<vk::Format, 7> kBcFormats = { vk::Format::eBc1RgbaUnormBlock,
    vk::Format::eBc1RgbaSrgbBlock,
    vk::Format::eBc3UnormBlock,
    vk::Format::eBc3SrgbBlock,
    vk::Format::eBc5UnormBlock,
    vk::Format::eBc7UnormBlock,
    vk::Format::eBc7SrgbBlock };
inline constexpr std::array<vk::Format, 2> kEtc2Formats = { vk::Format::eEtc2R8G8B8A8UnormBlock,
    vk::Format::eEtc2R8G8B8A8SrgbBlock };
inline constexpr std::array<vk::Format, 2> kAstcFormats = { vk::Format::eAstc4x4UnormBlock,
    vk::Format::eAstc4x4SrgbBlock };

/// A family counts when its feature is on and its every format, optimally tiled, samples, filters linearly and takes a
/// copy; `optimalFeatures` maps a format to vkGetPhysicalDeviceFormatProperties' optimalTilingFeatures.
template<typename OptimalFeatures>
auto blockSupportFrom(const vk::PhysicalDeviceFeatures &features, uint32_t maxSide, OptimalFeatures &&optimalFeatures)
  -> BlockSupport
{
    const vk::FormatFeatureFlags needed = vk::FormatFeatureFlagBits::eSampledImage
                                          | vk::FormatFeatureFlagBits::eSampledImageFilterLinear
                                          | vk::FormatFeatureFlagBits::eTransferDst;
    const auto all = [&](const auto &formats) {
        return std::ranges::all_of(
          formats, [&](vk::Format format) { return (optimalFeatures(format) & needed) == needed; });
    };
    return BlockSupport{ .bc = features.textureCompressionBC == VK_TRUE && all(kBcFormats),
        .etc2 = features.textureCompressionETC2 == VK_TRUE && all(kEtc2Formats),
        .astc = features.textureCompressionASTC_LDR == VK_TRUE && all(kAstcFormats),
        .maxSide = maxSide };
}

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

/// One level of a packed chain to upload: its size and where its bytes start in the staged ones.
struct ChainLevel
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t offset = 0;
};

/// The levels of a `levelCount`-level chain over `width` x `height` that fit `limit`, offsets from the first of them,
/// the bytes the whole chain holds and where the kept levels start; no level is kept when none fits. A block chain
/// starts at whole blocks, as OxidANT's `first_block_base` picks, and keeps the partial ones below.
struct ChainPlan
{
    std::vector<ChainLevel> levels;
    uint64_t firstOffset = 0;
    uint64_t totalBytes = 0;
};

inline auto planChainUpload(ChainFormat format, uint32_t width, uint32_t height, uint32_t levelCount, uint32_t limit)
  -> ChainPlan
{
    ChainPlan plan;
    const bool blocks = format != ChainFormat::Rgba8;
    for (uint32_t level = 0; level < levelCount && level < 32U; ++level) {
        const uint32_t level_width = std::max(width >> level, 1U);
        const uint32_t level_height = std::max(height >> level, 1U);
        const bool fits = std::max(level_width, level_height) <= limit;
        const bool whole = !blocks || (level_width % 4U == 0U && level_height % 4U == 0U);
        if (fits && (whole || !plan.levels.empty())) {
            if (plan.levels.empty()) { plan.firstOffset = plan.totalBytes; }
            plan.levels.push_back({ level_width, level_height, plan.totalBytes - plan.firstOffset });
        }
        plan.totalBytes += chainLevelBytes(format, level_width, level_height);
    }
    return plan;
}

}// namespace Kataglyphis
