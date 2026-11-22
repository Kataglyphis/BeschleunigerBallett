module;
#include <memory>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <glm/glm.hpp>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "common/FormatHelper.hpp"
#include "common/ImageBarrierHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/RenderingHelper.hpp"
#include "common/Utilities.hpp"
#include "common/ViewportHelper.hpp"
#include "renderer/pushConstants/PushConstantRasterizer.hpp"

module kataglyphis.vulkan.deferred_rasterizer;

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

using namespace Kataglyphis::VulkanRendererInternals;

DeferredRasterizer::DeferredRasterizer() = default;

void DeferredRasterizer::init(const std::shared_ptr<VulkanDevice> &in_device,
  VulkanSwapChain *swap_chain,
  std::span<const vk::DescriptorSetLayout> descriptorSetLayouts)
{
    device = in_device;
    vulkanSwapChain = swap_chain;

    createTextures();
    createPushConstantRange();
    createPipelines(descriptorSetLayouts);
}

void DeferredRasterizer::shaderHotReload(std::span<const vk::DescriptorSetLayout> descriptor_set_layouts)
{
    destroyTransparentPipeline();
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), geometryPipeline, geometryPipelineLayout);
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), lightingPipeline, lightingPipelineLayout);
    createPipelines(descriptor_set_layouts);
}

Kataglyphis::Texture &DeferredRasterizer::getOffscreenTexture(uint32_t index) { return *offscreenTextures[index]; }

void DeferredRasterizer::setPushConstant(PushConstantRasterizer push_constant) { pushConstant = push_constant; }

void DeferredRasterizer::createTextures()
{
    uint32_t count = vulkanSwapChain->getNumberSwapChainImages();
    offscreenTextures.resize(count);
    gBufferNormals.resize(count);
    gBufferAlbedos.resize(count);
    gBufferMaterials.resize(count);

    const vk::Extent2D &extent = vulkanSwapChain->getSwapChainExtent();
    auto createAttachment = [&](std::vector<std::unique_ptr<Texture>> &textures,
                              vk::Format format,
                              vk::ImageUsageFlags usage,
                              const char *name) {
        for (uint32_t i = 0; i < count; i++) {
            auto tex = std::make_unique<Texture>();
            createColorAttachment(*tex, device, extent, format, usage, name);
            textures[i] = std::move(tex);
        }
    };

    // Use specific formats for GBuffer
    createAttachment(offscreenTextures,
      FINAL_FORMAT,
      vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage
        | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc,
      "deferred hdr");
    // Sampled, no position target: the lighting pass texel-fetches these and rebuilds position from depth.
    createAttachment(gBufferNormals,
      GBUFFER_NORMAL_FORMAT,
      vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
      "g-buffer normal");
    createAttachment(gBufferAlbedos,
      GBUFFER_ALBEDO_FORMAT,
      vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
      "g-buffer albedo");
    createAttachment(gBufferMaterials,
      GBUFFER_MATERIAL_FORMAT,
      vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
      "g-buffer material");

    // No transition here: recordCommands() performs the first one from eUndefined every frame.
    depthBufferImage = std::make_unique<Texture>();
    // Sampled view: exactly one aspect, not depthStencilTransitionAspect. See its doc comment.
    depth_format = createDepthAttachment(*depthBufferImage,
      device,
      extent,
      vk::ImageUsageFlagBits::eSampled,
      vk::ImageAspectFlagBits::eDepth,
      "deferred depth");
}

void DeferredRasterizer::createPushConstantRange()
{
    push_constant_range.stageFlags = vk::ShaderStageFlagBits::eAll;
    push_constant_range.offset = 0;
    push_constant_range.size = sizeof(PushConstantRasterizer);
}


void DeferredRasterizer::cleanUp()
{
    // Idempotent: the destructor calls it again after an explicit cleanUp.
    if (!device) { return; }

    auto logicalDevice = device->getLogicalDevice();
    destroyTransparentPipeline();
    Kataglyphis::destroyPipelineAndLayout(logicalDevice, geometryPipeline, geometryPipelineLayout);
    Kataglyphis::destroyPipelineAndLayout(logicalDevice, lightingPipeline, lightingPipelineLayout);

    releaseFrameTextures();

    device.reset();
}

