module;
#include <memory>
#include <optional>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "common/EnvironmentVariable.hpp"
#include "common/ImageBarrierHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "renderer/PathTracingDispatch.hpp"
#include "renderer/PathTracingHistory.hpp"
#include "renderer/pushConstants/PushConstantPathTracing.hpp"

module kataglyphis.vulkan.path_tracing;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.image;
import kataglyphis.vulkan.shader_helper;

// See https://github.com/nvpro-samples/vk_mini_path_tracer/blob/main/vk_mini_path_tracer/main.cpp

namespace {

// A furnace variable as a float, or nullopt when it is unset, empty or not finite, as Rust's clamped() heals one.
std::optional<float> furnaceVariable(const char *name)
{
    const auto value = Kataglyphis::environmentVariable(name);
    if (!value.has_value() || value->empty()) { return std::nullopt; }
    const float parsed = std::strtof(value->c_str(), nullptr);
    return std::isfinite(parsed) ? std::optional<float>(parsed) : std::nullopt;
}

// Read per frame, as the furnace's variables are: several tests share one process.
uint64_t dispatchSegmentsFromEnvironment()
{
    const auto value = Kataglyphis::environmentVariable("KATAGLYPHIS_PT_DISPATCH_SEGMENTS");
    return Kataglyphis::pathTracingDispatchBudget(value.has_value() ? std::string_view(*value) : std::string_view{});
}

}// namespace

auto Kataglyphis::VulkanRendererInternals::resolvePathTracingFurnace(bool editor_furnace) -> PathTracingFurnace
{
    // Read per frame, not cached, because several tests share one process.
    PathTracingFurnace furnace;
    furnace.on = editor_furnace || Kataglyphis::environmentFlag("KATAGLYPHIS_PT_FURNACE");
    if (const std::optional<float> radiance = furnaceVariable("KATAGLYPHIS_PT_FURNACE"); radiance.has_value()) {
        furnace.radiance = std::max(*radiance, 0.0F);
    }
    if (const std::optional<float> metallic = furnaceVariable("KATAGLYPHIS_PT_FURNACE_METALLIC");
      metallic.has_value()) {
        furnace.metallic = std::clamp(*metallic, 0.0F, 1.0F);
    }
    // 0.045 is material_roughness()'s floor, which keeps the GGX lobe finite.
    if (const std::optional<float> roughness = furnaceVariable("KATAGLYPHIS_PT_FURNACE_ROUGHNESS");
      roughness.has_value()) {
        furnace.roughness = std::clamp(*roughness, 0.045F, 1.0F);
    }
    return furnace;
}

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
  VulkanImage &vulkanImage,
  VulkanImage &accumulationImage,
  vk::Extent2D extent,
  std::span<const vk::DescriptorSet> descriptorSets,
  const PathTracingFrame &frame)
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

    push_constant.width = extent.width;
    push_constant.height = extent.height;
    const PathTracingFurnace &furnace = frame.furnace;
    push_constant.clearColor =
      furnace.on ? vec4(furnace.radiance, furnace.radiance, furnace.radiance, 1.0F) : vec4(0.0F, 0.0F, 0.0F, 0.0F);
    push_constant.furnace_metallic = furnace.metallic;
    push_constant.furnace_roughness = furnace.roughness;
    const uint32_t max_bounces = std::max(frame.max_bounces, 1U);
    push_constant.max_bounces = max_bounces;
    push_constant.trace_cloud_depth = frame.trace_cloud_depth ? 1U : 0U;
    push_constant.skybox_enabled = frame.skybox_enabled ? 1U : 0U;

    commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);

    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_layout, 0, descriptorSets, nullptr);

    const std::vector<Kataglyphis::PathTracingDispatch> dispatches =
      Kataglyphis::planPathTracingDispatches(extent.width,
        extent.height,
        frame.samples_per_pixel,
        max_bounces,
        frame.accumulated,
        dispatchSegmentsFromEnvironment());
    // Logged when it changes: how a frame splits is what keeps it under a GPU watchdog, and the goldens read it.
    const std::tuple<std::size_t, uint32_t, uint32_t> plan_shape{
        dispatches.size(), dispatches.front().samples, max_bounces
    };
    if (plan_shape != logged_plan_shape) {
        logged_plan_shape = plan_shape;
        spdlog::info("path tracer: {} dispatches a frame, up to {} samples of {} bounces each",
          std::get<0>(plan_shape),
          std::get<1>(plan_shape),
          std::get<2>(plan_shape));
    }
    for (std::size_t index = 0; index < dispatches.size(); ++index) {
        const Kataglyphis::PathTracingDispatch &tile = dispatches[index];
        // A later chunk reads what the one before wrote; within one, sync validation tracks a storage image whole.
        if (index > 0) {
            Kataglyphis::recordMemoryBarrier(commandBuffer,
              vk::PipelineStageFlagBits2::eComputeShader,
              vk::AccessFlagBits2::eShaderStorageWrite,
              vk::PipelineStageFlagBits2::eComputeShader,
              vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);
        }
        push_constant.tile_x = tile.x;
        push_constant.tile_y = tile.y;
        push_constant.tile_width = tile.width;
        push_constant.tile_height = tile.height;
        push_constant.first_sample = tile.first_sample;
        push_constant.samples = tile.samples;
        push_constant.accumulated = tile.accumulated;
        commandBuffer.pushConstants(
          pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(PushConstantPathTracing), &push_constant);
        commandBuffer.dispatch(
          (tile.width + Kataglyphis::kPathTracingWorkgroupSizeX - 1) / Kataglyphis::kPathTracingWorkgroupSizeX,
          (tile.height + Kataglyphis::kPathTracingWorkgroupSizeY - 1) / Kataglyphis::kPathTracingWorkgroupSizeY,
          1);
    }

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