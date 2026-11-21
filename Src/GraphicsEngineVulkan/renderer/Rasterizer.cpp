module;
#include <memory>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <glm/glm.hpp>
#include <span>
#include <sstream>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "renderer/pushConstants/PushConstantRasterizer.hpp"

#include "common/FormatHelper.hpp"
#include "common/ImageBarrierHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/RenderingHelper.hpp"
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
    createPushConstantRange();
    createGraphicsPipeline(descriptorSetLayouts);
}

auto Kataglyphis::VulkanRendererInternals::buildTransparentScenePipeline(const std::shared_ptr<VulkanDevice> &device,
  vk::PipelineLayout layout,
  vk::Format color_format,
  vk::Format depth_format) -> vk::Pipeline
{
    // Relative path: the engine runs from the repo root.
    std::string const slang_spv_dir = "Resources/ShadersSlang/build/spirv/rasterizer/";
    ShaderStagePair stages{
        device, slang_spv_dir + "rasterizer.vs_main.spv", slang_spv_dir + "rasterizer.fs_main.spv"
    };

    vk::VertexInputBindingDescription binding_description;
    binding_description.binding = 0;
    binding_description.stride = sizeof(Vertex);
    binding_description.inputRate = vk::VertexInputRate::eVertex;
    const auto attribute_descriptions = vertex::getVertexInputAttributeDesc();

    PipelineBuilder pipeline_builder;
    return pipeline_builder.setShaderStages({ stages.stages().begin(), stages.stages().end() })
      .setVertexInput({ binding_description }, { attribute_descriptions.begin(), attribute_descriptions.end() })
      .setAlphaBlending(true)
      // Over the opaque pixels and the sky alike the result stays opaque.
      .setCoverageAccumulation(true)
      // Tested against the opaque depth, never written: a nearer translucent layer must not hide a farther one.
      .setDepthWrite(false)
      .setDynamicCulling(true)
      .setRenderingFormats({ color_format }, depth_format)
      .build(device->getLogicalDevice(), layout, device->getPipelineCache());
}

void Kataglyphis::VulkanRendererInternals::Rasterizer::shaderHotReload(
  std::span<const vk::DescriptorSetLayout> descriptor_set_layouts)
{
    destroyTransparentPipeline();
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), graphics_pipeline, pipeline_layout);
    createGraphicsPipeline(descriptor_set_layouts);
}

auto Kataglyphis::VulkanRendererInternals::Rasterizer::getOffscreenTexture(uint32_t index) -> Kataglyphis::Texture &
{ return *offscreenTextures[index]; }

void Kataglyphis::VulkanRendererInternals::Rasterizer::setPushConstant(PushConstantRasterizer push_constant)
{ this->pushConstant = push_constant; }

void Kataglyphis::VulkanRendererInternals::Rasterizer::recordCommands(vk::CommandBuffer &commandBuffer,
  uint32_t image_index,
  Scene *scene,
  std::span<const vk::DescriptorSet> descriptorSets,
  const glm::vec3 &eye,
  const std::optional<FrustumPlanes> &cameraFrustum,
  const std::function<void(vk::CommandBuffer)> &drawSky)
{
    // Every draw reads set 0, as Clouds::recordComputeCommands' dispatch does.
    if (descriptorSets.empty()) {
        spdlog::error("Rasterizer::recordCommands called with an empty shared descriptor set span");
        return;
    }

    const vk::Extent2D &swap_chain_extent = vulkanSwapChain->getSwapChainExtent();
    const vk::Image offscreen_image = offscreenTextures[image_index]->getImage();
    const vk::Image depth_image = depthBufferImage->getImage();
    const vk::ImageAspectFlags depth_aspect = depthStencilTransitionAspect(depth_format);

    // UNDEFINED: both cleared; the shared depth waits out the clouds' read, the offscreen image post and the tracers.
    const std::array<vk::ImageMemoryBarrier2, 2> to_attachment = {
        buildImageMemoryBarrier(offscreen_image,
          vk::ImageLayout::eUndefined,
          vk::ImageLayout::eColorAttachmentOptimal,
          vk::PipelineStageFlagBits2::eColorAttachmentOutput | vk::PipelineStageFlagBits2::eFragmentShader
            | vk::PipelineStageFlagBits2::eComputeShader,
          vk::AccessFlagBits2::eColorAttachmentWrite,
          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
          vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite),
        buildImageMemoryBarrier(depth_image,
          vk::ImageLayout::eUndefined,
          vk::ImageLayout::eDepthStencilAttachmentOptimal,
          vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests
            | vk::PipelineStageFlagBits2::eComputeShader,
          vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
          vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
          vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
          depth_aspect),
    };
    recordImageBarriers(commandBuffer, to_attachment);

    const vk::RenderingAttachmentInfo color_attachment =
      buildRenderingAttachmentInfo(offscreenTextures[image_index]->getImageView(),
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::AttachmentLoadOp::eClear,
        vk::AttachmentStoreOp::eStore,
        vk::ClearValue{ vk::ClearColorValue{ std::array<float, 4>{ 0.0F, 0.0F, 0.0F, 0.0F } } });
    // Stored: the clouds stop their march at it.
    const vk::RenderingAttachmentInfo depth_attachment = buildRenderingAttachmentInfo(depthBufferImage->getImageView(),
      vk::ImageLayout::eDepthStencilAttachmentOptimal,
      vk::AttachmentLoadOp::eClear,
      vk::AttachmentStoreOp::eStore,
      vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0F, 0 } });
    const vk::RenderingInfo rendering_info = buildRenderingInfo(
      swap_chain_extent, std::span<const vk::RenderingAttachmentInfo>(&color_attachment, 1), &depth_attachment);

    commandBuffer.beginRendering(rendering_info);

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

    // After the opaque draws, so it fills only what they left at depth 1; before BLEND, which shows it through.
    if (drawSky) { drawSky(commandBuffer); }

    // glTF BLEND last, farthest first, over the opaque colour and sky and against their depth.
    const std::vector<BlendDraw> blend_draws = collectBlendDraws(scene, cameraFrustum, eye);
    if (!blend_draws.empty()) {
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, transparent_pipeline);
        // The sky's own set 0 replaced the shared one.
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0, descriptorSets, nullptr);
        meshesDrawn += recordBlendDraws(commandBuffer,
          pipeline_layout,
          vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
          scene,
          blend_draws,
          pushConstant);
    }

    commandBuffer.endRendering();

    // Both leave for sampling: post reads the colour, the clouds' compute pass the depth.
    const std::array<vk::ImageMemoryBarrier2, 2> to_sampled = {
        buildImageMemoryBarrier(offscreen_image,
          vk::ImageLayout::eColorAttachmentOptimal,
          vk::ImageLayout::eShaderReadOnlyOptimal,
          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
          vk::AccessFlagBits2::eColorAttachmentWrite,
          vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
          vk::AccessFlagBits2::eShaderSampledRead),
        buildImageMemoryBarrier(depth_image,
          vk::ImageLayout::eDepthStencilAttachmentOptimal,
          vk::ImageLayout::eShaderReadOnlyOptimal,
          vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
          vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
          vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
          vk::AccessFlagBits2::eShaderSampledRead,
          depth_aspect),
    };
    recordImageBarriers(commandBuffer, to_sampled);
}

