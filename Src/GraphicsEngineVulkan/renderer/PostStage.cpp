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
#include "common/FramebufferHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/RenderPassHelper.hpp"
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
    createRenderpass();
    createGraphicsPipeline(descriptorSetLayouts);
    createFramebuffer();
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
  bool cloudsEnabled)
{
    const vk::Extent2D &swap_chain_extent = vulkanSwapChain->getSwapChainExtent();

    // No clear values: the colour attachment loads the skybox pass's output.
    const vk::RenderPassBeginInfo render_pass_begin_info = Kataglyphis::buildRenderPassBeginInfo(
      render_pass, framebuffers[image_index], swap_chain_extent, std::span<const vk::ClearValue>{});

    commandBuffer.beginRenderPass(render_pass_begin_info, vk::SubpassContents::eInline);

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

    {
        Kataglyphis::debug::ScopedCmdLabel const gui_label(commandBuffer, "gui", { 0.90F, 0.70F, 0.20F, 1.0F });
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), static_cast<VkCommandBuffer>(commandBuffer));
    }

    commandBuffer.endRenderPass();
}

void Kataglyphis::VulkanRendererInternals::PostStage::cleanUp()
{
    // Idempotent: the destructor calls it again after an explicit cleanUp.
    if (!device) { return; }

    destroyFramebuffers();

    if (offscreenTextureSampler) {
        device->getLogicalDevice().destroySampler(offscreenTextureSampler);
        offscreenTextureSampler = nullptr;
    }
    Kataglyphis::destroyRenderPass(device->getLogicalDevice(), render_pass);
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), graphics_pipeline, pipeline_layout);

    device.reset();
}

Kataglyphis::VulkanRendererInternals::PostStage::~PostStage() { cleanUp(); }

void Kataglyphis::VulkanRendererInternals::PostStage::destroyFramebuffers()
{
    Kataglyphis::destroyFramebuffers(device->getLogicalDevice(), framebuffers);
}

// Does not destroy the old framebuffers: recreateSwapChain() does that while their swapchain images still exist.
void Kataglyphis::VulkanRendererInternals::PostStage::recreateFrameResources()
{
    createFramebuffer();
}

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
    ASSERT_VULKAN(result, "Failed to create a texture sampler!");
}

void Kataglyphis::VulkanRendererInternals::PostStage::createPushConstantRange()
{
    push_constant_range.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    push_constant_range.offset = 0;
    push_constant_range.size = sizeof(PushConstantPost);
}

void Kataglyphis::VulkanRendererInternals::PostStage::createRenderpass()
{
    // Loads the skybox pass's output instead of clearing it, then hands it to presentation.
    const vk::AttachmentDescription color_attachment = buildAttachmentDescription(
      vulkanSwapChain->getSwapChainFormat(),
      vk::ImageLayout::ePresentSrcKHR,
      vk::AttachmentLoadOp::eLoad,
      vk::AttachmentStoreOp::eStore,
      vk::ImageLayout::eColorAttachmentOptimal);

    vk::AttachmentReference color_attachment_reference;
    color_attachment_reference.attachment = 0;
    color_attachment_reference.layout = vk::ImageLayout::eColorAttachmentOptimal;

    const vk::SubpassDescription subpass = buildSubpassDescription(
      std::span<const vk::AttachmentReference>(&color_attachment_reference, 1), nullptr);

    // Colour only: this pass has no depth to order.
    const std::array<vk::SubpassDependency, 1> subpass_dependencies = { buildExternalColorDependency() };

    std::array<vk::AttachmentDescription, 1> render_pass_attachments = { color_attachment };
    const std::array<vk::SubpassDescription, 1> subpasses = { subpass };

    vk::RenderPassCreateInfo const render_pass_create_info =
      Kataglyphis::buildRenderPassCreateInfo(render_pass_attachments, subpasses, subpass_dependencies);

    auto result = device->getLogicalDevice().createRenderPass(render_pass_create_info);
    ASSERT_VULKAN(result.result, "Failed to create render pass!");
    render_pass = result.value;
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

    vk::ResultValue<vk::PipelineLayout> pipeline_layout_result =
      device->getLogicalDevice().createPipelineLayout(pipeline_layout_create_info);
    ASSERT_VULKAN(pipeline_layout_result.result, "Failed to create pipeline layout!");
    pipeline_layout = pipeline_layout_result.value;

    PipelineBuilder pipeline_builder;
    graphics_pipeline = pipeline_builder.setShaderStages({ stages.stages().begin(), stages.stages().end() })
                          .setCullMode(vk::CullModeFlagBits::eNone)
                          .setAlphaBlending(true)
                          .setDepthTest(false)
                          .setDepthWrite(false)
                          .build(device->getLogicalDevice(), pipeline_layout, render_pass, device->getPipelineCache());
}

void Kataglyphis::VulkanRendererInternals::PostStage::createFramebuffer()
{
    framebuffers.resize(vulkanSwapChain->getNumberSwapChainImages());

    for (size_t i = 0; i < vulkanSwapChain->getNumberSwapChainImages(); i++) {
        Texture &swap_chain_image = vulkanSwapChain->getSwapChainImage(static_cast<uint32_t>(i));

        std::array<vk::ImageView, 1> attachments = { swap_chain_image.getImageView() };

        const vk::FramebufferCreateInfo frame_buffer_create_info = Kataglyphis::buildFramebufferCreateInfo(
          render_pass, attachments, vulkanSwapChain->getSwapChainExtent());

        auto result = device->getLogicalDevice().createFramebuffer(frame_buffer_create_info);
        ASSERT_VULKAN(result.result, "Failed to create framebuffer!");
        framebuffers[i] = result.value;
    }
}