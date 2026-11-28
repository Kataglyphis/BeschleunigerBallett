module;
#include "common/FormatHelper.hpp"
#include "common/ImageBarrierHelper.hpp"
#include "common/ImageViewHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/RenderingHelper.hpp"
#include "common/Utilities.hpp"
#include "common/ViewportHelper.hpp"
#include "vulkan_base/PhysicalDeviceChoices.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.punctual_shadow_map;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.shader_helper;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.mesh;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.pipeline_builder;
import kataglyphis.vulkan.mesh_draw_recorder;
import kataglyphis.vulkan.cascaded_shadow_map;
import kataglyphis.vulkan.punctual_light;

namespace Kataglyphis {

namespace {
    constexpr uint32_t kCubeFaces = 6;
    // One bit per face: the pipeline and vkCmdBeginRendering must name the same mask.
    constexpr uint32_t kCubeViewMask = (1U << kCubeFaces) - 1U;
    // shadow_map.slang's cubeFaceMatrices, set 1.
    constexpr uint32_t kFaceMatrixBinding = 2;
    constexpr vk::DeviceSize kFaceMatricesSize = sizeof(glm::mat4) * kCubeFaces;
    static_assert(kFaceMatricesSize <= PUNCTUAL_SHADOW_SLOT_STRIDE, "a slot's six matrices must fit its stride");
}// namespace

bool castsIntoPunctualShadow(const AABB &casterBounds, const glm::vec3 &lightPosition, float farPlane)
{
    if (!casterBounds.isValid()) { return true; }
    const glm::vec3 nearest = glm::clamp(lightPosition, casterBounds.min, casterBounds.max);
    const glm::vec3 offset = nearest - lightPosition;
    return glm::dot(offset, offset) <= farPlane * farPlane;
}

AABB sceneWorldBounds(Scene &scene)
{
    AABB bounds{ glm::vec3(1.0F), glm::vec3(-1.0F) };
    for (uint32_t m = 0; m < scene.getModelCount(); m++) {
        const glm::mat4 model = scene.getModelMatrix(m);
        for (uint32_t k = 0; k < scene.getMeshCount(m); k++) {
            const AABB placed = transformAABB(model, scene.getMeshBounds(m, k));
            if (!placed.isValid()) { continue; }
            bounds =
              bounds.isValid() ? AABB{ glm::min(bounds.min, placed.min), glm::max(bounds.max, placed.max) } : placed;
        }
    }
    return bounds;
}

void PunctualShadowMaps::init(const std::shared_ptr<VulkanDevice> &in_device,
  vk::DescriptorSetLayout sharedLayout,
  uint32_t imageCount)
{
    device = in_device;
    sharedRenderDescriptorSetLayout = sharedLayout;
    swapChainImageCount = imageCount;
    depthFormat = Kataglyphis::chooseDepthFormat(device->getPhysicalDevice());
    if (!isSupported()) { spdlog::warn("Punctual-light shadows need six multiview views; lights stay unshadowed."); }
    // A 1x1 placeholder until a scene names its lights: the lighting shaders bind the array regardless.
    createCubeArray(1, 1);

    faceMatrixDescriptors.addBinding(
      kFaceMatrixBinding, vk::DescriptorType::eUniformBufferDynamic, 1, vk::ShaderStageFlagBits::eVertex);
    if (!faceMatrixDescriptors.create(device, swapChainImageCount)) {
        spdlog::error("Failed to create the punctual shadow descriptor resources!");
    }
    // One host-visible buffer per swapchain image, so a rewrite never touches one an in-flight pass reads.
    faceMatrixBuffers.resize(swapChainImageCount);
    for (uint32_t i = 0; i < swapChainImageCount; i++) {
        faceMatrixBuffers[i].create(device,
          PUNCTUAL_SHADOW_SLOT_STRIDE * MAX_SHADOWED_PUNCTUAL_LIGHTS,
          vk::BufferUsageFlagBits::eUniformBuffer,
          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        faceMatrixDescriptors.writeBuffer(i, kFaceMatrixBinding, faceMatrixBuffers[i].getBuffer(), kFaceMatricesSize);
    }
}

bool PunctualShadowMaps::isSupported() const
{ return device && device->supportsImageCubeArray() && device->getMaxMultiviewViewCount() >= kCubeFaces; }

void PunctualShadowMaps::createCubeArray(uint32_t cubes, uint32_t size)
{
    cubeArray = std::make_unique<Texture>();
    cubeArray->createImage(device,
      size,
      size,
      1,
      depthFormat,
      vk::ImageTiling::eOptimal,
      vk::ImageUsageFlagBits::
          eDepthStencilAttachment// DEPTH_ATTACHMENT_CHAIN_OK: sampled-cube-array-not-a-plain-attachment
        | vk::ImageUsageFlagBits::eSampled,
      vk::MemoryPropertyFlagBits::eDeviceLocal,
      cubes * kCubeFaces,
      vk::ImageCreateFlagBits::eCubeCompatible);
    cubeArray->createImageView(
      device, depthFormat, vk::ImageAspectFlagBits::eDepth, 1, vk::ImageViewType::eCubeArray, cubes * kCubeFaces);
    // Comparison sampler: linear filtering makes each tap a 2x2 PCF, as the cascades' sampler does.
    cubeArray->createTextureSampler(
      device, vk::Filter::eLinear, vk::SamplerAddressMode::eClampToEdge, VK_TRUE, vk::CompareOp::eLessOrEqual);

    for (uint32_t cube = 0; cube < cubes; cube++) {
        vk::ImageViewCreateInfo info = buildImageViewCreateInfo(cubeArray->getImage(),
          depthFormat,
          vk::ImageAspectFlagBits::eDepth,
          1,
          vk::ImageViewType::e2DArray,
          kCubeFaces);
        info.subresourceRange.baseArrayLayer = cube * kCubeFaces;
        slotViews.push_back(
          vkCheck(device->getLogicalDevice().createImageView(info), "Failed to create a punctual shadow cube view!"));
    }
    cubeCount = cubes;
    resolution = size;
}

void PunctualShadowMaps::releaseCubeArray()
{
    // The views go before the image they look at (VUID-vkDestroyImage-image-01000).
    for (vk::ImageView view : slotViews) { device->getLogicalDevice().destroyImageView(view); }
    slotViews.clear();
    if (cubeArray) {
        cubeArray->cleanUp();
        cubeArray.reset();
    }
    cubeCount = 0;
}

void PunctualShadowMaps::setSlots(std::span<const PunctualShadowSlot> newSlots)
{
    slots.clear();
    if (!isSupported()) { return; }
    const std::size_t kept = std::min(newSlots.size(), MAX_SHADOWED_PUNCTUAL_LIGHTS);
    slots.assign(newSlots.begin(), newSlots.begin() + static_cast<std::ptrdiff_t>(kept));
    for (std::size_t slot = 0; slot < slots.size(); slot++) {
        const std::array<glm::mat4, 6> faces = punctualShadowFaceMatrices(slots[slot]);
        std::copy(faces.begin(), faces.end(), faceMatrices.begin() + static_cast<std::ptrdiff_t>(slot * kCubeFaces));
    }
    // Light-less scenes keep the 1x1 placeholder: only shadowed lights pay for full-size faces.
    const uint32_t cubes = std::max(slotCount(), 1U);
    const uint32_t size = slots.empty() ? 1U : PUNCTUAL_SHADOW_RESOLUTION;
    if (cubes != cubeCount || size != resolution) {
        releaseCubeArray();
        createCubeArray(cubes, size);
    }
}

void PunctualShadowMaps::uploadFaceMatrices(uint32_t image_index)
{
    if (image_index >= faceMatrixBuffers.size()) { return; }
    // Straight into the mapped buffer: the frame path allocates nothing.
    if (void *mapped = faceMatrixBuffers[image_index].getMappedData(); mapped != nullptr) {
        auto *const bytes = static_cast<std::byte *>(mapped);
        for (std::size_t slot = 0; slot < slots.size(); slot++) {
            std::memcpy(
              bytes + (slot * PUNCTUAL_SHADOW_SLOT_STRIDE), &faceMatrices[slot * kCubeFaces], kFaceMatricesSize);
        }
    }
}

void PunctualShadowMaps::createGraphicsPipeline() { buildGraphicsPipeline(); }

void PunctualShadowMaps::shaderHotReload()
{
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), graphicsPipeline, pipelineLayout);
    buildGraphicsPipeline();
}

