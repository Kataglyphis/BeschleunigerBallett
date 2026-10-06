module;
#include <memory>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.image_view;

import kataglyphis.vulkan.device;

export namespace Kataglyphis {
class VulkanImageView
{
  public:
    VulkanImageView();
    VulkanImageView(const VulkanImageView &) = delete;
    VulkanImageView &operator=(const VulkanImageView &) = delete;
    VulkanImageView(VulkanImageView &&other) noexcept;
    VulkanImageView &operator=(VulkanImageView &&other) noexcept;

    void setImageView(vk::ImageView imageView);

    vk::ImageView &getImageView() { return imageView; };

    void create(const std::shared_ptr<VulkanDevice> &in_device,
      vk::Image image,
      vk::Format format,
      vk::ImageAspectFlags aspect_flags,
      uint32_t mip_levels,
      vk::ImageViewType view_type = vk::ImageViewType::e2D,
      uint32_t array_layers = 1);

    void cleanUp();

    ~VulkanImageView();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };

    vk::ImageView imageView{};
};
}// namespace Kataglyphis
