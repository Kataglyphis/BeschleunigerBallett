module;

#include "spdlog/spdlog.h"
#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.buffer_manager;

import kataglyphis.vulkan.command_buffer_manager;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.device;

namespace {
bool copy_buffer_impl(vk::Device device,
  vk::Queue transfer_queue,
  vk::CommandPool transfer_command_pool,
  Kataglyphis::VulkanBuffer &src_buffer,
  Kataglyphis::VulkanBuffer &dst_buffer,
  vk::DeviceSize buffer_size)
{
    vk::CommandBuffer command_buffer =
      Kataglyphis::VulkanRendererInternals::CommandBufferManager::beginCommandBuffer(device, transfer_command_pool);
    if (!command_buffer) {
        spdlog::error("Skipping buffer copy due to invalid command buffer.");
        return false;
    }

    vk::BufferCopy buffer_copy_region{};
    buffer_copy_region.srcOffset = 0;
    buffer_copy_region.dstOffset = 0;
    buffer_copy_region.size = buffer_size;

    command_buffer.copyBuffer(src_buffer.getBuffer(), dst_buffer.getBuffer(), buffer_copy_region);

    const bool submitted = Kataglyphis::VulkanRendererInternals::CommandBufferManager::endAndSubmitCommandBuffer(
      device, transfer_command_pool, transfer_queue, command_buffer);
    return submitted;
}
}// namespace

Kataglyphis::VulkanBufferManager::VulkanBufferManager() = default;

bool Kataglyphis::VulkanBufferManager::copyBuffer(vk::Device device,
  vk::Queue transfer_queue,
  vk::CommandPool transfer_command_pool,
  VulkanBuffer &src_buffer,
  VulkanBuffer &dst_buffer,
  vk::DeviceSize buffer_size)
{
    return copy_buffer_impl(device, transfer_queue, transfer_command_pool, src_buffer, dst_buffer, buffer_size);
}

void Kataglyphis::VulkanBufferManager::copyImageBuffer(vk::CommandBuffer command_buffer,
  vk::Buffer src_buffer,
  vk::Image image,
  uint32_t width,
  uint32_t height)
{
    vk::BufferImageCopy image_region{};
    image_region.bufferOffset = 0;
    image_region.bufferRowLength = 0;
    image_region.bufferImageHeight = 0;
    image_region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    image_region.imageSubresource.mipLevel = 0;
    image_region.imageSubresource.baseArrayLayer = 0;
    image_region.imageSubresource.layerCount = 1;
    image_region.imageOffset = vk::Offset3D{ 0, 0, 0 };
    image_region.imageExtent = vk::Extent3D{ width, height, 1 };

    command_buffer.copyBufferToImage(src_buffer, image, vk::ImageLayout::eTransferDstOptimal, image_region);
}

void Kataglyphis::VulkanBufferManager::ensureStagingBufferCapacity(const std::shared_ptr<VulkanDevice> &device,
  vk::DeviceSize size)
{
    if (stagingBuffer.getMappedData() != nullptr && stagingBufferCapacity >= size) { return; }

    // Grow geometrically so repeated uploads converge to zero reallocations.
    constexpr vk::DeviceSize initial_capacity = 64ULL * 1024ULL;
    vk::DeviceSize new_capacity = stagingBufferCapacity > initial_capacity ? stagingBufferCapacity : initial_capacity;
    while (new_capacity < size) { new_capacity *= 2; }

    // Safe: every upload submit is fence-waited before returning, so no recorded copy still reads it.
    stagingBuffer.cleanUp();
    stagingBuffer.create(device,
      new_capacity,
      vk::BufferUsageFlagBits::eTransferSrc,
      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    stagingBufferCapacity = new_capacity;
}

void Kataglyphis::VulkanBufferManager::cleanUp()
{
    stagingBuffer.cleanUp();
    stagingBufferCapacity = 0;
}

Kataglyphis::VulkanBufferManager::~VulkanBufferManager() = default;
