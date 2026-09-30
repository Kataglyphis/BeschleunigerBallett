module;
#include <memory>

#include "common/ImageViewHelper.hpp"
#include "common/Utilities.hpp"
#include <cstdint>
#include <utility>
#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.image_view;

import kataglyphis.vulkan.device;

Kataglyphis::VulkanImageView::VulkanImageView() = default;

Kataglyphis::VulkanImageView::VulkanImageView(VulkanImageView &&other) noexcept
  : device(other.device), imageView(other.imageView)
{
    other.device = nullptr;
    other.imageView = nullptr;
}

auto Kataglyphis::VulkanImageView::operator=(VulkanImageView &&other) noexcept -> VulkanImageView &
{
    if (this != &other) {
        cleanUp();

        device = other.device;
        imageView = other.imageView;

        other.device = nullptr;
        other.imageView = nullptr;
    }

    return *this;
}

void Kataglyphis::VulkanImageView::setImageView(vk::ImageView in_imageView) { this->imageView = in_imageView; }

void Kataglyphis::VulkanImageView::create(const std::shared_ptr<VulkanDevice> &in_device,
  vk::Image image,
  vk::Format format,
  vk::ImageAspectFlags aspect_flags,
  uint32_t mip_levels,
  vk::ImageViewType view_type,
  uint32_t array_layers)
{
    cleanUp();

    this->device = in_device;

    const vk::ImageViewCreateInfo view_create_info =
      Kataglyphis::buildImageViewCreateInfo(image, format, aspect_flags, mip_levels, view_type, array_layers);

    // VULKAN_HPP_NO_EXCEPTIONS: an unchecked failure stores a null view and surfaces as UB later, so fail fast.
    auto imageViewResult = device->getLogicalDevice().createImageView(view_create_info);
    ASSERT_VULKAN(imageViewResult.result, "Failed to create image view!");
    imageView = imageViewResult.value;
}

void Kataglyphis::VulkanImageView::cleanUp()
{
    if (device != nullptr && imageView) { device->getLogicalDevice().destroyImageView(imageView); }

    imageView = nullptr;
}

Kataglyphis::VulkanImageView::~VulkanImageView() { cleanUp(); }
