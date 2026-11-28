module;
#include <memory>

#include <vulkan/vulkan.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/ImageBarrierHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/RenderingHelper.hpp"
#include "common/Utilities.hpp"
#include "common/ViewportHelper.hpp"
#include <spdlog/spdlog.h>

module kataglyphis.vulkan.bloom_stage;

import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.color_attachment;
import kataglyphis.vulkan.command_buffer_manager;
import kataglyphis.vulkan.descriptor_set_group;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.pipeline_builder;
import kataglyphis.vulkan.sampler_builder;
import kataglyphis.vulkan.shader_helper;
import kataglyphis.vulkan.swapchain;
import kataglyphis.vulkan.texture;

namespace {
// bloom.slang's set 0, as the WebGPU layout has it.
constexpr uint32_t kSourceBinding = 0;
constexpr uint32_t kSamplerBinding = 1;
constexpr uint32_t kExposureBinding = 2;
constexpr uint32_t kCloudBinding = 3;
constexpr uint32_t kUniformsBinding = 4;
constexpr vk::DeviceSize kUniformsBytes = 4 * sizeof(float);
constexpr vk::DeviceSize kExposureBytes = 2 * sizeof(float);

constexpr vk::PipelineStageFlags2 kFragment = vk::PipelineStageFlagBits2::eFragmentShader;
constexpr vk::PipelineStageFlags2 kColorOutput = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
constexpr vk::AccessFlags2 kColorWrite = vk::AccessFlagBits2::eColorAttachmentWrite;
constexpr vk::AccessFlags2 kSampledRead = vk::AccessFlagBits2::eShaderSampledRead;

// A target's write in this pass, after earlier frames' and passes' reads and writes of it; UNDEFINED discards.
vk::ImageMemoryBarrier2 toTarget(vk::Image image)
{
    return Kataglyphis::buildImageMemoryBarrier(image,
      vk::ImageLayout::eUndefined,
      vk::ImageLayout::eColorAttachmentOptimal,
      kColorOutput | kFragment | vk::PipelineStageFlagBits2::eTransfer,
      kColorWrite,
      kColorOutput,
      kColorWrite);
}

// The next pass, or post, samples what this one wrote.
vk::ImageMemoryBarrier2 toSampled(vk::Image image)
{
    return Kataglyphis::buildImageMemoryBarrier(image,
      vk::ImageLayout::eColorAttachmentOptimal,
      vk::ImageLayout::eShaderReadOnlyOptimal,
      kColorOutput,
      kColorWrite,
      kFragment,
      kSampledRead);
}
}// namespace

void Kataglyphis::VulkanRendererInternals::clearToShaderRead(const std::shared_ptr<VulkanDevice> &device,
  vk::CommandPool commandPool,
  std::span<const vk::Image> images,
  vk::ClearColorValue color)
{
    vk::CommandBuffer commandBuffer = CommandBufferManager::beginCommandBuffer(device->getLogicalDevice(), commandPool);
    if (!commandBuffer) {
        spdlog::error("clearToShaderRead: no command buffer for the targets' first clear.");
        return;
    }
    std::vector<vk::ImageMemoryBarrier2> barriers;
    for (const vk::Image image : images) {
        barriers.push_back(buildImageMemoryBarrier(image,
          vk::ImageLayout::eUndefined,
          vk::ImageLayout::eTransferDstOptimal,
          vk::PipelineStageFlagBits2::eNone,
          vk::AccessFlagBits2::eNone,
          vk::PipelineStageFlagBits2::eClear,
          vk::AccessFlagBits2::eTransferWrite));
    }
    recordImageBarriers(commandBuffer, barriers);
    const vk::ImageSubresourceRange range{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
    for (const vk::Image image : images) {
        commandBuffer.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, &color, 1, &range);
    }
    barriers.clear();
    for (const vk::Image image : images) {
        barriers.push_back(buildImageMemoryBarrier(image,
          vk::ImageLayout::eTransferDstOptimal,
          vk::ImageLayout::eShaderReadOnlyOptimal,
          vk::PipelineStageFlagBits2::eClear,
          vk::AccessFlagBits2::eTransferWrite,
          kFragment,
          kSampledRead));
    }
    recordImageBarriers(commandBuffer, barriers);
    // clang-format off: BuildIntegrity reads a submit's result off the call's own line.
    bool const cleared = CommandBufferManager::endAndSubmitCommandBuffer(device->getLogicalDevice(), commandPool, device->getGraphicsQueue(), commandBuffer);
    // clang-format on
    if (!cleared) { spdlog::error("clearToShaderRead: the targets' first clear was not submitted."); }
}

