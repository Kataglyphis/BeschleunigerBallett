module;
#include <memory>

#include <vulkan/vulkan.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "common/ImageBarrierHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/RenderingHelper.hpp"
#include "common/Utilities.hpp"
#include "common/ViewportHelper.hpp"
#include "renderer/SsaoUniforms.hpp"
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

module kataglyphis.vulkan.ssao_stage;

import kataglyphis.vulkan.bloom_stage;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.color_attachment;
import kataglyphis.vulkan.descriptor_set_group;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.pipeline_builder;
import kataglyphis.vulkan.sampler_builder;
import kataglyphis.vulkan.shader_helper;
import kataglyphis.vulkan.swapchain;
import kataglyphis.vulkan.texture;

namespace {
// ssao.slang's set 0, as the WebGPU layout has it.
constexpr uint32_t kDepthBinding = 0;
constexpr uint32_t kUniformsBinding = 1;
constexpr uint32_t kOcclusionBinding = 2;
constexpr uint32_t kSourceCount = 2;

constexpr vk::PipelineStageFlags2 kFragment = vk::PipelineStageFlagBits2::eFragmentShader;
constexpr vk::PipelineStageFlags2 kColorOutput = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
constexpr vk::AccessFlags2 kColorWrite = vk::AccessFlagBits2::eColorAttachmentWrite;
constexpr vk::AccessFlags2 kSampledRead = vk::AccessFlagBits2::eShaderSampledRead;

// A target this pass overwrites, after the last frame's reads of it; UNDEFINED discards.
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

// The blur, or the lit passes' fragments, sample what this pass wrote.
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

Kataglyphis::VulkanRendererInternals::SsaoStage::SsaoStage() = default;

void Kataglyphis::VulkanRendererInternals::SsaoStage::init(const std::shared_ptr<VulkanDevice> &in_device,
  VulkanSwapChain *swap_chain,
  vk::CommandPool commandPool)
{
    device = in_device;
    vulkanSwapChain = swap_chain;

    // The lit passes filter the half-resolution occlusion up; the edge clamps.
    const vk::SamplerCreateInfo sampler_info = buildSamplerCreateInfo(vk::Filter::eLinear,
      vk::SamplerAddressMode::eClampToEdge,
      0.0F,
      VK_FALSE,
      1.0F,
      vk::BorderColor::eFloatOpaqueWhite);
    sampler = vkCheck(device->getLogicalDevice().createSampler(sampler_info), "Failed to create the SSAO sampler!");

    createFrameResources(commandPool);
    createPipelines();
}

void Kataglyphis::VulkanRendererInternals::SsaoStage::createFrameResources(vk::CommandPool commandPool)
{
    extent = postHalfExtent(vulkanSwapChain->getSwapChainExtent());
    imageCount = vulkanSwapChain->getNumberSwapChainImages();

    const vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
                                      | vk::ImageUsageFlagBits::eTransferDst;
    raw = std::make_unique<Texture>();
    createColorAttachment(*raw, device, extent, kFormat, usage, "ssao raw");
    blurred = std::make_unique<Texture>();
    createColorAttachment(*blurred, device, extent, kFormat, usage, "ssao");

    // The lit passes bind the occlusion every frame, so it starts white (unoccluded) and sampleable.
    const std::array<vk::Image, 2> targets = { raw->getImage(), blurred->getImage() };
    clearToShaderRead(
      device, commandPool, targets, vk::ClearColorValue{ std::array<float, 4>{ 1.0F, 1.0F, 1.0F, 1.0F } });

    uniforms.clear();
    uniforms.resize(imageCount);
    for (VulkanBuffer &buffer : uniforms) {
        buffer.create(device,
          sizeof(SsaoUniforms),
          vk::BufferUsageFlagBits::eUniformBuffer,
          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    }

    descriptors.addBinding(kDepthBinding, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment)
      .addBinding(kUniformsBinding, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment)
      .addBinding(kOcclusionBinding, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment);
    if (!descriptors.create(device, kSourceCount * imageCount + kSourceCount)) {
        spdlog::error("Failed to create the SSAO descriptor resources!");
        return;
    }
    for (const SsaoDepthSource source : { SsaoDepthSource::ForwardPrepass, SsaoDepthSource::Deferred }) {
        // The occlusion pass reads no occlusion; the blurred target is sampleable while it runs.
        for (uint32_t i = 0; i < imageCount; ++i) {
            const uint32_t set = occlusionSet(source, i);
            descriptors.writeBuffer(set, kUniformsBinding, uniforms[i].getBuffer(), sizeof(SsaoUniforms));
            descriptors.writeImage(
              set, kOcclusionBinding, blurred->getImageView(), vk::ImageLayout::eShaderReadOnlyOptimal);
        }
        // The blur reads the raw occlusion and nothing else of the layout; image 0's uniforms stand in.
        descriptors.writeBuffer(blurSet(source), kUniformsBinding, uniforms[0].getBuffer(), sizeof(SsaoUniforms));
        descriptors.writeImage(
          blurSet(source), kOcclusionBinding, raw->getImageView(), vk::ImageLayout::eShaderReadOnlyOptimal);
    }
}

uint32_t Kataglyphis::VulkanRendererInternals::SsaoStage::occlusionSet(SsaoDepthSource source,
  uint32_t imageIndex) const
{ return static_cast<uint32_t>(source) * imageCount + imageIndex; }

uint32_t Kataglyphis::VulkanRendererInternals::SsaoStage::blurSet(SsaoDepthSource source) const
{ return kSourceCount * imageCount + static_cast<uint32_t>(source); }

void Kataglyphis::VulkanRendererInternals::SsaoStage::bindDepth(SsaoDepthSource source, vk::ImageView depthView)
{
    if (descriptors.sets().size() < kSourceCount * imageCount + kSourceCount) {
        spdlog::error("SsaoStage::bindDepth: the sets do not exist");
        return;
    }
    for (uint32_t i = 0; i < imageCount; ++i) {
        descriptors.writeImage(
          occlusionSet(source, i), kDepthBinding, depthView, vk::ImageLayout::eShaderReadOnlyOptimal);
    }
    descriptors.writeImage(blurSet(source), kDepthBinding, depthView, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void Kataglyphis::VulkanRendererInternals::SsaoStage::releaseFrameResources()
{
    descriptors.cleanUp();
    uniforms.clear();
    if (raw) { raw->cleanUp(); }
    if (blurred) { blurred->cleanUp(); }
    raw.reset();
    blurred.reset();
}

void Kataglyphis::VulkanRendererInternals::SsaoStage::recreateFrameResources(vk::CommandPool commandPool)
{
    releaseFrameResources();
    createFrameResources(commandPool);
}

void Kataglyphis::VulkanRendererInternals::SsaoStage::createPipelines()
{
    // Post's SV_VulkanVertexID triangle in front of OxidANT's fragment entries; the paths resolve from the repo root.
    const std::string spv_dir = "Resources/ShadersSlang/build/spirv/";
    const std::string vertex = spv_dir + "post/post.vs_main.spv";

    const std::array<vk::DescriptorSetLayout, 1> layouts = { descriptors.getLayout() };
    const vk::PipelineLayoutCreateInfo layout_info = buildPipelineLayoutCreateInfo(layouts);
    pipelineLayout = vkCheck(
      device->getLogicalDevice().createPipelineLayout(layout_info), "Failed to create the SSAO pipeline layout!");

    const auto build = [&](const char *entry, const char *name) {
        ShaderStagePair stages{ device, vertex, spv_dir + "ssao/ssao." + entry + ".spv" };
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
            "Failed to create an SSAO pipeline!");
    };
    occlusionPipeline = build("fs_ssao", "ssao");
    blurPipeline = build("fs_blur", "ssao blur");
}

void Kataglyphis::VulkanRendererInternals::SsaoStage::destroyPipelines()
{
    // The two share one layout, which goes with the last.
    vk::PipelineLayout shared_elsewhere{};
    const vk::Device logical = device->getLogicalDevice();
    Kataglyphis::destroyPipelineAndLayout(logical, occlusionPipeline, shared_elsewhere);
    Kataglyphis::destroyPipelineAndLayout(logical, blurPipeline, pipelineLayout);
}

void Kataglyphis::VulkanRendererInternals::SsaoStage::shaderHotReload()
{
    vk::PipelineLayout shared_elsewhere{};
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), occlusionPipeline, shared_elsewhere);
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), blurPipeline, pipelineLayout);
    createPipelines();
}

