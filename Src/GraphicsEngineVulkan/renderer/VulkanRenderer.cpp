module;
#include "common/FrameSubmitHelper.hpp"
#include "common/GuiModelTransform.hpp"
#include "common/ImageBarrierHelper.hpp"
#include "common/SceneUboMarshal.hpp"
#include "common/Utilities.hpp"
#include "common/host_device_shared_vars.hpp"
#include "renderer/PathTracingHistory.hpp"
#include "renderer/SwapChainDetails.hpp"
#include "renderer/pushConstants/PushConstantPost.hpp"
#include "renderer/pushConstants/PushConstantRasterizer.hpp"
#include "renderer/pushConstants/PushConstantRayTracing.hpp"
#include "scene/sky_box/SkyUniforms.hpp"
#include "spdlog/spdlog.h"
#include <optional>

#include <cstdint>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <limits>
#include <vulkan/vulkan.hpp>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN

#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#ifndef VMA_IMPLEMENTATION
#define VMA_IMPLEMENTATION
#endif// !VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <imgui.h>
#include <imgui_internal.h>

#include "common/Globals.hpp"
#include "renderer/SceneUBO.hpp"

module kataglyphis.vulkan.renderer;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.object_description;
import kataglyphis.vulkan.queue_family_indices;
import kataglyphis.vulkan.debug;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.scene_config;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.as_manager;
import kataglyphis.vulkan.buffer_manager;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.command_buffer_manager;
import kataglyphis.vulkan.descriptor_set_group;
import kataglyphis.vulkan.instance;
import kataglyphis.vulkan.render_settings;
import kataglyphis.vulkan.render_stage_registry;
import kataglyphis.vulkan.scene_ubo;
import kataglyphis.vulkan.global_ubo;
import kataglyphis.vulkan.swapchain;
import kataglyphis.vulkan.window;
import kataglyphis.vulkan.color_attachment;

namespace {

// Not transitionImageLayout's eGeneral overload: it derives eAllCommands -> eAllCommands, a full stall every frame.
void recordGeneralImageBarrier(vk::CommandBuffer commandBuffer,
  vk::Image image,
  vk::PipelineStageFlags2 srcStage,
  vk::PipelineStageFlags2 dstStage,
  vk::AccessFlags2 srcAccess,
  vk::AccessFlags2 dstAccess)
{
    Kataglyphis::recordImageBarrier(commandBuffer,
      Kataglyphis::buildImageMemoryBarrier(
        image, vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral, srcStage, srcAccess, dstStage, dstAccess));
}

}// namespace

Kataglyphis::VulkanRenderer::VulkanRenderer(Kataglyphis::Frontend::Window *window,
  Scene *scene,
  Camera *camera,
  const RenderSettings &settings,
  RendererFeedback &feedback)
  : window(window), scene(scene), camera(camera), frameSettings(settings)
{
    instance = std::make_shared<VulkanInstance>();
    device = std::make_shared<VulkanDevice>(instance, create_surface());

    create_command_pool();

    vulkanSwapChain.initVulkanContext(device, window, device->getSurface(), settings.renderer.vsync);
    gpuTiming.create(*device, vulkanSwapChain.getNumberSwapChainImages(), output.gpuTimings);
    create_uniform_buffers();
    create_command_buffers();

    createSynchronization();

    initDescriptorResources();

    std::array<vk::DescriptorSetLayout, 1> const descriptor_set_layouts_rasterizer = {
        sharedRenderDescriptors.getLayout()
    };
    std::array<vk::DescriptorSetLayout, 2> const descriptor_set_layouts_deferred = {
        sharedRenderDescriptors.getLayout(), gbufferDescriptors.getLayout()
    };

    rasterizer.init(device, &vulkanSwapChain, descriptor_set_layouts_rasterizer, graphics_command_pool.get());
    deferredRasterizer.init(device, &vulkanSwapChain, descriptor_set_layouts_deferred);

    clouds.init(device,
      graphics_command_pool.get(),
      sharedRenderDescriptors.getLayout(),
      vulkanSwapChain.getSwapChainExtent().width,
      vulkanSwapChain.getSwapChainExtent().height);
    // Same path as later shadow-setting changes, so the settings' default is the one source of truth.
    reinitShadowMapForCurrentSettings();
    reinitPunctualShadows();

    std::array<vk::DescriptorSetLayout, 1> const descriptor_set_layouts_post = { postDescriptors.getLayout() };
    postStage.init(device, &vulkanSwapChain, descriptor_set_layouts_post);
    fxaaStage.init(device, &vulkanSwapChain);

    if (device->supportsHardwareAcceleratedRRT()) {
        createRaytracingDescriptorResources();

        std::array<vk::DescriptorSetLayout, 2> const layouts = { sharedRenderDescriptors.getLayout(),
            raytracingDescriptors.getLayout() };
        raytracingStage.init(device, layouts, &vulkanSwapChain);
        pathTracing.init(device, layouts);
        createPathTracingAccumulationResources();
        createTracedDepthResources();
    }

    updateUniforms(scene, camera, settings);
    updateAllDescriptorSets();

    skyBox.init(device, graphics_command_pool.get(), vulkanSwapChain.getNumberSwapChainImages());
    // One raster sky pipeline serves both paths, so their depth buffers must share a format.
    if (rasterizer.getDepthFormat() != deferredRasterizer.getDepthFormat()) {
        spdlog::critical("The forward and deferred depth formats differ; the sky has one raster pipeline.");
        std::abort();
    }
    skyBox.createGraphicsPipelines(
      Kataglyphis::VulkanRendererInternals::Rasterizer::OFFSCREEN_FORMAT, rasterizer.getDepthFormat());
    ambientMaps.init(device, graphics_command_pool.get());
    if (Kataglyphis::Texture *cube_map = skyBox.getCubeMapTexture(); cube_map != nullptr) {
        (void)ambientMaps.bake(*cube_map, skyBox.getCubeFaceSize());
    }

    registerBuiltInStages();

    // Anything depending on scene contents waits for finishModelLoad(), on the frame the model arrives.
    scene->beginModelLoadAsync();

    // Descriptors need valid contents before the first frame, while the model still loads.
    create_object_description_buffer();
    updateAllDescriptorSets();
    bindCloudSceneDepth();

    feedback = output;
}

void Kataglyphis::VulkanRenderer::updateUniforms([[maybe_unused]] Scene *scene_data,
  Camera *camera_data,
  const RenderSettings &settings)
{
    const GUISceneSharedVars &sceneSettings = settings.scene;
    const vk::Extent2D extent = vulkanSwapChain.getSwapChainExtent();
    // The docks cover the image's edges, so aspect and centre come from the part the scene shows through.
    const auto &scene_viewport = settings.renderer.scene_viewport;
    const ViewportFraction viewport =
      sanitizeViewportFraction(scene_viewport.x0, scene_viewport.y0, scene_viewport.x1, scene_viewport.y1);
    float const aspect_ratio = viewportAspectRatio(extent.width, extent.height, viewport);

    // Clamped here: tests and config loads write the settings too. Readers use get_fov(), not the settings value.
    camera_data->set_fov(std::clamp(sceneSettings.camera_fov, kMinCameraFov, kMaxCameraFov));

    globalUBO.view = camera_data->calculate_viewmatrix();
    globalUBO.projection =
      makeViewportRemap(viewport)
      * makeVulkanProjection(
        camera_data->get_fov(), aspect_ratio, camera_data->get_near_plane(), camera_data->get_far_plane());

    fillSceneUboCamera(sceneUBO, camera_data->get_camera_position(), camera_data->get_camera_direction());

    fillSceneUboDirectionalLight(sceneUBO,
      glm::vec3(sceneSettings.directional_light_direction[0],
        sceneSettings.directional_light_direction[1],
        sceneSettings.directional_light_direction[2]),
      glm::vec3(sceneSettings.directional_light_color[0],
        sceneSettings.directional_light_color[1],
        sceneSettings.directional_light_color[2]),
      sceneSettings.directional_light_radiance,
      sceneSettings.directional_light_ambient);

    // Populate the settings into SceneUBO
    sceneUBO.pcfRadius = clampPcfRadius(sceneSettings.pcf_radius);
    sceneUBO.cascadedShadowIntensity = sceneSettings.cascaded_shadow_intensity;
    // The punctual_shadows stage's own condition, less the traced frames, which never read it.
    sceneUBO.punctualShadowCount = sceneSettings.punctual_shadows_enabled ? punctualShadows.slotCount() : 0U;

    // Calculate CSM cascades
    dirShadowMap.updateCascades(globalUBO.view,
      camera_data->get_fov(),
      aspect_ratio,
      camera_data->get_near_plane(),
      camera_data->get_far_plane(),
      glm::vec3(sceneUBO.dirLight.direction),
      sceneSettings.shadow_distance,
      sceneSettings.cascade_split_lambda);

    // Inverses for the clouds compute shader (see GlobalUBO.hpp).
    globalUBO.inv_projection = glm::inverse(globalUBO.projection);
    globalUBO.inv_view = glm::inverse(globalUBO.view);

    const auto &cascadeData = dirShadowMap.getCascadeData();
    const size_t active_cascades = std::min(cascadeData.size(), static_cast<size_t>(CASCADE_COUNT));
    std::array<float, CASCADE_COUNT> cascadeSplitDepths{};
    std::array<glm::mat4, CASCADE_COUNT> cascadeViewProjMatrices{};
    for (size_t i = 0; i < active_cascades; ++i) {
        cascadeSplitDepths[i] = cascadeData[i].splitDepth;
        cascadeViewProjMatrices[i] = cascadeData[i].viewProjMatrix;
    }
    fillSceneUboCascades(sceneUBO,
      std::span<const float>(cascadeSplitDepths).first(active_cascades),
      std::span<const glm::mat4>(cascadeViewProjMatrices).first(active_cascades),
      sceneSettings.shadows_enabled);

    fillSceneUboClouds(sceneUBO,
      glm::vec3(
        sceneSettings.cloud_mesh_scale[0], sceneSettings.cloud_mesh_scale[1], sceneSettings.cloud_mesh_scale[2]),
      sceneSettings.cloud_density_multiplier,
      glm::vec3(
        sceneSettings.cloud_mesh_offset[0], sceneSettings.cloud_mesh_offset[1], sceneSettings.cloud_mesh_offset[2]),
      sceneSettings.cloud_coverage_threshold,
      sceneSettings.cloud_num_march_steps,
      sceneSettings.cloud_num_march_steps_to_light,
      sceneSettings.cloud_pillowness,
      sceneSettings.cloud_cirrus_effect,
      sceneSettings.cloud_powder_effect,
      cloudWindOffset(glm::vec3(sceneSettings.cloud_wind[0], sceneSettings.cloud_wind[1], sceneSettings.cloud_wind[2]),
        settings.time_seconds));
    fillSceneUboCloudAmbient(sceneUBO, sceneSettings.skybox_enabled, skyBox.getCloudAmbient());
    fillSceneUboAmbientMaps(
      sceneUBO, sceneSettings.skybox_enabled, ambientMaps.baked(), ambientMaps.prefilteredTopMip());
}

