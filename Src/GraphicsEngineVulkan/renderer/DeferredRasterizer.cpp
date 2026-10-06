module;
#include <memory>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "renderer/pushConstants/PushConstantRasterizer.hpp"
#include "common/FormatHelper.hpp"
#include "common/FramebufferHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/RenderPassHelper.hpp"
#include "common/ViewportHelper.hpp"
#include "common/Utilities.hpp"

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
    createRenderPass();
    createPushConstantRange();
    createPipelines(descriptorSetLayouts);
    createFramebuffer();
}

void DeferredRasterizer::shaderHotReload(std::span<const vk::DescriptorSetLayout> descriptor_set_layouts)
{
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), geometryPipeline, geometryPipelineLayout);
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), lightingPipeline, lightingPipelineLayout);
    createPipelines(descriptor_set_layouts);
}

Kataglyphis::Texture &DeferredRasterizer::getOffscreenTexture(uint32_t index)
{
    return *offscreenTextures[index];
}

void DeferredRasterizer::setPushConstant(PushConstantRasterizer push_constant)
{
    pushConstant = push_constant;
}

void DeferredRasterizer::createTextures()
{
    uint32_t count = vulkanSwapChain->getNumberSwapChainImages();
    offscreenTextures.resize(count);
    gBufferNormals.resize(count);
    gBufferAlbedos.resize(count);
    gBufferMaterials.resize(count);

    const vk::Extent2D &extent = vulkanSwapChain->getSwapChainExtent();
    auto createAttachment = [&](std::vector<std::unique_ptr<Texture>>& textures, vk::Format format, vk::ImageUsageFlags usage) {
        for (uint32_t i = 0; i < count; i++) {
            auto tex = std::make_unique<Texture>();
            createColorAttachment(*tex, device, extent, format, usage);
            textures[i] = std::move(tex);
        }
    };

    // Use specific formats for GBuffer
    createAttachment(offscreenTextures, FINAL_FORMAT, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst);
    // No position attachment: the lighting pass reconstructs position from depth, saving an rgba16f target.
    createAttachment(gBufferNormals, GBUFFER_NORMAL_FORMAT, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eInputAttachment);
    createAttachment(gBufferAlbedos, GBUFFER_ALBEDO_FORMAT, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eInputAttachment);
    createAttachment(gBufferMaterials, GBUFFER_MATERIAL_FORMAT, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eInputAttachment);

    // No transition here: the render pass performs the first one from eUndefined.
    depthBufferImage = std::make_unique<Texture>();
    // Input-attachment view: exactly one aspect, not Kataglyphis::depthStencilTransitionAspect. See its doc comment.
    depth_format = createDepthAttachment(
      *depthBufferImage, device, extent, vk::ImageUsageFlagBits::eInputAttachment, vk::ImageAspectFlagBits::eDepth);
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
    Kataglyphis::destroyPipelineAndLayout(logicalDevice, geometryPipeline, geometryPipelineLayout);
    Kataglyphis::destroyPipelineAndLayout(logicalDevice, lightingPipeline, lightingPipelineLayout);
    Kataglyphis::destroyRenderPass(logicalDevice, renderPass);

    destroyFramebuffers();

    releaseFrameTextures();

    device.reset();
}

DeferredRasterizer::~DeferredRasterizer() { cleanUp(); }

void Kataglyphis::VulkanRendererInternals::DeferredRasterizer::destroyFramebuffers()
{
    Kataglyphis::destroyFramebuffers(device->getLogicalDevice(), framebuffer);
}

void Kataglyphis::VulkanRendererInternals::DeferredRasterizer::releaseFrameTextures()
{
    for (auto& tex : offscreenTextures) { if (tex) tex->cleanUp(); }
    offscreenTextures.clear();
    for (auto& tex : gBufferNormals) { if (tex) tex->cleanUp(); }
    gBufferNormals.clear();
    for (auto& tex : gBufferAlbedos) { if (tex) tex->cleanUp(); }
    gBufferAlbedos.clear();
    for (auto& tex : gBufferMaterials) { if (tex) tex->cleanUp(); }
    gBufferMaterials.clear();
    if (depthBufferImage) { depthBufferImage->cleanUp(); }
    depthBufferImage.reset();
}

