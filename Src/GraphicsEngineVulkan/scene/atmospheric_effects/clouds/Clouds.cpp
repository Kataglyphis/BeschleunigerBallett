module;
#include "common/FormatHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/Utilities.hpp"
#include "scene/atmospheric_effects/clouds/CloudDispatch.hpp"
#include <array>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.clouds;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.shader_helper;
import kataglyphis.vulkan.command_buffer_manager;

namespace Kataglyphis {

void Clouds::init(const std::shared_ptr<VulkanDevice> &device,
  vk::CommandPool commandPool,
  vk::DescriptorSetLayout sharedLayout,
  uint32_t width,
  uint32_t height)
{
    this->device = device;
    this->width = width;
    this->height = height;
    createTextures(commandPool);
    createDescriptorSets();
    createComputePipelines(sharedLayout);
    dispatchNoiseGeneration(commandPool);
}

std::unique_ptr<Kataglyphis::Texture> Clouds::createStorageTexture(vk::CommandPool commandPool,
  uint32_t w,
  uint32_t h,
  uint32_t depth,
  vk::ImageType type,
  vk::ImageViewType viewType)
{
    auto texture = std::make_unique<Texture>();
    texture->createImage(device,
      w,
      h,
      1,
      vk::Format::eR16G16B16A16Sfloat,
      vk::ImageTiling::eOptimal,
      vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
      vk::MemoryPropertyFlagBits::eDeviceLocal,
      1,
      vk::ImageCreateFlags{},
      type,
      depth);
    // clang-format off: BuildIntegrity wants the call, its eColor aspect and the marker on one line.
    texture->createImageView(device, vk::Format::eR16G16B16A16Sfloat, vk::ImageAspectFlagBits::eColor, 1, viewType, 1);// COLOR_ATTACHMENT_CHAIN_OK: storage-texture-3d-or-array-view-type
    // clang-format on
    texture->createTextureSampler(device);

    vk::CommandBuffer commandBuffer = Kataglyphis::VulkanRendererInternals::CommandBufferManager::beginCommandBuffer(
      device->getLogicalDevice(), commandPool);
    if (!commandBuffer) {
        // Fatal rather than a null texture: the post pipeline's binding 1 is not optional.
        ASSERT_VULKAN(
          VK_ERROR_INITIALIZATION_FAILED, "Clouds: failed to begin a command buffer for a storage texture!");
    }
    texture->getVulkanImage().transitionImageLayout(
      commandBuffer, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, 1, vk::ImageAspectFlagBits::eColor);
    // clang-format off: BuildIntegrity reads a submit's result off the call's own line.
    bool const transition_submitted = Kataglyphis::VulkanRendererInternals::CommandBufferManager::endAndSubmitCommandBuffer(device->getLogicalDevice(), commandPool, device->getGraphicsQueue(), commandBuffer);
    // clang-format on
    if (!transition_submitted) {
        // Fatal too: the descriptor written later declares eGeneral for an image still in eUndefined.
        ASSERT_VULKAN(
          VK_ERROR_INITIALIZATION_FAILED, "Clouds: failed to submit the layout transition for a storage texture!");
    }

    return texture;
}

void Clouds::createTextures(vk::CommandPool commandPool)
{
    // 3D Texture for Noise
    cloudNoiseTexture = createStorageTexture(commandPool,
      kNoiseVolumeExtent,
      kNoiseVolumeExtent,
      kNoiseVolumeExtent,
      vk::ImageType::e3D,
      vk::ImageViewType::e3D);

    // 2D Texture for Cloud Output. Assume screen size or half-screen size for performance
    cloudOutputTexture =
      createStorageTexture(commandPool, width, height, 1, vk::ImageType::e2D, vk::ImageViewType::e2D);
}

void Clouds::createDescriptorSets()
{
    cloudDescriptors.addBinding(0, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute)
      .addBinding(1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute);
    if (!cloudDescriptors.create(device, 1)) { spdlog::error("Failed to create cloud descriptor resources!"); }

    noiseDescriptors.addBinding(0, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute);
    if (!noiseDescriptors.create(device, 1)) { spdlog::error("Failed to create noise descriptor resources!"); }

    cloudDescriptors.writeImage(0, 0, cloudOutputTexture->getImageView(), vk::ImageLayout::eGeneral);
    cloudDescriptors.writeImage(
      0, 1, cloudNoiseTexture->getImageView(), vk::ImageLayout::eGeneral, cloudNoiseTexture->getSampler());
    noiseDescriptors.writeImage(0, 0, cloudNoiseTexture->getImageView(), vk::ImageLayout::eGeneral);
}

void Clouds::createComputePipelines(vk::DescriptorSetLayout sharedLayout)
{
    // Build-SlangShaders.ps1 emits this SPIR-V; the paths resolve from the repo root.

    // cloud specific set AND sharedRenderDescriptorSet
    std::array<vk::DescriptorSetLayout, 2> cloudLayouts = { cloudDescriptors.getLayout(), sharedLayout };
    ComputePipelineHandles cloudHandles =
      createComputePipeline(device, "Resources/ShadersSlang/build/spirv/compute/clouds.clouds_main.spv", cloudLayouts);
    cloudPipelineLayout = cloudHandles.layout;
    cloudComputePipeline = cloudHandles.pipeline;

    // Noise pipeline (Slang-emitted SPIR-V)
    std::array<vk::DescriptorSetLayout, 1> noiseLayouts = { noiseDescriptors.getLayout() };
    ComputePipelineHandles noiseHandles =
      createComputePipeline(device, "Resources/ShadersSlang/build/spirv/compute/noise.noise_main.spv", noiseLayouts);
    noisePipelineLayout = noiseHandles.layout;
    noiseComputePipeline = noiseHandles.pipeline;
}

void Clouds::dispatchNoiseGeneration(vk::CommandPool commandPool)
{
    // Graphics family only, so the eExclusive noise volume never needs a queue-family ownership transfer.
    if (!device->graphicsFamilySupportsCompute()) {
        spdlog::warn(
          "Graphics queue family does not support compute; skipping noise generation dispatch "
          "(cloud noise volume will render as uniform haze).");
        return;
    }

    vk::CommandBuffer commandBuffer = Kataglyphis::VulkanRendererInternals::CommandBufferManager::beginCommandBuffer(
      device->getLogicalDevice(), commandPool);
    if (!commandBuffer) {
        spdlog::error("Clouds::dispatchNoiseGeneration: failed to begin command buffer, skipping noise generation.");
        return;
    }

    const vk::DescriptorSet noiseDescriptorSet = noiseDescriptors.sets()[0];
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, noiseComputePipeline);
    commandBuffer.bindDescriptorSets(
      vk::PipelineBindPoint::eCompute, noisePipelineLayout, 0, 1, &noiseDescriptorSet, 0, nullptr);