void Kataglyphis::VulkanRendererInternals::Rasterizer::cleanUp()
{
    // Idempotent: the destructor calls it again after an explicit cleanUp.
    if (!device) { return; }

    releaseFrameTextures();

    destroyTransparentPipeline();
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), graphics_pipeline, pipeline_layout);

    device.reset();
}

void Kataglyphis::VulkanRendererInternals::Rasterizer::destroyTransparentPipeline()
{
    if (transparent_pipeline) {
        device->getLogicalDevice().destroyPipeline(transparent_pipeline);
        transparent_pipeline = nullptr;
    }
}

Kataglyphis::VulkanRendererInternals::Rasterizer::~Rasterizer() { cleanUp(); }

void Kataglyphis::VulkanRendererInternals::Rasterizer::releaseFrameTextures()
{
    for (const auto &texture : offscreenTextures) {
        if (texture) { texture->cleanUp(); }
    }
    offscreenTextures.clear();

    if (depthBufferImage) { depthBufferImage->cleanUp(); }
    depthBufferImage.reset();
}

void Kataglyphis::VulkanRendererInternals::Rasterizer::recreateFrameResources(vk::CommandPool commandPool)
{
    releaseFrameTextures();

    createTextures(commandPool);
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
            | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc);

        offscreenTextures[index] = std::move(texture);
    }

    const vk::Extent2D &swap_chain_extent = vulkanSwapChain->getSwapChainExtent();
    depthBufferImage = std::make_unique<Texture>();

    vk::ImageAspectFlags depth_aspect_flags =
      depthStencilTransitionAspect(chooseDepthFormat(device->getPhysicalDevice()));

    // Sampled view: exactly one aspect, not depth_aspect_flags. See depthStencilTransitionAspect's doc comment.
    depth_format = createDepthAttachment(
      *depthBufferImage, device, swap_chain_extent, vk::ImageUsageFlagBits::eSampled, vk::ImageAspectFlagBits::eDepth);

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

    ShaderStagePair stages{
        device, slang_spv_dir + "rasterizer.vs_main.spv", slang_spv_dir + "rasterizer.fs_main.spv"
    };

    vk::VertexInputBindingDescription binding_description;
    binding_description.binding = 0;
    binding_description.stride = sizeof(Vertex);
    binding_description.inputRate = vk::VertexInputRate::eVertex;

    const auto attribute_describtions = vertex::getVertexInputAttributeDesc();

    const std::array<vk::PushConstantRange, 1> push_constant_ranges = { push_constant_range };
    vk::PipelineLayoutCreateInfo pipeline_layout_create_info =
      buildPipelineLayoutCreateInfo(descriptorSetLayouts, push_constant_ranges);

    pipeline_layout = vkCheck(device->getLogicalDevice().createPipelineLayout(pipeline_layout_create_info),
      "Failed to create pipeline layout!");

    PipelineBuilder pipeline_builder;
    graphics_pipeline =
      pipeline_builder.setShaderStages({ stages.stages().begin(), stages.stages().end() })
        .setVertexInput({ binding_description }, { attribute_describtions.begin(), attribute_describtions.end() })
        .setAlphaBlending(true)
        // Dynamic, so doubleSided glTF meshes skip back-face culling and mirrored models flip their winding per draw.
        .setDynamicCulling(true)
        .setRenderingFormats({ OFFSCREEN_FORMAT }, depth_format)
        .build(device->getLogicalDevice(), pipeline_layout, device->getPipelineCache());

    transparent_pipeline = buildTransparentScenePipeline(device, pipeline_layout, OFFSCREEN_FORMAT, depth_format);
}