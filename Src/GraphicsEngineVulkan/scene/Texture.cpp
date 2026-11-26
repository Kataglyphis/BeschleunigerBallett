module;
#include <memory>

#include "common/FormatHelper.hpp"
#include "common/ImageBarrierHelper.hpp"
#include "common/ImageLayoutHelper.hpp"
#include "common/Utilities.hpp"
#include "scene/TextureFit.hpp"
#include "spdlog/spdlog.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stb_image.h>
#include <string>
#include <type_traits>
#include <vector>
#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.texture;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.buffer_manager;
import kataglyphis.vulkan.image;
import kataglyphis.vulkan.image_view;
import kataglyphis.vulkan.command_buffer_manager;
import kataglyphis.vulkan.sampler_builder;
import kataglyphis.vulkan.texture_decode;

using namespace Kataglyphis;

Kataglyphis::Texture::Texture() = default;

Kataglyphis::Texture::Texture(Texture &&other) noexcept
  : mip_levels(other.mip_levels), vulkanImage(std::move(other.vulkanImage)),
    vulkanImageView(std::move(other.vulkanImageView)), textureSampler(other.textureSampler), device(other.device)
{
    other.textureSampler = nullptr;
    other.device = nullptr;
    other.mip_levels = 0;
}

Kataglyphis::Texture &Kataglyphis::Texture::operator=(Texture &&other) noexcept
{
    if (this != &other) {
        cleanUp();
        mip_levels = other.mip_levels;
        vulkanImage = std::move(other.vulkanImage);
        vulkanImageView = std::move(other.vulkanImageView);
        textureSampler = other.textureSampler;
        device = other.device;

        other.textureSampler = nullptr;
        other.device = nullptr;
        other.mip_levels = 0;
    }
    return *this;
}

namespace {
auto deviceSupportsMipmapGeneration(vk::PhysicalDevice physical_device, vk::Format image_format) -> bool
{
    vk::FormatProperties format_properties = physical_device.getFormatProperties(image_format);
    return Kataglyphis::supportsMipmapGeneration(format_properties.optimalTilingFeatures);
}
}// namespace

auto Kataglyphis::Texture::createFromFile(const std::shared_ptr<VulkanDevice> &in_device,
  vk::CommandPool commandPool,
  const std::string &fileName,
  bool srgb) -> bool
{
    int width = 0;
    int height = 0;
    vk::DeviceSize size = 0;
    unsigned char *image_data = loadTextureData(fileName, &width, &height, &size);
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> image_data_ptr(image_data, stbi_image_free);

    if (!image_data || width == 0 || height == 0) {
        spdlog::warn("Texture file could not be loaded, skipping creation: {}", fileName);
        return false;
    }

    return uploadRgba(in_device, commandPool, width, height, size, image_data, srgb);
}

auto Kataglyphis::Texture::createFromMemory(const std::shared_ptr<VulkanDevice> &in_device,
  vk::CommandPool commandPool,
  const unsigned char *encodedBytes,
  size_t byteCount,
  bool srgb) -> bool
{
    if (encodedBytes == nullptr || byteCount == 0) { return false; }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char *image_data =
      stbi_load_from_memory(encodedBytes, static_cast<int>(byteCount), &width, &height, &channels, STBI_rgb_alpha);
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> image_data_ptr(image_data, stbi_image_free);

    if (!image_data || width == 0 || height == 0) {
        spdlog::warn("glTF embedded texture could not be decoded ({} bytes); skipping.", byteCount);
        return false;
    }

    // STBI_rgb_alpha forced 4 channels, so the tight size is width*height*4.
    const vk::DeviceSize size = static_cast<vk::DeviceSize>(width) * static_cast<vk::DeviceSize>(height) * 4U;
    return uploadRgba(in_device, commandPool, width, height, size, image_data, srgb);
}

