module;
#include "common/FormatHelper.hpp"
#include "common/ImageBarrierHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/RenderingHelper.hpp"
#include "common/Utilities.hpp"
#include "common/ViewportHelper.hpp"
#include "renderer/SceneUBO.hpp"
#include "vulkan_base/PhysicalDeviceChoices.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <span>
#include <sstream>
#include <vector>
#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.cascaded_shadow_map;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.shader_helper;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.mesh;
import kataglyphis.vulkan.vertex;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.pipeline_builder;
import kataglyphis.vulkan.mesh_draw_recorder;

namespace Kataglyphis {


void CascadedShadowMap::init(const std::shared_ptr<VulkanDevice> &in_device,
  uint32_t width,
  uint32_t height,
  uint32_t num_cascades,
  vk::DescriptorSetLayout in_sharedRenderDescriptorSetLayout,
  uint32_t in_swapChainImageCount,
  vk::CommandPool in_commandPool)
{
    this->device = in_device;
    this->shadowWidth = width;
    this->shadowHeight = height;
    this->numCascades = num_cascades;
    this->sharedRenderDescriptorSetLayout = in_sharedRenderDescriptorSetLayout;
    this->swapChainImageCount = in_swapChainImageCount;
    this->commandPool = in_commandPool;

    cascadeData.resize(numCascades);

    vk::Format depthFormat = Kataglyphis::chooseDepthFormat(device->getPhysicalDevice());
    // Cached so the pipeline's rendering formats name the very same format as the image.
    depth_format = depthFormat;

    shadowMapArray = std::make_unique<Texture>();
    // Sampled array with a comparison sampler, so deliberately outside createDepthAttachment (DepthAttachment.ixx).
    shadowMapArray->createImage(device,
      shadowWidth,
      shadowHeight,
      1,
      depthFormat,
      vk::ImageTiling::eOptimal,
      vk::ImageUsageFlagBits::eDepthStencilAttachment// DEPTH_ATTACHMENT_CHAIN_OK: sampled-array-not-a-plain-attachment
        | vk::ImageUsageFlagBits::eSampled,
      vk::MemoryPropertyFlagBits::eDeviceLocal,
      numCascades);

    // Sampled view: exactly one aspect, not Kataglyphis::depthStencilTransitionAspect. See its doc comment.
    shadowMapArray->createImageView(
      device, depthFormat, vk::ImageAspectFlagBits::eDepth, 1, vk::ImageViewType::e2DArray, numCascades);
    // Comparison sampler: linear filtering gives a free 2x2 PCF tap; eLessOrEqual matches the Rust shadow_sampler.
    shadowMapArray->createTextureSampler(
      device, vk::Filter::eLinear, vk::SamplerAddressMode::eClampToEdge, VK_TRUE, vk::CompareOp::eLessOrEqual);
}

// The pure cascade math lives in CascadedShadowMapMath.cpp, so perfSuite links it without this TU's device deps.

void CascadedShadowMap::updateCascades(const glm::mat4 &cameraView,
  float cameraFov,
  float aspect,
  float nearPlane,
  float farPlane,
  const glm::vec3 &lightDir,
  float shadowDistance,
  float splitLambda)
{
    // Frame path must not allocate: only init() sizes cascadeData, so a mismatch is an upstream bug, not a resize.
    if (cascadeData.size() != numCascades) {
        spdlog::error(
          "CascadedShadowMap::updateCascades: cascadeData ({}) not sized for numCascades ({}); init() must size it",
          cascadeData.size(),
          numCascades);
        return;
    }
    computeCascadeDataInto(cascadeData,
      numCascades,
      CascadeFitParams{
        .cameraView = cameraView,
        .cameraFov = cameraFov,
        .aspect = aspect,
        .nearPlane = nearPlane,
        .farPlane = farPlane,
        .lightDir = lightDir,
        .shadowDistance = shadowDistance,
        .splitLambda = splitLambda,
        .shadowMapResolution = shadowWidth,
      });
}

void CascadedShadowMap::uploadLightMatrices(uint32_t image_index)
{
    // Empty is a silent no-op: the renderer's first uniform upload runs before init(), which reseeds every image.
    if (lightMatricesBuffers.empty()) { return; }
    if (image_index >= lightMatricesBuffers.size()) {
        spdlog::error("CascadedShadowMap::uploadLightMatrices: image_index ({}) exceeds buffer count ({})",
          image_index,
          lightMatricesBuffers.size());
        return;
    }

    // Straight into the mapped buffer: a staging vector would be a heap allocation per frame.
    if (void *mapped = lightMatricesBuffers[image_index].getMappedData(); mapped != nullptr) {
        auto *const matrices = static_cast<std::byte *>(mapped);
        for (size_t i = 0; i < cascadeData.size(); i++) {
            std::memcpy(matrices + (i * sizeof(glm::mat4)), &cascadeData[i].viewProjMatrix, sizeof(glm::mat4));
        }
    }
}

void CascadedShadowMap::cleanUp()
{
    // Idempotent and leaves the object reusable: VulkanRenderer re-inits this stage when shadow settings change.
    if (!device) { return; }

    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), graphicsPipeline, pipelineLayout);
    lightMatricesDescriptors.cleanUp();
    if (shadowMapArray) {
        shadowMapArray->cleanUp();
        shadowMapArray.reset();
    }
    for (auto &buffer : lightMatricesBuffers) { buffer.cleanUp(); }
    lightMatricesBuffers.clear();

    device.reset();
}

