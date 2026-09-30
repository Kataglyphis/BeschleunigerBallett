#pragma once

#include <vulkan/vulkan.hpp>

#include <stdexcept>
#include <vector>

#include "spdlog/spdlog.h"

namespace Kataglyphis {
inline vk::Format choose_supported_format(vk::PhysicalDevice physical_device,
  const std::vector<vk::Format> &formats,
  vk::ImageTiling tiling,
  vk::FormatFeatureFlags feature_flags)
{
    // loop through options and find compatible one
    for (vk::Format format : formats) {
        // get properties for give format on this device
        vk::FormatProperties properties = physical_device.getFormatProperties(format);

        // depending on tiling choice, need to check for different bit flag
        if (tiling == vk::ImageTiling::eLinear && (properties.linearTilingFeatures & feature_flags) == feature_flags) {
            return format;

        } else if (tiling == vk::ImageTiling::eOptimal
                   && (properties.optimalTilingFeatures & feature_flags) == feature_flags) {
            return format;
        }
    }

    spdlog::error("Failed to find supported format!");
    return vk::Format::eUndefined;
}

// The one depth format for both attachment and image; stencil-free first since nothing uses stencil.
inline vk::Format chooseDepthFormat(vk::PhysicalDevice physical_device)
{
    return choose_supported_format(physical_device,
      { vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint },
      vk::ImageTiling::eOptimal,
      vk::FormatFeatureFlagBits::eDepthStencilAttachment);
}

// A linear vkCmdBlitImage needs all three bits; the filter bit alone lets an invalid blit through.
constexpr bool supportsMipmapGeneration(vk::FormatFeatureFlags optimalTilingFeatures)
{
    constexpr vk::FormatFeatureFlags required = vk::FormatFeatureFlagBits::eSampledImageFilterLinear
                                                 | vk::FormatFeatureFlagBits::eBlitSrc
                                                 | vk::FormatFeatureFlagBits::eBlitDst;
    return (optimalTilingFeatures & required) == required;
}

// True for depth formats that also carry a stencil aspect.
constexpr bool formatHasStencil(vk::Format format)
{
    return format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint
           || format == vk::Format::eD16UnormS8Uint || format == vk::Format::eS8Uint;
}

// For transitions and attachment views only; sampled and input-attachment views must name eDepth alone.
constexpr vk::ImageAspectFlags depthStencilTransitionAspect(vk::Format format)
{
    vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eDepth;
    if (formatHasStencil(format)) { aspect |= vk::ImageAspectFlagBits::eStencil; }
    return aspect;
}

// The 8-bit RGBA/BGRA formats FrameCapture::take() can copy at 4 bytes per texel.
constexpr bool isCapturableSwapchainFormat(vk::Format format)
{
    return format == vk::Format::eR8G8B8A8Unorm || format == vk::Format::eR8G8B8A8Srgb
           || format == vk::Format::eR8G8B8A8Snorm || format == vk::Format::eR8G8B8A8Uint
           || format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb
           || format == vk::Format::eB8G8R8A8Snorm || format == vk::Format::eB8G8R8A8Uint;
}

// The BGRA formats FrameCapture::take() must swizzle back to RGBA.
constexpr bool capturedFormatIsBgra(vk::Format format)
{
    return format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb
           || format == vk::Format::eB8G8R8A8Snorm || format == vk::Format::eB8G8R8A8Uint;
}
}// namespace Kataglyphis