    // noise texture is kNoiseVolumeExtent^3, workgroup size is kNoiseWorkgroupSize^3
    commandBuffer.dispatch(kNoiseVolumeExtent / kNoiseWorkgroupSize,
      kNoiseVolumeExtent / kNoiseWorkgroupSize,
      kNoiseVolumeExtent / kNoiseWorkgroupSize);

    // clang-format off: BuildIntegrity reads a submit's result off the call's own line.
    bool const dispatch_submitted = Kataglyphis::VulkanRendererInternals::CommandBufferManager::endAndSubmitCommandBuffer(device->getLogicalDevice(), commandPool, device->getGraphicsQueue(), commandBuffer);
    // clang-format on
    if (!dispatch_submitted) {
        spdlog::error(
          "Clouds::dispatchNoiseGeneration: failed to submit noise dispatch commands (cloud noise "
          "volume will render as uniform haze).");
    }
}

void Clouds::shaderHotReload(vk::DescriptorSetLayout sharedLayout)
{
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), cloudComputePipeline, cloudPipelineLayout);
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), noiseComputePipeline, noisePipelineLayout);
    // The noise volume is content, not pipeline state, so it is not regenerated.
    createComputePipelines(sharedLayout);
}

void Clouds::recordComputeCommands(vk::CommandBuffer &commandBuffer, std::span<const vk::DescriptorSet> descriptorSets)
{
    if (descriptorSets.empty()) {
        spdlog::error("Clouds::recordComputeCommands called with an empty shared descriptor set span");
        return;
    }

    // Bind cloud compute pipeline and dispatch thread groups based on screen extent
    const vk::DescriptorSet cloudDescriptorSet = cloudDescriptors.sets()[0];
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, cloudComputePipeline);
    commandBuffer.bindDescriptorSets(
      vk::PipelineBindPoint::eCompute, cloudPipelineLayout, 0, 1, &cloudDescriptorSet, 0, nullptr);

    // Also bind the shared rendering descriptor set (which was passed to us as layout 1)
    commandBuffer.bindDescriptorSets(
      vk::PipelineBindPoint::eCompute, cloudPipelineLayout, 1, 1, &descriptorSets[0], 0, nullptr);

    // Image size is dynamically set to width x height
    commandBuffer.dispatch((width + kCloudWorkgroupSize - 1) / kCloudWorkgroupSize,
      (height + kCloudWorkgroupSize - 1) / kCloudWorkgroupSize,
      1);
}

void Clouds::recreateFrameResources(vk::CommandPool commandPool, uint32_t width, uint32_t height)
{
    this->width = width;
    this->height = height;

    if (cloudOutputTexture) { cloudOutputTexture->cleanUp(); }

    cloudOutputTexture =
      createStorageTexture(commandPool, width, height, 1, vk::ImageType::e2D, vk::ImageViewType::e2D);

    cloudDescriptors.writeImage(0, 0, cloudOutputTexture->getImageView(), vk::ImageLayout::eGeneral);
}

void Clouds::cleanUp()
{
    // Idempotent: the destructor calls it again as a safety net.
    if (!device) { return; }

    cloudDescriptors.cleanUp();
    noiseDescriptors.cleanUp();
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), noiseComputePipeline, noisePipelineLayout);
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), cloudComputePipeline, cloudPipelineLayout);
    if (cloudNoiseTexture) { cloudNoiseTexture->cleanUp(); }
    cloudNoiseTexture.reset();
    if (cloudOutputTexture) { cloudOutputTexture->cleanUp(); }
    cloudOutputTexture.reset();

    device.reset();
}

}// namespace Kataglyphis
