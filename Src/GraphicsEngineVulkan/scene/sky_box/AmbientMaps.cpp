module;

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "common/ImageBarrierHelper.hpp"
#include "common/PipelineLayoutHelper.hpp"
#include "common/RenderingHelper.hpp"
#include "common/Utilities.hpp"
#include "common/ViewportHelper.hpp"

module kataglyphis.vulkan.ambient_maps;

import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.shader_helper;
import kataglyphis.vulkan.pipeline_builder;
import kataglyphis.vulkan.command_buffer_manager;
import kataglyphis.vulkan.sampler_builder;
import kataglyphis.vulkan.color_attachment;

namespace Kataglyphis {

namespace {
    // ibl.slang's set 0; binding 1, the Rust bake's equirect source, is read by no entry this engine draws.
    constexpr uint32_t kParamsBinding = 0;
    constexpr uint32_t kSourceCubeBinding = 2;
    constexpr uint32_t kSourceSamplerBinding = 3;
    constexpr uint32_t kSunMaskBinding = 4;

    constexpr vk::Format kCubeFormat = vk::Format::eR16G16B16A16Sfloat;
    constexpr vk::Format kLutFormat = vk::Format::eR16G16Sfloat;

    const std::string kIblSpirvDir = "Resources/ShadersSlang/build/spirv/ibl/";

    // A 2D view of one face at one mip, the target one draw writes.
    vk::ImageView faceView(vk::Device device, vk::Image image, vk::Format format, uint32_t face, uint32_t mip)
    {
        const vk::ImageViewCreateInfo info{ {},
            image,
            vk::ImageViewType::e2D,
            format,
            vk::ComponentMapping{},
            vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, mip, 1, face, 1 } };
        return vkCheck(device.createImageView(info), "Failed to create an IBL face view!");
    }

    // A cube view of one mip: a downsample reads only its source mip while it writes the next.
    vk::ImageView cubeMipView(vk::Device device, vk::Image image, uint32_t mip)
    {
        const vk::ImageViewCreateInfo info{ {},
            image,
            vk::ImageViewType::eCube,
            kCubeFormat,
            vk::ComponentMapping{},
            vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, mip, 1, 0, 6 } };
        return vkCheck(device.createImageView(info), "Failed to create an IBL cube view!");
    }

    vk::ImageMemoryBarrier2 toTarget(vk::Image image, uint32_t mips, uint32_t layers)
    {
        return buildImageMemoryBarrier(image,
          vk::ImageLayout::eUndefined,
          vk::ImageLayout::eColorAttachmentOptimal,
          vk::PipelineStageFlagBits2::eFragmentShader,
          vk::AccessFlagBits2::eShaderSampledRead,
          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
          vk::AccessFlagBits2::eColorAttachmentWrite,
          vk::ImageAspectFlagBits::eColor,
          0,
          mips,
          0,
          layers);
    }

    // Written mips become sampled; the last bake barrier also covers the scene's ray-tracing stages.
    vk::ImageMemoryBarrier2 toSampled(vk::Image image, uint32_t baseMip, uint32_t mips, uint32_t layers)
    {
        return buildImageMemoryBarrier(image,
          vk::ImageLayout::eColorAttachmentOptimal,
          vk::ImageLayout::eShaderReadOnlyOptimal,
          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
          vk::AccessFlagBits2::eColorAttachmentWrite,
          vk::PipelineStageFlagBits2::eAllCommands,
          vk::AccessFlagBits2::eShaderSampledRead,
          vk::ImageAspectFlagBits::eColor,
          baseMip,
          mips,
          0,
          layers);
    }
}// namespace

AmbientMaps::AmbientMaps() = default;

AmbientMaps::~AmbientMaps() { cleanUp(); }