// Does not destroy the old framebuffers: recreateSwapChain() does that while their swapchain images still exist.
void Kataglyphis::VulkanRendererInternals::DeferredRasterizer::recreateFrameResources()
{
    releaseFrameTextures();

    createTextures();
    createFramebuffer();
}

void DeferredRasterizer::createRenderPass()
{
    // Reuses createTextures()'s depth_format so the attachment and its image cannot diverge.
    std::array<vk::AttachmentDescription, 5> attachments = {
        buildAttachmentDescription(FINAL_FORMAT, vk::ImageLayout::eShaderReadOnlyOptimal), // 0: Final Output
        buildAttachmentDescription(GBUFFER_NORMAL_FORMAT, vk::ImageLayout::eShaderReadOnlyOptimal), // 1: Normal
        buildAttachmentDescription(GBUFFER_ALBEDO_FORMAT, vk::ImageLayout::eShaderReadOnlyOptimal), // 2: Albedo
        buildAttachmentDescription(GBUFFER_MATERIAL_FORMAT, vk::ImageLayout::eShaderReadOnlyOptimal), // 3: Material
        buildAttachmentDescription(depth_format, vk::ImageLayout::eDepthStencilAttachmentOptimal) // 4: Depth
    };

    // Subpass 0: Geometry Pass
    std::array<vk::AttachmentReference, 3> geometryColorRefs = {
        vk::AttachmentReference{1, vk::ImageLayout::eColorAttachmentOptimal}, // Normal
        vk::AttachmentReference{2, vk::ImageLayout::eColorAttachmentOptimal}, // Albedo
        vk::AttachmentReference{3, vk::ImageLayout::eColorAttachmentOptimal}  // Material
    };
    vk::AttachmentReference geometryDepthRef{4, vk::ImageLayout::eDepthStencilAttachmentOptimal};

    const vk::SubpassDescription geometrySubpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>(geometryColorRefs), &geometryDepthRef);

    // Subpass 1: Lighting Pass
    vk::AttachmentReference lightingColorRef{0, vk::ImageLayout::eColorAttachmentOptimal};
    
    std::array<vk::AttachmentReference, 4> lightingInputRefs = {
        vk::AttachmentReference{1, vk::ImageLayout::eShaderReadOnlyOptimal}, // -> GBUFFER_NORMAL_BINDING
        vk::AttachmentReference{2, vk::ImageLayout::eShaderReadOnlyOptimal}, // -> GBUFFER_ALBEDO_BINDING
        vk::AttachmentReference{3, vk::ImageLayout::eShaderReadOnlyOptimal}, // -> GBUFFER_MATERIAL_BINDING
        vk::AttachmentReference{4, vk::ImageLayout::eShaderReadOnlyOptimal}  // -> GBUFFER_DEPTH_BINDING
    };

    const vk::SubpassDescription lightingSubpass = buildSubpassDescription(
      std::span<const vk::AttachmentReference>(&lightingColorRef, 1), nullptr,
      std::span<const vk::AttachmentReference>(lightingInputRefs));

    // Dependencies
    std::array<vk::SubpassDependency, 3> dependencies;

    // External -> Geometry Subpass
    dependencies[0] = buildExternalColorDepthDependency();

    // Geometry Subpass -> Lighting Subpass
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = 1;
    // Early tests too: depth's loadOp clear writes there, before this subpass boundary moves it to a read layout.
    dependencies[1].srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput
                                   | vk::PipelineStageFlagBits::eEarlyFragmentTests
                                   | vk::PipelineStageFlagBits::eLateFragmentTests;
    // Attachments 1-4 store in subpass 1, after the transition this orders: sync validation saw a WAW without it.
    dependencies[1].dstStageMask =
      vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput
      | vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
    dependencies[1].srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    dependencies[1].dstAccessMask = vk::AccessFlagBits::eInputAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite
                                    | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    dependencies[1].dependencyFlags = vk::DependencyFlagBits::eByRegion;

    // Lighting Subpass -> External
    dependencies[2].srcSubpass = 1;
    dependencies[2].dstSubpass = VK_SUBPASS_EXTERNAL;
    // Depth's store in subpass 1 must precede the final transition back to its attachment layout.
    dependencies[2].srcStageMask =
      vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eLateFragmentTests;
    dependencies[2].dstStageMask = vk::PipelineStageFlagBits::eBottomOfPipe;
    dependencies[2].srcAccessMask = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite
                                    | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    dependencies[2].dstAccessMask = vk::AccessFlagBits::eMemoryRead;
    dependencies[2].dependencyFlags = vk::DependencyFlagBits::eByRegion;

    std::array<vk::SubpassDescription, 2> subpasses = {geometrySubpass, lightingSubpass};

    vk::RenderPassCreateInfo const renderPassInfo =
      Kataglyphis::buildRenderPassCreateInfo(attachments, subpasses, dependencies);

    auto result = device->getLogicalDevice().createRenderPass(renderPassInfo);
    ASSERT_VULKAN(result.result, "Failed to create deferred render pass!");
    renderPass = result.value;
}

