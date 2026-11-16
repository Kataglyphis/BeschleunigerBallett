module;
#include "common/FrameSubmitHelper.hpp"
#include "common/GuiModelTransform.hpp"
#include "common/ImageBarrierHelper.hpp"
#include "common/SceneUboMarshal.hpp"
#include "common/Utilities.hpp"
#include "common/host_device_shared_vars.hpp"
#include "renderer/PathTracingHistory.hpp"
#include "renderer/pushConstants/PushConstantPost.hpp"
#include "renderer/pushConstants/PushConstantRasterizer.hpp"
#include "renderer/pushConstants/PushConstantRayTracing.hpp"
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

    vulkanSwapChain.initVulkanContext(device, window, device->getSurface());
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

    std::array<vk::DescriptorSetLayout, 2> const descriptor_set_layouts_post = { postDescriptors.getLayout(),
        sharedRenderDescriptors.getLayout() };
    postStage.init(device, &vulkanSwapChain, descriptor_set_layouts_post);

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

    skyBox.init(device, graphics_command_pool.get());
    skyBox.createGraphicsPipeline(sharedRenderDescriptors.getLayout(), vulkanSwapChain.getSwapChainFormat());

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
    camera_data->set_fov(std::clamp(sceneSettings.camera_fov, 20.0F, 110.0F));

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
      sceneSettings.directional_light_radiance);

    // Populate the settings into SceneUBO
    sceneUBO.pcfRadius = clampPcfRadius(sceneSettings.pcf_radius);
    sceneUBO.cascadedShadowIntensity = sceneSettings.cascaded_shadow_intensity;

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
    const size_t active_cascades = std::min(cascadeData.size(), static_cast<size_t>(MAX_CASCADES));
    std::array<float, MAX_CASCADES> cascadeSplitDepths{};
    std::array<glm::mat4, MAX_CASCADES> cascadeViewProjMatrices{};
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
      sceneSettings.cloud_powder_effect);
    fillSceneUboCloudAmbient(sceneUBO, sceneSettings.skybox_enabled, skyBox.getCloudAmbient());
}

auto Kataglyphis::VulkanRenderer::supportsHardwareRaytracing() const -> bool
{ return device && device->supportsHardwareAcceleratedRRT(); }

void Kataglyphis::VulkanRenderer::finishModelLoad() { refreshAfterSceneChange(true); }

