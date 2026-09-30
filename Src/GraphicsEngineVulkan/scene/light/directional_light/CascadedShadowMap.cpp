module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>
#include <memory>
#include <filesystem>
#include <span>
#include <sstream>
#include <vulkan/vulkan.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include "common/FormatHelper.hpp"
#include "common/FramebufferHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/RenderPassHelper.hpp"
#include "common/Utilities.hpp"
#include "common/ViewportHelper.hpp"
#include "renderer/SceneUBO.hpp"

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


void CascadedShadowMap::init(const std::shared_ptr<VulkanDevice> &in_device, uint32_t width, uint32_t height, uint32_t num_cascades,
  vk::DescriptorSetLayout sharedRenderDescriptorSetLayout, uint32_t swapChainImageCount,
  vk::CommandPool commandPool)
{
    this->device = in_device;
    this->shadowWidth = width;
    this->shadowHeight = height;
    this->numCascades = num_cascades;
    this->sharedRenderDescriptorSetLayout = sharedRenderDescriptorSetLayout;
    this->swapChainImageCount = swapChainImageCount;
    this->commandPool = commandPool;

    cascadeData.resize(numCascades);

    vk::Format depthFormat = Kataglyphis::chooseDepthFormat(device->getPhysicalDevice());
    // Cached so createRenderPass() and createFramebuffers() attach the very same format.
    depth_format = depthFormat;

    shadowMapArray = std::make_unique<Texture>();
    // Sampled array with a comparison sampler, so deliberately outside createDepthAttachment (renderer/DepthAttachment.ixx).
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
    shadowMapArray->createImageView(device, depthFormat, vk::ImageAspectFlagBits::eDepth, 1, vk::ImageViewType::e2DArray, numCascades);
    // Comparison sampler: linear filtering gives a free 2x2 PCF tap; eLessOrEqual matches the Rust renderer's shadow_sampler.
    shadowMapArray->createTextureSampler(
      device, vk::Filter::eLinear, vk::SamplerAddressMode::eClampToEdge, VK_TRUE, vk::CompareOp::eLessOrEqual);

    createRenderPass();
    createFramebuffers();
}

// The pure cascade math lives in CascadedShadowMapMath.cpp, so perfSuite links it without this TU's device dependencies.

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
        spdlog::error("CascadedShadowMap::updateCascades: cascadeData ({}) not sized for numCascades ({}); init() must size it",
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
          image_index, lightMatricesBuffers.size());
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

void CascadedShadowMap::createRenderPass()
{
    // Stored and left in eShaderReadOnlyOptimal: the lighting pass samples this depth map.
    const vk::AttachmentDescription depthAttachment =
      buildAttachmentDescription(depth_format, vk::ImageLayout::eShaderReadOnlyOptimal);

    vk::AttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 0;
    depthAttachmentRef.layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;

    const vk::SubpassDescription subpass =
      buildSubpassDescription(std::span<const vk::AttachmentReference>{}, &depthAttachmentRef);

    std::array<vk::SubpassDependency, 2> dependencies;
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = vk::PipelineStageFlagBits::eFragmentShader;
    dependencies[0].dstStageMask = vk::PipelineStageFlagBits::eEarlyFragmentTests;
    dependencies[0].srcAccessMask = vk::AccessFlagBits::eShaderRead;
    dependencies[0].dstAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    dependencies[0].dependencyFlags = vk::DependencyFlagBits::eByRegion;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = vk::PipelineStageFlagBits::eLateFragmentTests;
    dependencies[1].dstStageMask = vk::PipelineStageFlagBits::eFragmentShader;
    dependencies[1].srcAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    dependencies[1].dstAccessMask = vk::AccessFlagBits::eShaderRead;
    dependencies[1].dependencyFlags = vk::DependencyFlagBits::eByRegion;

    // Multiview broadcasts each draw to every cascade layer; needs features11.multiview, requested at device creation.
    const uint32_t view_mask = (1U << numCascades) - 1U;
    vk::RenderPassMultiviewCreateInfo multiviewInfo{};
    multiviewInfo.subpassCount = 1;
    multiviewInfo.pViewMasks = &view_mask;
    multiviewInfo.correlationMaskCount = 1;
    multiviewInfo.pCorrelationMasks = &view_mask;

    const std::array<vk::AttachmentDescription, 1> attachments = { depthAttachment };
    const std::array<vk::SubpassDescription, 1> subpasses = { subpass };

    vk::RenderPassCreateInfo renderPassInfo =
      Kataglyphis::buildRenderPassCreateInfo(attachments, subpasses, dependencies);
    renderPassInfo.pNext = &multiviewInfo;

    auto result = device->getLogicalDevice().createRenderPass(renderPassInfo);
    ASSERT_VULKAN(result.result, "Failed to create cascaded shadow map render pass!");
    renderPass = result.value;
}

void CascadedShadowMap::createFramebuffers()
{
    // Multiview wants one layer-1 framebuffer whose view spans every cascade: the sampled view already does.
    const vk::ImageView attachment = shadowMapArray->getImageView();
    const vk::FramebufferCreateInfo framebufferInfo = Kataglyphis::buildFramebufferCreateInfo(
      renderPass, std::span<const vk::ImageView>(&attachment, 1), vk::Extent2D{ shadowWidth, shadowHeight });

    auto fbResult = device->getLogicalDevice().createFramebuffer(framebufferInfo);
    ASSERT_VULKAN(fbResult.result, "Failed to create shadow map framebuffer!");
    framebuffer = fbResult.value;

}