auto Kataglyphis::Texture::uploadRgba(const std::shared_ptr<VulkanDevice> &in_device,
  vk::CommandPool commandPool,
  int width,
  int height,
  vk::DeviceSize size,
  const unsigned char *rgba,
  bool srgb) -> bool
{
    if (width == 0 || height == 0 || rgba == nullptr) { return false; }

    // Past maxImageDimension2D vmaCreateImage fails and vkCheck aborts, so the upload starts at the first level that fits.
    FittedRgba8 fitted;
    const uint32_t side_limit = in_device->getMaxImageDimension2D();
    if (static_cast<uint32_t>(std::max(width, height)) > side_limit) {
        fitted = fitRgba8ToLimit(rgba, static_cast<uint32_t>(width), static_cast<uint32_t>(height), side_limit, srgb);
        spdlog::warn("Texture {}x{} exceeds the device's {}-texel side; uploaded at {}x{}.",
          width,
          height,
          side_limit,
          fitted.width,
          fitted.height);
        width = static_cast<int>(fitted.width);
        height = static_cast<int>(fitted.height);
        size = fitted.texels.size();
        rgba = fitted.texels.data();
    }

    // Color texels are sRGB-encoded and must decode to linear at sample time; normal maps are already linear, so UNORM.
    const vk::Format texture_format = srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
    uint32_t levels = mipLevelCount(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    if (!deviceSupportsMipmapGeneration(in_device->getPhysicalDevice(), texture_format)) {
        vk::FormatFeatureFlags optimal_tiling_features =
          in_device->getPhysicalDevice().getFormatProperties(texture_format).optimalTilingFeatures;
        spdlog::warn(
          "Mipmap generation not supported for texture format (missing: filterLinear={}, blitSrc={}, "
          "blitDst={}); using single mip level.",
          !static_cast<bool>(optimal_tiling_features & vk::FormatFeatureFlagBits::eSampledImageFilterLinear),
          !static_cast<bool>(optimal_tiling_features & vk::FormatFeatureFlagBits::eBlitSrc),
          !static_cast<bool>(optimal_tiling_features & vk::FormatFeatureFlagBits::eBlitDst));
        levels = 1;
    }

    const ChainLevel base{ static_cast<uint32_t>(width), static_cast<uint32_t>(height), 0 };
    return uploadLevels(
      in_device, commandPool, texture_format, levels, { rgba, static_cast<size_t>(size) }, { &base, 1 });
}

auto Kataglyphis::Texture::createFromLevels(const std::shared_ptr<VulkanDevice> &in_device,
  vk::CommandPool commandPool,
  uint32_t width,
  uint32_t height,
  uint32_t levelCount,
  std::span<const unsigned char> rgba8,
  bool srgb) -> bool
{
    if (width == 0 || height == 0 || levelCount == 0 || levelCount > mipLevelCount(width, height)) { return false; }

    // The chain holds the halvings uploadRgba would compute, so a level past maxImageDimension2D is skipped instead.
    const uint32_t side_limit = in_device->getMaxImageDimension2D();
    const ChainPlan plan = planChainUpload(width, height, levelCount, side_limit);
    if (plan.totalBytes > rgba8.size()) {
        spdlog::warn("Texture chain of {} levels over {}x{} needs {} bytes but holds {}; skipping.",
          levelCount,
          width,
          height,
          plan.totalBytes,
          rgba8.size());
        return false;
    }
    if (plan.levels.empty()) {
        spdlog::warn("Texture {}x{}: none of its {} levels fits the device's {}-texel side; skipping.",
          width,
          height,
          levelCount,
          side_limit);
        return false;
    }
    if (plan.levels.size() < levelCount) {
        spdlog::warn("Texture {}x{} exceeds the device's {}-texel side; uploaded from level {} at {}x{}.",
          width,
          height,
          side_limit,
          levelCount - plan.levels.size(),
          plan.levels.front().width,
          plan.levels.front().height);
    }

    const vk::Format texture_format = srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
    const std::span<const unsigned char> uploaded = rgba8.subspan(plan.firstOffset, plan.totalBytes - plan.firstOffset);
    return uploadLevels(
      in_device, commandPool, texture_format, static_cast<uint32_t>(plan.levels.size()), uploaded, plan.levels);
}

auto Kataglyphis::Texture::uploadLevels(const std::shared_ptr<VulkanDevice> &in_device,
  vk::CommandPool commandPool,
  vk::Format texture_format,
  uint32_t level_count,
  std::span<const unsigned char> texels,
  std::span<const ChainLevel> stored) -> bool
{
    if (stored.empty() || texels.empty()) { return false; }
    const ChainLevel &base = stored.front();
    VulkanBuffer stagingBuffer;
    stagingBuffer.create(in_device,
      texels.size(),
      vk::BufferUsageFlagBits::eTransferSrc,
      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

    // Host-visible buffers are persistently mapped by VMA.
    memcpy(stagingBuffer.getMappedData(), texels.data(), texels.size());

    // VUID-vkDestroyImage-image-01000: the old view must go before createImage() replaces its image.
    vulkanImageView.cleanUp();

    createImage(in_device,
      base.width,
      base.height,
      level_count,
      texture_format,
      vk::ImageTiling::eOptimal,
      vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
      vk::MemoryPropertyFlagBits::eDeviceLocal);

    // One submit for transition, copies and mips instead of three fence-waited ones.
    vk::CommandBuffer command_buffer = Kataglyphis::VulkanRendererInternals::CommandBufferManager::beginCommandBuffer(
      in_device->getLogicalDevice(), commandPool);
    if (!command_buffer) {
        spdlog::error("Skipping texture upload due to invalid command buffer.");
        stagingBuffer.cleanUp();
        cleanUp();
        return false;
    }

    vulkanImage.transitionImageLayout(command_buffer,
      vk::ImageLayout::eUndefined,
      vk::ImageLayout::eTransferDstOptimal,
      mip_levels,
      vk::ImageAspectFlagBits::eColor);

    for (uint32_t level = 0; level < stored.size(); ++level) {
        VulkanBufferManager::copyImageBuffer(command_buffer,
          stagingBuffer.getBuffer(),
          vulkanImage.getImage(),
          stored[level].width,
          stored[level].height,
          level,
          stored[level].offset);
    }

    // uploadRgba hands over level 0 alone; the device blits the rest of its chain.
    if (mip_levels > 1 && stored.size() == 1) {
        generateMipMaps(
          command_buffer, vulkanImage.getImage(), static_cast<int32_t>(base.width), static_cast<int32_t>(base.height));
    } else {
        vulkanImage.transitionImageLayout(command_buffer,
          vk::ImageLayout::eTransferDstOptimal,
          vk::ImageLayout::eShaderReadOnlyOptimal,
          mip_levels,
          vk::ImageAspectFlagBits::eColor);
    }

    const bool upload_submitted = Kataglyphis::VulkanRendererInternals::CommandBufferManager::endAndSubmitCommandBuffer(
      in_device->getLogicalDevice(), commandPool, in_device->getGraphicsQueue(), command_buffer);

    stagingBuffer.cleanUp();

    if (!upload_submitted) {
        spdlog::error("Texture upload submit failed ({}x{}); leaving texture unwritten.", base.width, base.height);
        cleanUp();
        return false;
    }

    // clang-format off: BuildIntegrity wants the call, its eColor aspect and the marker on one line.
    createImageView(in_device, texture_format, vk::ImageAspectFlagBits::eColor, mip_levels, vk::ImageViewType::e2D, 1);// COLOR_ATTACHMENT_CHAIN_OK: real-mip-chain
    // clang-format on

    return true;
}

auto Kataglyphis::Texture::createDefaultTexture(const std::shared_ptr<VulkanDevice> &in_device,
  vk::CommandPool commandPool) -> bool
{
    constexpr vk::DeviceSize default_size = 4;
    constexpr unsigned char white_pixel[4] = { 255, 255, 255, 255 };

    return uploadRgba(in_device, commandPool, 1, 1, default_size, white_pixel);
}

void Kataglyphis::Texture::setImage(vk::Image image) { vulkanImage.setImage(image); }

void Kataglyphis::Texture::setImageView(vk::ImageView imageView) { vulkanImageView.setImageView(imageView); }

void Kataglyphis::Texture::createImage(const std::shared_ptr<VulkanDevice> &in_device,
  uint32_t width,
  uint32_t height,
  uint32_t in_mip_levels,
  vk::Format format,
  vk::ImageTiling tiling,
  vk::ImageUsageFlags use_flags,
  vk::MemoryPropertyFlags prop_flags,
  uint32_t array_layers,
  vk::ImageCreateFlags create_flags,
  vk::ImageType image_type,
  uint32_t depth)
{
    this->device = in_device;
    this->mip_levels = in_mip_levels;
    vulkanImage.create(in_device,
      width,
      height,
      in_mip_levels,
      format,
      tiling,
      use_flags,
      prop_flags,
      array_layers,
      create_flags,
      image_type,
      depth);
}

void Kataglyphis::Texture::createImageView(const std::shared_ptr<VulkanDevice> &in_device,
  vk::Format format,
  vk::ImageAspectFlags aspect_flags,
  uint32_t in_mip_levels,
  vk::ImageViewType view_type,
  uint32_t array_layers)
{
    this->device = in_device;
    vulkanImageView.create(
      in_device, vulkanImage.getImage(), format, aspect_flags, in_mip_levels, view_type, array_layers);
}

void Kataglyphis::Texture::createTextureSampler(const std::shared_ptr<VulkanDevice> &in_device,
  vk::Filter filter,
  vk::SamplerAddressMode addressMode,
  vk::Bool32 compareEnable,
  vk::CompareOp compareOp)
{
    // Before the device swap: the old sampler must be destroyed with the device that created it.
    releaseSampler();
    this->device = in_device;
    // maxLod = mip_levels is right for single-mip images too: Vulkan clamps LOD to the view's level count.
    vk::SamplerCreateInfo samplerInfo = buildSamplerCreateInfo(filter,
      addressMode,
      static_cast<float>(mip_levels),
      VK_FALSE,
      1.0f,
      vk::BorderColor::eIntOpaqueBlack,
      compareEnable,
      compareOp);

    textureSampler =
      vkCheck(device->getLogicalDevice().createSampler(samplerInfo), "Failed to create texture sampler!");
}

void Kataglyphis::Texture::releaseImageView() { vulkanImageView.cleanUp(); }

void Kataglyphis::Texture::releaseSampler()
{
    if (textureSampler && device) {
        device->getLogicalDevice().destroySampler(textureSampler);
        textureSampler = nullptr;
    }
}

void Kataglyphis::Texture::cleanUp()
{
    releaseSampler();
    vulkanImageView.cleanUp();
    vulkanImage.cleanUp();
    mip_levels = 0;
}

Kataglyphis::Texture::~Texture() { cleanUp(); }

auto Kataglyphis::Texture::loadTextureData(const std::string &file_name,
  int *width,
  int *height,
  vk::DeviceSize *image_size) -> unsigned char *
{
    // Decoding lives in texture_decode so texture_loading_fuzz_test reaches it without linking the renderer.
    static_assert(std::is_same_v<vk::DeviceSize, std::uint64_t>,
      "loadTextureData forwards image_size straight through to decodeImageRGBA8");

    unsigned char *image = TextureDecode::decodeImageRGBA8(file_name, width, height, image_size);

    if (image == nullptr) { spdlog::error("Failed to load a texture file! (" + file_name + ")"); }

    return image;
}

void Kataglyphis::Texture::generateMipMaps(vk::CommandBuffer command_buffer,
  vk::Image image,
  int32_t width,
  int32_t height)
{
    vk::ImageMemoryBarrier2 barrier{};
    barrier.image = image;
    barrier.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
    barrier.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.levelCount = 1;

    int32_t tmp_width = width;
    int32_t tmp_height = height;

    for (uint32_t i = 1; i < mip_levels; i++) {
        // Hand-written on purpose: the layout helper would widen this transfer-to-transfer edge to eAllCommands.
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
        barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;

        Kataglyphis::recordImageBarrier(command_buffer, barrier);

        vk::ImageBlit blit{};

        blit.srcOffsets[0] = vk::Offset3D{ 0, 0, 0 };
        blit.srcOffsets[1] = vk::Offset3D{ tmp_width, tmp_height, 1 };
        blit.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;

        blit.dstOffsets[0] = vk::Offset3D{ 0, 0, 0 };
        blit.dstOffsets[1] = vk::Offset3D{ tmp_width > 1 ? tmp_width / 2 : 1, tmp_height > 1 ? tmp_height / 2 : 1, 1 };
        blit.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;

        command_buffer.blitImage(image,
          vk::ImageLayout::eTransferSrcOptimal,
          image,
          vk::ImageLayout::eTransferDstOptimal,
          blit,
          vk::Filter::eLinear);

        // Raster, ray-tracing and compute shaders all sample these mips; a raster-only stage leaves two unsynchronized.
        barrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
        barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        barrier.srcAccessMask = Kataglyphis::accessFlagsForImageLayout(barrier.oldLayout);
        barrier.dstStageMask = Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
        barrier.dstAccessMask = Kataglyphis::accessFlagsForImageLayout(barrier.newLayout);

        Kataglyphis::recordImageBarrier(command_buffer, barrier);

        if (tmp_width > 1) { tmp_width /= 2; }
        if (tmp_height > 1) { tmp_height /= 2; }
    }

    barrier.subresourceRange.baseMipLevel = mip_levels - 1;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = Kataglyphis::accessFlagsForImageLayout(barrier.oldLayout);
    barrier.dstStageMask = Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
    barrier.dstAccessMask = Kataglyphis::accessFlagsForImageLayout(barrier.newLayout);

    Kataglyphis::recordImageBarrier(command_buffer, barrier);
}