auto Kataglyphis::VulkanRenderer::supportsHardwareRaytracing() const -> bool
{ return device && device->supportsHardwareAcceleratedRRT(); }

void Kataglyphis::VulkanRenderer::finishModelLoad() { refreshAfterSceneChange(true); }

void Kataglyphis::VulkanRenderer::refreshAfterSceneChange(bool rebuildBottomLevel)
{
    // Frames before new geometry drew another scene: about 240 sky-only frames once outweighed six of Dinosaurs.
    if (rebuildBottomLevel) { gpuTiming.restart(output.gpuTimings); }
    // Skipping any step leaves the model invisible or traced through stale descriptors.
    rebuildObjectDescriptions();

    // Before updateAllDescriptorSets, which binds the new TLAS and resets the path-tracing history.
    if (device->supportsHardwareAcceleratedRRT()) {
        if (rebuildBottomLevel) {
            asManager.createASForScene(device, graphics_command_pool.get(), scene);
        } else {
            asManager.createTLAS(device, graphics_command_pool.get(), scene);
        }
    }

    updateAllDescriptorSets();
}

void Kataglyphis::VulkanRenderer::updateStateDueToUserInput(const RenderSettings &settings, RendererFeedback &feedback)
{
    frameSettings = settings;
    output.publishedModelIndex.reset();
    output.publishedSun.reset();
    output.publishedShadowDistance.reset();
    output.publishedFov.reset();
    const GUISceneSharedVars &sceneSettings = settings.scene;

    // Polled first, so the rest of the frame sees a model on the frame it arrives.
    bool published_load = false;
    const bool load_was_pending = scene->isModelLoadPending();
    if (scene->pollModelLoad(device, graphics_command_pool.get())) {
        finishModelLoad();
        // A reload requested this frame replaces the model below and publishes its own.
        if (!sceneSettings.model_reload_requested) {
            publishLoadedModel();
            published_load = true;
        }
    } else if (load_was_pending && !scene->isModelLoadPending()) {
        // A failed load leaves no model, so no credit may outlive the scene it named.
        output.loadedModelCredit.clear();
    }

    handleShaderHotReloadRequest(settings.renderer);
    handleRasterizationModeChange(settings.renderer);
    handleVsyncChange(settings.renderer);

    handleShadowResolutionChange(sceneSettings);
    // The load just published resets the inspector, so its pending transform belongs to the replaced model.
    if (!published_load) { handleModelTransformChange(sceneSettings); }
    handleModelReloadRequest(sceneSettings);

    // Last: the loads above decide whether a TLAS exists, and with it what this frame draws.
    output.tracingReady = device->supportsHardwareAcceleratedRRT() && asManager.hasTLAS();
    handleCloudDepthSourceChange();
    feedback = output;
}

void Kataglyphis::VulkanRenderer::publishLoadedModel()
{
    // sceneConfig::getModelMatrix() is the identity, and both loads place the model with it.
    output.publishedModelIndex =
      sceneConfig::modelIndexOf(sceneConfig::getAvailableModelPaths(), scene->getLoadedModelPath());
    output.loadedModelPath = scene->getLoadedModelPath();
    // The Sun is a glTF's first directional light, which no longer reaches the punctual-light rows.
    output.publishedSun.reset();
    // A glTF light points the way it shines; the editor stores the way towards it.
    if (const auto sun = scene->getLoadedModelSun(); sun.has_value()) {
        output.publishedSun = Kataglyphis::VulkanRendererInternals::FrontendShared::PublishedSun{
            { -sun->direction.x, -sun->direction.y, -sun->direction.z }, { sun->color.r, sun->color.g, sun->color.b }
        };
    }
    const SceneMetadata metadata = scene->getLoadedModelMetadata();
    output.loadedModelCredit = metadata.credit;
    const CameraPose &fixed = camera->fixed_pose();
    const SceneFraming framing = frameScene(metadata, fixed);
    if (const std::optional<CameraPose> start = startPose(framing, fixed, lastLoadFramed); start.has_value()) {
        camera->apply_pose(*start);
        output.publishedShadowDistance = start->shadowDistance;
        output.publishedFov = start->fov;
    }
    lastLoadFramed = framing.framed;
}

auto Kataglyphis::VulkanRenderer::cloudDepthSourceForThisFrame() const -> CloudDepthSource
{
    if (drawnRenderMode() != 0) { return CloudDepthSource::Traced; }
    return frameSettings.renderer.rasterizationMode
               == Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode::Forward
             ? CloudDepthSource::ForwardRaster
             : CloudDepthSource::DeferredRaster;
}

void Kataglyphis::VulkanRenderer::bindCloudSceneDepth()
{
    const CloudDepthSource source = cloudDepthSourceForThisFrame();
    if (source == CloudDepthSource::Traced) {
        clouds.setSceneDepth(tracedDepth.getImageView(), vk::ImageLayout::eGeneral);
    } else if (source == CloudDepthSource::ForwardRaster) {
        clouds.setSceneDepth(rasterizer.getDepthImageView(), vk::ImageLayout::eShaderReadOnlyOptimal);
    } else {
        clouds.setSceneDepth(deferredRasterizer.getDepthBufferImageView(), vk::ImageLayout::eShaderReadOnlyOptimal);
    }
    boundCloudDepthSource = source;
}

void Kataglyphis::VulkanRenderer::handleCloudDepthSourceChange()
{
    if (cloudDepthSourceForThisFrame() == boundCloudDepthSource) { return; }
    // The clouds' one descriptor set may still be in flight; a mode switch is rare enough for a stall.
    (void)device->getLogicalDevice().waitIdle();
    bindCloudSceneDepth();
}

void Kataglyphis::VulkanRenderer::handleShaderHotReloadRequest(
  const Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &rendererSettings)
{
    if (rendererSettings.shader_hot_reload_triggered) { shaderHotReload(); }
}

void Kataglyphis::VulkanRenderer::handleRasterizationModeChange(
  const Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &rendererSettings)
{
    // The post pass's input image depends on the mode; waitIdle is fine for a rare UI event.
    if (rendererSettings.rasterizationMode != lastBoundRasterizationMode) {
        (void)device->getLogicalDevice().waitIdle();
        updateAllDescriptorSets();
        lastBoundRasterizationMode = rendererSettings.rasterizationMode;
    }
}

std::vector<vk::PresentModeKHR> Kataglyphis::VulkanRenderer::offeredPresentModes() const
{ return device->getSwapchainDetails().presentation_mode; }

void Kataglyphis::VulkanRenderer::handleVsyncChange(
  const Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &rendererSettings)
{
    // frameSettings already holds the new choice, which recreateSwapChain() reads.
    if (rendererSettings.vsync != vulkanSwapChain.vsyncRequested()) { recreateSwapChain(); }
}

void Kataglyphis::VulkanRenderer::reinitShadowMapForCurrentSettings()
{
    const GUISceneSharedVars &sceneSettings = frameSettings.scene;

    dirShadowMap.cleanUp();

    const uint32_t shadow_res = shadowResolutionForIndex(sceneSettings.shadow_map_res_index);

    // Clamp to the SceneUBO's CASCADE_COUNT and to maxMultiviewViewCount, which bounds the shadow pass's viewMask.
    const auto device_view_limit = device->getMaxMultiviewViewCount();
    const auto cascade_count = clampCascadeCount(static_cast<uint32_t>(sceneSettings.num_shadow_cascades),
      static_cast<uint32_t>(CASCADE_COUNT),
      device_view_limit);
    if (cascade_count == device_view_limit && device_view_limit < static_cast<uint32_t>(CASCADE_COUNT)) {
        spdlog::warn("Device maxMultiviewViewCount ({}) is the binding constraint on cascade count; clamping to {}.",
          device_view_limit,
          cascade_count);
    }
    dirShadowMap.init(device,
      shadow_res,
      shadow_res,
      cascade_count,
      sharedRenderDescriptors.getLayout(),
      vulkanSwapChain.getNumberSwapChainImages(),
      graphics_command_pool.get());
    dirShadowMap.createGraphicsPipeline();

    // The pipeline was seeded from stale cascadeData; re-seed every image once updateUniforms() has run.
    lightMatricesNeedFullReseed = true;
}

void Kataglyphis::VulkanRenderer::reinitPunctualShadows()
{
    punctualShadows.cleanUp();
    punctualShadows.init(device, sharedRenderDescriptors.getLayout(), vulkanSwapChain.getNumberSwapChainImages());
    punctualShadows.createGraphicsPipeline();
    punctualShadows.setSlots(punctualShadowSlots);
}

void Kataglyphis::VulkanRenderer::applyPunctualShadowSlots(std::vector<glm::vec4> &packedLights)
{
    const AABB bounds = sceneWorldBounds(*scene);
    punctualShadowSlots = assignPunctualShadowSlots(packedLights, bounds.min, bounds.max);
    // A scene change, so rare; the cube array may be reallocated under frames still sampling it.
    std::ignore = device->getLogicalDevice().waitIdle();
    punctualShadows.setSlots(punctualShadowSlots);
}

void Kataglyphis::VulkanRenderer::handleShadowResolutionChange(const GUISceneSharedVars &sceneSettings)
{
    if (sceneSettings.shadow_resolution_changed) {
        (void)device->getLogicalDevice().waitIdle();

        reinitShadowMapForCurrentSettings();

        // We must recreate descriptor sets that depend on the shadow map
        updateTexturesInSharedRenderDescriptorSet();
    }
}

void Kataglyphis::VulkanRenderer::handleModelTransformChange(const GUISceneSharedVars &sceneSettings)
{
    if (sceneSettings.model_transform_changed) {
        const glm::mat4 modelMatrix = makeGuiModelTransform(std::span<const float, 3>(sceneSettings.model_position),
          std::span<const float, 3>(sceneSettings.model_rotation));

        // Model 0 is the loaded one, also when no library row lists it; additional models keep their own matrix.
        if (scene->getModelCount() > 0) {
            scene->update_model_matrix(modelMatrix, 0);

            // Only the instance transform moved, so a TLAS-only rebuild suffices.
            (void)device->getLogicalDevice().waitIdle();
            refreshAfterSceneChange(false);
        }
    }
}

void Kataglyphis::VulkanRenderer::handleModelReloadRequest(const GUISceneSharedVars &sceneSettings)
{
    if (sceneSettings.model_reload_requested) {
        const auto model_paths = sceneConfig::getAvailableModelPaths();
        const int sel = sceneSettings.selected_model_index;
        if (sel >= 0 && sel < static_cast<int>(model_paths.size())) {
            const std::string selected_path = model_paths[static_cast<size_t>(sel)];

            (void)device->getLogicalDevice().waitIdle();

            scene->reloadModel(device, graphics_command_pool.get(), selected_path);

            // New geometry: full BLAS+TLAS rebuild, or RT/PT keep the destroyed TLAS bound.
            refreshAfterSceneChange(true);
            publishLoadedModel();
        }
    }
}

void Kataglyphis::VulkanRenderer::simulateFailedAmbientBakeForTesting()
{
    std::ignore = device->getLogicalDevice().waitIdle();
    ambientMaps.cleanUp();
    ambientMaps.init(device, graphics_command_pool.get());
    // A cube without a view, as SkyBox::uploadCubeMapFaces leaves one it could not fill.
    Kataglyphis::Texture unuploaded;
    (void)ambientMaps.bake(unuploaded, 0);
    updateAllDescriptorSets();
}