void DeferredRasterizer::createPipelines(std::span<const vk::DescriptorSetLayout> descriptorSetLayouts)
{
    // Relative path: the engine runs from the repo root.
    std::string const slang_spv_dir = "Resources/ShadersSlang/build/spirv/deferred/";

    ShaderStagePair geomStages{ device, slang_spv_dir + "deferred.geometry_vs_main.spv",
        slang_spv_dir + "deferred.geometry_fs_main.spv" };

    vk::VertexInputBindingDescription bindingDescription;
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = vk::VertexInputRate::eVertex;
    
    std::array<vk::VertexInputAttributeDescription, 5> attributeDescriptions = vertex::getVertexInputAttributeDesc();

    const std::array<vk::PushConstantRange, 1> push_constant_ranges = { push_constant_range };
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo =
      buildPipelineLayoutCreateInfo(descriptorSetLayouts, push_constant_ranges);

    vk::ResultValue<vk::PipelineLayout> geometry_pipeline_layout_result =
      device->getLogicalDevice().createPipelineLayout(pipelineLayoutInfo);
    ASSERT_VULKAN(geometry_pipeline_layout_result.result, "Failed to create deferred geometry pipeline layout!");
    geometryPipelineLayout = geometry_pipeline_layout_result.value;

    PipelineBuilder geometryPipelineBuilder;
    geometryPipeline =
      geometryPipelineBuilder.setShaderStages({ geomStages.stages().begin(), geomStages.stages().end() })
        .setVertexInput({ bindingDescription }, { attributeDescriptions.begin(), attributeDescriptions.end() })
        .setColorAttachmentCount(3)
        // Dynamic, so doubleSided glTF meshes skip back-face culling here too.
        .setDynamicCullMode(true)
        .build(device->getLogicalDevice(), geometryPipelineLayout, renderPass, device->getPipelineCache(), 0);

    // Lighting Pipeline (Slang-emitted SPIR-V)
    ShaderStagePair lightStages{ device, slang_spv_dir + "deferred.lighting_vs_main.spv",
        slang_spv_dir + "deferred.lighting_fs_main.spv" };

    vk::PipelineLayoutCreateInfo lightPipelineLayoutInfo = buildPipelineLayoutCreateInfo(descriptorSetLayouts);

    vk::ResultValue<vk::PipelineLayout> lighting_pipeline_layout_result =
      device->getLogicalDevice().createPipelineLayout(lightPipelineLayoutInfo);
    ASSERT_VULKAN(lighting_pipeline_layout_result.result, "Failed to create deferred lighting pipeline layout!");
    lightingPipelineLayout = lighting_pipeline_layout_result.value;

    PipelineBuilder lightingPipelineBuilder;
    lightingPipeline = lightingPipelineBuilder.setShaderStages({ lightStages.stages().begin(), lightStages.stages().end() })
                         // Fullscreen triangle from SV_VertexID: no vertex buffer.
                         .setVertexInput({}, {})
                         .setCullMode(vk::CullModeFlagBits::eNone)
                         .setDepthTest(false)
                         .setDepthWrite(false)
                         .build(device->getLogicalDevice(), lightingPipelineLayout, renderPass, device->getPipelineCache(), 1);
}