DeferredRasterizer::~DeferredRasterizer() { cleanUp(); }

void DeferredRasterizer::destroyTransparentPipeline()
{
    if (transparentPipeline) {
        device->getLogicalDevice().destroyPipeline(transparentPipeline);
        transparentPipeline = nullptr;
    }
}

void Kataglyphis::VulkanRendererInternals::DeferredRasterizer::releaseFrameTextures()
{
    for (auto &tex : offscreenTextures) {
        if (tex) tex->cleanUp();
    }
    offscreenTextures.clear();
    for (auto &tex : gBufferNormals) {
        if (tex) tex->cleanUp();
    }
    gBufferNormals.clear();
    for (auto &tex : gBufferAlbedos) {
        if (tex) tex->cleanUp();
    }
    gBufferAlbedos.clear();
    for (auto &tex : gBufferMaterials) {
        if (tex) tex->cleanUp();
    }
    gBufferMaterials.clear();
    if (depthBufferImage) { depthBufferImage->cleanUp(); }
    depthBufferImage.reset();
}

void Kataglyphis::VulkanRendererInternals::DeferredRasterizer::recreateFrameResources()
{
    releaseFrameTextures();

    createTextures();
}

void DeferredRasterizer::createPipelines(std::span<const vk::DescriptorSetLayout> descriptorSetLayouts)
{
    // Relative path: the engine runs from the repo root.
    std::string const slang_spv_dir = "Resources/ShadersSlang/build/spirv/deferred/";

    ShaderStagePair geomStages{
        device, slang_spv_dir + "deferred.geometry_vs_main.spv", slang_spv_dir + "deferred.geometry_fs_main.spv"
    };

    vk::VertexInputBindingDescription bindingDescription;
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = vk::VertexInputRate::eVertex;

    const auto attributeDescriptions = vertex::getVertexInputAttributeDesc();

    const std::array<vk::PushConstantRange, 1> push_constant_ranges = { push_constant_range };
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo =
      buildPipelineLayoutCreateInfo(descriptorSetLayouts, push_constant_ranges);

    geometryPipelineLayout = vkCheck(device->getLogicalDevice().createPipelineLayout(pipelineLayoutInfo),
      "Failed to create deferred geometry pipeline layout!");

    PipelineBuilder geometryPipelineBuilder;
    geometryPipeline =
      geometryPipelineBuilder.setShaderStages({ geomStages.stages().begin(), geomStages.stages().end() })
        .setVertexInput({ bindingDescription }, { attributeDescriptions.begin(), attributeDescriptions.end() })
        // Dynamic, so doubleSided meshes skip culling and mirrored models flip their winding here too.
        .setDynamicCulling(true)
        .setRenderingFormats({ GBUFFER_NORMAL_FORMAT, GBUFFER_ALBEDO_FORMAT, GBUFFER_MATERIAL_FORMAT }, depth_format)
        .setDebugName("g-buffer")
        .build(device->getLogicalDevice(), geometryPipelineLayout, device->getPipelineCache());

    // BLEND has no G-buffer encoding: it is shaded forward over the lit result, as the Rust renderer does.
    transparentPipeline = buildTransparentScenePipeline(device, geometryPipelineLayout, FINAL_FORMAT, depth_format);

    // Lighting Pipeline (Slang-emitted SPIR-V)
    ShaderStagePair lightStages{
        device, slang_spv_dir + "deferred.lighting_vs_main.spv", slang_spv_dir + "deferred.lighting_fs_main.spv"
    };

    vk::PipelineLayoutCreateInfo lightPipelineLayoutInfo = buildPipelineLayoutCreateInfo(descriptorSetLayouts);

    lightingPipelineLayout = vkCheck(device->getLogicalDevice().createPipelineLayout(lightPipelineLayoutInfo),
      "Failed to create deferred lighting pipeline layout!");

    PipelineBuilder lightingPipelineBuilder;
    lightingPipeline = lightingPipelineBuilder
                         .setShaderStages({ lightStages.stages().begin(), lightStages.stages().end() })
                         // Fullscreen triangle from SV_VertexID: no vertex buffer.
                         .setVertexInput({}, {})
                         .setCullMode(vk::CullModeFlagBits::eNone)
                         .setDepthTest(false)
                         .setDepthWrite(false)
                         // No depth attachment: the lighting pass samples the geometry pass's depth instead.
                         .setRenderingFormats({ FINAL_FORMAT })
                         .setDebugName("deferred lighting")
                         .build(device->getLogicalDevice(), lightingPipelineLayout, device->getPipelineCache());
}