Kataglyphis::VulkanRendererInternals::BloomStage::BloomStage() = default;

void Kataglyphis::VulkanRendererInternals::BloomStage::init(const std::shared_ptr<VulkanDevice> &in_device,
  VulkanSwapChain *swap_chain,
  vk::CommandPool commandPool,
  vk::Extent2D sceneExtent)
{
    device = in_device;
    vulkanSwapChain = swap_chain;

    // OxidANT's bloom sampler: the half-resolution taps filter, the edge clamps.
    const vk::SamplerCreateInfo sampler_info = buildSamplerCreateInfo(vk::Filter::eLinear,
      vk::SamplerAddressMode::eClampToEdge,
      0.0F,
      VK_FALSE,
      1.0F,
      vk::BorderColor::eFloatOpaqueBlack);
    sampler = vkCheck(device->getLogicalDevice().createSampler(sampler_info), "Failed to create the bloom sampler!");

    createFrameResources(commandPool, sceneExtent);
    createPipelines();
}

void Kataglyphis::VulkanRendererInternals::BloomStage::createFrameResources(vk::CommandPool commandPool,
  vk::Extent2D sceneExtent)
{
    extent = postHalfExtent(sceneExtent);
    imageCount = vulkanSwapChain->getNumberSwapChainImages();

    const vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
                                      | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
    output = std::make_unique<Texture>();
    createColorAttachment(*output, device, extent, kFormat, usage, "bloom");
    scratch = std::make_unique<Texture>();
    createColorAttachment(*scratch, device, extent, kFormat, usage, "bloom scratch");

    // Post samples the output every frame, blooming or not, so it starts black and sampleable.
    const std::array<vk::Image, 2> targets = { output->getImage(), scratch->getImage() };
    clearToShaderRead(
      device, commandPool, targets, vk::ClearColorValue{ std::array<float, 4>{ 0.0F, 0.0F, 0.0F, 0.0F } });

    uniforms.clear();
    uniforms.resize(imageCount);
    for (VulkanBuffer &buffer : uniforms) {
        buffer.create(device,
          kUniformsBytes,
          vk::BufferUsageFlagBits::eUniformBuffer,
          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    }

    descriptors.addBinding(kSourceBinding, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment)
      .addBinding(kSamplerBinding, vk::DescriptorType::eSampler, 1, vk::ShaderStageFlagBits::eFragment)
      .addBinding(kExposureBinding, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eFragment)
      .addBinding(kCloudBinding, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment)
      .addBinding(kUniformsBinding, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment);
    if (!descriptors.create(device, imageCount + 2U)) {
        spdlog::error("Failed to create the bloom descriptor resources!");
        return;
    }
    // The blurs read the previous target and nothing else of the layout; image 0's buffers stand in for the rest.
    const uint32_t horizontal = imageCount;
    const uint32_t vertical = imageCount + 1U;
    for (uint32_t set = 0; set < imageCount + 2U; ++set) {
        descriptors.writeImage(set, kSamplerBinding, nullptr, vk::ImageLayout::eUndefined, sampler);
        descriptors.writeBuffer(
          set, kUniformsBinding, uniforms[set < imageCount ? set : 0U].getBuffer(), kUniformsBytes);
    }
    // Each blur's unread cloud slot takes its source too, which is sampleable while that blur runs.
    for (const auto &[blur, source] : { std::pair{ horizontal, output.get() }, std::pair{ vertical, scratch.get() } }) {
        descriptors.writeImage(blur, kSourceBinding, source->getImageView(), vk::ImageLayout::eShaderReadOnlyOptimal);
        descriptors.writeImage(blur, kCloudBinding, source->getImageView(), vk::ImageLayout::eShaderReadOnlyOptimal);
    }
}

void Kataglyphis::VulkanRendererInternals::BloomStage::bindInputs(uint32_t imageIndex,
  vk::ImageView hdrView,
  vk::ImageView cloudView,
  vk::Buffer exposure)
{
    if (imageIndex >= imageCount || descriptors.sets().size() < imageCount + 2U) {
        spdlog::error("BloomStage::bindInputs: no set for image {}", imageIndex);
        return;
    }
    descriptors.writeImage(imageIndex, kSourceBinding, hdrView, vk::ImageLayout::eShaderReadOnlyOptimal);
    // The march writes the clouds as a storage image and leaves them in eGeneral, where post samples them too.
    descriptors.writeImage(imageIndex, kCloudBinding, cloudView, vk::ImageLayout::eGeneral);
    for (uint32_t set = 0; set < imageCount + 2U; ++set) {
        if (set != imageIndex && set < imageCount) { continue; }
        descriptors.writeBuffer(set, kExposureBinding, exposure, kExposureBytes);
    }
}

void Kataglyphis::VulkanRendererInternals::BloomStage::releaseFrameResources()
{
    descriptors.cleanUp();
    uniforms.clear();
    if (output) { output->cleanUp(); }
    if (scratch) { scratch->cleanUp(); }
    output.reset();
    scratch.reset();
}

void Kataglyphis::VulkanRendererInternals::BloomStage::recreateFrameResources(vk::CommandPool commandPool,
  vk::Extent2D sceneExtent)
{
    releaseFrameResources();
    createFrameResources(commandPool, sceneExtent);
}

void Kataglyphis::VulkanRendererInternals::BloomStage::createPipelines()
{
    // Post's SV_VulkanVertexID triangle in front of OxidANT's fragment entries; the paths resolve from the repo root.
    const std::string spv_dir = "Resources/ShadersSlang/build/spirv/";
    const std::string vertex = spv_dir + "post/post.vs_main.spv";

    const std::array<vk::DescriptorSetLayout, 1> layouts = { descriptors.getLayout() };
    const vk::PipelineLayoutCreateInfo layout_info = buildPipelineLayoutCreateInfo(layouts);
    pipelineLayout = vkCheck(
      device->getLogicalDevice().createPipelineLayout(layout_info), "Failed to create the bloom pipeline layout!");

    const auto build = [&](const char *entry, const char *name) {
        ShaderStagePair stages{ device, vertex, spv_dir + "bloom/bloom." + entry + ".spv" };
        return PipelineBuilder()
          .setShaderStages({ stages.stages().begin(), stages.stages().end() })
          .setVertexInput({}, {})
          .setCullMode(vk::CullModeFlagBits::eNone)
          .setDepthTest(false)
          .setDepthWrite(false)
          .setRenderingFormats({ kFormat })
          .setDebugName(name)
          .build(device->getLogicalDevice(),
            pipelineLayout,
            device->getPipelineCache(),
            "Failed to create a bloom pipeline!");
    };
    brightPipeline = build("fs_brightpass", "bloom bright pass");
    blurHorizontalPipeline = build("fs_blur_h", "bloom blur h");
    blurVerticalPipeline = build("fs_blur_v", "bloom blur v");
}

void Kataglyphis::VulkanRendererInternals::BloomStage::destroyPipelines()
{
    // The three share one layout, which goes with the last.
    vk::PipelineLayout shared_elsewhere{};
    const vk::Device logical = device->getLogicalDevice();
    Kataglyphis::destroyPipelineAndLayout(logical, brightPipeline, shared_elsewhere);
    Kataglyphis::destroyPipelineAndLayout(logical, blurHorizontalPipeline, shared_elsewhere);
    Kataglyphis::destroyPipelineAndLayout(logical, blurVerticalPipeline, pipelineLayout);
}

void Kataglyphis::VulkanRendererInternals::BloomStage::shaderHotReload()
{
    vk::PipelineLayout shared_elsewhere{};
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), brightPipeline, shared_elsewhere);
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), blurHorizontalPipeline, shared_elsewhere);
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), blurVerticalPipeline, pipelineLayout);
    createPipelines();
}