void Kataglyphis::VulkanRenderer::refreshAfterSceneChange(bool rebuildBottomLevel)
{
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
    const GUISceneSharedVars &sceneSettings = settings.scene;

    // Polled first, so the rest of the frame sees a model on the frame it arrives.
    bool published_load = false;
    if (scene->pollModelLoad(device, graphics_command_pool.get())) {
        finishModelLoad();
        // A reload requested this frame replaces the model below and publishes its own.
        if (!sceneSettings.model_reload_requested) {
            publishLoadedModel();
            published_load = true;
        }
    }

    handleShaderHotReloadRequest(settings.renderer);
    handleRasterizationModeChange(settings.renderer);

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

void Kataglyphis::VulkanRenderer::reinitShadowMapForCurrentSettings()
{
    const GUISceneSharedVars &sceneSettings = frameSettings.scene;

    dirShadowMap.cleanUp();

    const uint32_t shadow_res = shadowResolutionForIndex(sceneSettings.shadow_map_res_index);

    // Clamp to the SceneUBO's MAX_CASCADES and to maxMultiviewViewCount, which bounds the shadow pass's viewMask.
    const auto device_view_limit = device->getMaxMultiviewViewCount();
    const auto cascade_count = clampCascadeCount(
      static_cast<uint32_t>(sceneSettings.num_shadow_cascades), static_cast<uint32_t>(MAX_CASCADES), device_view_limit);
    if (cascade_count == device_view_limit && device_view_limit < static_cast<uint32_t>(MAX_CASCADES)) {
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

// Must reach every SPIR-V-loading stage (BuildIntegrity.EverySpirvLoadingSubsystemImplementsShaderHotReload).
void Kataglyphis::VulkanRenderer::shaderHotReload()
{
    std::ignore = device->getLogicalDevice().waitIdle();

    std::array<vk::DescriptorSetLayout, 1> const descriptor_set_layouts = { sharedRenderDescriptors.getLayout() };
    rasterizer.shaderHotReload(descriptor_set_layouts);

    std::array<vk::DescriptorSetLayout, 2> const descriptor_set_layouts_deferred = {
        sharedRenderDescriptors.getLayout(), gbufferDescriptors.getLayout()
    };
    deferredRasterizer.shaderHotReload(descriptor_set_layouts_deferred);

    std::array<vk::DescriptorSetLayout, 2> const descriptor_set_layouts_post = { postDescriptors.getLayout(),
        sharedRenderDescriptors.getLayout() };
    postStage.shaderHotReload(descriptor_set_layouts_post);

    skyBox.shaderHotReload(sharedRenderDescriptors.getLayout());
    dirShadowMap.shaderHotReload();
    clouds.shaderHotReload(sharedRenderDescriptors.getLayout());

    if (device->supportsHardwareAcceleratedRRT()) {
        std::array<vk::DescriptorSetLayout, 2> const layouts = { sharedRenderDescriptors.getLayout(),
            raytracingDescriptors.getLayout() };
        raytracingStage.shaderHotReload(layouts);
        pathTracing.shaderHotReload(layouts);
    }
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

    if (device->supportsHardwareAcceleratedRRT()) {
        raytracingDescriptors.cleanUp();
        createRaytracingDescriptorResources();
    }
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
    vulkanSwapChain.recreate(device, device->getSurface());

    // The query pool is sized per swapchain image.
    gpuTiming.create(*device, vulkanSwapChain.getNumberSwapChainImages(), output.gpuTimings);

    uint32_t newImageCount = vulkanSwapChain.getNumberSwapChainImages();

    // The extent-sized offscreen, G-buffer and depth targets follow the new swapchain.
    rasterizer.recreateFrameResources(graphics_command_pool.get());
    deferredRasterizer.recreateFrameResources();
    clouds.recreateFrameResources(graphics_command_pool.get(),
      vulkanSwapChain.getSwapChainExtent().width,
      vulkanSwapChain.getSwapChainExtent().height);

    // The accumulation history is extent-sized; recreating it also resets the frame counter.
    if (device->supportsHardwareAcceleratedRRT()) {
        createPathTracingAccumulationResources();
        createTracedDepthResources();
    }

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

    const GUISceneSharedVars &sceneSettings = frameSettings.scene;
    const Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &rendererSettings =
      frameSettings.renderer;

    vk::CommandBuffer &commandBuffer = command_buffers[image_index];

    namespace FrontendShared = Kataglyphis::VulkanRendererInternals::FrontendShared;
    using FrontendShared::GpuTimedPass;

    // Per-pass GPU timing: the query slice must be reset outside any render pass.
    const bool record_gpu_timings = gpuTiming.isSupported() && gpuTiming.queryPool();
    const uint32_t gpu_timing_base = image_index * gpuTiming.queriesPerImage();
    uint32_t recorded_pass_mask = 0U;

    if (record_gpu_timings) {
        commandBuffer.resetQueryPool(gpuTiming.queryPool(), gpu_timing_base, gpuTiming.queriesPerImage());
    }

    const auto write_pass_timestamp = [&](GpuTimedPass pass, bool start) -> void {
        if (!record_gpu_timings) { return; }
        const uint32_t pass_index = static_cast<uint32_t>(pass);
        const uint32_t query =
          gpu_timing_base + pass_index * Kataglyphis::GpuTimingSubsystem::QUERIES_PER_PASS + (start ? 0U : 1U);
        commandBuffer.writeTimestamp(
          start ? vk::PipelineStageFlagBits::eTopOfPipe : vk::PipelineStageFlagBits::eBottomOfPipe,
          gpuTiming.queryPool(),
          query);
        if (!start) { recorded_pass_mask |= (1U << pass_index); }
    };

    const std::array<vk::DescriptorSet, 1> rasterizer_descriptor_sets = { sharedRenderDescriptors.sets()[image_index] };
    const bool tracing_owns_frame = raytracingOwnsFrame(image_index);

    {
        // RT traces one binary shadow ray and PT uses NEE: neither samples the cascades.
        if (sceneSettings.shadows_enabled && !tracing_owns_frame) {
            Kataglyphis::debug::ScopedCmdLabel const label(
              commandBuffer, "shadow_cascades", { 0.55F, 0.35F, 0.10F, 1.0F });
            write_pass_timestamp(GpuTimedPass::ShadowCascades, true);
            dirShadowMap.recordCommands(
              commandBuffer, image_index, scene, rasterizer_descriptor_sets, rendererSettings.frustum_culling_enabled);
            write_pass_timestamp(GpuTimedPass::ShadowCascades, false);
            output.visibility.shadow_casters_drawn = dirShadowMap.getCastersDrawn();
            output.visibility.shadow_casters_total = dirShadowMap.getCastersConsidered();
        } else {
            dirShadowMap.recordSkippedPass(commandBuffer);
            output.visibility.shadow_casters_drawn = 0;
            output.visibility.shadow_casters_total = 0;
        }
    }

    write_pass_timestamp(GpuTimedPass::Main, true);

    // From globalUBO, like the shaders; the shadow pass above must not use it, as off-screen geometry casts into view.
    const std::optional<FrustumPlanes> camera_frustum =
      rendererSettings.frustum_culling_enabled
        ? std::optional<FrustumPlanes>(extractFrustumPlanes(globalUBO.projection * globalUBO.view))
        : std::nullopt;

    if (!tracing_owns_frame) {
        recordRasterPass(commandBuffer, image_index, rasterizer_descriptor_sets, camera_frustum);
    } else {
        output.visibility.meshes_drawn = 0;
        output.visibility.meshes_total = 0;
    }

    recordRaytracingOrPathTracing(commandBuffer, image_index, sceneSettings.clouds_enabled);

    write_pass_timestamp(GpuTimedPass::Main, false);

    // After the main pass: the march stops at the depth it just wrote.
    if (sceneSettings.clouds_enabled) {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "clouds", { 0.80F, 0.85F, 0.95F, 1.0F });
        write_pass_timestamp(GpuTimedPass::Clouds, true);

        // Cross-frame WAR on the single cloudOutputTexture; the frame fence alone covers only 3 frames back.
        recordGeneralImageBarrier(commandBuffer,
          clouds.getCloudOutputTexture()->getImage(),
          vk::PipelineStageFlagBits2::eFragmentShader,
          vk::PipelineStageFlagBits2::eComputeShader,
          vk::AccessFlagBits2::eNone,
          vk::AccessFlagBits2::eShaderStorageWrite);

        // A raster depth buffer reaches this read through its pass's closing barrier; traced depth does not.
        if (tracing_owns_frame) {
            recordGeneralImageBarrier(commandBuffer,
              tracedDepth.getImage(),
              vk::PipelineStageFlagBits2::eRayTracingShaderKHR | vk::PipelineStageFlagBits2::eComputeShader,
              vk::PipelineStageFlagBits2::eComputeShader,
              vk::AccessFlagBits2::eShaderStorageWrite,
              vk::AccessFlagBits2::eShaderSampledRead);
        }

        clouds.recordComputeCommands(commandBuffer, rasterizer_descriptor_sets);

        // The post pass samples the clouds; nothing else orders this compute write before it.
        recordGeneralImageBarrier(commandBuffer,
          clouds.getCloudOutputTexture()->getImage(),
          vk::PipelineStageFlagBits2::eComputeShader,
          vk::PipelineStageFlagBits2::eFragmentShader,
          vk::AccessFlagBits2::eShaderStorageWrite,
          vk::AccessFlagBits2::eShaderSampledRead);

        write_pass_timestamp(GpuTimedPass::Clouds, false);
    }

    write_pass_timestamp(GpuTimedPass::Sky, true);
    {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "sky", { 0.30F, 0.70F, 0.90F, 1.0F });
        Texture &swap_chain_image = vulkanSwapChain.getSwapChainImage(image_index);
        skyBox.recordCommands(commandBuffer,
          swap_chain_image.getImage(),
          swap_chain_image.getImageView(),
          vulkanSwapChain.getSwapChainExtent(),
          rasterizer_descriptor_sets,
          sceneSettings.skybox_enabled);
    }
    write_pass_timestamp(GpuTimedPass::Sky, false);

    // No swapchain barrier here: PostStage orders its load after the sky and leaves the image presentable.
    write_pass_timestamp(GpuTimedPass::Post, true);
    {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "post", { 0.35F, 0.80F, 0.40F, 1.0F });
        const std::array<vk::DescriptorSet, 2> post_descriptor_sets = { postDescriptors.sets()[image_index],
            sharedRenderDescriptors.sets()[image_index] };
        postStage.recordCommands(
          commandBuffer, image_index, post_descriptor_sets, sceneSettings.clouds_enabled, sceneSettings.skybox_enabled);
    }
    write_pass_timestamp(GpuTimedPass::Post, false);

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

    if (record_gpu_timings) { gpuTiming.setPassRecordedMask(image_index, recorded_pass_mask); }

    return true;
}