void Kataglyphis::VulkanRenderer::shaderHotReload()
{
    std::ignore = device->getLogicalDevice().waitIdle();
    renderStages.shaderHotReload();
}

// Must reach every SPIR-V-loading stage (BuildIntegrity.EverySpirvLoadingSubsystemImplementsShaderHotReload).
void Kataglyphis::VulkanRenderer::registerBuiltInStages()
{
    using Kataglyphis::VulkanRendererInternals::FrontendShared::GpuTimedPass;
    std::array<RenderStage, 7> built_in{};

    built_in[0].name = "shadow_cascades";
    built_in[0].order = RenderStageOrder::ShadowCascades;
    built_in[0].timedPass = GpuTimedPass::ShadowCascades;
    built_in[0].labelColor = std::array{ 0.55F, 0.35F, 0.10F, 1.0F };
    // RT traces one binary shadow ray and PT uses NEE: neither samples the cascades.
    built_in[0].enabled = [this](const RenderStageFrame &frame) {
        return frameSettings.scene.shadows_enabled && !frame.tracingOwnsFrame;
    };
    built_in[0].record = [this](const RenderStageFrame &frame) { recordShadowStage(frame); };
    built_in[0].recordSkipped = [this](const RenderStageFrame &frame) {
        vk::CommandBuffer commandBuffer = frame.commandBuffer;
        dirShadowMap.recordSkippedPass(commandBuffer);
        output.visibility.shadow_casters_drawn = 0;
        output.visibility.shadow_casters_total = 0;
    };
    built_in[0].shaderHotReload = [this] { dirShadowMap.shaderHotReload(); };

    built_in[5].name = "punctual_shadows";
    built_in[5].order = RenderStageOrder::PunctualShadows;
    built_in[5].timedPass = GpuTimedPass::PunctualShadows;
    built_in[5].labelColor = std::array{ 0.75F, 0.45F, 0.15F, 1.0F };
    // Every frame while enabled: lights and casters can move; traced frames shadow the lights by rays.
    built_in[5].enabled = [this](const RenderStageFrame &frame) {
        return frameSettings.scene.punctual_shadows_enabled && !frame.tracingOwnsFrame
               && punctualShadows.slotCount() > 0;
    };
    built_in[5].record = [this](const RenderStageFrame &frame) { recordPunctualShadowStage(frame); };
    built_in[5].recordSkipped = [this](const RenderStageFrame &frame) {
        punctualShadows.recordSkippedPass(frame.commandBuffer);
    };
    built_in[5].shaderHotReload = [this] { punctualShadows.shaderHotReload(); };

    built_in[1].name = "main";
    built_in[1].order = RenderStageOrder::Main;
    built_in[1].timedPass = GpuTimedPass::Main;
    built_in[1].record = [this](const RenderStageFrame &frame) { recordMainStage(frame); };
    // The extent-sized offscreen, G-buffer, depth and traced targets follow the new swapchain.
    built_in[1].resize = [this](const RenderStageResize &) {
        rasterizer.recreateFrameResources(graphics_command_pool.get());
        deferredRasterizer.recreateFrameResources();
        // The accumulation history is extent-sized; recreating it also resets the frame counter.
        if (device->supportsHardwareAcceleratedRRT()) {
            createPathTracingAccumulationResources();
            createTracedDepthResources();
        }
    };
    built_in[1].shaderHotReload = [this] {
        std::array<vk::DescriptorSetLayout, 1> const descriptor_set_layouts = { sharedRenderDescriptors.getLayout() };
        rasterizer.shaderHotReload(descriptor_set_layouts);
        std::array<vk::DescriptorSetLayout, 2> const descriptor_set_layouts_deferred = {
            sharedRenderDescriptors.getLayout(), gbufferDescriptors.getLayout()
        };
        deferredRasterizer.shaderHotReload(descriptor_set_layouts_deferred);
        // The sky draws inside the main stage's passes.
        skyBox.shaderHotReload();
        if (Kataglyphis::Texture *cube_map = skyBox.getCubeMapTexture(); cube_map != nullptr) {
            ambientMaps.shaderHotReload(*cube_map, skyBox.getCubeFaceSize());
        }
        if (device->supportsHardwareAcceleratedRRT()) {
            std::array<vk::DescriptorSetLayout, 2> const layouts = { sharedRenderDescriptors.getLayout(),
                raytracingDescriptors.getLayout() };
            raytracingStage.shaderHotReload(layouts);
            pathTracing.shaderHotReload(layouts);
            // A reloaded kernel is another estimator: averaging its samples into the old mean biases it.
            pathTracingAccumulatedFrames = 0;
        }
    };

    built_in[2].name = "clouds";
    built_in[2].order = RenderStageOrder::Clouds;
    built_in[2].timedPass = GpuTimedPass::Clouds;
    built_in[2].labelColor = std::array{ 0.80F, 0.85F, 0.95F, 1.0F };
    built_in[2].enabled = [this](const RenderStageFrame &) { return frameSettings.scene.clouds_enabled; };
    built_in[2].record = [this](const RenderStageFrame &frame) { recordCloudsStage(frame); };
    built_in[2].resize = [this](const RenderStageResize &resize) {
        clouds.recreateFrameResources(graphics_command_pool.get(), resize.extent.width, resize.extent.height);
    };
    built_in[2].shaderHotReload = [this] { clouds.shaderHotReload(sharedRenderDescriptors.getLayout()); };

    built_in[4].name = "cloud_upsample";
    built_in[4].order = RenderStageOrder::CloudUpsample;
    built_in[4].timedPass = GpuTimedPass::CloudUpsample;
    built_in[4].labelColor = std::array{ 0.70F, 0.75F, 0.95F, 1.0F };
    built_in[4].enabled = [this](const RenderStageFrame &) {
        return frameSettings.scene.clouds_enabled && frameSettings.scene.cloud_half_resolution;
    };
    built_in[4].record = [this](const RenderStageFrame &frame) { recordCloudUpsampleStage(frame); };

    built_in[3].name = "post";
    built_in[3].order = RenderStageOrder::Post;
    built_in[3].timedPass = GpuTimedPass::Post;
    built_in[3].labelColor = std::array{ 0.35F, 0.80F, 0.40F, 1.0F };
    built_in[3].record = [this](const RenderStageFrame &frame) { recordPostStage(frame); };
    built_in[3].shaderHotReload = [this] {
        std::array<vk::DescriptorSetLayout, 1> const descriptor_set_layouts_post = { postDescriptors.getLayout() };
        postStage.shaderHotReload(descriptor_set_layouts_post);
    };

    built_in[6].name = "fxaa";
    built_in[6].order = RenderStageOrder::Fxaa;
    built_in[6].timedPass = GpuTimedPass::Fxaa;
    built_in[6].labelColor = std::array{ 0.55F, 0.85F, 0.55F, 1.0F };
    built_in[6].enabled = [this](const RenderStageFrame &frame) { return fxaaFiltersFrame(frame); };
    built_in[6].record = [this](const RenderStageFrame &frame) {
        fxaaStage.recordCommands(frame.commandBuffer, frame.imageIndex);
    };
    // The intermediates follow the swapchain's extent and image count.
    built_in[6].resize = [this](const RenderStageResize &) { fxaaStage.recreateFrameResources(); };
    built_in[6].shaderHotReload = [this] { fxaaStage.shaderHotReload(); };

    for (RenderStage &stage : built_in) {
        if (renderStages.add(std::move(stage)) != RenderStageRegistry::AddResult::Added) {
            spdlog::critical("A built-in render stage was refused by the stage registry.");
            std::abort();
        }
    }
}

bool Kataglyphis::VulkanRenderer::removeRenderStage(std::string_view name)
{
    std::ignore = device->getLogicalDevice().waitIdle();
    return renderStages.remove(name);
}

bool Kataglyphis::VulkanRenderer::recordCommandsUnsubmittedForBenchmark()
{
    constexpr uint32_t image_index = 0;
    if (command_buffers.empty() || frameSync.imagesInFlightFenceCount() == 0) { return false; }
    // The buffer's last submit must have finished before it is reset.
    if (frameSync.imageInFlightFence(image_index)
        && device->getLogicalDevice().waitForFences(1, &frameSync.imageInFlightFence(image_index), VK_TRUE, UINT64_MAX)
             != vk::Result::eSuccess) {
        return false;
    }
    vk::CommandBuffer commandBuffer = command_buffers[image_index];
    vk::CommandBufferBeginInfo begin_info{};
    begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    if (commandBuffer.reset(vk::CommandBufferResetFlags{}) != vk::Result::eSuccess
        || commandBuffer.begin(&begin_info) != vk::Result::eSuccess) {
        return false;
    }
    const bool recorded = record_commands(image_index);
    const bool ended = commandBuffer.end() == vk::Result::eSuccess;
    // None of these queries reach the GPU, so the next readback must not read them.
    gpuTiming.clearPassRecordedMask(image_index);
    return recorded && ended;
}

void Kataglyphis::VulkanRenderer::drawFrame(const RenderSettings &settings, RendererFeedback &feedback)
{
    frameSettings = settings;
    drawFrameImpl();
    feedback = output;
}