void PunctualShadowMaps::buildGraphicsPipeline()
{
    std::string const slang_spv_dir = "Resources/ShadersSlang/build/spirv/rasterizer/shadows/";
    // The cascades' fragment entry: the same alpha test, so a masked caster cuts out alike for the sun and the lights.
    ShaderStagePair stages{ device,
        slang_spv_dir + "shadow_map.cube_shadow_vs_main.spv",
        materialShaderPath(slang_spv_dir + "shadow_map.shadow_fs_main.spv", device->materialTextureIndexing()) };
    const ShadowCasterVertexInput vertexInput = shadowCasterVertexInput();

    vk::PushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(ShadowPushConstants);

    std::array<vk::DescriptorSetLayout, 2> setLayouts = { sharedRenderDescriptorSetLayout,
        faceMatrixDescriptors.getLayout() };
    const std::array<vk::PushConstantRange, 1> pushConstantRanges = { pushConstantRange };
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo = buildPipelineLayoutCreateInfo(setLayouts, pushConstantRanges);
    pipelineLayout = vkCheck(device->getLogicalDevice().createPipelineLayout(pipelineLayoutInfo),
      "Failed to create the punctual shadow pipeline layout!");

    PipelineBuilder pipelineBuilder;
    graphicsPipeline =
      pipelineBuilder.setShaderStages({ stages.stages().begin(), stages.stages().end() })
        .setVertexInput({ vertexInput.binding }, { vertexInput.attributes.begin(), vertexInput.attributes.end() })
        // Both sides cast: a closed caster's back faces are what the light sees through an open one.
        .setCullMode(vk::CullModeFlagBits::eNone)
        .setRenderingFormats({}, depthFormat)
        .setViewMask(kCubeViewMask)
        .setDebugName("punctual shadows")
        .build(device->getLogicalDevice(),
          pipelineLayout,
          device->getPipelineCache(),
          "Failed to create the punctual shadow pipeline!");
}