void CascadedShadowMap::cleanUp()
{
    // Idempotent and leaves the object reusable: VulkanRenderer re-inits this stage when shadow settings change.
    if (!device) { return; }

    Kataglyphis::destroyFramebuffer(device->getLogicalDevice(), framebuffer);

    Kataglyphis::destroyRenderPass(device->getLogicalDevice(), renderPass);
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
        lightMatricesBuffers[i].create(device, sizeof(glm::mat4) * numCascades,
          vk::BufferUsageFlagBits::eUniformBuffer,
          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

        // UNIFORM_LIGHT_MATRICES_BINDING = 1
        lightMatricesDescriptors.writeBuffer(i, 1, lightMatricesBuffers[i].getBuffer(), sizeof(glm::mat4) * numCascades);
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

void CascadedShadowMap::buildGraphicsPipeline()
{
    // Build-SlangShaders.ps1 emits this SPIR-V; the paths resolve from the repo root.
    std::string const slang_spv_dir = "Resources/ShadersSlang/build/spirv/rasterizer/shadows/";

    // No geometry stage: under multiview the vertex shader picks the cascade matrix by SV_ViewID.
    ShaderStagePair stages{ device, slang_spv_dir + "shadow_map.shadow_vs_main.spv",
        slang_spv_dir + "shadow_map.shadow_fs_main.spv" };

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

    // COLOR_0 alpha too, or a masked shadow silhouette disagrees with the lit one; location 2 is the Vertex color slot.
    vk::VertexInputAttributeDescription colorAttr{};
    colorAttr.location = 2;
    colorAttr.binding = 0;
    colorAttr.format = vk::Format::eR32G32B32A32Sfloat;
    colorAttr.offset = offsetof(Vertex, color);

    vk::PushConstantRange pushConstantRange{};
    // The fragment stage now reads objectIndex from the push block too.
    pushConstantRange.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(glm::mat4) + sizeof(uint32_t);

    // Set 0 = VulkanRenderer's shared set, which the alpha test samples; set 1 = this pass's light matrices.
    std::array<vk::DescriptorSetLayout, 2> setLayouts = { sharedRenderDescriptorSetLayout, lightMatricesDescriptors.getLayout() };
    const std::array<vk::PushConstantRange, 1> pushConstantRanges = { pushConstantRange };

    vk::PipelineLayoutCreateInfo pipelineLayoutInfo = buildPipelineLayoutCreateInfo(setLayouts, pushConstantRanges);

    auto layoutRes = device->getLogicalDevice().createPipelineLayout(pipelineLayoutInfo);
    ASSERT_VULKAN(layoutRes.result, "Failed to create shadow map pipeline layout!");
    pipelineLayout = layoutRes.value;

    PipelineBuilder pipelineBuilder;
    graphicsPipeline =
      pipelineBuilder.setShaderStages({ stages.stages().begin(), stages.stages().end() })
        .setVertexInput({ bindingDesc }, { posAttr, uvAttr, colorAttr })
        // Culling off: the cascade orthos lack the camera's Y flip, so back-face culling dropped exactly the faces that cast.
        .setCullMode(vk::CullModeFlagBits::eNone)
        // Pancake casters in front of the near plane rather than clip them; a wider depth range would scale the bias.
        .setDepthClamp(device->supportsDepthClamp())
        .setUseColorBlendState(false)
        .build(device->getLogicalDevice(),
          pipelineLayout,
          renderPass,
          device->getPipelineCache(),
          0,
          "Failed to create shadow map graphics pipeline!");
}

void CascadedShadowMap::recordCommands(vk::CommandBuffer &commandBuffer, uint32_t image_index, Scene *scene, std::span<const vk::DescriptorSet> descriptorSets, bool cullingEnabled)
{
    castersDrawn = 0;
    castersConsidered = 0;

    // Multiview culls against the union of cascade frusta: only geometry outside every cascade's box is skipped.
    if (numCascades > MAX_CASCADES) {
        spdlog::error("CascadedShadowMap::recordCommands: numCascades ({}) exceeds MAX_CASCADES ({})", numCascades, MAX_CASCADES);
        return;
    }
    if (image_index >= lightMatricesDescriptors.sets().size()) {
        spdlog::error("CascadedShadowMap::recordCommands: image_index ({}) exceeds descriptor set count ({})",
          image_index, lightMatricesDescriptors.sets().size());
        return;
    }
    std::array<FrustumPlanes, MAX_CASCADES> cascadeFrusta{};
    if (cullingEnabled) {
        for (uint32_t cascade = 0; cascade < numCascades; cascade++) {
            cascadeFrusta[cascade] = extractFrustumPlanes(cascadeData[cascade].viewProjMatrix);
        }
    }

    std::array<vk::ClearValue, 1> clearValues{};
    clearValues[0].depthStencil = vk::ClearDepthStencilValue{1.0f, 0};
    const vk::RenderPassBeginInfo renderPassInfo = Kataglyphis::buildRenderPassBeginInfo(
      renderPass, framebuffer, vk::Extent2D{shadowWidth, shadowHeight}, clearValues);

    commandBuffer.beginRenderPass(renderPassInfo, vk::SubpassContents::eInline);
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, graphicsPipeline);

    setFullExtentViewportAndScissor(commandBuffer, vk::Extent2D{ shadowWidth, shadowHeight });

    // The layout always puts the light matrices at set 1; shadowSetBinding keeps firstSet right when set 0 is unbound.
    const vk::DescriptorSet lightMatricesSet = lightMatricesDescriptors.sets()[image_index];
    if (descriptorSets.empty()) {
        spdlog::warn("CascadedShadowMap::recordCommands: no shared render set bound; the fragment alpha test will "
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
      [&](const AABB &casterBounds) {
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

    commandBuffer.endRenderPass();
}
}