void Kataglyphis::VulkanRenderer::drawFrameImpl()
{
    const auto end_imgui_frame_if_needed = []() -> void {
        ImGuiContext const *imgui_context = ImGui::GetCurrentContext();
        if (imgui_context != nullptr && imgui_context->WithinFrameScope) { ImGui::EndFrame(); }
    };

    const auto abort_frame_with_fatal_error = [&](const char *message, vk::Result error_code) -> void {
        spdlog::error(fmt::format("{} (vk::Result={})", message, static_cast<int>(error_code)));
        fatal_frame_error = true;
        if (error_code == vk::Result::eErrorDeviceLost) { device_lost_detected = true; }
        if (window != nullptr && window->get_window() != nullptr) {
            glfwSetWindowShouldClose(window->get_window(), GLFW_TRUE);
        }
        end_imgui_frame_if_needed();
    };

    // Recoverable post-acquire returns: recreateSwapChain() retires the signaled, never-waited acquire semaphore.
    const auto abort_frame_after_acquire = [&](const char *message) -> void {
        spdlog::error(message);
        end_imgui_frame_if_needed();
        recreateSwapChain();
    };

    if (frameSync.frameSyncCount() == 0) {
        spdlog::error("No synchronization frames available; skipping draw frame.");
        end_imgui_frame_if_needed();
        return;
    }

    // Checked last: checkChangedFramebufferSize() clears the flag, so a blocked recreate would lose the resize.
    if (frameSync.frameSyncCount() > 0 && !frameSync.inFlightFencesEmpty() && checkChangedFramebufferSize()) {
        recreateSwapChain();
    }

    if (frameSync.currentFrame() >= frameSync.inFlightFenceCount()
        || frameSync.currentFrame() >= frameSync.imageAvailableCount()) {
        spdlog::error(fmt::format("Frame synchronization index out of range: {}", frameSync.currentFrame()));
        end_imgui_frame_if_needed();
        return;
    }

    if (!frameSync.inFlightFence() || !frameSync.imageAvailableSemaphore()) {
        spdlog::error(fmt::format("Synchronization handles are invalid for frame {}.", frameSync.currentFrame()));
        fatal_frame_error = true;
        if (window != nullptr && window->get_window() != nullptr) {
            glfwSetWindowShouldClose(window->get_window(), GLFW_TRUE);
        }
        end_imgui_frame_if_needed();
        return;
    }

    vk::Result result = device->getLogicalDevice().waitForFences(
      1, &frameSync.inFlightFence(), VK_TRUE, std::numeric_limits<uint64_t>::max());
    if (device_loss_simulated) { result = vk::Result::eErrorDeviceLost; }
    if (result != vk::Result::eSuccess) {
        abort_frame_with_fatal_error("Failed to wait for fences!", result);
        return;
    }

    uint32_t image_index = 0;
    std::tie(result, image_index) = device->getLogicalDevice().acquireNextImageKHR(vulkanSwapChain.getSwapChain(),
      std::numeric_limits<uint64_t>::max(),
      frameSync.imageAvailableSemaphore(),
      nullptr);

    if (result == vk::Result::eErrorOutOfDateKHR) {
        abort_frame_after_acquire("Swapchain out of date at acquire; recreating.");
        return;
    }

    // eSuboptimalKHR still signaled the semaphore, so render and present; the present path recreates afterwards.
    if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
        abort_frame_with_fatal_error("Failed to acquire next image!", result);
        return;
    }

    if (image_index >= frameSync.imagesInFlightFenceCount() || image_index >= command_buffers.size()) {
        abort_frame_after_acquire(fmt::format("Swapchain image index out of range: {}", image_index).c_str());
        return;
    }

    if (image_index >= frameSync.renderFinishedCount() || !frameSync.renderFinishedSemaphore(image_index)) {
        abort_frame_after_acquire(
          fmt::format("Render-finished semaphore missing for swapchain image {}.", image_index).c_str());
        return;
    }

    if (frameSync.imageInFlightFence(image_index)) {
        result =
          device->getLogicalDevice().waitForFences(1, &frameSync.imageInFlightFence(image_index), VK_TRUE, UINT64_MAX);
        if (result != vk::Result::eSuccess) {
            abort_frame_with_fatal_error("Failed to wait for image in-flight fence!", result);
            return;
        }
    }

    // The fence wait above means this image's queries are complete, so the readback never stalls.
    gpuTiming.readTimings(*device, image_index, output.gpuTimings);

    frameSync.imageInFlightFence(image_index) = frameSync.inFlightFence();

    result = command_buffers[image_index].reset(vk::CommandBufferResetFlags{});
    if (result != vk::Result::eSuccess) {
        abort_frame_with_fatal_error("Failed to reset command buffer!", result);
        return;
    }

    vk::CommandBufferBeginInfo buffer_begin_info{};
    buffer_begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    result = command_buffers[image_index].begin(&buffer_begin_info);
    if (result != vk::Result::eSuccess) {
        abort_frame_with_fatal_error("Failed to start recording a command buffer!", result);
        return;
    }

    if (!update_uniform_buffers(image_index)) {
        // End recording first: recreateSwapChain() must not free a buffer still recording.
        std::ignore = command_buffers[image_index].end();
        abort_frame_after_acquire("Failed to update uniform buffers; dropping this frame.");
        return;
    }

    const bool raytracing_available = device->supportsHardwareAcceleratedRRT();
    // The mode this frame draws, not the GUI's pick: without a TLAS a ray- or path-tracing pick still draws raster.
    const char *const render_mode =
      Kataglyphis::VulkanRendererInternals::FrontendShared::renderModeLogName(drawnRenderMode());

    if (!record_commands(image_index)) {
        std::ignore = command_buffers[image_index].end();
        abort_frame_after_acquire("record_commands failed; dropping this frame.");
        return;
    }

    result = command_buffers[image_index].end();
    if (result != vk::Result::eSuccess) {
        abort_frame_with_fatal_error("Failed to stop recording a command buffer!", result);
        return;
    }

    const vk::SemaphoreSubmitInfo wait_info = Kataglyphis::buildSemaphoreSubmitInfo(
      frameSync.imageAvailableSemaphore(), Kataglyphis::kSwapchainAcquireWaitStage);
    const vk::SemaphoreSubmitInfo signal_info = Kataglyphis::buildSemaphoreSubmitInfo(
      frameSync.renderFinishedSemaphore(image_index), Kataglyphis::kRenderFinishedSignalStage);
    const vk::CommandBufferSubmitInfo command_buffer_info{ command_buffers[image_index] };

    vk::SubmitInfo2 submit_info{};
    submit_info.waitSemaphoreInfoCount = 1;
    submit_info.pWaitSemaphoreInfos = &wait_info;
    submit_info.commandBufferInfoCount = 1;
    submit_info.pCommandBufferInfos = &command_buffer_info;
    submit_info.signalSemaphoreInfoCount = 1;
    submit_info.pSignalSemaphoreInfos = &signal_info;

    result = device->getLogicalDevice().resetFences(1, &frameSync.inFlightFence());
    if (result != vk::Result::eSuccess) {
        abort_frame_with_fatal_error("Failed to reset fences!", result);
        return;
    }

    result = device->getGraphicsQueue().submit2(1, &submit_info, frameSync.inFlightFence());
    if (result != vk::Result::eSuccess) {
        spdlog::error(
          fmt::format("Queue submit context: frame={}, imageIndex={}, renderMode={}, supportsRRT={}, cmdBufferIndex={}",
            frameSync.currentFrame(),
            image_index,
            render_mode,
            raytracing_available,
            image_index));
        abort_frame_with_fatal_error("Failed to submit command buffer to queue!", result);
        return;
    }

    // takeCapturedFrame() waits on this frame's in-flight fence.
    frameCapture.bindSubmitFence(frameSync.inFlightFence());
    hdrCapture.bindSubmitFence(frameSync.inFlightFence());

    vk::PresentInfoKHR present_info{};
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &frameSync.renderFinishedSemaphore(image_index);
    present_info.swapchainCount = 1;
    const vk::SwapchainKHR swapchain = vulkanSwapChain.getSwapChain();
    present_info.pSwapchains = &swapchain;
    present_info.pImageIndices = &image_index;

    result = device->getPresentationQueue().presentKHR(&present_info);

    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR) {
        recreateSwapChain();
    } else if (result != vk::Result::eSuccess) {
        abort_frame_with_fatal_error("Failed to present image!", result);
        return;
    }

    frameSync.advanceFrame();
}

bool Kataglyphis::VulkanRenderer::checkChangedFramebufferSize()
{
    if (window == nullptr) { return false; }

    if (window->framebuffer_size_has_changed()) {
        window->reset_framebuffer_has_changed();
        return true;
    }

    return false;
}

void Kataglyphis::VulkanRenderer::reprovisionPerImageResources()
{
    cleanUpUBOs();
    create_uniform_buffers();

    cleanUpDescriptorResources();
    initDescriptorResources();

    // After initDescriptorResources(): the shadow map is per image and caches the shared layout just replaced.
    reinitShadowMapForCurrentSettings();
    reinitPunctualShadows();

    if (device->supportsHardwareAcceleratedRRT()) {
        raytracingDescriptors.cleanUp();
        createRaytracingDescriptorResources();
    }

    skyBox.createFrameResources(vulkanSwapChain.getNumberSwapChainImages());
}

void Kataglyphis::VulkanRenderer::recreateSwapChain()
{
    int width = 0, height = 0;
    glfwGetFramebufferSize(window->get_window(), &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(window->get_window(), &width, &height);
        glfwWaitEvents();
    }

    std::ignore = device->getLogicalDevice().waitIdle();

    // createSynchronization() recreates every fence; after waitIdle the captured pixels stay readable.
    frameCapture.invalidateFence();
    hdrCapture.invalidateFence();

    uint32_t oldImageCount = vulkanSwapChain.getNumberSwapChainImages();

    // No framebuffers to rebuild: every pass names its swapchain image view at vkCmdBeginRendering.
    vulkanSwapChain.recreate(device, device->getSurface(), frameSettings.renderer.vsync);

    // The query pool is sized per swapchain image.
    gpuTiming.create(*device, vulkanSwapChain.getNumberSwapChainImages(), output.gpuTimings);

    uint32_t newImageCount = vulkanSwapChain.getNumberSwapChainImages();

    renderStages.resize(
      RenderStageResize{ .extent = vulkanSwapChain.getSwapChainExtent(), .imageCount = newImageCount });

    // A new image count re-provisions every per-image resource, not just the descriptor pools.
    if (newImageCount != oldImageCount) {
        reprovisionPerImageResources();
        // updateAllDescriptorSets() does not rewrite the object-description binding.
        updateObjectDescriptionDescriptorSets();
    }

    updateAllDescriptorSets();
    // Every depth image the clouds can read was just recreated.
    bindCloudSceneDepth();

    create_command_buffers();
    createSynchronization();
}

bool Kataglyphis::VulkanRenderer::update_uniform_buffers(uint32_t image_index)
{
    if (image_index >= globalUBOMapped.size() || image_index >= sceneUBOMapped.size()) {
        spdlog::error(fmt::format("Uniform buffer index out of range: {}", image_index));
        return false;
    }

    std::memcpy(globalUBOMapped[image_index], &globalUBO, sizeof(VulkanRendererInternals::GlobalUBO));
    std::memcpy(sceneUBOMapped[image_index], &sceneUBO, sizeof(VulkanRendererInternals::SceneUBO));
    skyBox.uploadUniforms(image_index,
      makeSkyUniforms(globalUBO.projection,
        globalUBO.view,
        glm::vec3(sceneUBO.dirLight.direction),
        sceneUBO.dirLight.color.w,
        frameSettings.scene.skybox_enabled));

    if (lightMatricesNeedFullReseed) {
        // After a shadow re-init every image is stale; cascadeData is fresh here, as updateUniforms() already ran.
        for (uint32_t i = 0; i < vulkanSwapChain.getNumberSwapChainImages(); i++) {
            dirShadowMap.uploadLightMatrices(i);
        }
        lightMatricesNeedFullReseed = false;
    } else {
        // Must land in the same image's buffers as the sceneUBO sample matrices.
        dirShadowMap.uploadLightMatrices(image_index);
    }
    punctualShadows.uploadFaceMatrices(image_index);
    return true;
}

void Kataglyphis::VulkanRenderer::updateUBODescriptorSets()
{
    for (uint32_t i = 0; i < vulkanSwapChain.getNumberSwapChainImages(); i++) {
        sharedRenderDescriptors.writeBuffer(i, globalUBO_BINDING, globalUBOBuffer[i].getBuffer(), sizeof(globalUBO));
        sharedRenderDescriptors.writeBuffer(i, sceneUBO_BINDING, sceneUBOBuffer[i].getBuffer(), sizeof(sceneUBO));
    }
}

