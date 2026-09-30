module;
#include <memory>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <sstream>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "renderer/pushConstants/PushConstantRasterizer.hpp"

#include "common/FormatHelper.hpp"
#include "common/FramebufferHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/RenderPassHelper.hpp"
#include "common/ViewportHelper.hpp"

#include "common/Utilities.hpp"

module kataglyphis.vulkan.rasterizer;

import kataglyphis.vulkan.vertex;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.image;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.shader_helper;
import kataglyphis.vulkan.pipeline_builder;
import kataglyphis.vulkan.mesh_draw_recorder;
import kataglyphis.vulkan.depth_attachment;
import kataglyphis.vulkan.color_attachment;

Kataglyphis::VulkanRendererInternals::Rasterizer::Rasterizer() = default;

void Kataglyphis::VulkanRendererInternals::Rasterizer::init(const std::shared_ptr<VulkanDevice> &in_device,
  VulkanSwapChain *swap_chain,
  std::span<const vk::DescriptorSetLayout> descriptorSetLayouts,
  vk::CommandPool &commandPool)
{
    this->device = in_device;
    this->vulkanSwapChain = swap_chain;

    createTextures(commandPool);
    createRenderPass();
    createPushConstantRange();
    createGraphicsPipeline(descriptorSetLayouts);
    createFramebuffer();
}

void Kataglyphis::VulkanRendererInternals::Rasterizer::shaderHotReload(
  std::span<const vk::DescriptorSetLayout> descriptor_set_layouts)
{
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), graphics_pipeline, pipeline_layout);
    createGraphicsPipeline(descriptor_set_layouts);
}

auto Kataglyphis::VulkanRendererInternals::Rasterizer::getOffscreenTexture(uint32_t index) -> Kataglyphis::Texture &
{
    return *offscreenTextures[index];
}

void Kataglyphis::VulkanRendererInternals::Rasterizer::setPushConstant(PushConstantRasterizer push_constant)
{
    this->pushConstant = push_constant;
}

void Kataglyphis::VulkanRendererInternals::Rasterizer::recordCommands(vk::CommandBuffer &commandBuffer,
  uint32_t image_index,
  Scene *scene,
  std::span<const vk::DescriptorSet> descriptorSets,
  const std::optional<FrustumPlanes> &cameraFrustum)
{
    const vk::Extent2D &swap_chain_extent = vulkanSwapChain->getSwapChainExtent();

    std::array<vk::ClearValue, 2> clear_values = {};
    clear_values[0].color = vk::ClearColorValue{ std::array<float, 4>{ 0.0F, 0.0F, 0.0F, 0.0F } };
    clear_values[1].depthStencil = vk::ClearDepthStencilValue{ 1.0F, 0 };

    const vk::RenderPassBeginInfo render_pass_begin_info = Kataglyphis::buildRenderPassBeginInfo(
      render_pass, framebuffer[image_index], swap_chain_extent, clear_values);

    commandBuffer.beginRenderPass(render_pass_begin_info, vk::SubpassContents::eInline);

    setFullExtentViewportAndScissor(commandBuffer, swap_chain_extent);

    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, graphics_pipeline);
    // The set is identical for every mesh: bind once, not per draw.
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0, descriptorSets, nullptr);

    const MeshDrawStats draw_stats = recordSceneMeshDraws(commandBuffer,
      pipeline_layout,
      vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
      scene,
      cameraFrustum,
      pushConstant);
    meshesDrawn = draw_stats.drawn;
    meshesConsidered = draw_stats.considered;

    commandBuffer.endRenderPass();
}

void Kataglyphis::VulkanRendererInternals::Rasterizer::cleanUp()
{
    // Idempotent: the destructor calls it again after an explicit cleanUp.
    if (!device) { return; }

    destroyFramebuffers();

    releaseFrameTextures();

    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), graphics_pipeline, pipeline_layout);
    Kataglyphis::destroyRenderPass(device->getLogicalDevice(), render_pass);

    device.reset();
}

