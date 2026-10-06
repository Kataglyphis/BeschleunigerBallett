module;
#include <optional>
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
import kataglyphis.vulkan.gui_renderer_shared_vars;
import kataglyphis.vulkan.gui_scene_shared_vars;
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
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.scene_ubo;
import kataglyphis.vulkan.global_ubo;
import kataglyphis.vulkan.swapchain;
import kataglyphis.vulkan.window;
import kataglyphis.vulkan.color_attachment;

namespace {

// Not transitionImageLayout's eGeneral overload: it derives eAllCommands -> eAllCommands, a full stall every frame.
void recordCloudOutputBarrier(vk::CommandBuffer commandBuffer,
  vk::Image image,
  vk::PipelineStageFlags srcStage,
  vk::PipelineStageFlags dstStage,
  vk::AccessFlags srcAccess,
  vk::AccessFlags dstAccess)
{
    const vk::ImageMemoryBarrier barrier = Kataglyphis::buildImageMemoryBarrier(
      image, vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral, srcAccess, dstAccess);
    commandBuffer.pipelineBarrier(srcStage, dstStage, {}, nullptr, nullptr, barrier);
}

}// namespace

Kataglyphis::VulkanRenderer::VulkanRenderer(Kataglyphis::Frontend::Window *window,
  Scene *scene,
  Kataglyphis::Frontend::GUI *gui,
  Camera *camera)
  : window(window), scene(scene), gui(gui), camera(camera)
{
    instance = VulkanInstance();

    vk::DebugReportFlagsEXT const debugReportFlags =
      vk::DebugReportFlagBitsEXT::eError | vk::DebugReportFlagBitsEXT::eWarning;
    if (Kataglyphis::validationLayersEnabled()) {
        debug::setupDebugging(instance.getVulkanInstance(), debugReportFlags, nullptr);
    }

    create_surface();

    device = std::make_shared<VulkanDevice>(&instance, &surface);

    create_command_pool();

    vulkanSwapChain.initVulkanContext(device, window, surface);
    gpuTiming.create(*device, vulkanSwapChain.getNumberSwapChainImages(), gui->getGuiRendererSharedVars());
    create_uniform_buffers();
    create_command_buffers();

    createSynchronization();

    initDescriptorResources();

    std::array<vk::DescriptorSetLayout, 1> const descriptor_set_layouts_rasterizer = { sharedRenderDescriptors.getLayout() };
    std::array<vk::DescriptorSetLayout, 2> const descriptor_set_layouts_deferred = { sharedRenderDescriptors.getLayout(), gbufferDescriptors.getLayout() };

    rasterizer.init(device, &vulkanSwapChain, descriptor_set_layouts_rasterizer, graphics_command_pool);
    deferredRasterizer.init(device, &vulkanSwapChain, descriptor_set_layouts_deferred);

    clouds.init(device, graphics_command_pool, sharedRenderDescriptors.getLayout(), vulkanSwapChain.getSwapChainExtent().width, vulkanSwapChain.getSwapChainExtent().height);
    // Same path as later shadow-setting changes, so the GUI default is the one source of truth.
    reinitShadowMapForCurrentSettings();

    std::array<vk::DescriptorSetLayout, 1> const descriptor_set_layouts_post = { postDescriptors.getLayout() };
    postStage.init(device, &vulkanSwapChain, descriptor_set_layouts_post);

    if (device->supportsHardwareAcceleratedRRT()) {
        createRaytracingDescriptorResources();

        std::array<vk::DescriptorSetLayout, 2> const layouts = { sharedRenderDescriptors.getLayout(),
            raytracingDescriptors.getLayout() };
        raytracingStage.init(device, layouts, &vulkanSwapChain);
        pathTracing.init(device, layouts);
        createPathTracingAccumulationResources();
    }

    updateUniforms(scene, camera, gui->getGuiSceneSharedVars());
    updateAllDescriptorSets();

    skyBox.init(device, graphics_command_pool);
    skyBox.createRenderPass(vulkanSwapChain.getSwapChainFormat());
    skyBox.createGraphicsPipeline(sharedRenderDescriptors.getLayout());
    skyBox.createFramebuffers(swapchainImageViews(),
        vulkanSwapChain.getSwapChainExtent().width, vulkanSwapChain.getSwapChainExtent().height);

    // Anything depending on scene contents waits for finishModelLoad(), on the frame the model arrives.
    scene->beginModelLoadAsync();

    // Descriptors need valid contents before the first frame, while the model still loads.
    create_object_description_buffer();
    updateAllDescriptorSets();

    gui->initializeVulkanContext(device,
      instance.getVulkanInstance(),
      postStage.getRenderPass(),
      vulkanSwapChain.getNumberSwapChainImages());
    gui->setUserSelectionForRRT(device->supportsHardwareAcceleratedRRT());
}