std::optional<uint32_t> Kataglyphis::VulkanRenderer::addModel(const std::string &modelPath,
  const glm::mat4 &modelMatrix)
{
    if (!scene || !device) { return std::nullopt; }

    // Nothing in flight may still read the descriptor sets rewritten below.
    std::ignore = device->getLogicalDevice().waitIdle();

    const std::optional<uint32_t> index =
      scene->loadAdditionalModel(device, graphics_command_pool.get(), modelPath, modelMatrix);
    if (!index.has_value()) { return std::nullopt; }

    // New geometry needs its BLAS too, or ray and path tracing never see it.
    refreshAfterSceneChange(true);
    return index;
}

void Kataglyphis::VulkanRenderer::updateAllDescriptorSets()
{
    updateUBODescriptorSets();
    updatePostDescriptorSets();
    updateGBufferDescriptorSets();
    updateTexturesInSharedRenderDescriptorSet();
    if (device->supportsHardwareAcceleratedRRT()) { updateRaytracingDescriptorSets(); }
}

void Kataglyphis::VulkanRenderer::cleanUpDescriptorResources()
{
    sharedRenderDescriptors.cleanUp();
    postDescriptors.cleanUp();
    gbufferDescriptors.cleanUp();
}

void Kataglyphis::VulkanRenderer::initDescriptorResources()
{
    createSharedRenderDescriptorResources();
    create_post_descriptor_resources();
    create_gbuffer_descriptor_resources();
}

void Kataglyphis::VulkanRenderer::writeRaytracingDescriptorsForImage(uint32_t image_index)
{
    vk::AccelerationStructureKHR &vulkanTLAS = asManager.getTLAS();
    Texture &renderResult = activeOffscreenTexture(image_index);

    raytracingDescriptors.writeAccelerationStructure(image_index, TLAS_BINDING, vulkanTLAS);
    raytracingDescriptors.writeImage(
      image_index, OUT_IMAGE_BINDING, renderResult.getImageView(), vk::ImageLayout::eGeneral);
    raytracingDescriptors.writeImage(
      image_index, ACCUMULATION_IMAGE_BINDING, pathTracingAccumulation.getImageView(), vk::ImageLayout::eGeneral);
    raytracingDescriptors.writeImage(
      image_index, TRACED_DEPTH_IMAGE_BINDING, tracedDepth.getImageView(), vk::ImageLayout::eGeneral);
    // Uploaded once and left read-only, as the drawn sky samples it; a TLAS exists only after skyBox.init().
    if (Kataglyphis::Texture *cube_map = skyBox.getCubeMapTexture(); cube_map != nullptr) {
        raytracingDescriptors.writeImage(image_index,
          SKYBOX_CUBEMAP_BINDING,
          cube_map->getImageView(),
          vk::ImageLayout::eShaderReadOnlyOptimal,
          cube_map->getSampler());
    }
}

auto Kataglyphis::VulkanRenderer::activeOffscreenTexture(uint32_t index) -> Texture &
{
    return frameSettings.renderer.rasterizationMode
               == Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode::Forward
             ? rasterizer.getOffscreenTexture(index)
             : deferredRasterizer.getOffscreenTexture(index);
}

bool Kataglyphis::VulkanRenderer::record_commands(uint32_t image_index)
{
    if (image_index >= command_buffers.size() || image_index >= sharedRenderDescriptors.sets().size()
        || image_index >= postDescriptors.sets().size()) {
        spdlog::error(fmt::format("Command recording index out of range: {}", image_index));
        return false;
    }

    vk::CommandBuffer &commandBuffer = command_buffers[image_index];

    // Per-pass GPU timing: the query slice must be reset outside any render pass.
    gpuTiming.beginFrame(commandBuffer, image_index);

    Texture &swap_chain_image = vulkanSwapChain.getSwapChainImage(image_index);
    const RenderStageFrame frame{
        .commandBuffer = commandBuffer,
        .imageIndex = image_index,
        .extent = vulkanSwapChain.getSwapChainExtent(),
        .swapchainImage = swap_chain_image.getImage(),
        .swapchainImageView = swap_chain_image.getImageView(),
        .swapchainFormat = vulkanSwapChain.getSwapChainFormat(),
        .tracingOwnsFrame = raytracingOwnsFrame(image_index),
    };

    // Every pass records through the registry, in RenderStageOrder; the bracket labels and times each one.
    renderStages.recordFrame(frame, [&](const RenderStage &stage, bool begin) {
        if (begin && stage.labelColor.has_value()) {
            Kataglyphis::debug::beginCmdLabel(commandBuffer, stage.name.c_str(), *stage.labelColor);
        }
        if (stage.timedPass.has_value()) { gpuTiming.writeTimestamp(commandBuffer, *stage.timedPass, begin); }
        if (!begin && stage.labelColor.has_value()) { Kataglyphis::debug::endCmdLabel(commandBuffer); }
    });

    // Capture copies from the ePresentSrcKHR image and restores that layout before the present.
    if (frameCapture.isArmed()) {
        frameCapture.record(device, commandBuffer, vulkanSwapChain, image_index, device_lost_detected);
    }
    // The scene radiance post just read, still RGBA16F: a value past fp16's range shows here, not in the 8-bit frame.
    if (hdrCapture.isArmed()) {
        hdrCapture.recordImage(device,
          commandBuffer,
          activeOffscreenTexture(image_index).getImage(),
          vulkanSwapChain.getSwapChainExtent(),
          Kataglyphis::VulkanRendererInternals::Rasterizer::OFFSCREEN_FORMAT,
          vk::ImageLayout::eShaderReadOnlyOptimal,
          vk::PipelineStageFlagBits2::eFragmentShader,
          device_lost_detected);
    }

    gpuTiming.endFrame(image_index);

    return true;
}

void Kataglyphis::VulkanRenderer::recordShadowStage(const RenderStageFrame &frame)
{
    const std::array<vk::DescriptorSet, 1> rasterizer_descriptor_sets = {
        sharedRenderDescriptors.sets()[frame.imageIndex]
    };
    vk::CommandBuffer commandBuffer = frame.commandBuffer;
    dirShadowMap.recordCommands(commandBuffer,
      frame.imageIndex,
      scene,
      rasterizer_descriptor_sets,
      frameSettings.renderer.frustum_culling_enabled);
    output.visibility.shadow_casters_drawn = dirShadowMap.getCastersDrawn();
    output.visibility.shadow_casters_total = dirShadowMap.getCastersConsidered();
}

void Kataglyphis::VulkanRenderer::recordPunctualShadowStage(const RenderStageFrame &frame)
{
    punctualShadows.recordCommands(frame.commandBuffer,
      frame.imageIndex,
      scene,
      sharedRenderDescriptors.sets()[frame.imageIndex],
      frameSettings.renderer.frustum_culling_enabled);
}

void Kataglyphis::VulkanRenderer::recordMainStage(const RenderStageFrame &frame)
{
    const GUISceneSharedVars &sceneSettings = frameSettings.scene;
    vk::CommandBuffer commandBuffer = frame.commandBuffer;
    const uint32_t image_index = frame.imageIndex;
    const std::array<vk::DescriptorSet, 1> rasterizer_descriptor_sets = { sharedRenderDescriptors.sets()[image_index] };

    // From globalUBO, like the shaders; the shadow pass must not use it, as off-screen geometry casts into view.
    const std::optional<FrustumPlanes> camera_frustum =
      frameSettings.renderer.frustum_culling_enabled
        ? std::optional<FrustumPlanes>(extractFrustumPlanes(globalUBO.projection * globalUBO.view))
        : std::nullopt;

    if (!frame.tracingOwnsFrame) {
        recordRasterPass(commandBuffer, image_index, rasterizer_descriptor_sets, camera_frustum);
    } else {
        output.visibility.meshes_drawn = 0;
        output.visibility.meshes_total = 0;
    }

    recordRaytracingOrPathTracing(commandBuffer, image_index, sceneSettings.clouds_enabled);
}

// After the main stage: the march stops at the depth it just wrote.
void Kataglyphis::VulkanRenderer::recordCloudsStage(const RenderStageFrame &frame)
{
    vk::CommandBuffer commandBuffer = frame.commandBuffer;
    const std::array<vk::DescriptorSet, 1> rasterizer_descriptor_sets = {
        sharedRenderDescriptors.sets()[frame.imageIndex]
    };

    const bool half_resolution = frameSettings.scene.cloud_half_resolution;
    // Cross-frame WAR on the march's single target: post reads the full one, the upsample the half one.
    recordGeneralImageBarrier(commandBuffer,
      half_resolution ? clouds.getCloudHalfTexture()->getImage() : clouds.getCloudOutputTexture()->getImage(),
      half_resolution ? vk::PipelineStageFlagBits2::eComputeShader : vk::PipelineStageFlagBits2::eFragmentShader,
      vk::PipelineStageFlagBits2::eComputeShader,
      vk::AccessFlagBits2::eNone,
      vk::AccessFlagBits2::eShaderStorageWrite);

    // A raster depth buffer reaches this read through its pass's closing barrier; traced depth does not.
    if (frame.tracingOwnsFrame) {
        recordGeneralImageBarrier(commandBuffer,
          tracedDepth.getImage(),
          vk::PipelineStageFlagBits2::eRayTracingShaderKHR | vk::PipelineStageFlagBits2::eComputeShader,
          vk::PipelineStageFlagBits2::eComputeShader,
          vk::AccessFlagBits2::eShaderStorageWrite,
          vk::AccessFlagBits2::eShaderSampledRead);
    }

    clouds.recordComputeCommands(commandBuffer, rasterizer_descriptor_sets, half_resolution);

    if (half_resolution) {
        // The upsample stage reads the half march next.
        recordGeneralImageBarrier(commandBuffer,
          clouds.getCloudHalfTexture()->getImage(),
          vk::PipelineStageFlagBits2::eComputeShader,
          vk::PipelineStageFlagBits2::eComputeShader,
          vk::AccessFlagBits2::eShaderStorageWrite,
          vk::AccessFlagBits2::eShaderSampledRead);
        return;
    }
    // The post pass samples the clouds; nothing else orders this compute write before it.
    recordGeneralImageBarrier(commandBuffer,
      clouds.getCloudOutputTexture()->getImage(),
      vk::PipelineStageFlagBits2::eComputeShader,
      vk::PipelineStageFlagBits2::eFragmentShader,
      vk::AccessFlagBits2::eShaderStorageWrite,
      vk::AccessFlagBits2::eShaderSampledRead);
}

// After the half-resolution march: the full-size target post composites, filled by depth.
void Kataglyphis::VulkanRenderer::recordCloudUpsampleStage(const RenderStageFrame &frame)
{
    vk::CommandBuffer commandBuffer = frame.commandBuffer;
    const std::array<vk::DescriptorSet, 1> rasterizer_descriptor_sets = {
        sharedRenderDescriptors.sets()[frame.imageIndex]
    };

    // Cross-frame WAR on the single cloudOutputTexture, which the previous frame's post sampled.
    recordGeneralImageBarrier(commandBuffer,
      clouds.getCloudOutputTexture()->getImage(),
      vk::PipelineStageFlagBits2::eFragmentShader,
      vk::PipelineStageFlagBits2::eComputeShader,
      vk::AccessFlagBits2::eNone,
      vk::AccessFlagBits2::eShaderStorageWrite);

    clouds.recordUpsampleCommands(commandBuffer, rasterizer_descriptor_sets);

    // The post pass samples the clouds; nothing else orders this compute write before it.
    recordGeneralImageBarrier(commandBuffer,
      clouds.getCloudOutputTexture()->getImage(),
      vk::PipelineStageFlagBits2::eComputeShader,
      vk::PipelineStageFlagBits2::eFragmentShader,
      vk::AccessFlagBits2::eShaderStorageWrite,
      vk::AccessFlagBits2::eShaderSampledRead);
}