void Kataglyphis::VulkanRenderer::recordRasterPass(vk::CommandBuffer &commandBuffer,
  uint32_t image_index,
  std::span<const vk::DescriptorSet> rasterizer_descriptor_sets,
  const std::optional<FrustumPlanes> &camera_frustum)
{
    const auto raster_mode = frameSettings.renderer.rasterizationMode;

    if (raster_mode == Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode::Forward) {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "forward", { 0.20F, 0.60F, 1.00F, 1.0F });
        rasterizer.recordCommands(
          commandBuffer, image_index, scene, rasterizer_descriptor_sets, glm::vec3(sceneUBO.cam_pos), camera_frustum);
    } else {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "deferred", { 0.20F, 0.40F, 0.80F, 1.0F });
        const std::array<vk::DescriptorSet, 2> deferred_sets = { sharedRenderDescriptors.sets()[image_index],
            gbufferDescriptors.sets()[image_index] };
        deferredRasterizer.recordCommands(
          commandBuffer, image_index, scene, deferred_sets, glm::vec3(sceneUBO.cam_pos), camera_frustum);
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

    const int mode = drawnRenderMode();
    if (mode == 1) {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "raytracing", { 0.85F, 0.25F, 0.55F, 1.0F });
        Texture &renderResult = activeOffscreenTexture(image_index);
        raytracingStage.recordCommands(commandBuffer, renderResult.getVulkanImage(), raytracing_descriptor_sets);
    } else if (mode == 2) {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "pathtracing", { 0.60F, 0.25F, 0.85F, 1.0F });
        Texture &renderResult = activeOffscreenTexture(image_index);

        // A camera, light or quality change restarts the running mean.
        const Kataglyphis::VulkanRendererInternals::PathTracingHistoryKey current_history{
            .view = camera->calculate_viewmatrix(),
            .projection = globalUBO.projection,
            .lightDirection = sceneUBO.dirLight.direction,
            .lightColorAndRadiance = sceneUBO.dirLight.color,
            .samplesPerPixel = rendererSettings.pathTracingSamplesPerPixel,
            .maxBounces = rendererSettings.pathTracingMaxBounces,
        };
        if (current_history != pathTracingLastHistory) {
            pathTracingAccumulatedFrames = 0;
            pathTracingLastHistory = current_history;
        }

        pathTracing.recordCommands(commandBuffer,
          image_index,
          renderResult.getVulkanImage(),
          pathTracingAccumulation.getVulkanImage(),
          &vulkanSwapChain,
          raytracing_descriptor_sets,
          pathTracingAccumulatedFrames,
          static_cast<uint32_t>(std::max(rendererSettings.pathTracingSamplesPerPixel, 1)),
          static_cast<uint32_t>(std::max(rendererSettings.pathTracingMaxBounces, 1)),
          clouds_enabled);
        ++pathTracingAccumulatedFrames;
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
      .addBinding(1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment)
      .addBinding(2, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment);

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
        // The constructor's first update runs before the skybox exists; its second, before any frame, writes this.
        if (Kataglyphis::Texture *cube_map = skyBox.getCubeMapTexture(); cube_map != nullptr) {
            postDescriptors.writeImage(
              i, 2, cube_map->getImageView(), vk::ImageLayout::eShaderReadOnlyOptimal, cube_map->getSampler());
        }
    }
}

