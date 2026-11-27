module;
#include <memory>

#include <vulkan/vulkan.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/ImageBarrierHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/Utilities.hpp"
#include <spdlog/spdlog.h>

module kataglyphis.vulkan.exposure_stage;

import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.descriptor_set_group;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.shader_helper;

namespace {
// histogram.slang's set 0, as the WebGPU layout has it.
constexpr uint32_t kHdrBinding = 0;
constexpr uint32_t kHistogramBinding = 1;
constexpr uint32_t kExposureStateBinding = 2;
constexpr uint32_t kParametersBinding = 3;

// render::auto_exposure's HISTOGRAM_BINS and workgroup sizes, which OxidANT pins against the shader.
constexpr uint32_t kHistogramBins = 64;
constexpr uint32_t kBuildWorkgroup = 16;
constexpr uint32_t kClearWorkgroup = 64;
constexpr vk::DeviceSize kParametersBytes = 4 * sizeof(float);

constexpr vk::PipelineStageFlags2 kCompute = vk::PipelineStageFlagBits2::eComputeShader;
constexpr vk::AccessFlags2 kStorageReadWrite =
  vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite;
}// namespace

Kataglyphis::VulkanRendererInternals::ExposureStage::ExposureStage() = default;

void Kataglyphis::VulkanRendererInternals::ExposureStage::init(const std::shared_ptr<VulkanDevice> &in_device,
  uint32_t imageCount,
  float initialEv)
{
    device = in_device;

    histogram.create(device,
      kHistogramBins * sizeof(uint32_t),
      vk::BufferUsageFlagBits::eStorageBuffer,
      vk::MemoryPropertyFlagBits::eDeviceLocal);
    exposureState.create(device,
      kExposureStateBytes,
      vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst
        | vk::BufferUsageFlagBits::eTransferSrc,
      vk::MemoryPropertyFlagBits::eDeviceLocal);
    captureReadback.create(device,
      kExposureStateBytes,
      vk::BufferUsageFlagBits::eTransferDst,
      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    // Its contents are undefined until written, so the first reduce must start from a value.
    pendingReset = initialEv;

    recreateFrameResources(imageCount);
    createPipelines();
}

void Kataglyphis::VulkanRendererInternals::ExposureStage::recreateFrameResources(uint32_t imageCount)
{
    descriptors.cleanUp();
    parameters.clear();
    parameters.resize(imageCount);
    for (VulkanBuffer &buffer : parameters) {
        buffer.create(device,
          kParametersBytes,
          vk::BufferUsageFlagBits::eUniformBuffer,
          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    }

    descriptors.addBinding(kHdrBinding, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute)
      .addBinding(kHistogramBinding, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute)
      .addBinding(kExposureStateBinding, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute)
      .addBinding(kParametersBinding, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eCompute);
    if (!descriptors.create(device, imageCount)) {
        spdlog::error("Failed to create the exposure descriptor resources!");
        return;
    }
    for (uint32_t i = 0; i < imageCount; ++i) {
        descriptors.writeBuffer(i, kHistogramBinding, histogram.getBuffer(), kHistogramBins * sizeof(uint32_t));
        descriptors.writeBuffer(i, kExposureStateBinding, exposureState.getBuffer(), kExposureStateBytes);
        descriptors.writeBuffer(i, kParametersBinding, parameters[i].getBuffer(), kParametersBytes);
    }
}

void Kataglyphis::VulkanRendererInternals::ExposureStage::bindHdrTarget(uint32_t imageIndex, vk::ImageView hdrView)
{
    if (imageIndex >= descriptors.sets().size()) {
        spdlog::error("ExposureStage::bindHdrTarget: no set for image {}", imageIndex);
        return;
    }
    descriptors.writeImage(imageIndex, kHdrBinding, hdrView, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void Kataglyphis::VulkanRendererInternals::ExposureStage::createPipelines()
{
    // OxidANT's kernels, compiled to SPIR-V by this repository's manifest; the paths resolve from the repo root.
    const std::string spv_dir = "Resources/ShadersSlang/build/spirv/histogram/";
    const std::array<vk::DescriptorSetLayout, 1> layouts = { descriptors.getLayout() };
    ComputePipelineHandles clear = createComputePipeline(device, spv_dir + "histogram.cs_clear_histogram.spv", layouts);
    ComputePipelineHandles build = createComputePipeline(device, spv_dir + "histogram.cs_build_histogram.spv", layouts);
    ComputePipelineHandles reduce =
      createComputePipeline(device, spv_dir + "histogram.cs_reduce_exposure.spv", layouts);
    clearLayout = clear.layout;
    clearPipeline = clear.pipeline;
    buildLayout = build.layout;
    buildPipeline = build.pipeline;
    reduceLayout = reduce.layout;
    reducePipeline = reduce.pipeline;
}

void Kataglyphis::VulkanRendererInternals::ExposureStage::destroyPipelines()
{
    const vk::Device logical = device->getLogicalDevice();
    Kataglyphis::destroyPipelineAndLayout(logical, clearPipeline, clearLayout);
    Kataglyphis::destroyPipelineAndLayout(logical, buildPipeline, buildLayout);
    Kataglyphis::destroyPipelineAndLayout(logical, reducePipeline, reduceLayout);
}

void Kataglyphis::VulkanRendererInternals::ExposureStage::shaderHotReload()
{
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), clearPipeline, clearLayout);
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), buildPipeline, buildLayout);
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), reducePipeline, reduceLayout);
    createPipelines();
}