void AmbientMaps::init(const std::shared_ptr<VulkanDevice> &in_device, vk::CommandPool in_command_pool)
{
    device = in_device;
    commandPool = in_command_pool;
    createMaps();
    mapsCreated = clearMaps();
    if (!mapsCreated) { spdlog::error("AmbientMaps: the maps could not be zeroed; set 0 leaves them out."); }

    const vk::DeviceSize alignment = device->getPhysicalDevice().getProperties().limits.minUniformBufferOffsetAlignment;
    paramsStride = (sizeof(AmbientBakeParams) + alignment - 1) / alignment * alignment;
    params.create(device,
      paramsStride * kAmbientBakeDraws,
      vk::BufferUsageFlagBits::eUniformBuffer,
      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

    descriptors.addBinding(kParamsBinding, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment)
      .addBinding(kSourceCubeBinding, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment)
      .addBinding(kSourceSamplerBinding, vk::DescriptorType::eSampler, 1, vk::ShaderStageFlagBits::eFragment)
      .addBinding(kSunMaskBinding, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment);
    if (!descriptors.create(device, kAmbientBakeDraws)) {
        spdlog::error("AmbientMaps: no descriptor sets; the scene keeps the analytic ambient.");
        return;
    }
    createPipelines();
}

void AmbientMaps::createMaps()
{
    const auto cube = [this](Texture &texture, uint32_t size, uint32_t mips) {
        texture.createImage(device,
          size,
          size,
          mips,
          kCubeFormat,
          vk::ImageTiling::eOptimal,
          vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
            | vk::ImageUsageFlagBits::eTransferDst,
          vk::MemoryPropertyFlagBits::eDeviceLocal,
          6,
          vk::ImageCreateFlagBits::eCubeCompatible);
        // clang-format off: BuildIntegrity wants the call, its eColor aspect and the marker on one line.
        texture.createImageView(device, kCubeFormat, vk::ImageAspectFlagBits::eColor, mips, vk::ImageViewType::eCube, 6);// COLOR_ATTACHMENT_CHAIN_OK: ibl-cube-six-layers-cube-compatible
        // clang-format on
        texture.createTextureSampler(device, vk::Filter::eLinear, vk::SamplerAddressMode::eClampToEdge);
    };
    cube(environment, kAmbientEnvironmentSize, kAmbientEnvironmentMips);
    cube(irradiance, kAmbientIrradianceSize, 1);
    cube(prefiltered, kAmbientPrefilterSize, kAmbientPrefilterMips);
    VulkanRendererInternals::createColorAttachment(sunMask,
      device,
      vk::Extent2D{ 2, 1 },
      kCubeFormat,
      vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
        | vk::ImageUsageFlagBits::eTransferDst,
      "IBL sun mask");

    brdfLut.createImage(device,
      kAmbientBrdfLutSize,
      kAmbientBrdfLutSize,
      1,
      kLutFormat,
      vk::ImageTiling::eOptimal,
      vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
        | vk::ImageUsageFlagBits::eTransferDst,
      vk::MemoryPropertyFlagBits::eDeviceLocal);
    // clang-format off: BuildIntegrity wants the call, its eColor aspect and the marker on one line.
    brdfLut.createImageView(device, kLutFormat, vk::ImageAspectFlagBits::eColor, 1);// COLOR_ATTACHMENT_CHAIN_OK: ibl-brdf-table-sampled-by-every-pass
    // clang-format on
    brdfLut.createTextureSampler(device, vk::Filter::eLinear, vk::SamplerAddressMode::eClampToEdge);

    // Every source mip is sampled at an explicit level, so one sampler reaches all of them.
    const vk::SamplerCreateInfo sampler_info = buildSamplerCreateInfo(vk::Filter::eLinear,
      vk::SamplerAddressMode::eClampToEdge,
      static_cast<float>(kAmbientEnvironmentMips),
      VK_FALSE,
      1.0F,
      vk::BorderColor::eFloatOpaqueBlack);
    bakeSampler = vkCheck(device->getLogicalDevice().createSampler(sampler_info), "Failed to create the IBL sampler!");
}

bool AmbientMaps::clearMaps()
{
    const vk::Device logical = device->getLogicalDevice();
    vk::CommandBuffer commandBuffer =
      VulkanRendererInternals::CommandBufferManager::beginCommandBuffer(logical, commandPool);
    if (!commandBuffer) { return false; }

    struct Map
    {
        vk::Image image;
        uint32_t mips;
        uint32_t layers;
    };
    const std::array<Map, 5> maps = { Map{ environment.getImage(), kAmbientEnvironmentMips, 6 },
        Map{ irradiance.getImage(), 1, 6 },
        Map{ prefiltered.getImage(), kAmbientPrefilterMips, 6 },
        Map{ brdfLut.getImage(), 1, 1 },
        Map{ sunMask.getImage(), 1, 1 } };
    std::array<vk::ImageMemoryBarrier2, 5> barriers{};
    for (std::size_t i = 0; i < maps.size(); ++i) {
        barriers[i] = buildImageMemoryBarrier(maps[i].image,
          vk::ImageLayout::eUndefined,
          vk::ImageLayout::eTransferDstOptimal,
          vk::PipelineStageFlagBits2::eNone,
          vk::AccessFlagBits2::eNone,
          vk::PipelineStageFlagBits2::eClear,
          vk::AccessFlagBits2::eTransferWrite,
          vk::ImageAspectFlagBits::eColor,
          0,
          maps[i].mips,
          0,
          maps[i].layers);
    }
    recordImageBarriers(commandBuffer, barriers);
    const vk::ClearColorValue zero{ std::array<float, 4>{ 0.0F, 0.0F, 0.0F, 0.0F } };
    for (const Map &map : maps) {
        const vk::ImageSubresourceRange range{ vk::ImageAspectFlagBits::eColor, 0, map.mips, 0, map.layers };
        commandBuffer.clearColorImage(map.image, vk::ImageLayout::eTransferDstOptimal, zero, range);
    }
    for (std::size_t i = 0; i < maps.size(); ++i) {
        barriers[i] = buildImageMemoryBarrier(maps[i].image,
          vk::ImageLayout::eTransferDstOptimal,
          vk::ImageLayout::eShaderReadOnlyOptimal,
          vk::PipelineStageFlagBits2::eClear,
          vk::AccessFlagBits2::eTransferWrite,
          vk::PipelineStageFlagBits2::eAllCommands,
          vk::AccessFlagBits2::eShaderSampledRead,
          vk::ImageAspectFlagBits::eColor,
          0,
          maps[i].mips,
          0,
          maps[i].layers);
    }
    recordImageBarriers(commandBuffer, barriers);
    const bool submitted = VulkanRendererInternals::CommandBufferManager::endAndSubmitCommandBuffer(
      logical, commandPool, device->getGraphicsQueue(), commandBuffer);
    return submitted;
}

void AmbientMaps::createPipelines()
{
    const std::array<vk::DescriptorSetLayout, 1> layouts = { descriptors.getLayout() };
    const vk::PipelineLayoutCreateInfo layout_info = buildPipelineLayoutCreateInfo(layouts);
    pipelineLayout = vkCheck(
      device->getLogicalDevice().createPipelineLayout(layout_info), "Failed to create the IBL pipeline layout!");

    const auto build = [this](const char *entry, vk::Format format, const char *name) {
        ShaderStagePair stages{
            device, kIblSpirvDir + "ibl.vs_vulkan_fullscreen.spv", kIblSpirvDir + "ibl." + entry + ".spv"
        };
        return PipelineBuilder()
          .setShaderStages({ stages.stages().begin(), stages.stages().end() })
          .setVertexInput({}, {})
          .setCullMode(vk::CullModeFlagBits::eNone)
          .setDepthTest(false)
          .setDepthWrite(false)
          .setRenderingFormats({ format })
          .setDebugName(name)
          .build(device->getLogicalDevice(),
            pipelineLayout,
            device->getPipelineCache(),
            "Failed to create an IBL pipeline!");
    };
    skyboxToCube = build("fs_skybox_to_cube", kCubeFormat, "IBL skybox to cube");
    findSun = build("fs_find_sun", kCubeFormat, "IBL sun search");
    downsample = build("fs_downsample_cube", kCubeFormat, "IBL downsample");
    irradianceFilter = build("fs_irradiance", kCubeFormat, "IBL irradiance");
    prefilter = build("fs_prefilter", kCubeFormat, "IBL prefilter");
    brdfTable = build("fs_brdf_lut", kLutFormat, "IBL BRDF table");
}

void AmbientMaps::destroyFilterPipelines()
{
    const vk::Device logical = device->getLogicalDevice();
    for (vk::Pipeline *pipeline : { &skyboxToCube, &findSun, &downsample, &irradianceFilter, &prefilter }) {
        if (*pipeline) {
            logical.destroyPipeline(*pipeline);
            *pipeline = nullptr;
        }
    }
}

bool AmbientMaps::bake(Texture &skybox, uint32_t faceSize)
{
    bakedOnce = false;
    if (!device || descriptors.sets().size() < kAmbientBakeDraws || !skyboxToCube || !skybox.getImageView()) {
        spdlog::warn("AmbientMaps: nothing to bake with; the scene keeps the analytic ambient.");
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    const vk::Device logical = device->getLogicalDevice();
    std::vector<vk::ImageView> transientViews;
    auto *mapped = static_cast<char *>(params.getMappedData());
    uint32_t drawIndex = 0;

    vk::CommandBuffer commandBuffer =
      VulkanRendererInternals::CommandBufferManager::beginCommandBuffer(logical, commandPool);
    if (!commandBuffer) {
        spdlog::error("AmbientMaps: no command buffer; the scene keeps the analytic ambient.");
        return false;
    }

    // One fullscreen triangle into target, reading source (and the sun mask); each draw has its own params slot and set.
    const auto drawInto = [&](vk::Pipeline pipeline,
                            vk::ImageView target,
                            vk::Extent2D extent,
                            vk::ImageView source,
                            const AmbientBakeParams &values,
                            vk::ImageView mask) {
        const uint32_t set = drawIndex++;
        std::memcpy(mapped + set * paramsStride, &values, sizeof(values));
        descriptors.writeBuffer(set, kParamsBinding, params.getBuffer(), sizeof(AmbientBakeParams), set * paramsStride);
        if (source) {
            descriptors.writeImage(set, kSourceCubeBinding, source, vk::ImageLayout::eShaderReadOnlyOptimal);
        }
        descriptors.writeImage(set, kSourceSamplerBinding, nullptr, vk::ImageLayout::eUndefined, bakeSampler);
        if (mask) { descriptors.writeImage(set, kSunMaskBinding, mask, vk::ImageLayout::eShaderReadOnlyOptimal); }

        const std::array<vk::RenderingAttachmentInfo, 1> colour = { buildRenderingAttachmentInfo(
          target, vk::ImageLayout::eColorAttachmentOptimal, vk::AttachmentLoadOp::eDontCare) };
        commandBuffer.beginRendering(buildRenderingInfo(extent, colour, nullptr));
        setFullExtentViewportAndScissor(commandBuffer, extent);
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        commandBuffer.bindDescriptorSets(
          vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, 1, &descriptors.sets()[set], 0, nullptr);
        commandBuffer.draw(3, 1, 0, 0);
        commandBuffer.endRendering();
    };
    const auto draw = [&](vk::Pipeline pipeline,
                        vk::ImageView target,
                        uint32_t size,
                        vk::ImageView source,
                        const AmbientBakeParams &values) {
        drawInto(pipeline, target, vk::Extent2D{ size, size }, source, values, nullptr);
    };
    const auto face = [&](Texture &texture, uint32_t index, uint32_t mip) {
        transientViews.push_back(faceView(logical, texture.getImage(), kCubeFormat, index, mip));
        return transientViews.back();
    };

    // The table depends on nothing but the BRDF; baked with the rest, so a reload rebakes it too.
    {
        const std::array<vk::ImageMemoryBarrier2, 4> barriers = { toTarget(brdfLut.getImage(), 1, 1),
            toTarget(environment.getImage(), kAmbientEnvironmentMips, 6),
            toTarget(irradiance.getImage(), 1, 6),
            toTarget(prefiltered.getImage(), kAmbientPrefilterMips, 6) };
        recordImageBarriers(commandBuffer, barriers);
    }
    draw(brdfTable, brdfLut.getImageView(), kAmbientBrdfLutSize, nullptr, AmbientBakeParams{});

    // The drawn skybox's radiance at mip 0, then each mip from the one above; masked leaves out the sun found.
    const float faceTexels = static_cast<float>(faceSize);
    const auto environmentPasses = [&](bool masked) {
        const float leaveOutSun = masked ? 1.0F : 0.0F;
        for (uint32_t f = 0; f < 6; ++f) {
            drawInto(skyboxToCube,
              face(environment, f, 0),
              vk::Extent2D{ kAmbientEnvironmentSize, kAmbientEnvironmentSize },
              skybox.getImageView(),
              AmbientBakeParams{ { static_cast<float>(f), 0.0F, 0.0F, 0.0F }, { faceTexels, leaveOutSun, 0.0F, 0.0F } },
              sunMask.getImageView());
        }
        for (uint32_t mip = 1; mip < kAmbientEnvironmentMips; ++mip) {
            recordImageBarrier(commandBuffer, toSampled(environment.getImage(), mip - 1, 1, 6));
            transientViews.push_back(cubeMipView(logical, environment.getImage(), mip - 1));
            const vk::ImageView source = transientViews.back();
            const float sourceTexels = static_cast<float>(kAmbientEnvironmentSize >> (mip - 1));
            for (uint32_t f = 0; f < 6; ++f) {
                draw(downsample,
                  face(environment, f, mip),
                  kAmbientEnvironmentSize >> mip,
                  source,
                  AmbientBakeParams{ { static_cast<float>(f), 0.0F, 0.0F, 0.0F }, { sourceTexels, 0.0F, 0.0F, 0.0F } });
            }
        }
        recordImageBarrier(commandBuffer, toSampled(environment.getImage(), kAmbientEnvironmentMips - 1, 1, 6));
    };
    // The search reads the whole unmasked chain, so the masked environment is drawn again over it.
    environmentPasses(false);
    recordImageBarrier(commandBuffer, toTarget(sunMask.getImage(), 1, 1));
    drawInto(
      findSun, sunMask.getImageView(), vk::Extent2D{ 2, 1 }, environment.getImageView(), AmbientBakeParams{}, nullptr);
    {
        const std::array<vk::ImageMemoryBarrier2, 2> barriers = { toSampled(sunMask.getImage(), 0, 1, 1),
            toTarget(environment.getImage(), kAmbientEnvironmentMips, 6) };
        recordImageBarriers(commandBuffer, barriers);
    }
    environmentPasses(true);

    const float environmentTexels = static_cast<float>(kAmbientEnvironmentSize);
    for (uint32_t f = 0; f < 6; ++f) {
        draw(irradianceFilter,
          face(irradiance, f, 0),
          kAmbientIrradianceSize,
          environment.getImageView(),
          AmbientBakeParams{ { static_cast<float>(f), 0.0F, 0.0F, 0.0F }, { environmentTexels, 0.0F, 0.0F, 0.0F } });
    }
    for (uint32_t mip = 0; mip < kAmbientPrefilterMips; ++mip) {
        const float roughness = static_cast<float>(mip) / static_cast<float>(kAmbientPrefilterMips - 1);
        for (uint32_t f = 0; f < 6; ++f) {
            draw(prefilter,
              face(prefiltered, f, mip),
              kAmbientPrefilterSize >> mip,
              environment.getImageView(),
              AmbientBakeParams{
                { static_cast<float>(f), roughness, static_cast<float>(kAmbientPrefilterSamples), 0.0F },
                { environmentTexels, 0.0F, 0.0F, 0.0F } });
        }
    }

    {
        const std::array<vk::ImageMemoryBarrier2, 3> barriers = { toSampled(brdfLut.getImage(), 0, 1, 1),
            toSampled(irradiance.getImage(), 0, 1, 6),
            toSampled(prefiltered.getImage(), 0, kAmbientPrefilterMips, 6) };
        recordImageBarriers(commandBuffer, barriers);
    }

    const bool submitted = VulkanRendererInternals::CommandBufferManager::endAndSubmitCommandBuffer(
      logical, commandPool, device->getGraphicsQueue(), commandBuffer);
    for (vk::ImageView view : transientViews) { logical.destroyImageView(view); }
    if (!submitted) {
        spdlog::error("AmbientMaps: the bake did not submit; the scene keeps the analytic ambient.");
        return false;
    }
    bakedOnce = true;
    spdlog::info("AmbientMaps: baked in {} ms.",
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    return true;
}

void AmbientMaps::shaderHotReload(Texture &skybox, uint32_t faceSize)
{
    // Without the sets init built no pipeline, and the layout would name a set layout that does not exist.
    if (!device || descriptors.sets().size() < kAmbientBakeDraws) { return; }
    destroyFilterPipelines();
    destroyPipelineAndLayout(device->getLogicalDevice(), brdfTable, pipelineLayout);
    createPipelines();
    (void)bake(skybox, faceSize);
}

void AmbientMaps::cleanUp()
{
    if (!device) { return; }
    destroyFilterPipelines();
    destroyPipelineAndLayout(device->getLogicalDevice(), brdfTable, pipelineLayout);
    descriptors.cleanUp();
    params.cleanUp();
    if (bakeSampler) {
        device->getLogicalDevice().destroySampler(bakeSampler);
        bakeSampler = nullptr;
    }
    brdfLut.cleanUp();
    sunMask.cleanUp();
    prefiltered.cleanUp();
    irradiance.cleanUp();
    environment.cleanUp();
    mapsCreated = false;
    bakedOnce = false;
    device = nullptr;
}

}// namespace Kataglyphis
