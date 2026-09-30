#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

// UNORM on purpose: the shaders that write the swapchain apply linear_to_srgb themselves.
inline vk::SurfaceFormatKHR chooseBestSurfaceFormat(const std::vector<vk::SurfaceFormatKHR> &formats)
{
    // An empty list has no formats[0] to fall back on, so use the unrestricted default.
    if (formats.empty()) { return { vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear }; }

    // the condition below means all formats are available (no restrictions)
    if (formats.size() == 1 && formats[0].format == vk::Format::eUndefined) {
        return { vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear };
    }

    // if restricted, search for optimal format
    for (const auto &format : formats) {
        if ((format.format == vk::Format::eR8G8B8A8Unorm || format.format == vk::Format::eB8G8R8A8Unorm)
            && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
            return format;
        }
    }

    // Last resort, and how a non-capturable format reaches runtime (see FormatHelper.hpp's isCapturableSwapchainFormat).
    return formats[0];
}

inline vk::PresentModeKHR chooseBestPresentationMode(const std::vector<vk::PresentModeKHR> &presentation_modes)
{
    // look for mailbox presentation mode
    for (const auto &presentation_mode : presentation_modes) {
        if (presentation_mode == vk::PresentModeKHR::eMailbox) { return presentation_mode; }
    }

    // if can't find, use FIFO as Vulkan spec says it must be present
    return vk::PresentModeKHR::eFifo;
}

// The currentExtent short-circuit stays with the caller, which has the live window.
inline vk::Extent2D clampSwapExtent(const vk::SurfaceCapabilitiesKHR &surface_capabilities,
  uint32_t width,
  uint32_t height)
{
    vk::Extent2D new_extent{};
    new_extent.width = width;
    new_extent.height = height;

    uint32_t const min_width = surface_capabilities.minImageExtent.width;
    uint32_t const max_width = surface_capabilities.maxImageExtent.width;
    uint32_t const min_height = surface_capabilities.minImageExtent.height;
    uint32_t const max_height = surface_capabilities.maxImageExtent.height;

    new_extent.width = std::max(min_width, std::min(max_width, new_extent.width));
    new_extent.height = std::max(min_height, std::min(max_height, new_extent.height));

    return new_extent;
}

}// namespace Kataglyphis