void CascadedShadowMap::createDescriptorSetAndPipeline()
{
    // UNIFORM_LIGHT_MATRICES_BINDING = 1
    lightMatricesDescriptors.addBinding(1, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex);
    if (!lightMatricesDescriptors.create(device, swapChainImageCount)) {
        spdlog::error("Failed to create shadow map descriptor resources!");
    }

    // One host-visible buffer per swapchain image, so a rewrite never touches one an in-flight shadow pass reads.
    lightMatricesBuffers.resize(swapChainImageCount);
    for (uint32_t i = 0; i < swapChainImageCount; i++) {
        lightMatricesBuffers[i].create(device,
          sizeof(glm::mat4) * numCascades,
          vk::BufferUsageFlagBits::eUniformBuffer,
          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

        // UNIFORM_LIGHT_MATRICES_BINDING = 1
        lightMatricesDescriptors.writeBuffer(
          i, 1, lightMatricesBuffers[i].getBuffer(), sizeof(glm::mat4) * numCascades);
    }
    for (uint32_t i = 0; i < swapChainImageCount; i++) { uploadLightMatrices(i); }
}

void CascadedShadowMap::createGraphicsPipeline()
{
    createDescriptorSetAndPipeline();
    buildGraphicsPipeline();
}

void CascadedShadowMap::shaderHotReload()
{
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), graphicsPipeline, pipelineLayout);
    // Pipeline only: createDescriptorSetAndPipeline() would leak the previous descriptor set and buffers.
    buildGraphicsPipeline();
}

ShadowCasterVertexInput shadowCasterVertexInput()
{
    vk::VertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(Vertex);
    bindingDesc.inputRate = vk::VertexInputRate::eVertex;

    vk::VertexInputAttributeDescription posAttr{};
    posAttr.location = 0;
    posAttr.binding = 0;
    posAttr.format = vk::Format::eR32G32B32Sfloat;
    posAttr.offset = 0;

    // UV for the MASK alpha test; location 3 is the Vertex texture_coords slot in both vertex shaders.
    vk::VertexInputAttributeDescription uvAttr{};
    uvAttr.location = 3;
    uvAttr.binding = 0;
    uvAttr.format = vk::Format::eR32G32Sfloat;
    uvAttr.offset = offsetof(Vertex, texture_coords);

    // TEXCOORD_1 for a base colour on UV set 1; location 5 matches the Vertex texture_coords1 slot.
    vk::VertexInputAttributeDescription uv1Attr{};
    uv1Attr.location = 5;
    uv1Attr.binding = 0;
    uv1Attr.format = vk::Format::eR32G32Sfloat;
    uv1Attr.offset = offsetof(Vertex, texture_coords1);

    // COLOR_0 alpha too, or a masked shadow silhouette disagrees with the lit one; location 2 is the Vertex color slot.
    vk::VertexInputAttributeDescription colorAttr{};
    colorAttr.location = 2;
    colorAttr.binding = 0;
    colorAttr.format = vk::Format::eR32G32B32A32Sfloat;
    colorAttr.offset = offsetof(Vertex, color);

    return ShadowCasterVertexInput{ bindingDesc, { posAttr, uvAttr, uv1Attr, colorAttr } };
}

