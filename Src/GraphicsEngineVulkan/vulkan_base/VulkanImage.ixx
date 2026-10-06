module;
#include <memory>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.image;

import kataglyphis.vulkan.command_buffer_manager;
import kataglyphis.vulkan.device;

export namespace Kataglyphis {
class VulkanImage
{
  public:
    VulkanImage();
    VulkanImage(const VulkanImage &) = delete;
    VulkanImage &operator=(const VulkanImage &) = delete;
    VulkanImage(VulkanImage &&other) noexcept;
    VulkanImage &operator=(VulkanImage &&other) noexcept;

    void create(const std::shared_ptr<VulkanDevice> &in_device,
      uint32_t width,
      uint32_t height,
      uint32_t mip_levels,
      vk::Format format,
      vk::ImageTiling tiling,
      vk::ImageUsageFlags use_flags,
      vk::MemoryPropertyFlags prop_flags,
      uint32_t array_layers = 1,
      vk::ImageCreateFlags create_flags = {},
      vk::ImageType image_type = vk::ImageType::e2D,
      uint32_t depth = 1);

    void transitionImageLayout(vk::Device in_logical_device,
      vk::Queue queue,
      vk::CommandPool command_pool,
      vk::ImageLayout old_layout,
      vk::ImageLayout new_layout,
      vk::ImageAspectFlags aspectMask,
      uint32_t mip_levels,
      uint32_t array_layers = 1);

    void transitionImageLayout(vk::CommandBuffer command_buffer,
      vk::ImageLayout old_layout,
      vk::ImageLayout new_layout,
      uint32_t mip_levels,
      vk::ImageAspectFlags aspectMask,
      uint32_t array_layers = 1);

    void setImage(vk::Image image);
    vk::Image &getImage() { return image; };

    void cleanUp();

    ~VulkanImage();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };

    vk::Image image{};
    // Backing memory owned by the device's VMA allocator; VK_NULL_HANDLE for
    // wrapped external images (setImage).
    VmaAllocation allocation{ VK_NULL_HANDLE };
    // False for wrapped external images (setImage, e.g. swapchain images),
    // whose lifetime belongs to their creator. Mirrors VulkanBuffer's flag.
    bool owns_image{ false };
};
}// namespace Kataglyphis