bool Kataglyphis::VulkanRendererInternals::ExposureStage::ready(uint32_t imageIndex) const
{
    return imageIndex < descriptors.sets().size() && imageIndex < parameters.size() && static_cast<bool>(clearPipeline)
           && static_cast<bool>(buildPipeline) && static_cast<bool>(reducePipeline);
}

void Kataglyphis::VulkanRendererInternals::ExposureStage::recordHistogram(vk::CommandBuffer commandBuffer,
  uint32_t imageIndex,
  vk::Extent2D extent)
{
    if (!ready(imageIndex)) {
        spdlog::error("ExposureStage::recordHistogram: no set or pipeline for image {}", imageIndex);
        return;
    }
    const vk::DescriptorSet set = descriptors.sets()[imageIndex];

    // The previous frame's build wrote the bins and its reduce read them; the clear overwrites both.
    recordBufferBarrier(commandBuffer,
      buildBufferMemoryBarrier(
        histogram.getBuffer(), kCompute, vk::AccessFlagBits2::eShaderStorageWrite, kCompute, kStorageReadWrite));
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, clearPipeline);
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, clearLayout, 0, 1, &set, 0, nullptr);
    commandBuffer.dispatch((kHistogramBins + kClearWorkgroup - 1) / kClearWorkgroup, 1, 1);

    recordBufferBarrier(commandBuffer,
      buildBufferMemoryBarrier(
        histogram.getBuffer(), kCompute, vk::AccessFlagBits2::eShaderStorageWrite, kCompute, kStorageReadWrite));
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, buildPipeline);
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, buildLayout, 0, 1, &set, 0, nullptr);
    // Round up: truncating leaves the right and bottom edges unsampled and biases the exposure.
    commandBuffer.dispatch(std::max((extent.width + kBuildWorkgroup - 1) / kBuildWorkgroup, 1U),
      std::max((extent.height + kBuildWorkgroup - 1) / kBuildWorkgroup, 1U),
      1);

    // The reduce reads the finished bins.
    recordBufferBarrier(commandBuffer,
      buildBufferMemoryBarrier(histogram.getBuffer(),
        kCompute,
        vk::AccessFlagBits2::eShaderStorageWrite,
        kCompute,
        vk::AccessFlagBits2::eShaderStorageRead));
}