Kataglyphis::VulkanRendererInternals::Rasterizer::~Rasterizer() { cleanUp(); }

void Kataglyphis::VulkanRendererInternals::Rasterizer::destroyFramebuffers()
{
    Kataglyphis::destroyFramebuffers(device->getLogicalDevice(), framebuffer);
}

void Kataglyphis::VulkanRendererInternals::Rasterizer::releaseFrameTextures()
{
    for (const auto &texture : offscreenTextures) {
        if (texture) { texture->cleanUp(); }
    }
    offscreenTextures.clear();

    if (depthBufferImage) { depthBufferImage->cleanUp(); }
    depthBufferImage.reset();
}

// Does not destroy the old framebuffers: recreateSwapChain() does that while their swapchain images still exist.
void Kataglyphis::VulkanRendererInternals::Rasterizer::recreateFrameResources(vk::CommandPool commandPool)
{
    releaseFrameTextures();

    createTextures(commandPool);
    createFramebuffer();
}

void Kataglyphis::VulkanRendererInternals::Rasterizer::createRenderPass()
{
    // The offscreen colour target is sampled by the post stage afterwards.
    const vk::AttachmentDescription color_attachment =
      buildAttachmentDescription(OFFSCREEN_FORMAT, vk::ImageLayout::eShaderReadOnlyOptimal);

    // Store eDontCare: depth is never read after the pass. Reuses createTextures()'s depth_format.
    const vk::AttachmentDescription depth_attachment =
      buildAttachmentDescription(depth_format,
        vk::ImageLayout::eDepthStencilAttachmentOptimal,
        vk::AttachmentLoadOp::eClear,
        vk::AttachmentStoreOp::eDontCare);

    vk::AttachmentReference color_attachment_reference;
    color_attachment_reference.attachment = 0;
    color_attachment_reference.layout = vk::ImageLayout::eColorAttachmentOptimal;

    vk::AttachmentReference depth_attachment_reference;
    depth_attachment_reference.attachment = 1;
    depth_attachment_reference.layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;

    const vk::SubpassDescription subpass = buildSubpassDescription(
      std::span<const vk::AttachmentReference>(&color_attachment_reference, 1), &depth_attachment_reference);

    // Without the fragment-test stages the depth clear races the layout transition (SYNC-HAZARD-WRITE-AFTER-WRITE).
    const std::array<vk::SubpassDependency, 1> subpass_dependencies = { buildExternalColorDepthDependency() };

    std::array<vk::AttachmentDescription, 2> render_pass_attachments = { color_attachment, depth_attachment };
    const std::array<vk::SubpassDescription, 1> subpasses = { subpass };

    vk::RenderPassCreateInfo const render_pass_create_info =
      Kataglyphis::buildRenderPassCreateInfo(render_pass_attachments, subpasses, subpass_dependencies);

    auto result = device->getLogicalDevice().createRenderPass(render_pass_create_info);
    if (result.result == vk::Result::eSuccess) {
        render_pass = result.value;
    } else {
        ASSERT_VULKAN(result.result, "Failed to create render pass!");
    }
}

void Kataglyphis::VulkanRendererInternals::Rasterizer::createFramebuffer()
{
    framebuffer.resize(vulkanSwapChain->getNumberSwapChainImages());

    for (size_t i = 0; i < framebuffer.size(); i++) {
        std::array<vk::ImageView, 2> attachments = { offscreenTextures[i]->getImageView(),
            depthBufferImage->getImageView() };

        const vk::FramebufferCreateInfo frame_buffer_create_info = Kataglyphis::buildFramebufferCreateInfo(
          render_pass, attachments, vulkanSwapChain->getSwapChainExtent());

        auto result = device->getLogicalDevice().createFramebuffer(frame_buffer_create_info);
        if (result.result == vk::Result::eSuccess) {
            framebuffer[i] = result.value;
        } else {
            ASSERT_VULKAN(result.result, "Failed to create framebuffer!");
        }
    }
}