void Kataglyphis::VulkanRenderer::updateUniforms(Scene *scene_data,
  Camera *camera_data,
  const GUISceneSharedVars &guiSceneSharedVars)
{
    const vk::Extent2D extent = vulkanSwapChain.getSwapChainExtent();
    float const aspect_ratio = aspectRatioOf(extent.width, extent.height);

    // Clamped here: tests and config loads write the GUI vars too. Readers use get_fov(), not the GUI value.
    camera_data->set_fov(std::clamp(guiSceneSharedVars.camera_fov, 20.0F, 110.0F));

    globalUBO.view = camera_data->calculate_viewmatrix();
    globalUBO.projection = makeVulkanProjection(camera_data->get_fov(),
      aspect_ratio,
      camera_data->get_near_plane(),
      camera_data->get_far_plane());

    fillSceneUboCamera(sceneUBO, camera_data->get_camera_position(), camera_data->get_camera_direction());

    fillSceneUboDirectionalLight(sceneUBO,
      glm::vec3(guiSceneSharedVars.directional_light_direction[0],
        guiSceneSharedVars.directional_light_direction[1],
        guiSceneSharedVars.directional_light_direction[2]),
      glm::vec3(guiSceneSharedVars.directional_light_color[0],
        guiSceneSharedVars.directional_light_color[1],
        guiSceneSharedVars.directional_light_color[2]),
      guiSceneSharedVars.directional_light_radiance);

    // Populate GUI state into SceneUBO
    sceneUBO.pcfRadius = clampPcfRadius(guiSceneSharedVars.pcf_radius);
    sceneUBO.cascadedShadowIntensity = guiSceneSharedVars.cascaded_shadow_intensity;

    // Calculate CSM cascades
    dirShadowMap.updateCascades(globalUBO.view, camera_data->get_fov(),
        aspect_ratio,
        camera_data->get_near_plane(), camera_data->get_far_plane(),
        glm::vec3(sceneUBO.dirLight.direction),
        guiSceneSharedVars.shadow_distance,
        guiSceneSharedVars.cascade_split_lambda);

    // Inverses for the clouds compute shader (see GlobalUBO.hpp).
    globalUBO.inv_projection = glm::inverse(globalUBO.projection);
    globalUBO.inv_view = glm::inverse(globalUBO.view);

    const auto& cascadeData = dirShadowMap.getCascadeData();
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
      guiSceneSharedVars.shadows_enabled);

    fillSceneUboClouds(sceneUBO,
      glm::vec3(guiSceneSharedVars.cloud_mesh_scale[0],
        guiSceneSharedVars.cloud_mesh_scale[1],
        guiSceneSharedVars.cloud_mesh_scale[2]),
      guiSceneSharedVars.cloud_density_multiplier,
      glm::vec3(guiSceneSharedVars.cloud_mesh_offset[0],
        guiSceneSharedVars.cloud_mesh_offset[1],
        guiSceneSharedVars.cloud_mesh_offset[2]),
      guiSceneSharedVars.cloud_coverage_threshold,
      guiSceneSharedVars.cloud_num_march_steps,
      guiSceneSharedVars.cloud_num_march_steps_to_light,
      guiSceneSharedVars.cloud_pillowness,
      guiSceneSharedVars.cloud_cirrus_effect,
      guiSceneSharedVars.cloud_powder_effect);
}

auto Kataglyphis::VulkanRenderer::supportsHardwareRaytracing() const -> bool
{
    return device && device->supportsHardwareAcceleratedRRT();
}

void Kataglyphis::VulkanRenderer::finishModelLoad() { refreshAfterSceneChange(true); }

void Kataglyphis::VulkanRenderer::refreshAfterSceneChange(bool rebuildBottomLevel)
{
    // Skipping any step leaves the model invisible or traced through stale descriptors.
    rebuildObjectDescriptions();

    // Before updateAllDescriptorSets, which binds the new TLAS and resets the path-tracing history.
    if (device->supportsHardwareAcceleratedRRT()) {
        if (rebuildBottomLevel) {
            asManager.createASForScene(device, graphics_command_pool, scene);
        } else {
            asManager.createTLAS(device, graphics_command_pool, scene);
        }
    }

    updateAllDescriptorSets();
}

void Kataglyphis::VulkanRenderer::updateStateDueToUserInput(GUISceneSharedVars &guiSceneSharedVars)
{
    // Polled first, so the rest of the frame sees a model on the frame it arrives.
    if (scene->pollModelLoad(device, graphics_command_pool)) { finishModelLoad(); }

    Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &guiRendererSharedVars =
      gui->getGuiRendererSharedVars();

    handleShaderHotReloadRequest(guiRendererSharedVars);
    handleRasterizationModeChange(guiRendererSharedVars);

    handleShadowResolutionChange(guiSceneSharedVars);
    handleModelTransformChange(guiSceneSharedVars);
    handleModelReloadRequest(guiSceneSharedVars);
}

void Kataglyphis::VulkanRenderer::handleShaderHotReloadRequest(
    Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &guiRendererSharedVars)
{
    if (guiRendererSharedVars.shader_hot_reload_triggered) {
        shaderHotReload();
        guiRendererSharedVars.shader_hot_reload_triggered = false;
    }
}

