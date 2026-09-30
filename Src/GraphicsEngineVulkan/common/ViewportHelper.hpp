#pragma once

#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

// Full extent, unflipped y, [0, 1] depth; a pass needing anything else builds its own and says why.
constexpr vk::Viewport fullExtentViewport(vk::Extent2D extent)
{
    return vk::Viewport{ 0.0F,
        0.0F,
        static_cast<float>(extent.width),
        static_cast<float>(extent.height),
        0.0F,
        1.0F };
}

constexpr vk::Rect2D fullExtentScissor(vk::Extent2D extent) { return vk::Rect2D{ vk::Offset2D{ 0, 0 }, extent }; }

inline void setFullExtentViewportAndScissor(vk::CommandBuffer commandBuffer, vk::Extent2D extent)
{
    const vk::Viewport viewport = fullExtentViewport(extent);
    commandBuffer.setViewport(0, viewport);
    const vk::Rect2D scissor = fullExtentScissor(extent);
    commandBuffer.setScissor(0, scissor);
}

}// namespace Kataglyphis