bool Kataglyphis::VulkanRendererInternals::BloomStage::ready(uint32_t imageIndex) const
{
    return imageIndex < imageCount && descriptors.sets().size() >= imageCount + 2U && output && scratch
           && static_cast<bool>(brightPipeline) && static_cast<bool>(blurHorizontalPipeline)
           && static_cast<bool>(blurVerticalPipeline);
}

void Kataglyphis::VulkanRendererInternals::BloomStage::recordFullscreen(vk::CommandBuffer commandBuffer,
  vk::Pipeline pipeline,
  vk::DescriptorSet set,
  Kataglyphis::Texture &target)
{
    // eDontCare: the triangle writes every texel.
    const vk::RenderingAttachmentInfo attachment = buildRenderingAttachmentInfo(
      target.getImageView(), vk::ImageLayout::eColorAttachmentOptimal, vk::AttachmentLoadOp::eDontCare);
    commandBuffer.beginRendering(
      buildRenderingInfo(extent, std::span<const vk::RenderingAttachmentInfo>(&attachment, 1), nullptr));
    setFullExtentViewportAndScissor(commandBuffer, extent);
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, 1, &set, 0, nullptr);
    commandBuffer.draw(3, 1, 0, 0);
    commandBuffer.endRendering();
}

void Kataglyphis::VulkanRendererInternals::BloomStage::recordCommands(vk::CommandBuffer commandBuffer,
  uint32_t imageIndex,
  bool cloudsEnabled)
{
    if (!ready(imageIndex)) {
        spdlog::error("BloomStage::recordCommands: no set or pipeline for image {}", imageIndex);
        return;
    }
    // The image's previous frame finished before this one records (its fence), so the host write races nothing.
    const std::array<float, 4> params = { cloudsEnabled ? 1.0F : 0.0F, 0.0F, 0.0F, 0.0F };
    std::memcpy(uniforms[imageIndex].getMappedData(), params.data(), sizeof(params));

    const std::vector<vk::DescriptorSet> &sets = descriptors.sets();
    const vk::Image output_image = output->getImage();
    const vk::Image scratch_image = scratch->getImage();

    // The bright pass into the output, which the last frame's post sampled.
    recordImageBarrier(commandBuffer, toTarget(output_image));
    recordFullscreen(commandBuffer, brightPipeline, sets[imageIndex], *output);

    const std::array<vk::ImageMemoryBarrier2, 2> to_horizontal = { toSampled(output_image), toTarget(scratch_image) };
    recordImageBarriers(commandBuffer, to_horizontal);
    recordFullscreen(commandBuffer, blurHorizontalPipeline, sets[imageCount], *scratch);

    // The vertical blur goes back into the output, whose bright pass the horizontal blur just read.
    const std::array<vk::ImageMemoryBarrier2, 2> to_vertical = { toSampled(scratch_image), toTarget(output_image) };
    recordImageBarriers(commandBuffer, to_vertical);
    recordFullscreen(commandBuffer, blurVerticalPipeline, sets[imageCount + 1U], *output);

    recordImageBarrier(commandBuffer, toSampled(output_image));
}

void Kataglyphis::VulkanRendererInternals::BloomStage::cleanUp()
{
    // Idempotent: the destructor calls it again after an explicit cleanUp.
    if (!device) { return; }

    destroyPipelines();
    releaseFrameResources();
    if (sampler) {
        device->getLogicalDevice().destroySampler(sampler);
        sampler = nullptr;
    }

    device.reset();
}

Kataglyphis::VulkanRendererInternals::BloomStage::~BloomStage() { cleanUp(); }