bool Kataglyphis::VulkanRenderer::fxaaFiltersFrame(const RenderStageFrame &frame) const
{
    return Kataglyphis::VulkanRendererInternals::FrontendShared::fxaaFilters(
      frameSettings.renderer.fxaaMode, frame.tracingOwnsFrame, fxaaStage.ready(frame.imageIndex));
}

// No swapchain barrier before it: PostStage takes the image from the acquire, or renders into FXAA's intermediate.
void Kataglyphis::VulkanRenderer::recordPostStage(const RenderStageFrame &frame)
{
    const std::array<vk::DescriptorSet, 1> post_descriptor_sets = { postDescriptors.sets()[frame.imageIndex] };
    vk::CommandBuffer commandBuffer = frame.commandBuffer;
    postStage.recordCommands(commandBuffer,
      frame.imageIndex,
      post_descriptor_sets,
      frameSettings.scene.clouds_enabled,
      fxaaFiltersFrame(frame) ? &fxaaStage.getIntermediate(frame.imageIndex) : nullptr);
}

void Kataglyphis::VulkanRenderer::recordRasterPass(vk::CommandBuffer &commandBuffer,
  uint32_t image_index,
  std::span<const vk::DescriptorSet> rasterizer_descriptor_sets,
  const std::optional<FrustumPlanes> &camera_frustum)
{
    using Kataglyphis::VulkanRendererInternals::FrontendShared::GpuTimedPass;
    const auto raster_mode = frameSettings.renderer.rasterizationMode;
    // Inside either path's pass with its depth attached, so Forward and DeferredLighting time it as Rust's do.
    const auto draw_sky = [this, image_index](
                            vk::CommandBuffer passes) { skyBox.recordRasterDraw(passes, image_index); };

    if (raster_mode == Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode::Forward) {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "forward", { 0.20F, 0.60F, 1.00F, 1.0F });
        gpuTiming.writeTimestamp(commandBuffer, GpuTimedPass::Forward, true);
        rasterizer.recordCommands(commandBuffer,
          image_index,
          scene,
          rasterizer_descriptor_sets,
          glm::vec3(sceneUBO.cam_pos),
          camera_frustum,
          draw_sky);
        gpuTiming.writeTimestamp(commandBuffer, GpuTimedPass::Forward, false);
    } else {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "deferred", { 0.20F, 0.40F, 0.80F, 1.0F });
        const std::array<vk::DescriptorSet, 2> deferred_sets = { sharedRenderDescriptors.sets()[image_index],
            gbufferDescriptors.sets()[image_index] };
        gpuTiming.writeTimestamp(commandBuffer, GpuTimedPass::GBuffer, true);
        deferredRasterizer.recordCommands(
          commandBuffer,
          image_index,
          scene,
          deferred_sets,
          glm::vec3(sceneUBO.cam_pos),
          camera_frustum,
          [this](vk::CommandBuffer passes) {
              gpuTiming.writeTimestamp(passes, GpuTimedPass::GBuffer, false);
              gpuTiming.writeTimestamp(passes, GpuTimedPass::DeferredLighting, true);
          },
          draw_sky);
        gpuTiming.writeTimestamp(commandBuffer, GpuTimedPass::DeferredLighting, false);
    }

    // Publish only the path that recorded: the inactive one still holds pre-switch numbers.
    const bool forward_active =
      raster_mode == Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode::Forward;
    output.visibility.meshes_drawn = forward_active ? rasterizer.getMeshesDrawn() : deferredRasterizer.getMeshesDrawn();
    output.visibility.meshes_total =
      forward_active ? rasterizer.getMeshesConsidered() : deferredRasterizer.getMeshesConsidered();
}

bool Kataglyphis::VulkanRenderer::raytracingOwnsFrame(uint32_t image_index)
{ return image_index < raytracingDescriptors.sets().size() && drawnRenderMode() != 0; }

int Kataglyphis::VulkanRenderer::drawnRenderMode() const
{
    // No TLAS yet during the async model load, so the raster pass must still run.
    return Kataglyphis::VulkanRendererInternals::FrontendShared::drawnRenderMode(frameSettings.renderer.raytracing,
      frameSettings.renderer.pathTracing,
      device->supportsHardwareAcceleratedRRT() && asManager.hasTLAS());
}

void Kataglyphis::VulkanRenderer::recordRaytracingOrPathTracing(vk::CommandBuffer &commandBuffer,
  uint32_t image_index,
  bool clouds_enabled)
{
    const Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &rendererSettings =
      frameSettings.renderer;

    // Before the model loads the ray-tracing descriptor sets are unwritten.
    if (!raytracingOwnsFrame(image_index)) { return; }

    const std::array<vk::DescriptorSet, 2> raytracing_descriptor_sets = { sharedRenderDescriptors.sets()[image_index],
        raytracingDescriptors.sets()[image_index] };

    // One traced-depth image across frames: the last frame's write and the clouds' read of it come first.
    recordGeneralImageBarrier(commandBuffer,
      tracedDepth.getImage(),
      vk::PipelineStageFlagBits2::eRayTracingShaderKHR | vk::PipelineStageFlagBits2::eComputeShader,
      vk::PipelineStageFlagBits2::eRayTracingShaderKHR | vk::PipelineStageFlagBits2::eComputeShader,
      vk::AccessFlagBits2::eShaderStorageWrite,
      vk::AccessFlagBits2::eShaderStorageWrite);

    using Kataglyphis::VulkanRendererInternals::FrontendShared::GpuTimedPass;
    const int mode = drawnRenderMode();
    if (mode == 1) {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "raytracing", { 0.85F, 0.25F, 0.55F, 1.0F });
        Texture &renderResult = activeOffscreenTexture(image_index);
        gpuTiming.writeTimestamp(commandBuffer, GpuTimedPass::RayTrace, true);
        raytracingStage.recordCommands(commandBuffer, renderResult.getVulkanImage(), raytracing_descriptor_sets);
        // Where the primary ray missed, tracedDepth is 1 and the target's alpha 0: the sky fills those pixels.
        skyBox.recordTracedDraw(commandBuffer,
          image_index,
          renderResult.getImage(),
          renderResult.getImageView(),
          vulkanSwapChain.getSwapChainExtent());
        gpuTiming.writeTimestamp(commandBuffer, GpuTimedPass::RayTrace, false);
    } else if (mode == 2) {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "pathtracing", { 0.60F, 0.25F, 0.85F, 1.0F });
        Texture &renderResult = activeOffscreenTexture(image_index);

        // A camera, light or quality change restarts the running mean.
        const Kataglyphis::VulkanRendererInternals::PathTracingHistoryKey current_history =
          Kataglyphis::VulkanRendererInternals::makePathTracingHistoryKey(camera->calculate_viewmatrix(),
            globalUBO.projection,
            sceneUBO.dirLight,
            rendererSettings.pathTracingSamplesPerPixel,
            rendererSettings.pathTracingMaxBounces,
            frameSettings.scene.skybox_enabled);
        if (current_history != pathTracingLastHistory) {
            pathTracingAccumulatedFrames = 0;
            pathTracingLastHistory = current_history;
        }

        gpuTiming.writeTimestamp(commandBuffer, GpuTimedPass::PathTrace, true);
        pathTracing.recordCommands(commandBuffer,
          image_index,
          renderResult.getVulkanImage(),
          pathTracingAccumulation.getVulkanImage(),
          &vulkanSwapChain,
          raytracing_descriptor_sets,
          pathTracingAccumulatedFrames,
          static_cast<uint32_t>(std::max(rendererSettings.pathTracingSamplesPerPixel, 1)),
          static_cast<uint32_t>(std::max(rendererSettings.pathTracingMaxBounces, 1)),
          clouds_enabled,
          frameSettings.scene.skybox_enabled);
        gpuTiming.writeTimestamp(commandBuffer, GpuTimedPass::PathTrace, false);
        ++pathTracingAccumulatedFrames;
        output.pathTracingAccumulatedFrames = pathTracingAccumulatedFrames;
    }
}

auto Kataglyphis::VulkanRenderer::supportsFrameCapture() const -> bool
{ return device != nullptr && frameCapture.supportsCapture(vulkanSwapChain, device_lost_detected); }

void Kataglyphis::VulkanRenderer::requestFrameCapture()
{
    if (!supportsFrameCapture()) {
        spdlog::warn("Frame capture requested but the surface does not support eTransferSrc; ignoring.");
        return;
    }

    frameCapture.request();
}

auto Kataglyphis::VulkanRenderer::takeCapturedFrame(uint32_t &outWidth, uint32_t &outHeight) -> std::vector<uint8_t>
{ return frameCapture.take(device, device_lost_detected, outWidth, outHeight); }

void Kataglyphis::VulkanRenderer::requestHdrCapture() { hdrCapture.request(); }

auto Kataglyphis::VulkanRenderer::takeCapturedHdrFrame(uint32_t &outWidth, uint32_t &outHeight) -> std::vector<uint8_t>
{ return hdrCapture.take(device, device_lost_detected, outWidth, outHeight); }

void Kataglyphis::VulkanRenderer::cleanUpUBOs()
{
    // Buffers are persistently mapped by VMA; unmapping happens on destruction.
    for (size_t i = 0; i < globalUBOBuffer.size(); i++) { globalUBOBuffer[i].cleanUp(); }
    for (size_t i = 0; i < sceneUBOBuffer.size(); i++) { sceneUBOBuffer[i].cleanUp(); }
    globalUBOBuffer.clear();
    globalUBOMapped.clear();
    sceneUBOBuffer.clear();
    sceneUBOMapped.clear();
}

Kataglyphis::VulkanRenderer::~VulkanRenderer()
{
    if (!device) { return; }
    // Nothing may still be in flight when the members release; on a lost device the wait returns at once.
    std::ignore = device->getLogicalDevice().waitIdle();
    gpuTiming.writeJsonIfRequested();
}

auto Kataglyphis::VulkanRenderer::create_surface() -> vk::SurfaceKHR
{
    VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
    vkCheck(glfwCreateWindowSurface(instance->getVulkanInstance(), window->get_window(), nullptr, &rawSurface),
      "Failed to create a surface!");
    return vk::SurfaceKHR(rawSurface);
}

void Kataglyphis::VulkanRenderer::create_post_descriptor_resources()
{
    postDescriptors.addBinding(0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment)
      .addBinding(1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment);

    if (!postDescriptors.create(device, vulkanSwapChain.getNumberSwapChainImages())) {
        spdlog::error("Failed to create post descriptor resources!");
    }
}

