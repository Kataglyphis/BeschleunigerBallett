#pragma once

#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

// Identity swizzle only; the explicit constructor keeps it constexpr (value-init is not).
constexpr vk::ImageViewCreateInfo buildImageViewCreateInfo(vk::Image image,
  vk::Format format,
  vk::ImageAspectFlags aspect_flags,
  uint32_t mip_levels,
  vk::ImageViewType view_type = vk::ImageViewType::e2D,
  uint32_t array_layers = 1)
{
    return vk::ImageViewCreateInfo{ vk::ImageViewCreateFlags{}, image, view_type, format, vk::ComponentMapping{},
        vk::ImageSubresourceRange{ aspect_flags, 0, mip_levels, 0, array_layers } };
}

}// namespace Kataglyphis
