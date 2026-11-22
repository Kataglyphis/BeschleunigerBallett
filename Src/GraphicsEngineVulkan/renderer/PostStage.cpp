module;
#include <memory>

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "common/FormatHelper.hpp"
#include "common/FrameSubmitHelper.hpp"
#include "common/ImageBarrierHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/RenderingHelper.hpp"
#include "common/ViewportHelper.hpp"
#include "renderer/pushConstants/PushConstantPost.hpp"

#include "common/Utilities.hpp"
#include "vulkan_base/PhysicalDeviceChoices.hpp"
#include <imgui.h>
#include <imgui_impl_vulkan.h>

module kataglyphis.vulkan.post_stage;

import kataglyphis.vulkan.debug;
import kataglyphis.vulkan.vertex;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.swapchain;
import kataglyphis.vulkan.shader_helper;
import kataglyphis.vulkan.pipeline_builder;
import kataglyphis.vulkan.sampler_builder;

Kataglyphis::VulkanRendererInternals::PostStage::PostStage() = default;

void Kataglyphis::VulkanRendererInternals::PostStage::init(const std::shared_ptr<VulkanDevice> &in_device,
  VulkanSwapChain *swapchain,
  std::span<const vk::DescriptorSetLayout> descriptorSetLayouts)
{
    this->device = in_device;
    this->vulkanSwapChain = swapchain;

    createOffscreenTextureSampler();

    createPushConstantRange();
    createGraphicsPipeline(descriptorSetLayouts);
}

void Kataglyphis::VulkanRendererInternals::PostStage::shaderHotReload(
  std::span<const vk::DescriptorSetLayout> descriptor_set_layouts)
{
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), graphics_pipeline, pipeline_layout);
    createGraphicsPipeline(descriptor_set_layouts);
}

void Kataglyphis::VulkanRendererInternals::PostStage::recordCommands(vk::CommandBuffer &commandBuffer,
  uint32_t image_index,
  std::span<const vk::DescriptorSet> descriptorSets,
  bool cloudsEnabled,
  Kataglyphis::Texture *fxaaInput)
{
    const vk::Extent2D &swap_chain_extent = vulkanSwapChain->getSwapChainExtent();
    Texture &target = fxaaInput != nullptr ? *fxaaInput : vulkanSwapChain->getSwapChainImage(image_index);

    // UNDEFINED: post overwrites every pixel; the swapchain waits on the acquire, the intermediate on FXAA's read.
    Kataglyphis::recordImageBarrier(commandBuffer,
      Kataglyphis::buildImageMemoryBarrier(target.getImage(),
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eColorAttachmentOptimal,
        fxaaInput != nullptr ? vk::PipelineStageFlagBits2::eFragmentShader : Kataglyphis::kSwapchainAcquireWaitStage,
        vk::AccessFlagBits2::eNone,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite));

    // eDontCare, no clear value: the fullscreen triangle writes every pixel before the GUI draws over it.
    const vk::RenderingAttachmentInfo color_attachment =
      Kataglyphis::buildRenderingAttachmentInfo(target.getImageView(),
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::AttachmentLoadOp::eDontCare,
        vk::AttachmentStoreOp::eStore);
    commandBuffer.beginRendering(Kataglyphis::buildRenderingInfo(
      swap_chain_extent, std::span<const vk::RenderingAttachmentInfo>(&color_attachment, 1), nullptr));

    setFullExtentViewportAndScissor(commandBuffer, swap_chain_extent);

    PushConstantPost pc_post{};
    pc_post.clouds_enabled = cloudsEnabled ? 1u : 0u;
    commandBuffer.pushConstants(pipeline_layout,
      vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
      0,
      sizeof(PushConstantPost),
      &pc_post);
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, graphics_pipeline);
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0, descriptorSets, nullptr);
    commandBuffer.draw(3, 1, 0, 0);

    // The GUI goes over the last pass that writes the swapchain image, FXAA's while it filters.
    if (fxaaInput == nullptr) {
        Kataglyphis::debug::ScopedCmdLabel const gui_label(commandBuffer, "gui", { 0.90F, 0.70F, 0.20F, 1.0F });
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), static_cast<VkCommandBuffer>(commandBuffer));
    }

    commandBuffer.endRendering();

    if (fxaaInput != nullptr) {
        // The consumer is fxaa.slang's fragment shader.
        Kataglyphis::recordImageBarrier(commandBuffer,
          Kataglyphis::buildImageMemoryBarrier(target.getImage(),
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite,
            vk::PipelineStageFlagBits2::eFragmentShader,
            vk::AccessFlagBits2::eShaderSampledRead));
        return;
    }

    // Colour output as the destination, not none: a frame capture's barrier chains from it.
    Kataglyphis::recordImageBarrier(commandBuffer,
      Kataglyphis::buildImageMemoryBarrier(target.getImage(),
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::ePresentSrcKHR,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eNone));
}