void CascadedShadowMap::buildGraphicsPipeline()
{
    // Build-SlangShaders.ps1 emits this SPIR-V; the paths resolve from the repo root.
    std::string const slang_spv_dir = "Resources/ShadersSlang/build/spirv/rasterizer/shadows/";

    // No geometry stage: under multiview the vertex shader picks the cascade matrix by SV_ViewID.
    ShaderStagePair stages{ device,
        slang_spv_dir + "shadow_map.shadow_vs_main.spv",
        materialShaderPath(slang_spv_dir + "shadow_map.shadow_fs_main.spv", device->materialTextureIndexing()) };

    const ShadowCasterVertexInput vertexInput = shadowCasterVertexInput();

    vk::PushConstantRange pushConstantRange{};
    // The fragment stage now reads objectIndex from the push block too.
    pushConstantRange.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(glm::mat4) + sizeof(uint32_t);

    // Set 0 = VulkanRenderer's shared set, which the alpha test samples; set 1 = this pass's light matrices.
    std::array<vk::DescriptorSetLayout, 2> setLayouts = { sharedRenderDescriptorSetLayout,
        lightMatricesDescriptors.getLayout() };
    const std::array<vk::PushConstantRange, 1> pushConstantRanges = { pushConstantRange };

    vk::PipelineLayoutCreateInfo pipelineLayoutInfo = buildPipelineLayoutCreateInfo(setLayouts, pushConstantRanges);

    pipelineLayout = vkCheck(device->getLogicalDevice().createPipelineLayout(pipelineLayoutInfo),
      "Failed to create shadow map pipeline layout!");

    PipelineBuilder pipelineBuilder;
    graphicsPipeline =
      pipelineBuilder.setShaderStages({ stages.stages().begin(), stages.stages().end() })
        .setVertexInput({ vertexInput.binding }, { vertexInput.attributes.begin(), vertexInput.attributes.end() })
        // No culling: the cascade orthos lack the camera's Y flip, so back-face culling dropped the faces that cast.
        .setCullMode(vk::CullModeFlagBits::eNone)
        // Pancake casters in front of the near plane rather than clip them; a wider depth range would scale the bias.
        .setDepthClamp(device->supportsDepthClamp())
        // Depth only: no colour attachment, so no colour blend state either.
        .setRenderingFormats({}, depth_format)
        // Multiview draws to every cascade layer at once; features11.multiview is requested at device creation.
        .setViewMask(cascadeViewMask())
        .setDebugName("shadow cascades")
        .build(device->getLogicalDevice(),
          pipelineLayout,
          device->getPipelineCache(),
          "Failed to create shadow map graphics pipeline!");
}