void PunctualShadowMaps::recordCommands(vk::CommandBuffer commandBuffer,
  uint32_t image_index,
  Scene *scene,
  vk::DescriptorSet sharedRenderSet,
  bool cullingEnabled)
{
    castersDrawn = 0;
    castersConsidered = 0;
    if (slots.empty() || !cubeArray || image_index >= faceMatrixDescriptors.sets().size()) {
        recordSkippedPass(commandBuffer);
        return;
    }

    const vk::ImageAspectFlags depth_aspect = Kataglyphis::depthStencilTransitionAspect(depthFormat);
    // UNDEFINED: every face is cleared. The source stage waits out the previous frame's sampling.
    Kataglyphis::recordImageBarrier(commandBuffer,
      Kataglyphis::buildImageMemoryBarrier(cubeArray->getImage(),
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eDepthStencilAttachmentOptimal,
        vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eNone,
        vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
        vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        depth_aspect,
        0,
        1,
        0,
        cubeCount * kCubeFaces));

    const std::array<vk::DescriptorSet, 2> sets = { sharedRenderSet, faceMatrixDescriptors.sets()[image_index] };
    for (uint32_t slot = 0; slot < slotCount(); slot++) {
        recordLight(commandBuffer, slot, scene, sets, cullingEnabled);
    }

    // The forward and deferred lighting fragment shaders sample the cubes next.
    Kataglyphis::recordImageBarrier(commandBuffer,
      Kataglyphis::buildImageMemoryBarrier(cubeArray->getImage(),
        vk::ImageLayout::eDepthStencilAttachmentOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
        vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eShaderSampledRead,
        depth_aspect,
        0,
        1,
        0,
        cubeCount * kCubeFaces));
}