void Kataglyphis::VulkanRenderer::updatePostDescriptorSets()
{
    if (postDescriptors.sets().size() < vulkanSwapChain.getNumberSwapChainImages()) {
        spdlog::error("Post descriptor sets are not available; skipping update.");
        return;
    }

    for (uint32_t i = 0; i < vulkanSwapChain.getNumberSwapChainImages(); i++) {
        Texture &renderResult = activeOffscreenTexture(i);
        postDescriptors.writeImage(
          i, 0, renderResult.getImageView(), vk::ImageLayout::eShaderReadOnlyOptimal, postStage.getOffscreenSampler());
        postDescriptors.writeImage(i,
          1,
          clouds.getCloudOutputTexture()->getImageView(),
          vk::ImageLayout::eGeneral,
          clouds.getCloudOutputTexture()->getSampler());
    }
}

void Kataglyphis::VulkanRenderer::createRaytracingDescriptorResources()
{
    const vk::ShaderStageFlags raytracing_stages =
      vk::ShaderStageFlagBits::eRaygenKHR | vk::ShaderStageFlagBits::eClosestHitKHR | vk::ShaderStageFlagBits::eCompute;

    raytracingDescriptors.addBinding(TLAS_BINDING, vk::DescriptorType::eAccelerationStructureKHR, 1, raytracing_stages)
      .addBinding(OUT_IMAGE_BINDING, vk::DescriptorType::eStorageImage, 1, raytracing_stages)
      .addBinding(ACCUMULATION_IMAGE_BINDING, vk::DescriptorType::eStorageImage, 1, raytracing_stages)
      .addBinding(TRACED_DEPTH_IMAGE_BINDING, vk::DescriptorType::eStorageImage, 1, raytracing_stages)
      .addBinding(
        SKYBOX_CUBEMAP_BINDING, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute);

    if (!raytracingDescriptors.create(device, vulkanSwapChain.getNumberSwapChainImages())) {
        spdlog::error("Failed to create raytracing descriptor resources!");
    }
}

void Kataglyphis::VulkanRenderer::createPathTracingAccumulationResources()
{
    // Also the resize path: drop any previous image (and with it the history).
    pathTracingAccumulation.cleanUp();

    vk::Extent2D const extent = vulkanSwapChain.getSwapChainExtent();
    Kataglyphis::VulkanRendererInternals::createColorAttachment(pathTracingAccumulation,
      device,
      extent,
      vk::Format::eR32G32B32A32Sfloat,
      vk::ImageUsageFlagBits::eStorage,
      "path-tracing accumulation");

    // Storage images stay in eGeneral for their whole lifetime.
    vk::CommandBuffer commandBuffer = Kataglyphis::VulkanRendererInternals::CommandBufferManager::beginCommandBuffer(
      device->getLogicalDevice(), graphics_command_pool.get());
    if (!commandBuffer) {
        spdlog::error("Failed to begin command buffer for path tracing accumulation image transition!");
        pathTracingAccumulation.cleanUp();
        return;
    }
    pathTracingAccumulation.getVulkanImage().transitionImageLayout(
      commandBuffer, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, 1, vk::ImageAspectFlagBits::eColor);
    // clang-format off: BuildIntegrity reads a submit's result off the call's own line.
    bool const transition_submitted = Kataglyphis::VulkanRendererInternals::CommandBufferManager::endAndSubmitCommandBuffer(
      device->getLogicalDevice(), graphics_command_pool.get(), device->getGraphicsQueue(), commandBuffer);
    // clang-format on
    if (!transition_submitted) {
        spdlog::error("Failed to submit path tracing accumulation image transition!");
        pathTracingAccumulation.cleanUp();
        return;
    }

    pathTracingAccumulatedFrames = 0;
}

void Kataglyphis::VulkanRenderer::createTracedDepthResources()
{
    tracedDepth.cleanUp();

    Kataglyphis::VulkanRendererInternals::createColorAttachment(tracedDepth,
      device,
      vulkanSwapChain.getSwapChainExtent(),
      vk::Format::eR32Sfloat,
      vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
      "traced depth");

    // RT/PT write it as a storage image and the clouds sample it, so it stays in eGeneral.
    vk::CommandBuffer commandBuffer = Kataglyphis::VulkanRendererInternals::CommandBufferManager::beginCommandBuffer(
      device->getLogicalDevice(), graphics_command_pool.get());
    if (!commandBuffer) {
        spdlog::error("Failed to begin command buffer for the traced depth image transition!");
        tracedDepth.cleanUp();
        return;
    }
    tracedDepth.getVulkanImage().transitionImageLayout(
      commandBuffer, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, 1, vk::ImageAspectFlagBits::eColor);
    // clang-format off: BuildIntegrity reads a submit's result off the call's own line.
    bool const transition_submitted = Kataglyphis::VulkanRendererInternals::CommandBufferManager::endAndSubmitCommandBuffer(device->getLogicalDevice(), graphics_command_pool.get(), device->getGraphicsQueue(), commandBuffer);
    // clang-format on
    if (!transition_submitted) {
        spdlog::error("Failed to submit the traced depth image transition!");
        tracedDepth.cleanUp();
    }
}

void Kataglyphis::VulkanRenderer::rebuildObjectDescriptions()
{
    objectDescriptionBuffer.cleanUp();
    punctualLightBuffer.cleanUp();
    create_object_description_buffer();
}

void Kataglyphis::VulkanRenderer::create_object_description_buffer()
{
    std::vector<ObjectDescription> objectDescriptions = scene->getObjectDescriptions();

    // Shaders add each model's texture offset to its model-local textureIDs.
    Kataglyphis::assignTextureOffsets(
      objectDescriptions, scene->getMeshCountPerModel(), scene->getTextureCountPerModel());

    if (!objectDescriptions.empty()) {
        if (!vulkanBufferManager.createBufferAndUploadVectorOnDevice(device,
              graphics_command_pool.get(),
              objectDescriptionBuffer,
              vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
              vk::MemoryPropertyFlagBits::eDeviceLocal,
              objectDescriptions)) {
            spdlog::error(
              "VulkanRenderer::create_object_description_buffer: upload failed; object-description buffer left "
              "unwritten.");
        }
    } else {
        // Create an empty buffer (1 byte) if no object descriptions are present to avoid validation error
        if (!vulkanBufferManager.createBufferAndUploadVectorOnDevice(device,
              graphics_command_pool.get(),
              objectDescriptionBuffer,
              vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
              vk::MemoryPropertyFlagBits::eDeviceLocal,
              std::vector<uint32_t>{ 0 })) {
            spdlog::error("VulkanRenderer::create_object_description_buffer: empty placeholder buffer upload failed.");
        }
    }

    // Never empty: the count row is always there, so a scene without lights reads zero.
    std::vector<glm::vec4> packedLights = scene->packPunctualLights();
    applyPunctualShadowSlots(packedLights);
    if (!vulkanBufferManager.createBufferAndUploadVectorOnDevice(device,
          graphics_command_pool.get(),
          punctualLightBuffer,
          vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
          vk::MemoryPropertyFlagBits::eDeviceLocal,
          packedLights)) {
        spdlog::error("VulkanRenderer::create_object_description_buffer: punctual-light upload failed.");
    }

    updateObjectDescriptionDescriptorSets();
}

void Kataglyphis::VulkanRenderer::updateObjectDescriptionDescriptorSets()
{
    if (!objectDescriptionBuffer.getBuffer()) { return; }

    for (uint32_t i = 0; i < vulkanSwapChain.getNumberSwapChainImages(); i++) {
        if (i >= sharedRenderDescriptors.sets().size()) { break; }

        sharedRenderDescriptors.writeBuffer(
          i, OBJECT_DESCRIPTION_BINDING, objectDescriptionBuffer.getBuffer(), VK_WHOLE_SIZE);
        if (punctualLightBuffer.getBuffer()) {
            sharedRenderDescriptors.writeBuffer(
              i, PUNCTUAL_LIGHTS_BINDING, punctualLightBuffer.getBuffer(), VK_WHOLE_SIZE);
        }
    }
}

void Kataglyphis::VulkanRenderer::updateRaytracingDescriptorSets()
{
    vk::AccelerationStructureKHR &vulkanTLAS = asManager.getTLAS();
    if (!vulkanTLAS) { return; }

    // The traced world changed, so history from the half-loaded scene must not stay in the mean.
    pathTracingAccumulatedFrames = 0;

    for (uint32_t i = 0; i < vulkanSwapChain.getNumberSwapChainImages(); i++) { writeRaytracingDescriptorsForImage(i); }
}

void Kataglyphis::VulkanRenderer::createSharedRenderDescriptorResources()
{
    // Only a warning: MAX_TEXTURE_COUNT is a shader constant, so shrinking the host array would desync them.
    if (const uint32_t max_sampled_images = device->getMaxPerStageDescriptorSampledImages();
      max_sampled_images < static_cast<uint32_t>(MAX_TEXTURE_COUNT)) {
        spdlog::critical(
          "Device maxPerStageDescriptorSampledImages ({}) is below MAX_TEXTURE_COUNT ({}) - texture binding may fail.",
          max_sampled_images,
          MAX_TEXTURE_COUNT);
    }
    if (const uint32_t max_samplers = device->getMaxPerStageDescriptorSamplers();
      max_samplers < static_cast<uint32_t>(MAX_TEXTURE_COUNT)) {
        spdlog::critical(
          "Device maxPerStageDescriptorSamplers ({}) is below MAX_TEXTURE_COUNT ({}) - texture binding may fail.",
          max_samplers,
          MAX_TEXTURE_COUNT);
    }

    const bool raytracing_available = device->supportsHardwareAcceleratedRRT();

    // eFragment: deferred lighting reconstructs position from depth with the inverse matrices.
    vk::ShaderStageFlags global_ubo_stages = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    vk::ShaderStageFlags scene_ubo_stages = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    vk::ShaderStageFlags object_description_stages =
      vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    vk::ShaderStageFlags sampler_stages = vk::ShaderStageFlagBits::eFragment;
    vk::ShaderStageFlags textures_stages = vk::ShaderStageFlagBits::eFragment;
    vk::ShaderStageFlags punctual_light_stages = vk::ShaderStageFlagBits::eFragment;
    vk::ShaderStageFlags ambient_map_stages = vk::ShaderStageFlagBits::eFragment;

    if (raytracing_available) {
        global_ubo_stages |= vk::ShaderStageFlagBits::eRaygenKHR | vk::ShaderStageFlagBits::eCompute;
        scene_ubo_stages |= vk::ShaderStageFlagBits::eRaygenKHR | vk::ShaderStageFlagBits::eClosestHitKHR
                            | vk::ShaderStageFlagBits::eCompute;
        object_description_stages |= vk::ShaderStageFlagBits::eClosestHitKHR | vk::ShaderStageFlagBits::eAnyHitKHR
                                     | vk::ShaderStageFlagBits::eCompute;
        sampler_stages |= vk::ShaderStageFlagBits::eClosestHitKHR | vk::ShaderStageFlagBits::eAnyHitKHR
                          | vk::ShaderStageFlagBits::eCompute;
        textures_stages |= vk::ShaderStageFlagBits::eClosestHitKHR | vk::ShaderStageFlagBits::eAnyHitKHR
                           | vk::ShaderStageFlagBits::eCompute;
        punctual_light_stages |= vk::ShaderStageFlagBits::eClosestHitKHR | vk::ShaderStageFlagBits::eCompute;
        // eCompute: a path-traced bounce that misses reads the prefiltered map's sharpest level.
        ambient_map_stages |= vk::ShaderStageFlagBits::eClosestHitKHR | vk::ShaderStageFlagBits::eCompute;
    }

    sharedRenderDescriptors.addBinding(globalUBO_BINDING, vk::DescriptorType::eUniformBuffer, 1, global_ubo_stages)
      .addBinding(sceneUBO_BINDING, vk::DescriptorType::eUniformBuffer, 1, scene_ubo_stages)
      .addBinding(OBJECT_DESCRIPTION_BINDING, vk::DescriptorType::eStorageBuffer, 1, object_description_stages)
      .addBinding(
        TEXTURES_BINDING, vk::DescriptorType::eSampledImage, static_cast<uint32_t>(MAX_TEXTURE_COUNT), textures_stages)
      .addBinding(
        SAMPLER_BINDING, vk::DescriptorType::eSampler, static_cast<uint32_t>(MAX_TEXTURE_COUNT), sampler_stages)
      // Cascaded shadow map array, consumed by the forward lighting shader.
      .addBinding(SHADOW_MAP_BINDING, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment)
      .addBinding(PUNCTUAL_LIGHTS_BINDING, vk::DescriptorType::eStorageBuffer, 1, punctual_light_stages)
      // The punctual lights' cube shadow maps, sampled by the forward and deferred lighting shaders.
      .addBinding(
        PUNCTUAL_SHADOW_MAP_BINDING, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment)
      // The skybox's baked environment, read by the raster and traced modes' ambient term and the path tracer's bounce sky.
      .addBinding(IBL_IRRADIANCE_BINDING, vk::DescriptorType::eCombinedImageSampler, 1, ambient_map_stages)
      .addBinding(IBL_PREFILTERED_BINDING, vk::DescriptorType::eCombinedImageSampler, 1, ambient_map_stages)
      .addBinding(IBL_BRDF_LUT_BINDING, vk::DescriptorType::eCombinedImageSampler, 1, ambient_map_stages);

    if (!sharedRenderDescriptors.create(device, vulkanSwapChain.getNumberSwapChainImages())) {
        spdlog::error("Failed to create shared render descriptor resources!");
        return;
    }

    // Other bindings are written once their resources exist.
    updateUBODescriptorSets();
}