void Kataglyphis::VulkanRenderer::createRaytracingDescriptorResources()
{
    const vk::ShaderStageFlags raytracing_stages =
      vk::ShaderStageFlagBits::eRaygenKHR | vk::ShaderStageFlagBits::eClosestHitKHR | vk::ShaderStageFlagBits::eCompute;

    raytracingDescriptors.addBinding(TLAS_BINDING, vk::DescriptorType::eAccelerationStructureKHR, 1, raytracing_stages)
      .addBinding(OUT_IMAGE_BINDING, vk::DescriptorType::eStorageImage, 1, raytracing_stages)
      .addBinding(ACCUMULATION_IMAGE_BINDING, vk::DescriptorType::eStorageImage, 1, raytracing_stages)
      .addBinding(TRACED_DEPTH_IMAGE_BINDING, vk::DescriptorType::eStorageImage, 1, raytracing_stages);

    if (!raytracingDescriptors.create(device, vulkanSwapChain.getNumberSwapChainImages())) {
        spdlog::error("Failed to create raytracing descriptor resources!");
    }
}

void Kataglyphis::VulkanRenderer::createPathTracingAccumulationResources()
{
    // Also the resize path: drop any previous image (and with it the history).
    pathTracingAccumulation.cleanUp();

    vk::Extent2D const extent = vulkanSwapChain.getSwapChainExtent();
    Kataglyphis::VulkanRendererInternals::createColorAttachment(
      pathTracingAccumulation, device, extent, vk::Format::eR32G32B32A32Sfloat, vk::ImageUsageFlagBits::eStorage);

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
      vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled);

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

    updateObjectDescriptionDescriptorSets();
}