void PunctualShadowMaps::recordLight(vk::CommandBuffer commandBuffer,
  uint32_t slot,
  Scene *scene,
  std::span<const vk::DescriptorSet, 2> sets,
  bool cullingEnabled)
{
    const vk::Extent2D extent{ resolution, resolution };
    const vk::RenderingAttachmentInfo depth_attachment = Kataglyphis::buildRenderingAttachmentInfo(slotViews[slot],
      vk::ImageLayout::eDepthStencilAttachmentOptimal,
      vk::AttachmentLoadOp::eClear,
      vk::AttachmentStoreOp::eStore,
      vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0F, 0 } });
    const vk::RenderingInfo rendering_info = Kataglyphis::buildRenderingInfo(
      extent, std::span<const vk::RenderingAttachmentInfo>{}, &depth_attachment, kCubeViewMask);

    commandBuffer.beginRendering(rendering_info);
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, graphicsPipeline);
    setFullExtentViewportAndScissor(commandBuffer, extent);
    const std::array<uint32_t, 1> dynamicOffsets = { static_cast<uint32_t>(slot * PUNCTUAL_SHADOW_SLOT_STRIDE) };
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, sets, dynamicOffsets);

    const PunctualShadowSlot &light = slots[slot];
    const VulkanRendererInternals::MeshDrawStats stats = VulkanRendererInternals::walkSceneMeshes(
      commandBuffer,
      scene,
      [](const glm::mat4 & /*modelMatrix*/) {},
      [&](const AABB &casterBounds, const Mesh * /*mesh*/) {
          return cullingEnabled && !castsIntoPunctualShadow(casterBounds, light.position, light.farPlane);
      },
      [&](const glm::mat4 &modelMatrix, uint32_t object_index, Mesh * /*mesh*/) {
          const ShadowPushConstants push = makeShadowPush(modelMatrix, object_index);
          commandBuffer.pushConstants(pipelineLayout,
            vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
            0,
            sizeof(ShadowPushConstants),
            &push);
      });
    castersDrawn += stats.drawn;
    castersConsidered += stats.considered;
    commandBuffer.endRendering();
}

void PunctualShadowMaps::recordSkippedPass(vk::CommandBuffer commandBuffer)
{
    castersDrawn = 0;
    castersConsidered = 0;
    if (!cubeArray) { return; }
    // Never read (the scene UBO counts no cubes), so UNDEFINED is safe; it waits out the last frame's sampling.
    Kataglyphis::recordImageBarrier(commandBuffer,
      Kataglyphis::buildImageMemoryBarrier(cubeArray->getImage(),
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eNone,
        vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eShaderSampledRead,
        Kataglyphis::depthStencilTransitionAspect(depthFormat),
        0,
        1,
        0,
        cubeCount * kCubeFaces));
}

void PunctualShadowMaps::cleanUp()
{
    // Idempotent and reusable: the renderer re-inits this when the swapchain's image count changes.
    if (!device) { return; }
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), graphicsPipeline, pipelineLayout);
    faceMatrixDescriptors.cleanUp();
    for (auto &buffer : faceMatrixBuffers) { buffer.cleanUp(); }
    faceMatrixBuffers.clear();
    releaseCubeArray();
    slots.clear();
    resolution = 1;
    device.reset();
}

}// namespace Kataglyphis
