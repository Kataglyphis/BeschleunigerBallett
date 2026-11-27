#pragma once

#include <span>

#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

// Defaults to one colour mip and layer; a barrier needing a real subrange builds its own and says why.
constexpr vk::ImageMemoryBarrier2 buildImageMemoryBarrier(vk::Image image,
  vk::ImageLayout oldLayout,
  vk::ImageLayout newLayout,
  vk::PipelineStageFlags2 srcStage,
  vk::AccessFlags2 srcAccess,
  vk::PipelineStageFlags2 dstStage,
  vk::AccessFlags2 dstAccess,
  vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor,
  uint32_t baseMipLevel = 0,
  uint32_t levelCount = 1,
  uint32_t baseArrayLayer = 0,
  uint32_t layerCount = 1)
{
    return vk::ImageMemoryBarrier2{ srcStage,
        srcAccess,
        dstStage,
        dstAccess,
        oldLayout,
        newLayout,
        vk::QueueFamilyIgnored,
        vk::QueueFamilyIgnored,
        image,
        vk::ImageSubresourceRange{ aspect, baseMipLevel, levelCount, baseArrayLayer, layerCount } };
}

// One vkCmdPipelineBarrier2 per batch: the barriers of a pass boundary share it instead of one call each.
inline void recordImageBarriers(vk::CommandBuffer commandBuffer, std::span<const vk::ImageMemoryBarrier2> barriers)
{
    vk::DependencyInfo dependency_info{};
    dependency_info.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dependency_info.pImageMemoryBarriers = barriers.data();
    commandBuffer.pipelineBarrier2(dependency_info);
}

inline void recordImageBarrier(vk::CommandBuffer commandBuffer, const vk::ImageMemoryBarrier2 &barrier)
{ recordImageBarriers(commandBuffer, std::span<const vk::ImageMemoryBarrier2>(&barrier, 1)); }

// The whole buffer: the post chain's storage buffers hand all of their few words from one pass to the next.
constexpr vk::BufferMemoryBarrier2 buildBufferMemoryBarrier(vk::Buffer buffer,
  vk::PipelineStageFlags2 srcStage,
  vk::AccessFlags2 srcAccess,
  vk::PipelineStageFlags2 dstStage,
  vk::AccessFlags2 dstAccess)
{
    return vk::BufferMemoryBarrier2{ srcStage,
        srcAccess,
        dstStage,
        dstAccess,
        vk::QueueFamilyIgnored,
        vk::QueueFamilyIgnored,
        buffer,
        0,
        vk::WholeSize };
}

inline void recordBufferBarriers(vk::CommandBuffer commandBuffer, std::span<const vk::BufferMemoryBarrier2> barriers)
{
    vk::DependencyInfo dependency_info{};
    dependency_info.bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dependency_info.pBufferMemoryBarriers = barriers.data();
    commandBuffer.pipelineBarrier2(dependency_info);
}

inline void recordBufferBarrier(vk::CommandBuffer commandBuffer, const vk::BufferMemoryBarrier2 &barrier)
{ recordBufferBarriers(commandBuffer, std::span<const vk::BufferMemoryBarrier2>(&barrier, 1)); }

}// namespace Kataglyphis
