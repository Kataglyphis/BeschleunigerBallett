module;
#include <memory>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.buffer;

import kataglyphis.vulkan.device;

export namespace Kataglyphis {
class VulkanBuffer
{
  public:
    VulkanBuffer();
    VulkanBuffer(const VulkanBuffer &) = delete;
    VulkanBuffer &operator=(const VulkanBuffer &) = delete;
    VulkanBuffer(VulkanBuffer &&other) noexcept;
    VulkanBuffer &operator=(VulkanBuffer &&other) noexcept;

    void create(const std::shared_ptr<VulkanDevice> &vulkanDevice,
      vk::DeviceSize buffer_size,
      vk::BufferUsageFlags buffer_usage_flags,
      vk::MemoryPropertyFlags buffer_propertiy_flags,
      vk::MemoryAllocateFlags buffer_allocate_flags = {});

    void cleanUp();

    vk::Buffer &getBuffer() { return buffer; };
    // Host-visible buffers are created persistently mapped
    // (VMA_ALLOCATION_CREATE_MAPPED_BIT); returns nullptr for device-local
    // buffers. Valid until cleanUp()/destruction; no unmap necessary.
    void *getMappedData() const { return mappedData; };

    ~VulkanBuffer();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };

    vk::Buffer buffer{};
    VmaAllocation allocation{ VK_NULL_HANDLE };
    void *mappedData{ nullptr };

    bool created{ false };
};
}// namespace Kataglyphis