void Kataglyphis::VulkanRendererInternals::PostStage::cleanUp()
{
    // Idempotent: the destructor calls it again after an explicit cleanUp.
    if (!device) { return; }

    if (offscreenTextureSampler) {
        device->getLogicalDevice().destroySampler(offscreenTextureSampler);
        offscreenTextureSampler = nullptr;
    }
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), graphics_pipeline, pipeline_layout);

    device.reset();
}

Kataglyphis::VulkanRendererInternals::PostStage::~PostStage() { cleanUp(); }

void Kataglyphis::VulkanRendererInternals::PostStage::createOffscreenTextureSampler()
{
    const bool aniso = device->supportsSamplerAnisotropy();

    vk::SamplerCreateInfo sampler_create_info = buildSamplerCreateInfo(vk::Filter::eLinear,
      vk::SamplerAddressMode::eRepeat,
      0.0F,
      aniso,
      resolveMaxAnisotropy(aniso, device->maxSamplerAnisotropy()),
      vk::BorderColor::eFloatOpaqueBlack);

    vk::Result const result =
      device->getLogicalDevice().createSampler(&sampler_create_info, nullptr, &offscreenTextureSampler);
    vkCheck(result, "Failed to create a texture sampler!");
}

void Kataglyphis::VulkanRendererInternals::PostStage::createPushConstantRange()
{
    push_constant_range.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    push_constant_range.offset = 0;
    push_constant_range.size = sizeof(PushConstantPost);
}

void Kataglyphis::VulkanRendererInternals::PostStage::createGraphicsPipeline(
  std::span<const vk::DescriptorSetLayout> descriptorSetLayouts)
{
    // Relative path: the engine runs from the repo root.
    std::string const slang_spv_dir = "Resources/ShadersSlang/build/spirv/post/";

    std::string const post_vert_spv = "post.vs_main.spv";
    std::string const post_frag_spv = "post.fs_main.spv";

    ShaderStagePair stages{ device, slang_spv_dir + post_vert_spv, slang_spv_dir + post_frag_spv };

    const std::array<vk::PushConstantRange, 1> push_constant_ranges = { push_constant_range };
    vk::PipelineLayoutCreateInfo pipeline_layout_create_info =
      buildPipelineLayoutCreateInfo(descriptorSetLayouts, push_constant_ranges);

    pipeline_layout = vkCheck(device->getLogicalDevice().createPipelineLayout(pipeline_layout_create_info),
      "Failed to create pipeline layout!");

    PipelineBuilder pipeline_builder;
    graphics_pipeline = pipeline_builder.setShaderStages({ stages.stages().begin(), stages.stages().end() })
                          .setCullMode(vk::CullModeFlagBits::eNone)
                          .setDepthTest(false)
                          .setDepthWrite(false)
                          .setRenderingFormats({ vulkanSwapChain->getSwapChainFormat() })
                          .setDebugName("post")
                          .build(device->getLogicalDevice(), pipeline_layout, device->getPipelineCache());
}