void Kataglyphis::VulkanRenderer::create_command_pool()
{
    Kataglyphis::VulkanRendererInternals::QueueFamilyIndices const queue_family_indices = device->getQueueFamilies();

    {
        vk::CommandPoolCreateInfo pool_info{};
        pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
        pool_info.queueFamilyIndex = static_cast<uint32_t>(queue_family_indices.graphics_family);

        vk::CommandPool pool{};
        vk::Result const result = device->getLogicalDevice().createCommandPool(&pool_info, nullptr, &pool);
        if (result != vk::Result::eSuccess) {
            spdlog::error("Failed to create graphics command pool! Error: {}", static_cast<int>(result));
            std::abort();
        }
        graphics_command_pool = vk::UniqueCommandPool(pool, device->getLogicalDevice());
    }
}

void Kataglyphis::VulkanRenderer::create_command_buffers()
{
    if (!command_buffers.empty()) {
        device->getLogicalDevice().freeCommandBuffers(graphics_command_pool.get(), command_buffers);
    }
    command_buffers.resize(vulkanSwapChain.getNumberSwapChainImages());

    vk::CommandBufferAllocateInfo command_buffer_alloc_info{};
    command_buffer_alloc_info.commandPool = graphics_command_pool.get();
    command_buffer_alloc_info.level = vk::CommandBufferLevel::ePrimary;

    command_buffer_alloc_info.commandBufferCount = static_cast<uint32_t>(command_buffers.size());

    vk::Result const result =
      device->getLogicalDevice().allocateCommandBuffers(&command_buffer_alloc_info, command_buffers.data());
    vkCheck(result, "Failed to allocate command buffers!");
}

void Kataglyphis::VulkanRenderer::createSynchronization()
{ frameSync.create(device->getLogicalDevice(), vulkanSwapChain.getNumberSwapChainImages()); }

void Kataglyphis::VulkanRenderer::create_uniform_buffers()
{
    const uint32_t imageCount = vulkanSwapChain.getNumberSwapChainImages();
    globalUBOBuffer.resize(imageCount);
    globalUBOMapped.resize(imageCount);
    sceneUBOBuffer.resize(imageCount);
    sceneUBOMapped.resize(imageCount);

    for (size_t i = 0; i < imageCount; i++) {
        globalUBOBuffer[i].create(device,
          sizeof(VulkanRendererInternals::GlobalUBO),
          vk::BufferUsageFlagBits::eUniformBuffer,
          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

        // Host-visible UBOs are persistently mapped by VMA.
        globalUBOMapped[i] = globalUBOBuffer[i].getMappedData();

        sceneUBOBuffer[i].create(device,
          sizeof(VulkanRendererInternals::SceneUBO),
          vk::BufferUsageFlagBits::eUniformBuffer,
          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

        sceneUBOMapped[i] = sceneUBOBuffer[i].getMappedData();

        // Initial upload
        update_uniform_buffers(static_cast<uint32_t>(i));
    }
}

void Kataglyphis::VulkanRenderer::updateTexturesInSharedRenderDescriptorSet()
{
    if (sharedRenderDescriptors.sets().empty()) { return; }

    // Before the model early-returns: the shadow map exists without scene textures.
    if (Kataglyphis::Texture *shadow_map_array = dirShadowMap.getShadowMapArray();
      shadow_map_array != nullptr && shadow_map_array->getSampler()) {
        for (uint32_t i = 0; i < vulkanSwapChain.getNumberSwapChainImages(); i++) {
            sharedRenderDescriptors.writeImage(i,
              SHADOW_MAP_BINDING,
              shadow_map_array->getImageView(),
              vk::ImageLayout::eShaderReadOnlyOptimal,
              shadow_map_array->getSampler());
        }
    }

    if (Kataglyphis::Texture *cubes = punctualShadows.getCubeArray(); cubes != nullptr && cubes->getSampler()) {
        for (uint32_t i = 0; i < vulkanSwapChain.getNumberSwapChainImages(); i++) {
            sharedRenderDescriptors.writeImage(i,
              PUNCTUAL_SHADOW_MAP_BINDING,
              cubes->getImageView(),
              vk::ImageLayout::eShaderReadOnlyOptimal,
              cubes->getSampler());
        }
    }

    if (ambientMaps.ready()) {
        const std::array<std::pair<int, Kataglyphis::Texture *>, 3> maps = {
            std::pair{ IBL_IRRADIANCE_BINDING, &ambientMaps.getIrradiance() },
            std::pair{ IBL_PREFILTERED_BINDING, &ambientMaps.getPrefiltered() },
            std::pair{ IBL_BRDF_LUT_BINDING, &ambientMaps.getBrdfLut() },
        };
        for (uint32_t i = 0; i < vulkanSwapChain.getNumberSwapChainImages(); i++) {
            for (const auto &[binding, texture] : maps) {
                sharedRenderDescriptors.writeImage(i,
                  static_cast<uint32_t>(binding),
                  texture->getImageView(),
                  vk::ImageLayout::eShaderReadOnlyOptimal,
                  texture->getSampler());
            }
        }
    }

    if (scene->getModelCount() == 0) { return; }

    // Model order must match assignTextureOffsets; both read Scene::getTextureCountPerModel().
    const Kataglyphis::FlattenedTexturePlan plan = Kataglyphis::planFlattenedTextureSlots(
      scene->getTextureCountPerModel(), static_cast<uint32_t>(MAX_TEXTURE_COUNT));

    if (plan.exhausted) {
        spdlog::warn(
          "Texture slots exhausted: {} textures across {} models exceed MAX_TEXTURE_COUNT={} - "
          "models past the cap will sample the wrong slots.",
          plan.requestedCount,
          scene->getModelCount(),
          MAX_TEXTURE_COUNT);
    }

    if (plan.slots.empty()) { return; }

    std::vector<vk::DescriptorImageInfo> image_info_textures;
    std::vector<vk::DescriptorImageInfo> image_info_texture_sampler;
    image_info_textures.reserve(plan.slots.size());
    image_info_texture_sampler.reserve(plan.slots.size());

    for (const Kataglyphis::FlattenedTextureSlot &slot : plan.slots) {
        vk::DescriptorImageInfo texture_info{};
        texture_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        texture_info.imageView = scene->getTextures(slot.model)[slot.indexInModel].getImageView();
        image_info_textures.push_back(texture_info);

        vk::DescriptorImageInfo sampler_info{};
        sampler_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        sampler_info.sampler = scene->getTextureSampler(slot.model)[slot.indexInModel];
        image_info_texture_sampler.push_back(sampler_info);
    }

    for (uint32_t i = 0; i < vulkanSwapChain.getNumberSwapChainImages(); i++) {
        sharedRenderDescriptors.writeImageArray(i, TEXTURES_BINDING, image_info_textures);
        sharedRenderDescriptors.writeImageArray(i, SAMPLER_BINDING, image_info_texture_sampler);
    }
}

void Kataglyphis::VulkanRenderer::create_gbuffer_descriptor_resources()
{
    // Must match deferred.slang's GBUFFER_*_BINDING textures, which the lighting pass texel-fetches (no sampler).
    static constexpr std::array<uint32_t, 4> kGBufferBindings = {
        GBUFFER_NORMAL_BINDING, GBUFFER_ALBEDO_BINDING, GBUFFER_MATERIAL_BINDING, GBUFFER_DEPTH_BINDING
    };
    for (const uint32_t binding : kGBufferBindings) {
        gbufferDescriptors.addBinding(
          binding, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment);
    }

    if (!gbufferDescriptors.create(device, vulkanSwapChain.getNumberSwapChainImages())) {
        spdlog::error("Failed to create gbuffer descriptor resources!");
    }
}

void Kataglyphis::VulkanRenderer::updateGBufferDescriptorSets()
{
    for (uint32_t i = 0; i < vulkanSwapChain.getNumberSwapChainImages(); i++) {
        gbufferDescriptors.writeImage(
          i, GBUFFER_NORMAL_BINDING, deferredRasterizer.getGBufferNormal(i), vk::ImageLayout::eShaderReadOnlyOptimal);
        gbufferDescriptors.writeImage(
          i, GBUFFER_ALBEDO_BINDING, deferredRasterizer.getGBufferAlbedo(i), vk::ImageLayout::eShaderReadOnlyOptimal);
        gbufferDescriptors.writeImage(i,
          GBUFFER_MATERIAL_BINDING,
          deferredRasterizer.getGBufferMaterial(i),
          vk::ImageLayout::eShaderReadOnlyOptimal);
        gbufferDescriptors.writeImage(i,
          GBUFFER_DEPTH_BINDING,
          deferredRasterizer.getDepthBufferImageView(),
          vk::ImageLayout::eShaderReadOnlyOptimal);
    }
}