void Kataglyphis::VulkanRenderer::handleRasterizationModeChange(
    Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &guiRendererSharedVars)
{
    // The post pass's input image depends on the mode; waitIdle is fine for a rare UI event.
    if (guiRendererSharedVars.rasterizationMode != lastBoundRasterizationMode) {
        (void)device->getLogicalDevice().waitIdle();
        updateAllDescriptorSets();
        lastBoundRasterizationMode = guiRendererSharedVars.rasterizationMode;
    }
}

void Kataglyphis::VulkanRenderer::reinitShadowMapForCurrentSettings()
{
    GUISceneSharedVars &guiSceneSharedVars = gui->getGuiSceneSharedVars();

    dirShadowMap.cleanUp();

    const uint32_t shadow_res = shadowResolutionForIndex(guiSceneSharedVars.shadow_map_res_index);

    // Clamp to the SceneUBO's MAX_CASCADES and to maxMultiviewViewCount, which bounds the shadow pass's viewMask.
    const auto device_view_limit = device->getMaxMultiviewViewCount();
    const auto cascade_count = clampCascadeCount(
      static_cast<uint32_t>(guiSceneSharedVars.num_shadow_cascades), static_cast<uint32_t>(MAX_CASCADES), device_view_limit);
    if (cascade_count == device_view_limit && device_view_limit < static_cast<uint32_t>(MAX_CASCADES)) {
        spdlog::warn(
          "Device maxMultiviewViewCount ({}) is the binding constraint on cascade count; clamping to {}.",
          device_view_limit, cascade_count);
    }
    dirShadowMap.init(device, shadow_res, shadow_res, cascade_count, sharedRenderDescriptors.getLayout(), vulkanSwapChain.getNumberSwapChainImages(), graphics_command_pool);
    dirShadowMap.createGraphicsPipeline();

    // The pipeline was seeded from stale cascadeData; re-seed every image once updateUniforms() has run.
    lightMatricesNeedFullReseed = true;
}

void Kataglyphis::VulkanRenderer::handleShadowResolutionChange(
    GUISceneSharedVars &guiSceneSharedVars)
{
    if (guiSceneSharedVars.shadow_resolution_changed) {
        guiSceneSharedVars.shadow_resolution_changed = false;

        (void)device->getLogicalDevice().waitIdle();

        reinitShadowMapForCurrentSettings();

        // We must recreate descriptor sets that depend on the shadow map
        updateTexturesInSharedRenderDescriptorSet();
    }
}

void Kataglyphis::VulkanRenderer::handleModelTransformChange(
    GUISceneSharedVars &guiSceneSharedVars)
{
    if (guiSceneSharedVars.model_transform_changed) {
        guiSceneSharedVars.model_transform_changed = false;

        const glm::mat4 modelMatrix = makeGuiModelTransform(
          std::span<const float, 3>(guiSceneSharedVars.model_position),
          std::span<const float, 3>(guiSceneSharedVars.model_rotation));

        // selected_model_index indexes the file list, not the scene; reloadModel leaves the model at scene index 0.
        if (guiSceneSharedVars.selected_model_index >= 0) {
            scene->update_model_matrix(modelMatrix, 0);

            // Only the instance transform moved, so a TLAS-only rebuild suffices.
            (void)device->getLogicalDevice().waitIdle();
            refreshAfterSceneChange(false);
        }
    }
}

void Kataglyphis::VulkanRenderer::handleModelReloadRequest(
    GUISceneSharedVars &guiSceneSharedVars)
{
    if (guiSceneSharedVars.model_reload_requested) {
        guiSceneSharedVars.model_reload_requested = false;

        const auto model_paths = sceneConfig::getAvailableModelPaths();
        const int sel = guiSceneSharedVars.selected_model_index;
        if (sel >= 0 && sel < static_cast<int>(model_paths.size())) {
            const std::string selected_path = model_paths[static_cast<size_t>(sel)];

            (void)device->getLogicalDevice().waitIdle();

            scene->reloadModel(device, graphics_command_pool, selected_path);

            // New geometry: full BLAS+TLAS rebuild, or RT/PT keep the destroyed TLAS bound.
            refreshAfterSceneChange(true);
        }
    }
}

void Kataglyphis::VulkanRenderer::finishAllRenderCommands() { std::ignore = device->getLogicalDevice().waitIdle(); }

