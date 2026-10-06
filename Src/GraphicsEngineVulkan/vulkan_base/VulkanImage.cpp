module;
#include <memory>

#include <cstdint>
#include <spdlog/spdlog.h>
#include <tuple>
#include <utility>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.hpp>

#include "common/ImageLayoutHelper.hpp"
#include "common/Utilities.hpp"

module kataglyphis.vulkan.image;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.command_buffer_manager;

Kataglyphis::VulkanImage::VulkanImage() = default;

Kataglyphis::VulkanImage::VulkanImage(VulkanImage &&other) noexcept
  : device(other.device), image(other.image), allocation(other.allocation), owns_image(other.owns_image)
{
    other.device = nullptr;
    other.image = nullptr;
    other.allocation = VK_NULL_HANDLE;
    other.owns_image = false;
}

auto Kataglyphis::VulkanImage::operator=(VulkanImage &&other) noexcept -> VulkanImage &
{
    if (this != &other) {
        cleanUp();

        device = other.device;
        image = other.image;
        allocation = other.allocation;
        owns_image = other.owns_image;

        other.device = nullptr;
        other.image = nullptr;
        other.allocation = VK_NULL_HANDLE;
        other.owns_image = false;
    }

    return *this;
}

void Kataglyphis::VulkanImage::create(const std::shared_ptr<VulkanDevice> &in_device,
  uint32_t width,
  uint32_t height,
  uint32_t mip_levels,
  vk::Format format,
  vk::ImageTiling tiling,
  vk::ImageUsageFlags use_flags,
  vk::MemoryPropertyFlags prop_flags,
  uint32_t array_layers,
  vk::ImageCreateFlags create_flags,
  vk::ImageType image_type,
  uint32_t depth)
{
    cleanUp();

    this->device = in_device;
    this->owns_image = true;
    vk::ImageCreateInfo image_create_info{};
    image_create_info.imageType = image_type;// type of image (1D, 2D, 3D)
    image_create_info.extent.width = width;// width if image extent
    image_create_info.extent.height = height;// height if image extent
    image_create_info.extent.depth = depth;// height if image extent
    image_create_info.mipLevels = mip_levels;// number of mipmap levels
    image_create_info.arrayLayers = array_layers;// number of levels in image array
    image_create_info.format = format;// format type of image
    image_create_info.tiling = tiling;// tiling of image ("arranged" for optimal reading)
    image_create_info.initialLayout = vk::ImageLayout::eUndefined;// layout of image data on creation
    image_create_info.usage = use_flags;// bit flags defining what image will be used for
    image_create_info.samples = vk::SampleCountFlagBits::e1;// number of samples for multisampling
    image_create_info.sharingMode = vk::SharingMode::eExclusive;// whether image can be shared between queues
    image_create_info.flags = create_flags;

    // requiredFlags enforces the requested memory properties exactly.
    VmaAllocationCreateInfo allocation_create_info{};
    allocation_create_info.usage = VMA_MEMORY_USAGE_AUTO;
    allocation_create_info.requiredFlags = static_cast<VkMemoryPropertyFlags>(prop_flags);

    const VkImageCreateInfo &c_image_create_info = static_cast<const VkImageCreateInfo &>(image_create_info);
    VkImage c_image = VK_NULL_HANDLE;
    ASSERT_VULKAN(
      vmaCreateImage(
        device->getVmaAllocator(), &c_image_create_info, &allocation_create_info, &c_image, &allocation, nullptr),
      "Failed to create image via VMA!");

    image = c_image;
}

void Kataglyphis::VulkanImage::transitionImageLayout(vk::Device in_logical_device,
  vk::Queue queue,
  vk::CommandPool command_pool,
  vk::ImageLayout old_layout,
  vk::ImageLayout new_layout,
  vk::ImageAspectFlags aspectMask,
  uint32_t mip_levels,
  uint32_t array_layers)
{
    vk::CommandBuffer command_buffer =
      Kataglyphis::VulkanRendererInternals::CommandBufferManager::beginCommandBuffer(in_logical_device, command_pool);
    if (!command_buffer) {
        spdlog::error("Skipping image layout transition due to invalid command buffer.");
        return;
    }

    // Through the command-buffer overload, so the barrier logic lives in one place.
    transitionImageLayout(command_buffer, old_layout, new_layout, mip_levels, aspectMask, array_layers);

    // clang-format off: BuildIntegrity reads a submit's result off the call's own line.
    bool const transition_submitted = Kataglyphis::VulkanRendererInternals::CommandBufferManager::endAndSubmitCommandBuffer(
      in_logical_device, command_pool, queue, command_buffer);
    // clang-format on
    if (!transition_submitted) { spdlog::error("Failed to submit image layout transition commands."); }
}

void Kataglyphis::VulkanImage::transitionImageLayout(vk::CommandBuffer command_buffer,
  vk::ImageLayout old_layout,
  vk::ImageLayout new_layout,
  uint32_t mip_levels,
  vk::ImageAspectFlags aspectMask,
  uint32_t array_layers)
{
    vk::ImageMemoryBarrier memory_barrier{};
    memory_barrier.oldLayout = old_layout;
    memory_barrier.newLayout = new_layout;
    memory_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;// Queue family to transition from
    memory_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;// Queue family to transition to
    memory_barrier.image = image;// image being accessed and modified as part of barrier
    memory_barrier.subresourceRange.aspectMask = aspectMask;// aspect of image being altered
    memory_barrier.subresourceRange.baseMipLevel = 0;// first mip level to start alterations on
    memory_barrier.subresourceRange.levelCount = mip_levels;// number of mip levels to alter starting from baseMipLevel
    memory_barrier.subresourceRange.baseArrayLayer = 0;// first layer to start alterations on
    memory_barrier.subresourceRange.layerCount = array_layers;// number of layers to alter starting from baseArrayLayer

    memory_barrier.srcAccessMask = Kataglyphis::accessFlagsForImageLayout(old_layout);
    memory_barrier.dstAccessMask = Kataglyphis::accessFlagsForImageLayout(new_layout);

    vk::PipelineStageFlags const src_stage = Kataglyphis::pipelineStageForLayout(old_layout);
    vk::PipelineStageFlags const dst_stage = Kataglyphis::pipelineStageForLayout(new_layout);

    // if transitioning from new image to image ready to receive data

    command_buffer.pipelineBarrier(src_stage,
      dst_stage,// pipeline stages (match to src and dst accessmask)
      {},// no dependency flags
      nullptr,// memory barriers
      nullptr,// buffer memory barriers
      memory_barrier// image memory barriers
    );
}

void Kataglyphis::VulkanImage::setImage(vk::Image in_image)
{
    this->image = in_image;
    // Wrapped images (swapchain images, say) belong to their creator; destroying them here would double free.
    this->owns_image = false;
}

void Kataglyphis::VulkanImage::cleanUp()
{
    if (owns_image && device != nullptr && image) {
        vmaDestroyImage(device->getVmaAllocator(), static_cast<VkImage>(image), allocation);
    }

    image = nullptr;
    allocation = VK_NULL_HANDLE;
    owns_image = false;
}

Kataglyphis::VulkanImage::~VulkanImage() { cleanUp(); }