bool Kataglyphis::VulkanRendererInternals::SsaoStage::ready(uint32_t imageIndex) const
{
    return imageIndex < imageCount && descriptors.sets().size() >= kSourceCount * imageCount + kSourceCount && raw
           && blurred && static_cast<bool>(occlusionPipeline) && static_cast<bool>(blurPipeline);
}

void Kataglyphis::VulkanRendererInternals::SsaoStage::recordCommands(vk::CommandBuffer commandBuffer,
  uint32_t imageIndex,
  SsaoDepthSource source,
  const glm::mat4 &projection)
{
    if (!ready(imageIndex)) {
        spdlog::error("SsaoStage::recordCommands: no set or pipeline for image {}", imageIndex);
        return;
    }
    // OxidANT's radius, bias and intensity (render::forward::frame's write_uniforms); the image's last frame is done.
    const SsaoUniforms values{ projection, glm::inverse(projection), glm::vec4(0.6F, 0.02F, 1.0F, 0.0F) };
    std::memcpy(uniforms[imageIndex].getMappedData(), &values, sizeof(values));

    const auto draw = [&](vk::Pipeline pipeline, uint32_t set, Texture &target) {
        // eDontCare: the triangle writes every texel.
        const vk::RenderingAttachmentInfo attachment = buildRenderingAttachmentInfo(
          target.getImageView(), vk::ImageLayout::eColorAttachmentOptimal, vk::AttachmentLoadOp::eDontCare);
        commandBuffer.beginRendering(
          buildRenderingInfo(extent, std::span<const vk::RenderingAttachmentInfo>(&attachment, 1), nullptr));
        setFullExtentViewportAndScissor(commandBuffer, extent);
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        commandBuffer.bindDescriptorSets(
          vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, 1, &descriptors.sets()[set], 0, nullptr);
        commandBuffer.draw(3, 1, 0, 0);
        commandBuffer.endRendering();
    };

    recordImageBarrier(commandBuffer, toTarget(raw->getImage()));
    draw(occlusionPipeline, occlusionSet(source, imageIndex), *raw);

    // The blur overwrites the occlusion the last frame's lit passes sampled.
    const std::array<vk::ImageMemoryBarrier2, 2> to_blur = { toSampled(raw->getImage()),
        toTarget(blurred->getImage()) };
    recordImageBarriers(commandBuffer, to_blur);
    draw(blurPipeline, blurSet(source), *blurred);

    recordImageBarrier(commandBuffer, toSampled(blurred->getImage()));
}

void Kataglyphis::VulkanRendererInternals::SsaoStage::cleanUp()
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

Kataglyphis::VulkanRendererInternals::SsaoStage::~SsaoStage() { cleanUp(); }
