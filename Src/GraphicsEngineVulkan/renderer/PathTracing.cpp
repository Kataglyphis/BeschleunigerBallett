module;
#include <memory>
#include <optional>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <sstream>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "common/ImageBarrierHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "renderer/PathTracingDispatch.hpp"
#include "renderer/pushConstants/PushConstantPathTracing.hpp"

module kataglyphis.vulkan.path_tracing;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.image;
import kataglyphis.vulkan.shader_helper;

// See https://github.com/nvpro-samples/vk_mini_path_tracer/blob/main/vk_mini_path_tracer/main.cpp

Kataglyphis::VulkanRendererInternals::PathTracing::PathTracing() = default;

void Kataglyphis::VulkanRendererInternals::PathTracing::init(const std::shared_ptr<VulkanDevice> &in_device,
  std::span<const vk::DescriptorSetLayout> descriptorSetLayouts)
{
    this->device = in_device;

    createPipeline(descriptorSetLayouts);
}

void Kataglyphis::VulkanRendererInternals::PathTracing::shaderHotReload(
  std::span<const vk::DescriptorSetLayout> descriptor_set_layouts)
{
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), pipeline, pipeline_layout);
    createPipeline(descriptor_set_layouts);
}

void Kataglyphis::VulkanRendererInternals::PathTracing::recordCommands(vk::CommandBuffer &commandBuffer,
  uint32_t /*image_index*/,
  VulkanImage &vulkanImage,
  VulkanImage &accumulationImage,
  VulkanSwapChain *vulkanSwapChain,
  std::span<const vk::DescriptorSet> descriptorSets,
  uint32_t frame_index,
  uint32_t samples_per_pixel,
  uint32_t max_bounces)
{
    // Same queue, so no ownership transfer; eUndefined because the kernel overwrites every pixel.
    const vk::ImageMemoryBarrier presentToPathTracingImageBarrier =
      Kataglyphis::buildImageMemoryBarrier(vulkanImage.getImage(),
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eGeneral,
        {},
        vk::AccessFlagBits::eShaderWrite);

    // This frame's raster write and, since the image is not per-frame, the previous frame's post-pass read.
    commandBuffer.pipelineBarrier(
      vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eFragmentShader,
      vk::PipelineStageFlagBits::eComputeShader,
      vk::DependencyFlags{},
      {},
      {},
      { presentToPathTracingImageBarrier });

    // The previous frame's dispatch may still be writing the history; a pipeline barrier orders across command buffers.
    const vk::ImageMemoryBarrier accumulationBarrier =
      Kataglyphis::buildImageMemoryBarrier(accumulationImage.getImage(),
        vk::ImageLayout::eGeneral,
        vk::ImageLayout::eGeneral,
        vk::AccessFlagBits::eShaderWrite,
        vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);

    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
      vk::PipelineStageFlagBits::eComputeShader,
      vk::DependencyFlags{},
      {},
      {},
      { accumulationBarrier });

    vk::Extent2D const imageSize = vulkanSwapChain->getSwapChainExtent();
    push_constant.width = imageSize.width;
    push_constant.height = imageSize.height;
    // White-furnace mode; read per record, not cached, because several tests share one process.
    const char *furnace_value = std::getenv("KATAGLYPHIS_PT_FURNACE");
    if (furnace_value != nullptr && *furnace_value != '\0') {
        const float furnace_radiance = std::strtof(furnace_value, nullptr);
        push_constant.clearColor = { furnace_radiance, furnace_radiance, furnace_radiance, 1.0F };
    } else {
        push_constant.clearColor = { 0.0F, 0.0F, 0.0F, 0.0F };
    }
    push_constant.frame_index = frame_index;
    push_constant.samples_per_pixel = std::max(samples_per_pixel, 1U);
    push_constant.max_bounces = std::max(max_bounces, 1U);

    commandBuffer.pushConstants(
      pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(PushConstantPathTracing), &push_constant);

    commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);

    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_layout, 0, descriptorSets, nullptr);

    uint32_t const workGroupCountX = std::max(
      (imageSize.width + Kataglyphis::kPathTracingWorkgroupSizeX - 1) / Kataglyphis::kPathTracingWorkgroupSizeX, 1U);
    uint32_t const workGroupCountY = std::max(
      (imageSize.height + Kataglyphis::kPathTracingWorkgroupSizeY - 1) / Kataglyphis::kPathTracingWorkgroupSizeY, 1U);
    uint32_t const workGroupCountZ = 1;

    commandBuffer.dispatch(workGroupCountX, workGroupCountY, workGroupCountZ);

    const vk::ImageMemoryBarrier pathTracingToPresentImageBarrier =
      Kataglyphis::buildImageMemoryBarrier(vulkanImage.getImage(),
        vk::ImageLayout::eGeneral,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::AccessFlagBits::eShaderWrite,
        vk::AccessFlagBits::eShaderRead);

    // The consumer is post.slang's fragment shader.
    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
      vk::PipelineStageFlagBits::eFragmentShader,
      vk::DependencyFlags{},
      {},
      {},
      { pathTracingToPresentImageBarrier });
}

void Kataglyphis::VulkanRendererInternals::PathTracing::cleanUp()
{
    // Idempotent, and a no-op when init() never ran (no hardware ray tracing).
    if (!device) { return; }

    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), pipeline, pipeline_layout);

    device.reset();
}

Kataglyphis::VulkanRendererInternals::PathTracing::~PathTracing() { cleanUp(); }

void Kataglyphis::VulkanRendererInternals::PathTracing::createPipeline(
  std::span<const vk::DescriptorSetLayout> descriptorSetLayouts)
{
    vk::PushConstantRange push_constant_range{};
    push_constant_range.stageFlags = vk::ShaderStageFlagBits::eCompute;
    push_constant_range.offset = 0;
    push_constant_range.size = sizeof(PushConstantPathTracing);

    const std::array<vk::PushConstantRange, 1> push_constant_ranges = { push_constant_range };

    // Relative path: the engine runs from the repo root.
    std::string const slang_spv_dir = "Resources/ShadersSlang/build/spirv/path_tracing/";

    std::string const pathTracing_spv = "path_tracing.path_tracing_main.spv";

    Kataglyphis::ComputePipelineHandles handles = Kataglyphis::createComputePipeline(device,
      slang_spv_dir + pathTracing_spv,
      descriptorSetLayouts,
      push_constant_ranges,
      "Failed to create path tracing pipeline layout!",
      "Failed to create compute pipeline!");
    pipeline_layout = handles.layout;
    pipeline = handles.pipeline;
}