void Kataglyphis::VulkanRendererInternals::ExposureStage::recordReduce(vk::CommandBuffer commandBuffer,
  uint32_t imageIndex,
  const ExposureFrameParams &params)
{
    if (!ready(imageIndex)) {
        spdlog::error("ExposureStage::recordReduce: no set or pipeline for image {}", imageIndex);
        return;
    }
    // The image's previous frame finished before this one records (its fence), so the host write races nothing.
    const std::array<float, 4> values = {
        params.deltaSeconds, params.speed, params.autoExposure ? 1.0F : 0.0F, params.manualEv
    };
    std::memcpy(parameters[imageIndex].getMappedData(), values.data(), sizeof(values));

    // Earlier frames read the state in post and bloom, copied it out, and the last reduce or reset wrote it.
    constexpr vk::PipelineStageFlags2 kEarlierUses =
      vk::PipelineStageFlagBits2::eFragmentShader | kCompute | vk::PipelineStageFlagBits2::eTransfer;
    constexpr vk::AccessFlags2 kEarlierWrites =
      vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eTransferWrite;
    if (pendingReset.has_value()) {
        recordBufferBarrier(commandBuffer,
          buildBufferMemoryBarrier(exposureState.getBuffer(),
            kEarlierUses,
            kEarlierWrites,
            vk::PipelineStageFlagBits2::eTransfer,
            vk::AccessFlagBits2::eTransferWrite));
        const std::array<float, 2> reset = { *pendingReset, *pendingReset };
        commandBuffer.updateBuffer(exposureState.getBuffer(), 0, kExposureStateBytes, reset.data());
        pendingReset.reset();
        recordBufferBarrier(commandBuffer,
          buildBufferMemoryBarrier(exposureState.getBuffer(),
            vk::PipelineStageFlagBits2::eTransfer,
            vk::AccessFlagBits2::eTransferWrite,
            kCompute,
            kStorageReadWrite));
    } else {
        recordBufferBarrier(commandBuffer,
          buildBufferMemoryBarrier(
            exposureState.getBuffer(), kEarlierUses, kEarlierWrites, kCompute, kStorageReadWrite));
    }

    const vk::DescriptorSet set = descriptors.sets()[imageIndex];
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, reducePipeline);
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, reduceLayout, 0, 1, &set, 0, nullptr);
    commandBuffer.dispatch(1, 1, 1);

    // Post and bloom read the EV in their fragment shaders; an armed capture copies it out.
    recordBufferBarrier(commandBuffer,
      buildBufferMemoryBarrier(exposureState.getBuffer(),
        kCompute,
        vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eTransfer,
        vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eTransferRead));

    if (!captureArmed) { return; }
    captureArmed = false;
    const vk::BufferCopy region{ 0, 0, kExposureStateBytes };
    commandBuffer.copyBuffer(exposureState.getBuffer(), captureReadback.getBuffer(), 1, &region);
    recordBufferBarrier(commandBuffer,
      buildBufferMemoryBarrier(captureReadback.getBuffer(),
        vk::PipelineStageFlagBits2::eTransfer,
        vk::AccessFlagBits2::eTransferWrite,
        vk::PipelineStageFlagBits2::eHost,
        vk::AccessFlagBits2::eHostRead));
    capturePending = true;
}

void Kataglyphis::VulkanRendererInternals::ExposureStage::requestReset(float ev) { pendingReset = ev; }

void Kataglyphis::VulkanRendererInternals::ExposureStage::requestCapture()
{
    captureArmed = true;
    capturePending = false;
    captureFence = vk::Fence{};
}

void Kataglyphis::VulkanRendererInternals::ExposureStage::bindCaptureFence(vk::Fence fence)
{
    if (capturePending && !captureFence) { captureFence = fence; }
}

void Kataglyphis::VulkanRendererInternals::ExposureStage::invalidateCaptureFence() { captureFence = vk::Fence{}; }

auto Kataglyphis::VulkanRendererInternals::ExposureStage::takeCapture() -> std::optional<std::array<float, 2>>
{
    if (!capturePending || !device) { return std::nullopt; }
    capturePending = false;
    if (captureFence
        && device->getLogicalDevice().waitForFences(1, &captureFence, VK_TRUE, UINT64_MAX) != vk::Result::eSuccess) {
        return std::nullopt;
    }
    captureFence = vk::Fence{};
    std::array<float, 2> state{};
    std::memcpy(state.data(), captureReadback.getMappedData(), sizeof(state));
    return state;
}

void Kataglyphis::VulkanRendererInternals::ExposureStage::cleanUp()
{
    // Idempotent: the destructor calls it again after an explicit cleanUp.
    if (!device) { return; }

    destroyPipelines();
    descriptors.cleanUp();
    parameters.clear();
    captureReadback.cleanUp();
    exposureState.cleanUp();
    histogram.cleanUp();

    device.reset();
}

Kataglyphis::VulkanRendererInternals::ExposureStage::~ExposureStage() { cleanUp(); }
