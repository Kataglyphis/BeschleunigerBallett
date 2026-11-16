module;
#include "shared/scene/ObjMaterial.hpp"
#include <array>
#include <filesystem>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <spdlog/spdlog.h>
#include <stb_image.h>
#include <string>
#include <system_error>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "common/FrameSubmitHelper.hpp"
#include "common/ImageBarrierHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/RenderingHelper.hpp"
#include "common/Utilities.hpp"
#include "common/ViewportHelper.hpp"
#include "scene/sky_box/CubemapFaces.hpp"

module kataglyphis.vulkan.sky_box;

import kataglyphis.vulkan.vertex;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.mesh;
import kataglyphis.vulkan.shader_helper;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.command_buffer_manager;
import kataglyphis.vulkan.pipeline_builder;
import kataglyphis.shared.util.resource_paths;

namespace Kataglyphis {

SkyBox::SkyBox() = default;

void SkyBox::init(const std::shared_ptr<VulkanDevice> &in_device, vk::CommandPool commandPool)
{
    this->device = in_device;

    createMesh(commandPool);
    // Unconditional: createGraphicsPipeline() needs the layout even when the load below fails.
    createDescriptorSetForCubeMap();
    loadCubeMap(commandPool);
}

void SkyBox::loadCubeMap(vk::CommandPool commandPool)
{
    std::filesystem::path skybox_base_dir;
    if (const auto resolved = Kataglyphis::Shared::resolveResourceRelativePath("Textures/Skybox/DOOM2016");
      resolved.has_value()) {
        skybox_base_dir = *resolved;
    } else {
        std::error_code current_path_ec;
        std::filesystem::path const cwd = std::filesystem::current_path(current_path_ec);
        skybox_base_dir = (current_path_ec ? std::filesystem::path(".") : cwd) / "Resources/Textures/Skybox/DOOM2016";
    }

    std::array<std::string, 6> skybox_textures = {
        "DOOM16RT.png", "DOOM16LF.png", "DOOM16UP.png", "DOOM16DN.png", "DOOM16FT.png", "DOOM16BK.png"
    };

    cubeMapTexture = std::make_unique<Texture>();

    int width = 0, height = 0, bit_depth = 0;
    std::vector<unsigned char *> face_data(6);
    int face_widths[6] = {};
    int face_heights[6] = {};

    for (size_t i = 0; i < 6; i++) {
        std::string path = (skybox_base_dir / skybox_textures[i]).string();
        face_data[i] = stbi_load(path.c_str(), &face_widths[i], &face_heights[i], &bit_depth, 4);
        if (!face_data[i]) {
            spdlog::error("Failed to load skybox texture: {}", path);
            for (size_t j = 0; j < i; j++) { stbi_image_free(face_data[j]); }
            loadFallbackCubeMap(commandPool);
            return;
        }
    }

    // Memory safety, not validation: the upload copies the first face's layerSize bytes out of every face.
    if (!cubemapFacesConsistent(face_widths, face_heights)) {
        spdlog::error("Skybox faces have inconsistent or degenerate dimensions; all six faces must match.");
        for (size_t j = 0; j < 6; j++) { stbi_image_free(face_data[j]); }
        loadFallbackCubeMap(commandPool);
        return;
    }

    width = face_widths[0];
    height = face_heights[0];

    spdlog::info("SkyBox: All 6 textures loaded, width={}, height={}", width, height);

    std::array<const unsigned char *, 6> faces = {
        face_data[0], face_data[1], face_data[2], face_data[3], face_data[4], face_data[5]
    };
    // Uploaded as sRGB: the faces are sRGB-encoded PNGs and must decode to linear before post gamma-encodes.
    const bool uploaded =
      uploadCubeMapFaces(commandPool, static_cast<uint32_t>(width), static_cast<uint32_t>(height), faces);

    for (size_t i = 0; i < 6; i++) { stbi_image_free(face_data[i]); }

    if (!uploaded) {
        spdlog::error("SkyBox: cubemap upload failed, falling back to the black fallback cubemap.");
        loadFallbackCubeMap(commandPool);
        return;
    }

    updateDescriptorSetForCubeMap();
}

void SkyBox::loadFallbackCubeMap(vk::CommandPool commandPool)
{
    // Same upload path as the real faces, so a broken skybox renders black instead of binding nothing.
    std::array<const unsigned char *, 6> faces = { kFallbackCubemapFacePixel,
        kFallbackCubemapFacePixel,
        kFallbackCubemapFacePixel,
        kFallbackCubemapFacePixel,
        kFallbackCubemapFacePixel,
        kFallbackCubemapFacePixel };
    if (!uploadCubeMapFaces(commandPool, 1, 1, faces)) {
        spdlog::error("SkyBox: fallback cubemap upload failed; leaving the cubemap descriptor unwritten.");
        return;
    }
    updateDescriptorSetForCubeMap();
}

bool SkyBox::uploadCubeMapFaces(vk::CommandPool commandPool,
  uint32_t width,
  uint32_t height,
  std::span<const unsigned char *const, 6> faceData)
{
    vk::DeviceSize const layerSize = static_cast<vk::DeviceSize>(width) * static_cast<vk::DeviceSize>(height) * 4;
    vk::DeviceSize const imageSize = layerSize * 6;

    // VUID-vkDestroyImage-image-01000: the old view must go before createImage() replaces its image.
    cubeMapTexture->releaseImageView();

    cubeMapTexture->createImage(device,
      width,
      height,
      1,
      vk::Format::eR8G8B8A8Srgb,
      vk::ImageTiling::eOptimal,
      vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
      vk::MemoryPropertyFlagBits::eDeviceLocal,
      6,
      vk::ImageCreateFlagBits::eCubeCompatible);

    VulkanBuffer stagingBuffer;
    stagingBuffer.create(device,
      imageSize,
      vk::BufferUsageFlagBits::eTransferSrc,
      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

    // Host-visible buffers are persistently mapped by VMA.
    void *mappedData = stagingBuffer.getMappedData();
    for (size_t i = 0; i < 6; i++) {
        void *layerOffset = static_cast<char *>(mappedData) + i * layerSize;
        std::memcpy(layerOffset, faceData[i], static_cast<size_t>(layerSize));
    }

    vk::CommandBuffer commandBuffer = Kataglyphis::VulkanRendererInternals::CommandBufferManager::beginCommandBuffer(
      device->getLogicalDevice(), commandPool);
    if (!commandBuffer) {
        spdlog::error("SkyBox::uploadCubeMapFaces: failed to begin command buffer, skipping cubemap upload.");
        stagingBuffer.cleanUp();
        return false;
    }

    cubeMapTexture->getVulkanImage().transitionImageLayout(commandBuffer,
      vk::ImageLayout::eUndefined,
      vk::ImageLayout::eTransferDstOptimal,
      1,
      vk::ImageAspectFlagBits::eColor,
      6);

    vk::BufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = width;
    region.bufferImageHeight = height;
    region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 6;
    region.imageOffset = vk::Offset3D{ 0, 0, 0 };
    region.imageExtent = vk::Extent3D{ width, height, 1 };

    commandBuffer.copyBufferToImage(
      stagingBuffer.getBuffer(), cubeMapTexture->getImage(), vk::ImageLayout::eTransferDstOptimal, 1, &region);

    // The helper's eAllCommands destination strictly widens a fragment-only stage (see pipelineStageForLayout).
    cubeMapTexture->getVulkanImage().transitionImageLayout(commandBuffer,
      vk::ImageLayout::eTransferDstOptimal,
      vk::ImageLayout::eShaderReadOnlyOptimal,
      1,
      vk::ImageAspectFlagBits::eColor,
      6);

    const bool submitted = Kataglyphis::VulkanRendererInternals::CommandBufferManager::endAndSubmitCommandBuffer(
      device->getLogicalDevice(), commandPool, device->getGraphicsQueue(), commandBuffer);

    stagingBuffer.cleanUp();

    if (!submitted) {
        spdlog::error(
          "SkyBox::uploadCubeMapFaces: submit failed ({}x{}); leaving cubemap image unwritten.", width, height);
        return false;
    }

    // clang-format off: BuildIntegrity wants the call, its eColor aspect and the marker on one line.
    cubeMapTexture->createImageView(device, vk::Format::eR8G8B8A8Srgb, vk::ImageAspectFlagBits::eColor, 1, vk::ImageViewType::eCube, 6);// COLOR_ATTACHMENT_CHAIN_OK: cubemap-six-layers-cube-compatible
    // clang-format on
    cubeMapTexture->createTextureSampler(device);
    return true;
}

void SkyBox::createDescriptorSetForCubeMap()
{
    cubemapDescriptors.addBinding(1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment);
    if (!cubemapDescriptors.create(device, 1)) { spdlog::error("Failed to create skybox descriptor resources!"); }
}

void SkyBox::updateDescriptorSetForCubeMap()
{
    cubemapDescriptors.writeImage(
      0, 1, cubeMapTexture->getImageView(), vk::ImageLayout::eShaderReadOnlyOptimal, cubeMapTexture->getSampler());
}

void SkyBox::createGraphicsPipeline(vk::DescriptorSetLayout sharedLayout, vk::Format in_color_format)
{
    colorFormat = in_color_format;

    // Build-SlangShaders.ps1 emits this SPIR-V; the paths resolve from the repo root.
    std::string const slang_spv_dir = "Resources/ShadersSlang/build/spirv/skybox/";

    ShaderStagePair stages{ device, slang_spv_dir + "skybox.vs_main.spv", slang_spv_dir + "skybox.fs_main.spv" };

    vk::VertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = vk::VertexInputRate::eVertex;

    const auto attributeDescriptions = vertex::getVertexInputAttributeDesc();

    std::array<vk::DescriptorSetLayout, 2> combinedLayouts = { sharedLayout, cubemapDescriptors.getLayout() };

    vk::PushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = vk::ShaderStageFlagBits::eFragment;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(uint32_t);
    const std::array<vk::PushConstantRange, 1> pushConstantRanges = { pushConstantRange };

    vk::PipelineLayoutCreateInfo pipelineLayoutInfo =
      buildPipelineLayoutCreateInfo(combinedLayouts, pushConstantRanges);

    pipelineLayout = vkCheck(
      device->getLogicalDevice().createPipelineLayout(pipelineLayoutInfo), "Failed to create skybox pipeline layout!");

    PipelineBuilder pipelineBuilder;
    graphicsPipeline =
      pipelineBuilder.setShaderStages({ stages.stages().begin(), stages.stages().end() })
        .setVertexInput({ bindingDescription }, { attributeDescriptions.begin(), attributeDescriptions.end() })
        .setCullMode(vk::CullModeFlagBits::eNone)
        .setDepthTest(false)
        .setDepthWrite(false)
        .setDepthCompareOp(vk::CompareOp::eAlways)
        .setRenderingFormats({ colorFormat })
        .build(device->getLogicalDevice(),
          pipelineLayout,
          device->getPipelineCache(),
          "Failed to create skybox graphics pipeline!");
}

void SkyBox::shaderHotReload(vk::DescriptorSetLayout sharedLayout)
{
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), graphicsPipeline, pipelineLayout);
    createGraphicsPipeline(sharedLayout, colorFormat);
}