unsigned int DeferredRasterizer::recordSkyAndTransparentPass(vk::CommandBuffer &commandBuffer,
  uint32_t image_index,
  Kataglyphis::Scene *scene,
  std::span<const vk::DescriptorSet> descriptorSets,
  const std::vector<BlendDraw> &blend_draws,
  const vk::Extent2D &extent,
  vk::ImageAspectFlags depth_aspect,
  const std::function<void(vk::CommandBuffer)> &drawSky)
{
    const vk::Image offscreen_image = offscreenTextures[image_index]->getImage();
    const vk::Image depth_image = depthBufferImage->getImage();

    // The sky and BLEND go onto the lit colour; the depth the lighting pass sampled goes back to an attachment for the test.
    const std::array<vk::ImageMemoryBarrier2, 2> to_blend = {
        buildImageMemoryBarrier(offscreen_image,
          vk::ImageLayout::eColorAttachmentOptimal,
          vk::ImageLayout::eColorAttachmentOptimal,
          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
          vk::AccessFlagBits2::eColorAttachmentWrite,
          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
          vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite),
        buildImageMemoryBarrier(depth_image,
          vk::ImageLayout::eShaderReadOnlyOptimal,
          vk::ImageLayout::eDepthStencilAttachmentOptimal,
          vk::PipelineStageFlagBits2::eFragmentShader,
          vk::AccessFlagBits2::eNone,
          vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
          vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
          depth_aspect),
    };
    recordImageBarriers(commandBuffer, to_blend);

    const vk::RenderingAttachmentInfo colour_attachment =
      buildRenderingAttachmentInfo(offscreenTextures[image_index]->getImageView(),
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::AttachmentLoadOp::eLoad,
        vk::AttachmentStoreOp::eStore,
        vk::ClearValue{});
    // Kept: the clouds stop their march at it.
    const vk::RenderingAttachmentInfo depth_attachment = buildRenderingAttachmentInfo(depthBufferImage->getImageView(),
      vk::ImageLayout::eDepthStencilAttachmentOptimal,
      vk::AttachmentLoadOp::eLoad,
      vk::AttachmentStoreOp::eStore,
      vk::ClearValue{});
    commandBuffer.beginRendering(buildRenderingInfo(
      extent, std::span<const vk::RenderingAttachmentInfo>(&colour_attachment, 1), &depth_attachment));

    setFullExtentViewportAndScissor(commandBuffer, extent);
    // Where the lighting pass discarded, depth is still 1: there the sky fills in, under BLEND.
    if (drawSky) { drawSky(commandBuffer); }
    unsigned int drawn = 0;
    if (!blend_draws.empty()) {
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, transparentPipeline);
        commandBuffer.bindDescriptorSets(
          vk::PipelineBindPoint::eGraphics, geometryPipelineLayout, 0, 1, &descriptorSets[0], 0, nullptr);
        drawn = recordBlendDraws(
          commandBuffer, geometryPipelineLayout, vk::ShaderStageFlagBits::eAll, scene, blend_draws, pushConstant);
    }

    commandBuffer.endRendering();

    // Sampleable again for the clouds' compute read, as the geometry pass left it.
    recordImageBarrier(commandBuffer,
      buildImageMemoryBarrier(depth_image,
        vk::ImageLayout::eDepthStencilAttachmentOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
        vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderSampledRead,
        depth_aspect));
    return drawn;
}