// Must reach every SPIR-V-loading stage (BuildIntegrity.EverySpirvLoadingSubsystemImplementsShaderHotReload).
void Kataglyphis::VulkanRenderer::shaderHotReload()
{
    std::ignore = device->getLogicalDevice().waitIdle();

    std::array<vk::DescriptorSetLayout, 1> const descriptor_set_layouts = { sharedRenderDescriptors.getLayout() };
    rasterizer.shaderHotReload(descriptor_set_layouts);

    std::array<vk::DescriptorSetLayout, 2> const descriptor_set_layouts_deferred = { sharedRenderDescriptors.getLayout(),
        gbufferDescriptors.getLayout() };
    deferredRasterizer.shaderHotReload(descriptor_set_layouts_deferred);

    std::array<vk::DescriptorSetLayout, 1> const descriptor_set_layouts_post = { postDescriptors.getLayout() };
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

void Kataglyphis::VulkanRenderer::drawFrame(const GUISceneSharedVars &guiSceneSharedVars)
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
    if (result != vk::Result::eSuccess) {
        abort_frame_with_fatal_error("Failed to wait for fences!", result);
        return;
    }

    uint32_t image_index = 0;
    std::tie(result, image_index) = device->getLogicalDevice().acquireNextImageKHR(
      vulkanSwapChain.getSwapChain(), std::numeric_limits<uint64_t>::max(), frameSync.imageAvailableSemaphore(),
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
        abort_frame_after_acquire(
          fmt::format("Swapchain image index out of range: {}", image_index).c_str());
        return;
    }

    if (image_index >= frameSync.renderFinishedCount() || !frameSync.renderFinishedSemaphore(image_index)) {
        abort_frame_after_acquire(
          fmt::format("Render-finished semaphore missing for swapchain image {}.", image_index).c_str());
        return;
    }

    if (frameSync.imageInFlightFence(image_index)) {
        result = device->getLogicalDevice().waitForFences(
          1, &frameSync.imageInFlightFence(image_index), VK_TRUE, UINT64_MAX);
        if (result != vk::Result::eSuccess) {
            abort_frame_with_fatal_error("Failed to wait for image in-flight fence!", result);
            return;
        }
    }

    // The fence wait above means this image's queries are complete, so the readback never stalls.
    gpuTiming.readTimings(*device, image_index, gui->getGuiRendererSharedVars());

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

    Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars const &guiRendererSharedVars =
      gui->getGuiRendererSharedVars();
    const bool raytracing_available = device->supportsHardwareAcceleratedRRT();
    const char *const render_mode =
      (!raytracing_available || (!guiRendererSharedVars.raytracing && !guiRendererSharedVars.pathTracing))
        ? "rasterizer"
        : (guiRendererSharedVars.raytracing ? "raytracing" : "path_tracing");
    if (raytracing_available && guiRendererSharedVars.raytracing) { update_raytracing_descriptor_set(image_index); }

    if (!record_commands(image_index, guiSceneSharedVars)) {
        std::ignore = command_buffers[image_index].end();
        abort_frame_after_acquire("record_commands failed; dropping this frame.");
        return;
    }

    result = command_buffers[image_index].end();
    if (result != vk::Result::eSuccess) {
        abort_frame_with_fatal_error("Failed to stop recording a command buffer!", result);
        return;
    }

    vk::SubmitInfo submit_info{};
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &frameSync.imageAvailableSemaphore();

    vk::PipelineStageFlags const wait_stages = { vk::PipelineStageFlagBits::eColorAttachmentOutput };

    submit_info.pWaitDstStageMask = &wait_stages;

    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffers[image_index];
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &frameSync.renderFinishedSemaphore(image_index);

    result = device->getLogicalDevice().resetFences(1, &frameSync.inFlightFence());
    if (result != vk::Result::eSuccess) {
        abort_frame_with_fatal_error("Failed to reset fences!", result);
        return;
    }

    result = device->getGraphicsQueue().submit(1, &submit_info, frameSync.inFlightFence());
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

    uint32_t oldImageCount = vulkanSwapChain.getNumberSwapChainImages();

    // Before recreate(): these framebuffers reference the old swapchain image views.
    postStage.destroyFramebuffers();
    rasterizer.destroyFramebuffers();
    deferredRasterizer.destroyFramebuffers();
    skyBox.destroyFramebuffers();

    vulkanSwapChain.recreate(device, surface);

    // The query pool is sized per swapchain image.
    gpuTiming.create(*device, vulkanSwapChain.getNumberSwapChainImages(), gui->getGuiRendererSharedVars());

    uint32_t newImageCount = vulkanSwapChain.getNumberSwapChainImages();

    // Recreate depth buffers and framebuffers with new swapchain
    postStage.recreateFrameResources();
    rasterizer.recreateFrameResources(graphics_command_pool);
    deferredRasterizer.recreateFrameResources();
    clouds.recreateFrameResources(graphics_command_pool, vulkanSwapChain.getSwapChainExtent().width, vulkanSwapChain.getSwapChainExtent().height);

    // The accumulation history is extent-sized; recreating it also resets the frame counter.
    if (device->supportsHardwareAcceleratedRRT()) { createPathTracingAccumulationResources(); }

    skyBox.recreateFrameResources(swapchainImageViews(),
        vulkanSwapChain.getSwapChainExtent().width, vulkanSwapChain.getSwapChainExtent().height);

    // A new image count re-provisions every per-image resource, not just the descriptor pools.
    if (newImageCount != oldImageCount) {
        reprovisionPerImageResources();
        // updateAllDescriptorSets() does not rewrite the object-description binding.
        updateObjectDescriptionDescriptorSets();
    }

    updateAllDescriptorSets();

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
      scene->loadAdditionalModel(device, graphics_command_pool, modelPath, modelMatrix);
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
    if (device->supportsHardwareAcceleratedRRT()) {
        updateRaytracingDescriptorSets();
    }
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

void Kataglyphis::VulkanRenderer::update_raytracing_descriptor_set(uint32_t image_index)
{
    if (image_index >= raytracingDescriptors.sets().size()) {
        spdlog::error(fmt::format("Raytracing descriptor set index out of range: {}", image_index));
        return;
    }

    if (!asManager.getTLAS()) {
        return;
    }

    writeRaytracingDescriptorsForImage(image_index);
}

void Kataglyphis::VulkanRenderer::writeRaytracingDescriptorsForImage(uint32_t image_index)
{
    vk::AccelerationStructureKHR &vulkanTLAS = asManager.getTLAS();
    Texture &renderResult = activeOffscreenTexture(image_index);

    raytracingDescriptors.writeAccelerationStructure(image_index, TLAS_BINDING, vulkanTLAS);
    raytracingDescriptors.writeImage(image_index, OUT_IMAGE_BINDING, renderResult.getImageView(), vk::ImageLayout::eGeneral);
    raytracingDescriptors.writeImage(
      image_index, ACCUMULATION_IMAGE_BINDING, pathTracingAccumulation.getImageView(), vk::ImageLayout::eGeneral);
}

auto Kataglyphis::VulkanRenderer::activeOffscreenTexture(uint32_t index) -> Texture &
{
    Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars const &guiRendererSharedVars =
      gui->getGuiRendererSharedVars();
    return guiRendererSharedVars.rasterizationMode == Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode::Forward
             ? rasterizer.getOffscreenTexture(index)
             : deferredRasterizer.getOffscreenTexture(index);
}

std::vector<vk::ImageView> Kataglyphis::VulkanRenderer::swapchainImageViews()
{
    std::vector<vk::ImageView> views(vulkanSwapChain.getNumberSwapChainImages());
    for (uint32_t i = 0; i < vulkanSwapChain.getNumberSwapChainImages(); i++) {
        views[i] = vulkanSwapChain.getSwapChainImage(i).getImageView();
    }
    return views;
}

bool Kataglyphis::VulkanRenderer::record_commands(uint32_t image_index, const GUISceneSharedVars &guiSceneSharedVars)
{
    if (image_index >= command_buffers.size() || image_index >= sharedRenderDescriptors.sets().size()
        || image_index >= postDescriptors.sets().size()) {
        spdlog::error(fmt::format("Command recording index out of range: {}", image_index));
        return false;
    }

    Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars const &guiRendererSharedVars =
      gui->getGuiRendererSharedVars();

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

    if (guiSceneSharedVars.clouds_enabled) {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "clouds", { 0.80F, 0.85F, 0.95F, 1.0F });
        write_pass_timestamp(GpuTimedPass::Clouds, true);

        // Cross-frame WAR on the single cloudOutputTexture; the frame fence alone covers only 3 frames back.
        recordCloudOutputBarrier(commandBuffer,
          clouds.getCloudOutputTexture()->getImage(),
          vk::PipelineStageFlagBits::eFragmentShader,
          vk::PipelineStageFlagBits::eComputeShader,
          {},
          vk::AccessFlagBits::eShaderWrite);

        clouds.recordComputeCommands(commandBuffer, rasterizer_descriptor_sets);

        // PostStage's subpass dependency cannot order this compute write before the post pass samples it.
        recordCloudOutputBarrier(commandBuffer,
          clouds.getCloudOutputTexture()->getImage(),
          vk::PipelineStageFlagBits::eComputeShader,
          vk::PipelineStageFlagBits::eFragmentShader,
          vk::AccessFlagBits::eShaderWrite,
          vk::AccessFlagBits::eShaderRead);

        write_pass_timestamp(GpuTimedPass::Clouds, false);
    }

    {
        Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &mutable_gui_vars =
          gui->getGuiRendererSharedVars();
        if (guiSceneSharedVars.shadows_enabled) {
            Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "shadow_cascades", { 0.55F, 0.35F, 0.10F, 1.0F });
            write_pass_timestamp(GpuTimedPass::ShadowCascades, true);
            dirShadowMap.recordCommands(
              commandBuffer, image_index, scene, rasterizer_descriptor_sets, guiRendererSharedVars.frustum_culling_enabled);
            write_pass_timestamp(GpuTimedPass::ShadowCascades, false);
            mutable_gui_vars.visibility.shadow_casters_drawn = dirShadowMap.getCastersDrawn();
            mutable_gui_vars.visibility.shadow_casters_total = dirShadowMap.getCastersConsidered();
        } else {
            dirShadowMap.recordSkippedPass(commandBuffer);
            mutable_gui_vars.visibility.shadow_casters_drawn = 0;
            mutable_gui_vars.visibility.shadow_casters_total = 0;
        }
    }

    write_pass_timestamp(GpuTimedPass::Main, true);

    // From globalUBO, like the shaders; the shadow pass above must not use it, as off-screen geometry casts into view.
    const std::optional<FrustumPlanes> camera_frustum =
      guiRendererSharedVars.frustum_culling_enabled
        ? std::optional<FrustumPlanes>(extractFrustumPlanes(globalUBO.projection * globalUBO.view))
        : std::nullopt;

    if (!raytracingOwnsFrame(image_index)) {
        recordRasterPass(commandBuffer, image_index, rasterizer_descriptor_sets, camera_frustum);
    } else {
        Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &mutable_gui_vars =
          gui->getGuiRendererSharedVars();
        mutable_gui_vars.visibility.meshes_drawn = 0;
        mutable_gui_vars.visibility.meshes_total = 0;
    }

    recordRaytracingOrPathTracing(commandBuffer, image_index);

    write_pass_timestamp(GpuTimedPass::Main, false);

    write_pass_timestamp(GpuTimedPass::Sky, true);
    {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "sky", { 0.30F, 0.70F, 0.90F, 1.0F });
        skyBox.recordCommands(
          commandBuffer, image_index, rasterizer_descriptor_sets, guiSceneSharedVars.skybox_enabled);
    }
    write_pass_timestamp(GpuTimedPass::Sky, false);

    // No swapchain barrier here: the post render pass's external dependency orders it.
    write_pass_timestamp(GpuTimedPass::Post, true);
    {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "post", { 0.35F, 0.80F, 0.40F, 1.0F });
        const std::array<vk::DescriptorSet, 1> post_descriptor_sets = { postDescriptors.sets()[image_index] };
        postStage.recordCommands(commandBuffer, image_index, post_descriptor_sets, guiSceneSharedVars.clouds_enabled);
    }
    write_pass_timestamp(GpuTimedPass::Post, false);

    // Capture copies from the ePresentSrcKHR image and restores that layout before the present.
    if (frameCapture.isArmed()) {
        frameCapture.record(device, commandBuffer, vulkanSwapChain, image_index, device_lost_detected);
    }

    if (record_gpu_timings) { gpuTiming.setPassRecordedMask(image_index, recorded_pass_mask); }

    return true;
}

