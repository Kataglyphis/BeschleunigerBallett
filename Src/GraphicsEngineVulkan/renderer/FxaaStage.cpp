module;
#include <memory>

#include <vulkan/vulkan.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "common/FrameSubmitHelper.hpp"
#include "common/ImageBarrierHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/RenderingHelper.hpp"
#include "common/Utilities.hpp"
#include "common/ViewportHelper.hpp"
#include "renderer/FxaaUniforms.hpp"
#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <spdlog/spdlog.h>

module kataglyphis.vulkan.fxaa_stage;

import kataglyphis.vulkan.debug;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.swapchain;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.shader_helper;
import kataglyphis.vulkan.pipeline_builder;
import kataglyphis.vulkan.sampler_builder;
import kataglyphis.vulkan.color_attachment;

namespace {
// fxaa.slang's set 0: the LDR frame, its sampler and the texel size, as the WebGPU layout has them.
constexpr uint32_t kFxaaFrameBinding = 0;
constexpr uint32_t kFxaaSamplerBinding = 1;
constexpr uint32_t kFxaaUniformsBinding = 2;
}// namespace

Kataglyphis::VulkanRendererInternals::FxaaStage::FxaaStage() = default;

void Kataglyphis::VulkanRendererInternals::FxaaStage::init(const std::shared_ptr<VulkanDevice> &in_device,
  VulkanSwapChain *swap_chain)
{
    device = in_device;
    vulkanSwapChain = swap_chain;

    // Clamped: the edge walk steps past the border, which must read the border texel, not the far side.
    vk::SamplerCreateInfo sampler_info = buildSamplerCreateInfo(vk::Filter::eLinear,
      vk::SamplerAddressMode::eClampToEdge,
      0.0F,
      VK_FALSE,
      1.0F,
      vk::BorderColor::eFloatOpaqueBlack);
    sampler = vkCheck(device->getLogicalDevice().createSampler(sampler_info), "Failed to create the FXAA sampler!");
    uniformBuffer.create(device,
      sizeof(FxaaUniforms),
      vk::BufferUsageFlagBits::eUniformBuffer,
      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

    createFrameResources();
    createPipeline();
}

void Kataglyphis::VulkanRendererInternals::FxaaStage::createFrameResources()
{
    const vk::Extent2D extent = vulkanSwapChain->getSwapChainExtent();

    // One for every frame in flight: post's barrier into it waits out the last frame's FXAA read.
    intermediate = std::make_unique<Texture>();
    createColorAttachment(*intermediate,
      device,
      extent,
      vulkanSwapChain->getSwapChainFormat(),
      vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
      "fxaa intermediate");

    // The intermediate holds post's sRGB-encoded bytes, so the shader's luma needs no square root (params.z 0).
    const FxaaUniforms uniforms{ glm::vec4(1.0F / static_cast<float>(std::max(extent.width, 1U)),
      1.0F / static_cast<float>(std::max(extent.height, 1U)),
      0.0F,
      0.0F) };
    std::memcpy(uniformBuffer.getMappedData(), &uniforms, sizeof(FxaaUniforms));

    descriptors.addBinding(kFxaaFrameBinding, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment)
      .addBinding(kFxaaSamplerBinding, vk::DescriptorType::eSampler, 1, vk::ShaderStageFlagBits::eFragment)
      .addBinding(kFxaaUniformsBinding, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment);
    if (!descriptors.create(device, 1)) {
        spdlog::error("Failed to create FXAA descriptor resources!");
        return;
    }
    descriptors.writeImage(0, kFxaaFrameBinding, intermediate->getImageView(), vk::ImageLayout::eShaderReadOnlyOptimal);
    descriptors.writeImage(0, kFxaaSamplerBinding, nullptr, vk::ImageLayout::eUndefined, sampler);
    descriptors.writeBuffer(0, kFxaaUniformsBinding, uniformBuffer.getBuffer(), sizeof(FxaaUniforms));
}

void Kataglyphis::VulkanRendererInternals::FxaaStage::releaseFrameResources()
{
    descriptors.cleanUp();
    if (intermediate) { intermediate->cleanUp(); }
    intermediate.reset();
}

void Kataglyphis::VulkanRendererInternals::FxaaStage::recreateFrameResources()
{
    releaseFrameResources();
    createFrameResources();
}

void Kataglyphis::VulkanRendererInternals::FxaaStage::createPipeline()
{
    // Post's SV_VulkanVertexID triangle in front of OxidANT's fragment entry; the paths resolve from the repo root.
    std::string const slang_spv_dir = "Resources/ShadersSlang/build/spirv/";
    ShaderStagePair stages{ device, slang_spv_dir + "post/post.vs_main.spv", slang_spv_dir + "fxaa/fxaa.fs_main.spv" };

    const std::array<vk::DescriptorSetLayout, 1> layouts = { descriptors.getLayout() };
    vk::PipelineLayoutCreateInfo layout_info = buildPipelineLayoutCreateInfo(layouts);
    pipelineLayout = vkCheck(
      device->getLogicalDevice().createPipelineLayout(layout_info), "Failed to create the FXAA pipeline layout!");

    pipeline = PipelineBuilder()
                 .setShaderStages({ stages.stages().begin(), stages.stages().end() })
                 .setVertexInput({}, {})
                 .setCullMode(vk::CullModeFlagBits::eNone)
                 .setDepthTest(false)
                 .setDepthWrite(false)
                 .setRenderingFormats({ vulkanSwapChain->getSwapChainFormat() })
                 .setDebugName("fxaa")
                 .build(device->getLogicalDevice(),
                   pipelineLayout,
                   device->getPipelineCache(),
                   "Failed to create the FXAA pipeline!");
}

void Kataglyphis::VulkanRendererInternals::FxaaStage::shaderHotReload()
{
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), pipeline, pipelineLayout);
    createPipeline();
}

