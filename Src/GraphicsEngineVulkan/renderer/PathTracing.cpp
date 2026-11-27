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

#include "common/EnvironmentVariable.hpp"
#include "common/ImageBarrierHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "renderer/PathTracingDispatch.hpp"
#include "renderer/pushConstants/PushConstantPathTracing.hpp"

module kataglyphis.vulkan.path_tracing;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.image;
import kataglyphis.vulkan.shader_helper;

// See https://github.com/nvpro-samples/vk_mini_path_tracer/blob/main/vk_mini_path_tracer/main.cpp

namespace {

struct FurnaceSurface
{
    float metallic = 0.0F;
    float roughness = 0.5F;
};

// KATAGLYPHIS_PT_FURNACE_METALLIC and _ROUGHNESS pick the furnace's white surface; unset, a dielectric at 0.5.
FurnaceSurface furnaceSurfaceFromEnvironment()
{
    FurnaceSurface surface;
    if (const auto metallic = Kataglyphis::environmentVariable("KATAGLYPHIS_PT_FURNACE_METALLIC");
      metallic.has_value() && !metallic->empty()) {
        surface.metallic = std::clamp(std::strtof(metallic->c_str(), nullptr), 0.0F, 1.0F);
    }
    // 0.045 is material_roughness()'s floor, which keeps the GGX lobe finite.
    if (const auto roughness = Kataglyphis::environmentVariable("KATAGLYPHIS_PT_FURNACE_ROUGHNESS");
      roughness.has_value() && !roughness->empty()) {
        surface.roughness = std::clamp(std::strtof(roughness->c_str(), nullptr), 0.045F, 1.0F);
    }
    return surface;
}

}// namespace

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
  uint32_t max_bounces,
  bool trace_cloud_depth,
  bool skybox_enabled)
{
    // UNDEFINED: the output follows the last frame's post, bloom and histogram reads; the history its dispatch.
    const std::array<vk::ImageMemoryBarrier2, 2> before_dispatch = {
        Kataglyphis::buildImageMemoryBarrier(vulkanImage.getImage(),
          vk::ImageLayout::eUndefined,
          vk::ImageLayout::eGeneral,
          vk::PipelineStageFlagBits2::eColorAttachmentOutput | vk::PipelineStageFlagBits2::eFragmentShader
            | vk::PipelineStageFlagBits2::eComputeShader,
          vk::AccessFlagBits2::eNone,
          vk::PipelineStageFlagBits2::eComputeShader,
          vk::AccessFlagBits2::eShaderStorageWrite),
        Kataglyphis::buildImageMemoryBarrier(accumulationImage.getImage(),
          vk::ImageLayout::eGeneral,
          vk::ImageLayout::eGeneral,
          vk::PipelineStageFlagBits2::eComputeShader,
          vk::AccessFlagBits2::eShaderStorageWrite,
          vk::PipelineStageFlagBits2::eComputeShader,
          vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite),
    };
    Kataglyphis::recordImageBarriers(commandBuffer, before_dispatch);

    vk::Extent2D const imageSize = vulkanSwapChain->getSwapChainExtent();
    push_constant.width = imageSize.width;
    push_constant.height = imageSize.height;
    // White-furnace mode; read per record, not cached, because several tests share one process.
    const auto furnace_value = Kataglyphis::environmentVariable("KATAGLYPHIS_PT_FURNACE");
    if (furnace_value.has_value() && !furnace_value->empty()) {
        const float furnace_radiance = std::strtof(furnace_value->c_str(), nullptr);
        push_constant.clearColor = { furnace_radiance, furnace_radiance, furnace_radiance, 1.0F };
    } else {
        push_constant.clearColor = { 0.0F, 0.0F, 0.0F, 0.0F };
    }
    const FurnaceSurface furnace_surface = furnaceSurfaceFromEnvironment();
    push_constant.furnace_metallic = furnace_surface.metallic;
    push_constant.furnace_roughness = furnace_surface.roughness;
    push_constant.frame_index = frame_index;
    push_constant.samples_per_pixel = std::max(samples_per_pixel, 1U);
    push_constant.max_bounces = std::max(max_bounces, 1U);
    push_constant.trace_cloud_depth = trace_cloud_depth ? 1U : 0U;
    push_constant.skybox_enabled = skybox_enabled ? 1U : 0U;

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

    // The consumers are post.slang's fragment shader and the auto-exposure histogram's compute pass.
    Kataglyphis::recordImageBarrier(commandBuffer,
      Kataglyphis::buildImageMemoryBarrier(vulkanImage.getImage(),
        vk::ImageLayout::eGeneral,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderSampledRead));
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