void DeferredRasterizer::createFramebuffer()
{
    std::array<vk::ImageView, 5> attachments;
    const vk::Extent2D &extent = vulkanSwapChain->getSwapChainExtent();

    framebuffer.resize(vulkanSwapChain->getNumberSwapChainImages());
    for (uint32_t i = 0; i < vulkanSwapChain->getNumberSwapChainImages(); i++) {
        attachments[0] = offscreenTextures[i]->getImageView();
        attachments[1] = gBufferNormals[i]->getImageView();
        attachments[2] = gBufferAlbedos[i]->getImageView();
        attachments[3] = gBufferMaterials[i]->getImageView();
        attachments[4] = depthBufferImage->getImageView();

        const vk::FramebufferCreateInfo framebufferInfo =
          Kataglyphis::buildFramebufferCreateInfo(renderPass, attachments, extent);

        auto result = device->getLogicalDevice().createFramebuffer(framebufferInfo);
        ASSERT_VULKAN(result.result, "Failed to create deferred framebuffer!");
        framebuffer[i] = result.value;
    }
}


void DeferredRasterizer::recordCommands(vk::CommandBuffer &commandBuffer, uint32_t image_index, Kataglyphis::Scene *scene, std::span<const vk::DescriptorSet> descriptorSets, const std::optional<FrustumPlanes> &cameraFrustum)
{
    const vk::Extent2D &swap_chain_extent = vulkanSwapChain->getSwapChainExtent();

    std::array<vk::ClearValue, 5> clearValues{};
    clearValues[0].color = vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}};
    clearValues[1].color = vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}};
    clearValues[2].color = vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}};
    clearValues[3].color = vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[4].depthStencil = vk::ClearDepthStencilValue{1.0f, 0};

    const vk::RenderPassBeginInfo renderPassInfo = Kataglyphis::buildRenderPassBeginInfo(
      renderPass, framebuffer[image_index], swap_chain_extent, clearValues);

    commandBuffer.beginRenderPass(renderPassInfo, vk::SubpassContents::eInline);

    setFullExtentViewportAndScissor(commandBuffer, swap_chain_extent);

    // Subpass 0: Geometry
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, geometryPipeline);

    // Set 0 is identical for every mesh: bind once, not per draw.
    commandBuffer.bindDescriptorSets(
      vk::PipelineBindPoint::eGraphics, geometryPipelineLayout, 0, 1, &descriptorSets[0], 0, nullptr);

    // objectIndex advances for every mesh, culled ones included, to stay the flat object-description index.
    const MeshDrawStats draw_stats = recordSceneMeshDraws(
      commandBuffer, geometryPipelineLayout, vk::ShaderStageFlagBits::eAll, scene, cameraFrustum, pushConstant);
    meshesDrawn = draw_stats.drawn;
    meshesConsidered = draw_stats.considered;

    // Transition to Subpass 1: Lighting
    commandBuffer.nextSubpass(vk::SubpassContents::eInline);

    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, lightingPipeline);
    
    // descriptorSets[1] holds the G-buffer inputs (set 1).
    if (descriptorSets.size() > 1) {
        commandBuffer.bindDescriptorSets(
          vk::PipelineBindPoint::eGraphics, lightingPipelineLayout, 1, 1, &descriptorSets[1], 0, nullptr);
    }
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics, lightingPipelineLayout, 0, 1, &descriptorSets[0], 0, nullptr);

    commandBuffer.draw(3, 1, 0, 0); // Fullscreen triangle

    commandBuffer.endRenderPass();
}
