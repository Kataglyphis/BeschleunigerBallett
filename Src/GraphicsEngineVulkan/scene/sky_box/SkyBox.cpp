module;
#include <array>
#include <cstring>
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

#include "common/ImageBarrierHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/RenderingHelper.hpp"
#include "common/Utilities.hpp"
#include "common/ViewportHelper.hpp"
#include "scene/sky_box/AnalyticSky.hpp"
#include "scene/sky_box/CloudAmbient.hpp"
#include "scene/sky_box/CubemapFaces.hpp"
#include "scene/sky_box/SkyUniforms.hpp"

module kataglyphis.vulkan.sky_box;

import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.shader_helper;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.command_buffer_manager;
import kataglyphis.vulkan.pipeline_builder;
import kataglyphis.shared.util.resource_paths;

namespace Kataglyphis {

SkyBox::SkyBox() = default;

namespace {
    // sky.slang's set 0: its uniforms, the cube map and a separate sampler, as the WebGPU layout has them.
    constexpr uint32_t kSkyUniformsBinding = 0;
    constexpr uint32_t kSkyCubeMapBinding = 1;
    constexpr uint32_t kSkySamplerBinding = 2;
}// namespace

void SkyBox::init(const std::shared_ptr<VulkanDevice> &in_device, vk::CommandPool commandPool, uint32_t imageCount)
{
    this->device = in_device;

    // First: the cube map load below writes into every image's set.
    createFrameResources(imageCount);
    loadCubeMap(commandPool);
}

void SkyBox::createFrameResources(uint32_t imageCount)
{
    for (VulkanBuffer &buffer : uniformBuffers) { buffer.cleanUp(); }
    uniformBuffers.clear();
    descriptors.cleanUp();

    descriptors
      .addBinding(kSkyUniformsBinding, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment)
      .addBinding(kSkyCubeMapBinding, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment)
      .addBinding(kSkySamplerBinding, vk::DescriptorType::eSampler, 1, vk::ShaderStageFlagBits::eFragment);
    if (!descriptors.create(device, imageCount)) {
        spdlog::error("Failed to create sky descriptor resources!");
        return;
    }

    uniformBuffers.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        uniformBuffers[i].create(device,
          sizeof(SkyUniforms),
          vk::BufferUsageFlagBits::eUniformBuffer,
          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        descriptors.writeBuffer(i, kSkyUniformsBinding, uniformBuffers[i].getBuffer(), sizeof(SkyUniforms));
    }
    if (cubeMapTexture && cubeMapTexture->getImageView()) { writeCubeMapDescriptors(); }
}

void SkyBox::uploadUniforms(uint32_t imageIndex, const SkyUniforms &uniforms)
{
    // The renderer's first upload runs before init().
    if (imageIndex >= uniformBuffers.size()) { return; }
    std::memcpy(uniformBuffers[imageIndex].getMappedData(), &uniforms, sizeof(SkyUniforms));
}

void SkyBox::loadCubeMap(vk::CommandPool commandPool)
{
    std::filesystem::path skybox_base_dir;
    if (const auto overridden = skyboxDirectoryOverride(); overridden.has_value()) {
        skybox_base_dir = *overridden;
    } else if (const auto resolved =
                 Kataglyphis::Shared::resolveResourceRelativePath("Textures/Skybox/kloofendal_43d_clear_puresky");
      resolved.has_value()) {
        skybox_base_dir = *resolved;
    } else {
        std::error_code current_path_ec;
        std::filesystem::path const cwd = std::filesystem::current_path(current_path_ec);
        skybox_base_dir = (current_path_ec ? std::filesystem::path(".") : cwd)
                          / "Resources/Textures/Skybox/kloofendal_43d_clear_puresky";
    }

    std::error_code exists_ec;
    if (!std::filesystem::is_directory(skybox_base_dir, exists_ec)) {
        // FETCH_ASSETS=OFF or a failed fetch: no faces is a supported configuration, not a broken one.
        spdlog::warn("SkyBox: no skybox faces in {}; drawing the analytic sky", skybox_base_dir.string());
        loadAnalyticSky(commandPool);
        return;
    }

    std::array<std::string, 6> skybox_textures = { "RT.png", "LF.png", "UP.png", "DN.png", "FT.png", "BK.png" };

    cubeMapTexture = std::make_unique<Texture>();

    int width = 0, height = 0, bit_depth = 0;
    std::vector<unsigned char *> face_data(6);
    int face_widths[6] = {};
    int face_heights[6] = {};

    for (size_t i = 0; i < 6; i++) {
        std::string path = (skybox_base_dir / skybox_textures[i]).string();
        face_data[i] = stbi_load(path.c_str(), &face_widths[i], &face_heights[i], &bit_depth, 4);
        if (!face_data[i]) {
            spdlog::error("Failed to load skybox texture: {}; drawing the analytic sky", path);
            for (size_t j = 0; j < i; j++) { stbi_image_free(face_data[j]); }
            loadAnalyticSky(commandPool);
            return;
        }
    }

    // The upload copies the first face's layerSize bytes out of every face, into a cube that must be square.
    if (!cubemapFacesConsistent(face_widths, face_heights)) {
        spdlog::error("Skybox faces are not six equal squares; drawing the analytic sky.");
        for (size_t j = 0; j < 6; j++) { stbi_image_free(face_data[j]); }
        loadAnalyticSky(commandPool);
        return;
    }

    width = face_widths[0];
    height = face_heights[0];

    spdlog::info("SkyBox: All 6 textures loaded, width={}, height={}", width, height);

    std::array<const unsigned char *, 6> faces = {
        face_data[0], face_data[1], face_data[2], face_data[3], face_data[4], face_data[5]
    };
    // A face past maxImageDimensionCube is invalid usage that aborts the engine, so it is halved until it fits.
    FittedCubemap fitted;
    const uint32_t face_limit = device->getMaxImageDimensionCube();
    if (static_cast<uint32_t>(width) > face_limit) {
        fitted = fitCubemapFacesToLimit(faces, static_cast<uint32_t>(width), face_limit);
        spdlog::warn("SkyBox: {} px faces exceed the device's {}-texel cube side; uploaded at {} px.",
          width,
          face_limit,
          fitted.size);
        width = static_cast<int>(fitted.size);
        height = width;
        const std::size_t layer = static_cast<std::size_t>(fitted.size) * fitted.size * 4U;
        for (std::size_t i = 0; i < faces.size(); ++i) { faces[i] = fitted.texels.data() + i * layer; }
    }
    // Uploaded as sRGB: the faces are sRGB-encoded PNGs and must decode to linear before sky.slang untonemaps them.
    const bool uploaded =
      uploadCubeMapFaces(commandPool, static_cast<uint32_t>(width), static_cast<uint32_t>(height), faces);
    if (uploaded) {
        cloudAmbient = cloudAmbientOfCubemap(faces, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }

    for (size_t i = 0; i < 6; i++) { stbi_image_free(face_data[i]); }

    if (!uploaded) {
        spdlog::error("SkyBox: cubemap upload failed; drawing the analytic sky.");
        loadAnalyticSky(commandPool);
        return;
    }

    analyticSky = false;
    writeCubeMapDescriptors();
}

void SkyBox::loadAnalyticSky(vk::CommandPool commandPool)
{
    // OxidANT's procedural sky baked into faces, so the drawn sky, the cloud ambient and the path tracer share one cubemap.
    if (!cubeMapTexture) { cubeMapTexture = std::make_unique<Texture>(); }
    const std::vector<unsigned char> texels = analyticSkyFaces(kAnalyticSkyFaceSize);
    const std::size_t layer = static_cast<std::size_t>(kAnalyticSkyFaceSize) * kAnalyticSkyFaceSize * 4U;
    const std::array<const unsigned char *, 6> faces = { texels.data(),
        texels.data() + layer,
        texels.data() + 2 * layer,
        texels.data() + 3 * layer,
        texels.data() + 4 * layer,
        texels.data() + 5 * layer };
    cloudAmbient = cloudAmbientOfCubemap(faces, kAnalyticSkyFaceSize, kAnalyticSkyFaceSize);
    analyticSky = true;
    if (!uploadCubeMapFaces(commandPool, kAnalyticSkyFaceSize, kAnalyticSkyFaceSize, faces)) {
        spdlog::error("SkyBox: analytic sky upload failed; leaving the cubemap descriptor unwritten.");
        return;
    }
    writeCubeMapDescriptors();
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
    cubeFaceSize = width;
    return true;
}

void SkyBox::writeCubeMapDescriptors()
{
    for (uint32_t i = 0; i < descriptors.sets().size(); ++i) {
        descriptors.writeImage(
          i, kSkyCubeMapBinding, cubeMapTexture->getImageView(), vk::ImageLayout::eShaderReadOnlyOptimal);
        descriptors.writeImage(
          i, kSkySamplerBinding, nullptr, vk::ImageLayout::eUndefined, cubeMapTexture->getSampler());
    }
}

void SkyBox::createGraphicsPipelines(vk::Format in_color_format, vk::Format in_depth_format)
{
    colorFormat = in_color_format;
    depthFormat = in_depth_format;

    // OxidANT's sky.slang, emitted by Build-SlangShaders.ps1; the paths resolve from the repo root.
    std::string const slang_spv_dir = "Resources/ShadersSlang/build/spirv/sky/";
    ShaderStagePair stages{ device, slang_spv_dir + "sky.vs_vulkan_main.spv", slang_spv_dir + "sky.fs_main.spv" };

    const std::array<vk::DescriptorSetLayout, 1> layouts = { descriptors.getLayout() };
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo = buildPipelineLayoutCreateInfo(layouts);
    pipelineLayout = vkCheck(
      device->getLogicalDevice().createPipelineLayout(pipelineLayoutInfo), "Failed to create sky pipeline layout!");

    // A fullscreen triangle from the vertex index: no vertex buffer.
    rasterPipeline = PipelineBuilder()
                       .setShaderStages({ stages.stages().begin(), stages.stages().end() })
                       .setVertexInput({}, {})
                       .setCullMode(vk::CullModeFlagBits::eNone)
                       .setDepthWrite(false)
                       .setDepthCompareOp(vk::CompareOp::eLessOrEqual)
                       .setRenderingFormats({ colorFormat }, depthFormat)
                       .setDebugName("raster sky")
                       .build(device->getLogicalDevice(),
                         pipelineLayout,
                         device->getPipelineCache(),
                         "Failed to create the raster sky pipeline!");
    // No depth to test: the traced target's alpha says where a primary ray hit.
    tracedPipeline = PipelineBuilder()
                       .setShaderStages({ stages.stages().begin(), stages.stages().end() })
                       .setVertexInput({}, {})
                       .setCullMode(vk::CullModeFlagBits::eNone)
                       .setFillUncovered(true)
                       .setDepthTest(false)
                       .setDepthWrite(false)
                       .setRenderingFormats({ colorFormat })
                       .setDebugName("traced sky")
                       .build(device->getLogicalDevice(),
                         pipelineLayout,
                         device->getPipelineCache(),
                         "Failed to create the traced sky pipeline!");
}

void SkyBox::destroyTracedPipeline()
{
    if (tracedPipeline) {
        device->getLogicalDevice().destroyPipeline(tracedPipeline);
        tracedPipeline = nullptr;
    }
}

void SkyBox::shaderHotReload()
{
    destroyTracedPipeline();
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), rasterPipeline, pipelineLayout);
    createGraphicsPipelines(colorFormat, depthFormat);
}