void Kataglyphis::VulkanRenderer::recordRasterPass(vk::CommandBuffer &commandBuffer,
  uint32_t image_index,
  std::span<const vk::DescriptorSet> rasterizer_descriptor_sets,
  const std::optional<FrustumPlanes> &camera_frustum)
{
    Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars const &guiRendererSharedVars =
      gui->getGuiRendererSharedVars();

    if (guiRendererSharedVars.rasterizationMode == Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode::Forward) {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "forward", { 0.20F, 0.60F, 1.00F, 1.0F });
        rasterizer.recordCommands(commandBuffer, image_index, scene, rasterizer_descriptor_sets, camera_frustum);
    } else {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "deferred", { 0.20F, 0.40F, 0.80F, 1.0F });
        const std::array<vk::DescriptorSet, 2> deferred_sets = { sharedRenderDescriptors.sets()[image_index], gbufferDescriptors.sets()[image_index] };
        deferredRasterizer.recordCommands(commandBuffer, image_index, scene, deferred_sets, camera_frustum);
    }

    // Publish only the path that recorded: the inactive one still holds pre-switch numbers.
    const bool forward_active =
      guiRendererSharedVars.rasterizationMode == Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode::Forward;
    Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars &mutable_gui_vars =
      gui->getGuiRendererSharedVars();
    mutable_gui_vars.visibility.meshes_drawn =
      forward_active ? rasterizer.getMeshesDrawn() : deferredRasterizer.getMeshesDrawn();
    mutable_gui_vars.visibility.meshes_total =
      forward_active ? rasterizer.getMeshesConsidered() : deferredRasterizer.getMeshesConsidered();
}