void Kataglyphis::VulkanRendererInternals::Rasterizer::createPushConstantRange()
{
    // Fragment too: the frag shader indexes object_description with it.
    push_constant_range.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    push_constant_range.offset = 0;
    push_constant_range.size = sizeof(PushConstantRasterizer);
}

void Kataglyphis::VulkanRendererInternals::Rasterizer::createTextures(vk::CommandPool &commandPool)
{
    offscreenTextures.resize(vulkanSwapChain->getNumberSwapChainImages());

    for (uint32_t index = 0; index < vulkanSwapChain->getNumberSwapChainImages(); index++) {
        auto texture = std::make_unique<Texture>();
        const vk::Extent2D &swap_chain_extent = vulkanSwapChain->getSwapChainExtent();

        createColorAttachment(*texture,
          device,
          swap_chain_extent,
          OFFSCREEN_FORMAT,
          vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage
            | vk::ImageUsageFlagBits::eTransferDst);

        offscreenTextures[index] = std::move(texture);
    }

    const vk::Extent2D &swap_chain_extent = vulkanSwapChain->getSwapChainExtent();
    depthBufferImage = std::make_unique<Texture>();

    vk::ImageAspectFlags depth_aspect_flags =
      depthStencilTransitionAspect(chooseDepthFormat(device->getPhysicalDevice()));

    depth_format = createDepthAttachment(*depthBufferImage, device, swap_chain_extent, {}, depth_aspect_flags);

    // This overload submits and waits on its own command buffer.
    VulkanImage &vulkanImage = depthBufferImage->getVulkanImage();
    vulkanImage.transitionImageLayout(device->getLogicalDevice(),
      device->getGraphicsQueue(),
      commandPool,
      vk::ImageLayout::eUndefined,
      vk::ImageLayout::eDepthStencilAttachmentOptimal,
      depth_aspect_flags,
      1);
}

void Kataglyphis::VulkanRendererInternals::Rasterizer::createGraphicsPipeline(
  std::span<const vk::DescriptorSetLayout> descriptorSetLayouts)
{
    // Relative path: the engine runs from the repo root.
    std::string const slang_spv_dir = "Resources/ShadersSlang/build/spirv/rasterizer/";

    ShaderStagePair stages{ device, slang_spv_dir + "rasterizer.vs_main.spv", slang_spv_dir + "rasterizer.fs_main.spv" };

    vk::VertexInputBindingDescription binding_description;
    binding_description.binding = 0;
    binding_description.stride = sizeof(Vertex);
    binding_description.inputRate = vk::VertexInputRate::eVertex;

    std::array<vk::VertexInputAttributeDescription, 5> attribute_describtions = vertex::getVertexInputAttributeDesc();

    const std::array<vk::PushConstantRange, 1> push_constant_ranges = { push_constant_range };
    vk::PipelineLayoutCreateInfo pipeline_layout_create_info =
      buildPipelineLayoutCreateInfo(descriptorSetLayouts, push_constant_ranges);

    auto layout_result = device->getLogicalDevice().createPipelineLayout(pipeline_layout_create_info);
    ASSERT_VULKAN(layout_result.result, "Failed to create pipeline layout!");
    pipeline_layout = layout_result.value;

    PipelineBuilder pipeline_builder;
    graphics_pipeline =
      pipeline_builder.setShaderStages({ stages.stages().begin(), stages.stages().end() })
        .setVertexInput({ binding_description }, { attribute_describtions.begin(), attribute_describtions.end() })
        .setAlphaBlending(true)
        // Dynamic, so doubleSided glTF meshes can skip back-face culling per draw.
        .setDynamicCullMode(true)
        .build(device->getLogicalDevice(), pipeline_layout, render_pass, device->getPipelineCache());
}