void Kataglyphis::VulkanRenderer::updateObjectDescriptionDescriptorSets()
{
    if (!objectDescriptionBuffer.getBuffer()) { return; }

    for (uint32_t i = 0; i < vulkanSwapChain.getNumberSwapChainImages(); i++) {
        if (i >= sharedRenderDescriptors.sets().size()) { break; }

        sharedRenderDescriptors.writeBuffer(
          i, OBJECT_DESCRIPTION_BINDING, objectDescriptionBuffer.getBuffer(), VK_WHOLE_SIZE);
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
    }

    sharedRenderDescriptors.addBinding(globalUBO_BINDING, vk::DescriptorType::eUniformBuffer, 1, global_ubo_stages)
      .addBinding(sceneUBO_BINDING, vk::DescriptorType::eUniformBuffer, 1, scene_ubo_stages)
      .addBinding(OBJECT_DESCRIPTION_BINDING, vk::DescriptorType::eStorageBuffer, 1, object_description_stages)
      .addBinding(
        TEXTURES_BINDING, vk::DescriptorType::eSampledImage, static_cast<uint32_t>(MAX_TEXTURE_COUNT), textures_stages)
      .addBinding(
        SAMPLER_BINDING, vk::DescriptorType::eSampler, static_cast<uint32_t>(MAX_TEXTURE_COUNT), sampler_stages)
      // Cascaded shadow map array, consumed by the forward lighting shader.
      .addBinding(SHADOW_MAP_BINDING, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment);

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