bool Kataglyphis::VulkanRenderer::raytracingOwnsFrame(uint32_t image_index)
{
    Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars const &guiRendererSharedVars =
      gui->getGuiRendererSharedVars();

    // No TLAS yet during the async model load, so the raster pass must still run.
    return device->supportsHardwareAcceleratedRRT() && image_index < raytracingDescriptors.sets().size()
           && asManager.getTLAS() && (guiRendererSharedVars.raytracing || guiRendererSharedVars.pathTracing);
}

void Kataglyphis::VulkanRenderer::recordRaytracingOrPathTracing(vk::CommandBuffer &commandBuffer, uint32_t image_index)
{
    Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars const &guiRendererSharedVars =
      gui->getGuiRendererSharedVars();

    // Before the model loads the ray-tracing descriptor sets are unwritten.
    if (!raytracingOwnsFrame(image_index)) { return; }

    const std::array<vk::DescriptorSet, 2> raytracing_descriptor_sets = { sharedRenderDescriptors.sets()[image_index],
        raytracingDescriptors.sets()[image_index] };

    if (guiRendererSharedVars.raytracing) {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "raytracing", { 0.85F, 0.25F, 0.55F, 1.0F });
        Texture &renderResult = activeOffscreenTexture(image_index);
        raytracingStage.recordCommands(
          commandBuffer, renderResult.getVulkanImage(), raytracing_descriptor_sets);
    } else if (guiRendererSharedVars.pathTracing) {
        Kataglyphis::debug::ScopedCmdLabel const label(commandBuffer, "pathtracing", { 0.60F, 0.25F, 0.85F, 1.0F });
        Texture &renderResult = activeOffscreenTexture(image_index);

        // A camera, light or quality change restarts the running mean.
        const Kataglyphis::VulkanRendererInternals::PathTracingHistoryKey current_history{
            .view = camera->calculate_viewmatrix(),
            .projection = globalUBO.projection,
            .lightDirection = sceneUBO.dirLight.direction,
            .lightColorAndRadiance = sceneUBO.dirLight.color,
            .samplesPerPixel = guiRendererSharedVars.pathTracingSamplesPerPixel,
            .maxBounces = guiRendererSharedVars.pathTracingMaxBounces,
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
          static_cast<uint32_t>(std::max(guiRendererSharedVars.pathTracingSamplesPerPixel, 1)),
          static_cast<uint32_t>(std::max(guiRendererSharedVars.pathTracingMaxBounces, 1)));
        ++pathTracingAccumulatedFrames;
    }
}