void DeferredRasterizer::recordCommands(vk::CommandBuffer &commandBuffer,
  uint32_t image_index,
  Kataglyphis::Scene *scene,
  std::span<const vk::DescriptorSet> descriptorSets,
  const glm::vec3 &eye,
  const std::optional<FrustumPlanes> &cameraFrustum,
  const std::function<void(vk::CommandBuffer)> &afterGeometryPass,
  const std::function<void(vk::CommandBuffer)> &drawSky)
{
    // Both passes bind descriptorSets[0], as Clouds::recordComputeCommands' dispatch does.
    if (descriptorSets.empty()) {
        spdlog::error("DeferredRasterizer::recordCommands called with an empty shared descriptor set span");
        return;
    }

    const vk::Extent2D &swap_chain_extent = vulkanSwapChain->getSwapChainExtent();
    const vk::Image offscreen_image = offscreenTextures[image_index]->getImage();
    const std::array<vk::Image, 3> gbuffer_images = { gBufferNormals[image_index]->getImage(),
        gBufferAlbedos[image_index]->getImage(),
        gBufferMaterials[image_index]->getImage() };
    const vk::Image depth_image = depthBufferImage->getImage();
    const vk::ImageAspectFlags depth_aspect = depthStencilTransitionAspect(depth_format);

    // UNDEFINED: all cleared; the shared depth waits out the clouds' read, the colour targets their last sampling.
    const auto colour_to_attachment = [](vk::Image image) {
        return buildImageMemoryBarrier(image,
          vk::ImageLayout::eUndefined,
          vk::ImageLayout::eColorAttachmentOptimal,
          vk::PipelineStageFlagBits2::eColorAttachmentOutput | vk::PipelineStageFlagBits2::eFragmentShader
            | vk::PipelineStageFlagBits2::eComputeShader,
          vk::AccessFlagBits2::eColorAttachmentWrite,
          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
          vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite);
    };
    const std::array<vk::ImageMemoryBarrier2, 5> to_attachment = { colour_to_attachment(gbuffer_images[0]),
        colour_to_attachment(gbuffer_images[1]),
        colour_to_attachment(gbuffer_images[2]),
        colour_to_attachment(offscreen_image),
        buildImageMemoryBarrier(depth_image,
          vk::ImageLayout::eUndefined,
          vk::ImageLayout::eDepthStencilAttachmentOptimal,
          vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests
            | vk::PipelineStageFlagBits2::eComputeShader,
          vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
          vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
          vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
          depth_aspect) };
    recordImageBarriers(commandBuffer, to_attachment);

    // Geometry pass: the three G-buffer targets and depth.
    const vk::ClearValue clear_zero{ vk::ClearColorValue{ std::array<float, 4>{ 0.0F, 0.0F, 0.0F, 0.0F } } };
    const std::array<vk::RenderingAttachmentInfo, 3> gbuffer_attachments = {
        buildRenderingAttachmentInfo(gBufferNormals[image_index]->getImageView(),
          vk::ImageLayout::eColorAttachmentOptimal,
          vk::AttachmentLoadOp::eClear,
          vk::AttachmentStoreOp::eStore,
          clear_zero),
        buildRenderingAttachmentInfo(gBufferAlbedos[image_index]->getImageView(),
          vk::ImageLayout::eColorAttachmentOptimal,
          vk::AttachmentLoadOp::eClear,
          vk::AttachmentStoreOp::eStore,
          clear_zero),
        buildRenderingAttachmentInfo(gBufferMaterials[image_index]->getImageView(),
          vk::ImageLayout::eColorAttachmentOptimal,
          vk::AttachmentLoadOp::eClear,
          vk::AttachmentStoreOp::eStore,
          vk::ClearValue{ vk::ClearColorValue{ std::array<float, 4>{ 0.0F, 0.0F, 0.0F, 1.0F } } }),
    };
    // Stored: the lighting pass reconstructs position from it and the clouds stop their march at it.
    const vk::RenderingAttachmentInfo depth_attachment = buildRenderingAttachmentInfo(depthBufferImage->getImageView(),
      vk::ImageLayout::eDepthStencilAttachmentOptimal,
      vk::AttachmentLoadOp::eClear,
      vk::AttachmentStoreOp::eStore,
      vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0F, 0 } });
    commandBuffer.beginRendering(buildRenderingInfo(swap_chain_extent, gbuffer_attachments, &depth_attachment));

    setFullExtentViewportAndScissor(commandBuffer, swap_chain_extent);

    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, geometryPipeline);

    // Set 0 is identical for every mesh: bind once, not per draw.
    commandBuffer.bindDescriptorSets(
      vk::PipelineBindPoint::eGraphics, geometryPipelineLayout, 0, 1, &descriptorSets[0], 0, nullptr);

    // objectIndex advances for every mesh, culled ones included, to stay the flat object-description index.
    const MeshDrawStats draw_stats = recordSceneMeshDraws(
      commandBuffer, geometryPipelineLayout, vk::ShaderStageFlagBits::eAll, scene, cameraFrustum, pushConstant);
    meshesDrawn = draw_stats.drawn;
    meshesConsidered = draw_stats.considered;

    commandBuffer.endRendering();
    if (afterGeometryPass) { afterGeometryPass(commandBuffer); }

    // The lighting fragment shader samples the G-buffer and depth; depth stays sampleable for the clouds' compute read.
    const auto colour_to_sampled = [](vk::Image image, vk::PipelineStageFlags2 consumers) {
        return buildImageMemoryBarrier(image,
          vk::ImageLayout::eColorAttachmentOptimal,
          vk::ImageLayout::eShaderReadOnlyOptimal,
          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
          vk::AccessFlagBits2::eColorAttachmentWrite,
          consumers,
          vk::AccessFlagBits2::eShaderSampledRead);
    };
    const std::array<vk::ImageMemoryBarrier2, 4> gbuffer_to_sampled = { colour_to_sampled(gbuffer_images[0],
                                                                          vk::PipelineStageFlagBits2::eFragmentShader),
        colour_to_sampled(gbuffer_images[1], vk::PipelineStageFlagBits2::eFragmentShader),
        colour_to_sampled(gbuffer_images[2], vk::PipelineStageFlagBits2::eFragmentShader),
        buildImageMemoryBarrier(depth_image,
          vk::ImageLayout::eDepthStencilAttachmentOptimal,
          vk::ImageLayout::eShaderReadOnlyOptimal,
          vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
          vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
          vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
          vk::AccessFlagBits2::eShaderSampledRead,
          depth_aspect) };
    recordImageBarriers(commandBuffer, gbuffer_to_sampled);

    // Lighting pass: one fullscreen triangle into the offscreen target.
    const vk::RenderingAttachmentInfo lighting_attachment =
      buildRenderingAttachmentInfo(offscreenTextures[image_index]->getImageView(),
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::AttachmentLoadOp::eClear,
        vk::AttachmentStoreOp::eStore,
        clear_zero);
    commandBuffer.beginRendering(buildRenderingInfo(
      swap_chain_extent, std::span<const vk::RenderingAttachmentInfo>(&lighting_attachment, 1), nullptr));

    setFullExtentViewportAndScissor(commandBuffer, swap_chain_extent);

    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, lightingPipeline);

    // descriptorSets[1] holds the G-buffer textures (set 1).
    if (descriptorSets.size() > 1) {
        commandBuffer.bindDescriptorSets(
          vk::PipelineBindPoint::eGraphics, lightingPipelineLayout, 1, 1, &descriptorSets[1], 0, nullptr);
    }
    commandBuffer.bindDescriptorSets(
      vk::PipelineBindPoint::eGraphics, lightingPipelineLayout, 0, 1, &descriptorSets[0], 0, nullptr);

    commandBuffer.draw(3, 1, 0, 0);// Fullscreen triangle

    commandBuffer.endRendering();

    const std::vector<BlendDraw> blend_draws = collectBlendDraws(scene, cameraFrustum, eye);
    meshesDrawn += recordSkyAndTransparentPass(
      commandBuffer, image_index, scene, descriptorSets, blend_draws, swap_chain_extent, depth_aspect, drawSky);

    // Post samples the lit result.
    recordImageBarrier(commandBuffer,
      colour_to_sampled(
        offscreen_image, vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader));
}
