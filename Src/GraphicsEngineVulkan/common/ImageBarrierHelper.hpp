#pragma once

#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

// Defaults to one colour mip and layer; a barrier needing a real subrange builds its own and says why.
constexpr vk::ImageMemoryBarrier buildImageMemoryBarrier(vk::Image image,
  vk::ImageLayout oldLayout,
  vk::ImageLayout newLayout,
  vk::AccessFlags srcAccess,
  vk::AccessFlags dstAccess,
  vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor,
  uint32_t baseMipLevel = 0,
  uint32_t levelCount = 1,
  uint32_t baseArrayLayer = 0,
  uint32_t layerCount = 1)
{
    return vk::ImageMemoryBarrier{ srcAccess,
        dstAccess,
        oldLayout,
        newLayout,
        vk::QueueFamilyIgnored,
        vk::QueueFamilyIgnored,
        image,
        vk::ImageSubresourceRange{ aspect, baseMipLevel, levelCount, baseArrayLayer, layerCount } };
}

}// namespace Kataglyphis