auto Kataglyphis::VulkanRenderer::supportsFrameCapture() const -> bool
{
    return device != nullptr && frameCapture.supportsCapture(vulkanSwapChain, device_lost_detected);
}

void Kataglyphis::VulkanRenderer::requestFrameCapture()
{
    if (!supportsFrameCapture()) {
        spdlog::warn("Frame capture requested but the surface does not support eTransferSrc; ignoring.");
        return;
    }

    frameCapture.request();
}

auto Kataglyphis::VulkanRenderer::takeCapturedFrame(uint32_t &outWidth, uint32_t &outHeight) -> std::vector<uint8_t>
{
    return frameCapture.take(device, device_lost_detected, outWidth, outHeight);
}

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

void Kataglyphis::VulkanRenderer::cleanUp()
{
    if (!device) { return; }

    std::ignore = device->getLogicalDevice().waitIdle();

    // Behind the !device guard so it runs once, though cleanUp is reached twice (explicitly, then the destructor).
    gpuTiming.writeJsonIfRequested();

    if (device->supportsHardwareAcceleratedRRT()) {
        pathTracingAccumulation.cleanUp();
        pathTracing.cleanUp();
        raytracingStage.cleanUp();
        asManager.cleanUp();
    }

    rasterizer.cleanUp();
    deferredRasterizer.cleanUp();
    skyBox.cleanUp();
    clouds.cleanUp();
    dirShadowMap.cleanUp();
    postStage.cleanUp();

    objectDescriptionBuffer.cleanUp();
    frameCapture.cleanUp();
    // Before device->cleanUp(), which tears down the VMA allocator.
    vulkanBufferManager.cleanUp();

    gpuTiming.destroy(*device);
    cleanUpSync();
    cleanUpUBOs();
    cleanUpCommandPools();
    cleanUpDescriptorResources();
    raytracingDescriptors.cleanUp();

    vulkanSwapChain.cleanUp();
    // Last: the VMA allocator must outlive every buffer and image above.
    device->cleanUp();
    device.reset();

    if (surface) {
        instance.getVulkanInstance().destroySurfaceKHR(surface);
        surface = nullptr;
    }

    if (Kataglyphis::validationLayersEnabled()) { debug::freeDebugCallback(instance.getVulkanInstance()); }
    instance.cleanUp();
}

Kataglyphis::VulkanRenderer::~VulkanRenderer() { cleanUp(); }

void Kataglyphis::VulkanRenderer::create_surface()
{
    VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
    ASSERT_VULKAN(glfwCreateWindowSurface(instance.getVulkanInstance(), window->get_window(), nullptr, &rawSurface),
      "Failed to create a surface!");
    surface = vk::SurfaceKHR(rawSurface);
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
    const vk::ShaderStageFlags raytracing_stages = vk::ShaderStageFlagBits::eRaygenKHR
                                                   | vk::ShaderStageFlagBits::eClosestHitKHR
                                                   | vk::ShaderStageFlagBits::eCompute;

    raytracingDescriptors.addBinding(TLAS_BINDING, vk::DescriptorType::eAccelerationStructureKHR, 1, raytracing_stages)
      .addBinding(OUT_IMAGE_BINDING, vk::DescriptorType::eStorageImage, 1, raytracing_stages)
      .addBinding(ACCUMULATION_IMAGE_BINDING, vk::DescriptorType::eStorageImage, 1, raytracing_stages);

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
      device->getLogicalDevice(), graphics_command_pool);
    if (!commandBuffer) {
        spdlog::error("Failed to begin command buffer for path tracing accumulation image transition!");
        pathTracingAccumulation.cleanUp();
        return;
    }
    pathTracingAccumulation.getVulkanImage().transitionImageLayout(
      commandBuffer, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, 1, vk::ImageAspectFlagBits::eColor);
    bool const transition_submitted = Kataglyphis::VulkanRendererInternals::CommandBufferManager::endAndSubmitCommandBuffer(
      device->getLogicalDevice(), graphics_command_pool, device->getGraphicsQueue(), commandBuffer);
    if (!transition_submitted) {
        spdlog::error("Failed to submit path tracing accumulation image transition!");
        pathTracingAccumulation.cleanUp();
        return;
    }

    pathTracingAccumulatedFrames = 0;
}