void Kataglyphis::VulkanRendererInternals::FxaaStage::recordCommands(vk::CommandBuffer commandBuffer,
  uint32_t imageIndex)
{
    if (!ready()) {
        spdlog::error("FxaaStage::recordCommands: no set or pipeline for image {}", imageIndex);
        return;
    }
    const vk::Extent2D &extent = vulkanSwapChain->getSwapChainExtent();
    Texture &swap_chain_image = vulkanSwapChain->getSwapChainImage(imageIndex);

    // UNDEFINED: the triangle writes every pixel. The source stage is the submit's wait stage on the acquire semaphore.
    recordImageBarrier(commandBuffer,
      buildImageMemoryBarrier(swap_chain_image.getImage(),
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eColorAttachmentOptimal,
        kSwapchainAcquireWaitStage,
        vk::AccessFlagBits2::eNone,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite));

    const vk::RenderingAttachmentInfo color_attachment = buildRenderingAttachmentInfo(swap_chain_image.getImageView(),
      vk::ImageLayout::eColorAttachmentOptimal,
      vk::AttachmentLoadOp::eDontCare,
      vk::AttachmentStoreOp::eStore);
    commandBuffer.beginRendering(
      buildRenderingInfo(extent, std::span<const vk::RenderingAttachmentInfo>(&color_attachment, 1), nullptr));
    setFullExtentViewportAndScissor(commandBuffer, extent);
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    commandBuffer.bindDescriptorSets(
      vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, 1, &descriptors.sets()[0], 0, nullptr);
    commandBuffer.draw(3, 1, 0, 0);

    {
        Kataglyphis::debug::ScopedCmdLabel const gui_label(commandBuffer, "gui", { 0.90F, 0.70F, 0.20F, 1.0F });
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), static_cast<VkCommandBuffer>(commandBuffer));
    }

    commandBuffer.endRendering();

    // Colour output as the destination, not none: a frame capture's barrier chains from it.
    recordImageBarrier(commandBuffer,
      buildImageMemoryBarrier(swap_chain_image.getImage(),
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::ePresentSrcKHR,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eNone));
}

void Kataglyphis::VulkanRendererInternals::FxaaStage::cleanUp()
{
    // Idempotent: the destructor calls it again after an explicit cleanUp.
    if (!device) { return; }

    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), pipeline, pipelineLayout);
    releaseFrameResources();
    uniformBuffer.cleanUp();
    if (sampler) {
        device->getLogicalDevice().destroySampler(sampler);
        sampler = nullptr;
    }

    device.reset();
}

Kataglyphis::VulkanRendererInternals::FxaaStage::~FxaaStage() { cleanUp(); }
