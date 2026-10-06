module;
#include <cstring>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.buffer_manager;

import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.device;

export namespace Kataglyphis {
class VulkanBufferManager
{
  public:
    VulkanBufferManager();
    // The reusable staging buffer makes the manager move-only.
    VulkanBufferManager(const VulkanBufferManager &) = delete;
    VulkanBufferManager &operator=(const VulkanBufferManager &) = delete;
    VulkanBufferManager(VulkanBufferManager &&) noexcept = default;
    VulkanBufferManager &operator=(VulkanBufferManager &&) noexcept = default;

    bool copyBuffer(vk::Device device,
      vk::Queue transfer_queue,
      vk::CommandPool transfer_command_pool,
      VulkanBuffer &src_buffer,
      VulkanBuffer &dst_buffer,
      vk::DeviceSize buffer_size);

    static void copyImageBuffer(vk::CommandBuffer command_buffer,
      vk::Buffer src_buffer,
      vk::Image image,
      uint32_t width,
      uint32_t height);

    template<typename T>
    bool createBufferAndUploadVectorOnDevice(const std::shared_ptr<VulkanDevice> &device,
      vk::CommandPool commandPool,
      VulkanBuffer &vulkanBuffer,
      vk::BufferUsageFlags dstBufferUsageFlags,
      vk::MemoryPropertyFlags dstBufferMemoryPropertyFlags,
      const std::vector<T> &data,
      vk::MemoryAllocateFlags dstBufferMemoryAllocateFlags = {},
      vk::Queue transfer_queue = {});

    // Releases the reusable staging buffer. Must run (or the manager be
    // destroyed) while the device's VMA allocator is still alive, i.e.
    // before VulkanDevice::cleanUp(). Safe to call multiple times; the
    // staging buffer is lazily recreated on the next upload.
    void cleanUp();

    ~VulkanBufferManager();

  private:
    // Persistently mapped (VMA_ALLOCATION_CREATE_MAPPED_BIT) staging buffer
    // reused across uploads instead of being created/destroyed per upload.
    // Grown geometrically whenever an upload exceeds its capacity. Reuse is
    // safe because every upload submit is fence-synchronized before return.
    VulkanBuffer stagingBuffer;
    vk::DeviceSize stagingBufferCapacity{ 0 };

    void ensureStagingBufferCapacity(const std::shared_ptr<VulkanDevice> &device, vk::DeviceSize size);
};

template<typename T>
inline bool VulkanBufferManager::createBufferAndUploadVectorOnDevice(const std::shared_ptr<VulkanDevice> &device,
  vk::CommandPool commandPool,
  VulkanBuffer &vulkanBuffer,
  vk::BufferUsageFlags dstBufferUsageFlags,
  vk::MemoryPropertyFlags dstBufferMemoryPropertyFlags,
  const std::vector<T> &data,
  vk::MemoryAllocateFlags dstBufferMemoryAllocateFlags,
  vk::Queue transfer_queue)
{
    vk::DeviceSize bufferSize = sizeof(T) * data.size();
    if (bufferSize == 0) {
        bufferSize = sizeof(uint32_t);
        vulkanBuffer.create(
          device, bufferSize, dstBufferUsageFlags, dstBufferMemoryPropertyFlags, dstBufferMemoryAllocateFlags);
        return true;
    }

    ensureStagingBufferCapacity(device, bufferSize);

    // The staging buffer is host-visible and therefore persistently mapped by VMA.
    std::memcpy(stagingBuffer.getMappedData(), data.data(), static_cast<size_t>(bufferSize));

    vulkanBuffer.create(
      device, bufferSize, dstBufferUsageFlags, dstBufferMemoryPropertyFlags, dstBufferMemoryAllocateFlags);

    vk::Queue const queue = transfer_queue ? transfer_queue : device->getGraphicsQueue();

    // stagingBuffer is intentionally kept alive for reuse by the next upload;
    // it is released in cleanUp().
    return copyBuffer(device->getLogicalDevice(), queue, commandPool, stagingBuffer, vulkanBuffer, bufferSize);
}
}// namespace Kataglyphis