void SkyBox::recordDraw(vk::CommandBuffer commandBuffer, vk::Pipeline pipeline, uint32_t imageIndex) const
{
    if (!pipeline || imageIndex >= descriptors.sets().size()) {
        spdlog::error("SkyBox: no pipeline or no descriptor set for image {}; the sky stays undrawn", imageIndex);
        return;
    }
    commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    commandBuffer.bindDescriptorSets(
      vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, 1, &descriptors.sets()[imageIndex], 0, nullptr);
    commandBuffer.draw(3, 1, 0, 0);
}

void SkyBox::recordRasterDraw(vk::CommandBuffer commandBuffer, uint32_t imageIndex) const
{ recordDraw(commandBuffer, rasterPipeline, imageIndex); }

void SkyBox::recordTracedDraw(vk::CommandBuffer commandBuffer,
  uint32_t imageIndex,
  vk::Image target,
  vk::ImageView targetView,
  vk::Extent2D extent) const
{
    // The fill reads each pixel's alpha, so the trace's storage write must reach the colour attachment read.
    recordImageBarrier(commandBuffer,
      buildImageMemoryBarrier(target,
        vk::ImageLayout::eGeneral,
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
        vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite));

    const vk::RenderingAttachmentInfo color_attachment = buildRenderingAttachmentInfo(
      targetView, vk::ImageLayout::eColorAttachmentOptimal, vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore);
    commandBuffer.beginRendering(
      buildRenderingInfo(extent, std::span<const vk::RenderingAttachmentInfo>(&color_attachment, 1), nullptr));
    setFullExtentViewportAndScissor(commandBuffer, extent);
    recordDraw(commandBuffer, tracedPipeline, imageIndex);
    commandBuffer.endRendering();

    // The consumers are post.slang's fragment shader and the auto-exposure histogram's compute pass.
    recordImageBarrier(commandBuffer,
      buildImageMemoryBarrier(target,
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderSampledRead));
}

void SkyBox::cleanUp()
{
    // Idempotent: the destructor calls it again as a safety net.
    if (!device) { return; }

    destroyTracedPipeline();
    Kataglyphis::destroyPipelineAndLayout(device->getLogicalDevice(), rasterPipeline, pipelineLayout);
    descriptors.cleanUp();
    for (VulkanBuffer &buffer : uniformBuffers) { buffer.cleanUp(); }
    uniformBuffers.clear();
    if (cubeMapTexture) { cubeMapTexture->cleanUp(); }
    cubeMapTexture.reset();

    device.reset();
}

SkyBox::~SkyBox() { cleanUp(); }

}// namespace Kataglyphis