void Kataglyphis::VulkanRenderer::cleanUpSync() { frameSync.cleanUp(device->getLogicalDevice()); }

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
              graphics_command_pool,
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
              graphics_command_pool,
              objectDescriptionBuffer,
              vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
              vk::MemoryPropertyFlagBits::eDeviceLocal,
              std::vector<uint32_t>{0})) {
            spdlog::error(
              "VulkanRenderer::create_object_description_buffer: empty placeholder buffer upload failed.");
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
    if (!vulkanTLAS) {
        return;
    }

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
        sampler_stages |=
          vk::ShaderStageFlagBits::eClosestHitKHR | vk::ShaderStageFlagBits::eAnyHitKHR | vk::ShaderStageFlagBits::eCompute;
        textures_stages |=
          vk::ShaderStageFlagBits::eClosestHitKHR | vk::ShaderStageFlagBits::eAnyHitKHR | vk::ShaderStageFlagBits::eCompute;
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

        vk::Result const result =
          device->getLogicalDevice().createCommandPool(&pool_info, nullptr, &graphics_command_pool);
        if (result != vk::Result::eSuccess) {
            spdlog::error("Failed to create graphics command pool! Error: {}", static_cast<int>(result));
            std::abort();
        }
    }
}

void Kataglyphis::VulkanRenderer::cleanUpCommandPools()
{
    if (graphics_command_pool) {
        device->getLogicalDevice().destroyCommandPool(graphics_command_pool);
        graphics_command_pool = nullptr;
    }
}

void Kataglyphis::VulkanRenderer::create_command_buffers()
{
    if (!command_buffers.empty()) {
        device->getLogicalDevice().freeCommandBuffers(graphics_command_pool, command_buffers);
    }
    command_buffers.resize(vulkanSwapChain.getNumberSwapChainImages());

    vk::CommandBufferAllocateInfo command_buffer_alloc_info{};
    command_buffer_alloc_info.commandPool = graphics_command_pool;
    command_buffer_alloc_info.level = vk::CommandBufferLevel::ePrimary;

    command_buffer_alloc_info.commandBufferCount = static_cast<uint32_t>(command_buffers.size());

    vk::Result const result =
      device->getLogicalDevice().allocateCommandBuffers(&command_buffer_alloc_info, command_buffers.data());
    ASSERT_VULKAN(result, "Failed to allocate command buffers!");
}

void Kataglyphis::VulkanRenderer::createSynchronization()
{
    frameSync.create(device->getLogicalDevice(), vulkanSwapChain.getNumberSwapChainImages());
}

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
    if (sharedRenderDescriptors.sets().empty()) {
        return;
    }

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

    if (scene->getModelCount() == 0) {
        return;
    }

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
    // Must match deferred.slang's GBUFFER_*_BINDING input attachments.
    static constexpr std::array<uint32_t, 4> kGBufferBindings = {
        GBUFFER_NORMAL_BINDING, GBUFFER_ALBEDO_BINDING, GBUFFER_MATERIAL_BINDING, GBUFFER_DEPTH_BINDING
    };
    for (const uint32_t binding : kGBufferBindings) {
        gbufferDescriptors.addBinding(binding, vk::DescriptorType::eInputAttachment, 1, vk::ShaderStageFlagBits::eFragment);
    }

    if (!gbufferDescriptors.create(device, vulkanSwapChain.getNumberSwapChainImages())) {
        spdlog::error("Failed to create gbuffer descriptor resources!");
    }
}

void Kataglyphis::VulkanRenderer::updateGBufferDescriptorSets()
{
    for (uint32_t i = 0; i < vulkanSwapChain.getNumberSwapChainImages(); i++) {
        gbufferDescriptors.writeImage(i, GBUFFER_NORMAL_BINDING, deferredRasterizer.getGBufferNormal(i),
                                       vk::ImageLayout::eShaderReadOnlyOptimal);
        gbufferDescriptors.writeImage(i, GBUFFER_ALBEDO_BINDING, deferredRasterizer.getGBufferAlbedo(i),
                                       vk::ImageLayout::eShaderReadOnlyOptimal);
        gbufferDescriptors.writeImage(i, GBUFFER_MATERIAL_BINDING, deferredRasterizer.getGBufferMaterial(i),
                                       vk::ImageLayout::eShaderReadOnlyOptimal);
        gbufferDescriptors.writeImage(i, GBUFFER_DEPTH_BINDING, deferredRasterizer.getDepthBufferImageView(),
                                       vk::ImageLayout::eShaderReadOnlyOptimal);
    }
}