void CascadedShadowMap::recordCommands(vk::CommandBuffer &commandBuffer,
  uint32_t image_index,
  Scene *scene,
  std::span<const vk::DescriptorSet> descriptorSets,
  bool cullingEnabled)
{
    castersDrawn = 0;
    castersConsidered = 0;

    // Multiview culls against the union of cascade frusta: only geometry outside every cascade's box is skipped.
    if (numCascades > CASCADE_COUNT) {
        spdlog::error(
          "CascadedShadowMap::recordCommands: numCascades ({}) exceeds CASCADE_COUNT ({})", numCascades, CASCADE_COUNT);
        return;
    }
    if (image_index >= lightMatricesDescriptors.sets().size()) {
        spdlog::error("CascadedShadowMap::recordCommands: image_index ({}) exceeds descriptor set count ({})",
          image_index,
          lightMatricesDescriptors.sets().size());
        return;
    }
    std::array<FrustumPlanes, CASCADE_COUNT> cascadeFrusta{};
    if (cullingEnabled) {
        for (uint32_t cascade = 0; cascade < numCascades; cascade++) {
            cascadeFrusta[cascade] = extractFrustumPlanes(cascadeData[cascade].viewProjMatrix);
        }
    }

    const vk::ImageAspectFlags depth_aspect = Kataglyphis::depthStencilTransitionAspect(depth_format);
    // UNDEFINED: the clear overwrites every layer. The source stage waits out the previous frame's sampling.
    Kataglyphis::recordImageBarrier(commandBuffer,
      Kataglyphis::buildImageMemoryBarrier(shadowMapArray->getImage(),
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
        numCascades));

    // Multiview renders one layer per view-mask bit through the sampled view, which already spans every cascade.
    const vk::RenderingAttachmentInfo depth_attachment =
      Kataglyphis::buildRenderingAttachmentInfo(shadowMapArray->getImageView(),
        vk::ImageLayout::eDepthStencilAttachmentOptimal,
        vk::AttachmentLoadOp::eClear,
        vk::AttachmentStoreOp::eStore,
        vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0F, 0 } });
    const vk::RenderingInfo rendering_info = Kataglyphis::buildRenderingInfo(vk::Extent2D{ shadowWidth, shadowHeight },
      std::span<const vk::RenderingAttachmentInfo>{},
      &depth_attachment,
      cascadeViewMask());

    commandBuffer.beginRendering(rendering_info);
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, graphicsPipeline);

    setFullExtentViewportAndScissor(commandBuffer, vk::Extent2D{ shadowWidth, shadowHeight });

    // The layout always puts the light matrices at set 1; shadowSetBinding keeps firstSet right when set 0 is unbound.
    const vk::DescriptorSet lightMatricesSet = lightMatricesDescriptors.sets()[image_index];
    if (descriptorSets.empty()) {
        spdlog::warn(
          "CascadedShadowMap::recordCommands: no shared render set bound; the fragment alpha test will "
          "sample nothing (degraded mode)");
    }
    const std::array<vk::DescriptorSet, 2> sets =
      descriptorSets.empty() ? std::array<vk::DescriptorSet, 2>{ lightMatricesSet, VK_NULL_HANDLE }
                             : std::array<vk::DescriptorSet, 2>{ descriptorSets[0], lightMatricesSet };
    const ShadowSetBinding binding = shadowSetBinding(!descriptorSets.empty());
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
      pipelineLayout,
      binding.firstSet,
      std::span(sets.data(), binding.setCount),
      nullptr);

    // object_index is the flat mesh index the forward pass uses, so the alpha test fetches the same material.
    const VulkanRendererInternals::MeshDrawStats stats = VulkanRendererInternals::walkSceneMeshes(
      commandBuffer,
      scene,
      [](const glm::mat4 & /*modelMatrix*/) {},
      [&](const AABB &casterBounds, const Mesh * /*mesh*/) {
          if (!cullingEnabled) { return false; }
          for (uint32_t cascade = 0; cascade < numCascades; cascade++) {
              if (isVisibleAsShadowCaster(cascadeFrusta[cascade], casterBounds)) { return false; }
          }
          return true;
      },
      [&](const glm::mat4 &modelMatrix, uint32_t object_index, Mesh * /*mesh*/) {
          const ShadowPushConstants push = makeShadowPush(modelMatrix, object_index);
          commandBuffer.pushConstants(pipelineLayout,
            vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
            0,
            sizeof(ShadowPushConstants),
            &push);
      });
    castersDrawn = stats.drawn;
    castersConsidered = stats.considered;

    commandBuffer.endRendering();

    // The forward and deferred lighting fragment shaders sample the cascades next.
    Kataglyphis::recordImageBarrier(commandBuffer,
      Kataglyphis::buildImageMemoryBarrier(shadowMapArray->getImage(),
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
        numCascades));
}

void CascadedShadowMap::recordSkippedPass(vk::CommandBuffer &commandBuffer)
{
    castersDrawn = 0;
    castersConsidered = 0;
    if (!shadowMapArray) { return; }

    // Never read (the scene UBO says zero cascades), so UNDEFINED is safe; it waits out the last frame's sampling.
    Kataglyphis::recordImageBarrier(commandBuffer,
      Kataglyphis::buildImageMemoryBarrier(shadowMapArray->getImage(),
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eNone,
        vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eShaderSampledRead,
        Kataglyphis::depthStencilTransitionAspect(depth_format),
        0,
        1,
        0,
        numCascades));
}
}// namespace Kataglyphis
