module;

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.allocator;

export namespace Kataglyphis {

class Allocator
{
  public:
    Allocator();
    Allocator(const vk::Device &device,
      const vk::PhysicalDevice &physicalDevice,
      const vk::Instance &instance,
      bool enableBufferDeviceAddress = true,
      bool enableDeviceCoherentMemory = false);

    // Owns a single VmaAllocator handle, so it is move-only with RAII cleanup -
    // same ownership model as VulkanBuffer / VulkanImage. Copying would let two
    // instances both vmaDestroyAllocator the same handle.
    Allocator(const Allocator &) = delete;
    Allocator &operator=(const Allocator &) = delete;
    Allocator(Allocator &&other) noexcept;
    Allocator &operator=(Allocator &&other) noexcept;

    VmaAllocator getVmaAllocator() const { return vmaAllocator; }

    void cleanUp();

    ~Allocator();

  private:
    VmaAllocator vmaAllocator{ VK_NULL_HANDLE };
};
}// namespace Kataglyphis