void SkyBox::createMesh(vk::CommandPool commandPool)
{
    // Fullscreen quad indices
    std::vector<unsigned int> indices = { 0, 1, 2, 2, 1, 3 };

    // Fullscreen quad vertices with UVs
    std::vector<Vertex> vertices = {
        Vertex(glm::vec3(-1.0F, -1.0F, 0.0F), glm::vec3(0), glm::vec4(0), glm::vec2(0.0F, 0.0F)),
        Vertex(glm::vec3(1.0F, -1.0F, 0.0F), glm::vec3(0), glm::vec4(0), glm::vec2(1.0F, 0.0F)),
        Vertex(glm::vec3(-1.0F, 1.0F, 0.0F), glm::vec3(0), glm::vec4(0), glm::vec2(0.0F, 1.0F)),
        Vertex(glm::vec3(1.0F, 1.0F, 0.0F), glm::vec3(0), glm::vec4(0), glm::vec2(1.0F, 1.0F))
    };

    std::vector<unsigned int> materialIndex = { 0 };
    std::vector<ObjMaterial> materials = { ObjMaterial{} };
    skyMesh = std::make_unique<Mesh>(device, commandPool, vertices, indices, materialIndex, materials);
}

void SkyBox::recordCommands(vk::CommandBuffer &commandBuffer,
  vk::Image target,
  vk::ImageView targetView,
  vk::Extent2D extent,
  std::span<const vk::DescriptorSet> descriptorSets,
  bool skyboxEnabled)
{
    if (!target || !targetView) {
        spdlog::error("SkyBox::recordCommands called without a target image");
        return;
    }

    if (descriptorSets.empty() || !graphicsPipeline || cubemapDescriptors.sets().empty()) {
        spdlog::error("SkyBox::recordCommands called with an empty shared descriptor set span or an unready pipeline");
        return;
    }

    spdlog::debug("SkyBox: enabled={}, indexCount={}", skyboxEnabled, skyMesh->getIndexCount());

    // UNDEFINED: the clear overwrites it. The source stage is the submit's wait stage on the acquire semaphore.
    recordImageBarrier(commandBuffer,
      buildImageMemoryBarrier(target,
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eColorAttachmentOptimal,
        kSwapchainAcquireWaitStage,
        vk::AccessFlagBits2::eNone,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite));

    // Left in eColorAttachmentOptimal, not presentable: the post stage loads this attachment next.
    const vk::RenderingAttachmentInfo color_attachment = buildRenderingAttachmentInfo(targetView,
      vk::ImageLayout::eColorAttachmentOptimal,
      vk::AttachmentLoadOp::eClear,
      vk::AttachmentStoreOp::eStore,
      vk::ClearValue{ vk::ClearColorValue{ std::array<float, 4>{ 0.0F, 0.0F, 0.0F, 1.0F } } });
    commandBuffer.beginRendering(
      buildRenderingInfo(extent, std::span<const vk::RenderingAttachmentInfo>(&color_attachment, 1), nullptr));

    setFullExtentViewportAndScissor(commandBuffer, extent);

    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, graphicsPipeline);

    std::array<vk::DescriptorSet, 2> skyboxDescriptorSets = { descriptorSets[0], cubemapDescriptors.sets()[0] };
    commandBuffer.bindDescriptorSets(
      vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, skyboxDescriptorSets, nullptr);

    uint32_t skyboxEnabledVal = skyboxEnabled ? 1u : 0u;
    commandBuffer.pushConstants(
      pipelineLayout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(uint32_t), &skyboxEnabledVal);

    const vk::Buffer vertex_buffer = skyMesh->getVertexBuffer();
    const vk::DeviceSize offset = 0;
    commandBuffer.bindVertexBuffers(0, 1, &vertex_buffer, &offset);
    commandBuffer.bindIndexBuffer(skyMesh->getIndexBuffer(), 0, vk::IndexType::eUint32);

    commandBuffer.drawIndexed(skyMesh->getIndexCount(), 1, 0, 0, 0);
    commandBuffer.endRendering();
}

void SkyBox::cleanUp()
{
    // Idempotent: the destructor calls it again as a safety net.
    if (!device) { return; }

    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), graphicsPipeline, pipelineLayout);
    cubemapDescriptors.cleanUp();
    if (skyMesh) { skyMesh->cleanUp(); }
    skyMesh.reset();
    if (cubeMapTexture) { cubeMapTexture->cleanUp(); }
    cubeMapTexture.reset();

    device.reset();
}

SkyBox::~SkyBox() { cleanUp(); }

}// namespace Kataglyphis