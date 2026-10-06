module;
#include <memory>

#include "common/SceneUboMarshal.hpp"
#include "common/Utilities.hpp"
#include "common/host_device_shared_vars.hpp"
#include "scene/atmospheric_effects/clouds/CloudDispatch.hpp"

#include <array>
#include <cstdint>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.gui;

import kataglyphis.shared.frontend.common_gui_panels;
import kataglyphis.shared.imgui.fonts;
import kataglyphis.shared.imgui.style;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.queue_family_indices;
import kataglyphis.vulkan.window;
import kataglyphis.vulkan.scene_config;

using namespace Kataglyphis::Frontend;

namespace {
void applyKataglyphisDarkTheme() { Kataglyphis::Frontend::applyKataglyphisImGuiDarkTheme(); }
}// namespace

GUI::GUI(Window *window) : window(window) {}

void GUI::initializeVulkanContext(const std::shared_ptr<VulkanDevice> &vulkan_device,
  const vk::Instance &instance,
  const vk::RenderPass &post_render_pass,
  uint32_t image_count)
{
    this->device = vulkan_device;

    create_gui_context(window, instance, post_render_pass, image_count);
}

void GUI::setUserSelectionForRRT(bool rrtCapabilitiesAvailable)
{
    renderUserSelectionForRRT = rrtCapabilitiesAvailable;
}

void GUI::render()
{
    // Start the Dear ImGui frame
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("GUI v" PROJECT_VERSION);

    if (ImGui::CollapsingHeader("Model Selection")) {
        const auto model_paths = sceneConfig::getAvailableModelPaths();
        const auto model_names = sceneConfig::getAvailableModelDisplayNames();
        const int model_count = static_cast<int>(model_paths.size());
        
        // Must not stay -1: handleModelTransformChange gates the Position/Rotation controls on it.
        if (guiSceneSharedVars.selected_model_index == -1 && model_count > 0) {
            guiSceneSharedVars.selected_model_index =
              sceneConfig::defaultSelectedModelIndex(model_paths, sceneConfig::defaultModelRelativePath());
        }

        const int num_model_names = static_cast<int>(model_names.size());

        if (model_count > 0 && model_count == num_model_names) {
            int prev_index = guiSceneSharedVars.selected_model_index;
            const char *current_display = (prev_index >= 0 && prev_index < model_count)
              ? model_names[static_cast<size_t>(prev_index)].c_str()
              : "Select a model...";

            if (ImGui::BeginCombo("Model", current_display)) {
                for (int i = 0; i < model_count; i++) {
                    const bool is_selected = (guiSceneSharedVars.selected_model_index == i);
                    if (ImGui::Selectable(model_names[static_cast<size_t>(i)].c_str(), is_selected)) {
                        guiSceneSharedVars.selected_model_index = i;
                        guiSceneSharedVars.model_reload_requested = true;
                    }
                    if (is_selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::DragFloat3("Position", guiSceneSharedVars.model_position, 0.1f)) {
                guiSceneSharedVars.model_transform_changed = true;
            }
            if (ImGui::DragFloat3("Rotation", guiSceneSharedVars.model_rotation, 0.1f)) {
                guiSceneSharedVars.model_transform_changed = true;
            }
        } else {
            ImGui::TextDisabled("No loadable models (.obj/.gltf/.glb) found in Resources/Models/");
        }
    }

    if (ImGui::CollapsingHeader("Hot shader reload")) {
#ifndef NDEBUG
        if (ImGui::Button("All shader!")) { guiRendererSharedVars.shader_hot_reload_triggered = true; }
#else
        ImGui::TextDisabled("All shader! (disabled in Release build)");
#endif
    }

    ImGui::Separator();

    // Re-derived from the shared vars every frame, never cached, so writes from code are not stomped.
    int e = guiRendererSharedVars.pathTracing ? 2 : (guiRendererSharedVars.raytracing ? 1 : 0);
    ImGui::RadioButton("Rasterizer", &e, 0);
    ImGui::SameLine();
    if (renderUserSelectionForRRT) {
        ImGui::RadioButton("Raytracing", &e, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Path tracing", &e, 2);
    }

    if (e == 0) {
        ImGui::Separator();
        int raster_mode = guiRendererSharedVars.rasterizationMode
            == VulkanRendererInternals::FrontendShared::RasterizationMode::Forward ? 0 : 1;
        ImGui::RadioButton("Forward", &raster_mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Deferred", &raster_mode, 1);
        guiRendererSharedVars.rasterizationMode = raster_mode == 0 ? VulkanRendererInternals::FrontendShared::RasterizationMode::Forward : VulkanRendererInternals::FrontendShared::RasterizationMode::Deferred;
    }

    switch (e) {
    case 0:
        guiRendererSharedVars.raytracing = false;
        guiRendererSharedVars.pathTracing = false;
        break;
    case 1:
        guiRendererSharedVars.raytracing = true;
        guiRendererSharedVars.pathTracing = false;
        break;
    case 2:
        guiRendererSharedVars.raytracing = false;
        guiRendererSharedVars.pathTracing = true;
        break;
    }

    if (e == 2) {
        ImGui::Separator();
        ImGui::SliderInt("Samples / pixel", &guiRendererSharedVars.pathTracingSamplesPerPixel, 1, 64);
        ImGui::SliderInt("Max bounces", &guiRendererSharedVars.pathTracingMaxBounces, 1, 16);
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Graphic Settings")) {
        ImGui::Checkbox("Enable Skybox", &guiSceneSharedVars.skybox_enabled);

        if (ImGui::TreeNode("Camera")) {
            ImGui::SliderFloat("Field of view", &guiSceneSharedVars.camera_fov, 20.0F, 110.0F, "%.0f deg");
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Directional Light")) {
            ImGui::Separator();
            ImGui::SliderFloat("Radiance", &guiSceneSharedVars.directional_light_radiance, 0.0F, 50.0F);
            ImGui::Separator();
            // Edit a color (stored as ~4 floats)
            ImGui::ColorEdit3("Directional Light Color", guiSceneSharedVars.directional_light_color);
            ImGui::Separator();
            ImGui::SliderFloat3("Light Direction", guiSceneSharedVars.directional_light_direction, -1.F, 1.0F);

            if (ImGui::TreeNode("Shadows")) {
                ImGui::Checkbox("Enable Shadows", &guiSceneSharedVars.shadows_enabled);
                if (guiSceneSharedVars.shadows_enabled) {
                    int const shadow_map_res_index_before = guiSceneSharedVars.shadow_map_res_index;
                    ImGui::Combo("Shadow Map Resolution",
                      &guiSceneSharedVars.shadow_map_res_index,
                      guiSceneSharedVars.available_shadow_map_resolutions,
                      kShadowMapResolutionCount);
                    if (shadow_map_res_index_before != guiSceneSharedVars.shadow_map_res_index) { guiSceneSharedVars.shadow_resolution_changed = true; }

                    int const num_cascades_before = guiSceneSharedVars.num_shadow_cascades;
                    ImGui::SliderInt("# cascades", &guiSceneSharedVars.num_shadow_cascades, 1, MAX_CASCADES);
                    if (num_cascades_before != guiSceneSharedVars.num_shadow_cascades) { guiSceneSharedVars.shadow_resolution_changed = true; }

                    // AlwaysClamp: a Ctrl+click entry past the bound would otherwise stay in the GUI state.
                    ImGui::SliderInt("PCF radius",
                      &guiSceneSharedVars.pcf_radius,
                      0,
                      MAX_PCF_RADIUS,
                      "%d",
                      ImGuiSliderFlags_AlwaysClamp);
                    ImGui::SliderFloat("Shadow intensity", &guiSceneSharedVars.cascaded_shadow_intensity, 0.0F, 1.0F);
                    ImGui::SliderFloat(
                      "Shadow distance", &guiSceneSharedVars.shadow_distance, kMinShadowDistance, kMaxShadowDistance);
                    ImGui::SliderFloat("Cascade split lambda", &guiSceneSharedVars.cascade_split_lambda, 0.0F, 1.0F);
                }

                ImGui::TreePop();
            }

            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Cloud Settings")) {
            ImGui::Checkbox("Enable Clouds", &guiSceneSharedVars.clouds_enabled);
            if (guiSceneSharedVars.clouds_enabled) {
                ImGui::SliderInt("# march steps", &guiSceneSharedVars.cloud_num_march_steps,
                  Kataglyphis::kMinCloudMarchSteps, Kataglyphis::kMaxCloudMarchSteps);
                ImGui::SliderInt("# march steps to light", &guiSceneSharedVars.cloud_num_march_steps_to_light,
                  Kataglyphis::kMinCloudLightMarchSteps, Kataglyphis::kMaxCloudLightMarchSteps);
                // cloud.scale (clouds.slang) - the density multiplier applied to the noise sample.
                ImGui::SliderFloat("Density", &guiSceneSharedVars.cloud_density_multiplier, Kataglyphis::kMinCloudDensityMultiplier, 1.0F);
                // cloud.threshold (clouds.slang) - the noise cut-off below which a sample counts as clear sky.
                ImGui::SliderFloat("Coverage threshold", &guiSceneSharedVars.cloud_coverage_threshold, 0.F, 1.0F);
                ImGui::SliderFloat("Pillowness", &guiSceneSharedVars.cloud_pillowness, 0.F, 1.0F);
                ImGui::SliderFloat("Cirrus effect", &guiSceneSharedVars.cloud_cirrus_effect, 0.F, 1.0F);
                ImGui::Checkbox("Powder effect", &guiSceneSharedVars.cloud_powder_effect);
                ImGui::SliderFloat3("Scale", guiSceneSharedVars.cloud_mesh_scale, Kataglyphis::kMinCloudMeshExtent, 1000.0F);
                ImGui::SliderFloat3("Translation", guiSceneSharedVars.cloud_mesh_offset, -200.F, 400.0F);
            }

            ImGui::TreePop();
        }
    }

    ImGui::Separator();

    Kataglyphis::Frontend::renderCommonGuiStyleSettings();

    ImGui::Separator();

    Kataglyphis::Frontend::renderCommonKeyBindings();

    ImGui::Separator();

    Kataglyphis::Frontend::renderCommonFrameStats();

    ImGui::Separator();

    // Wrong culling has no visual signature beyond missing geometry; these counts make it visible.
    if (ImGui::CollapsingHeader("Visibility")) {
        ImGui::Checkbox("Frustum culling", &guiRendererSharedVars.frustum_culling_enabled);
        const unsigned int drawn = guiRendererSharedVars.visibility.meshes_drawn;
        const unsigned int total = guiRendererSharedVars.visibility.meshes_total;
        ImGui::Text("Meshes drawn: %u / %u", drawn, total);
        if (total > 0U) {
            ImGui::Text("Culled: %u (%.1f%%)", total - drawn, 100.0 * double(total - drawn) / double(total));
        }
        const unsigned int shadow_drawn = guiRendererSharedVars.visibility.shadow_casters_drawn;
        const unsigned int shadow_total = guiRendererSharedVars.visibility.shadow_casters_total;
        ImGui::Text("Shadow casters drawn: %u / %u", shadow_drawn, shadow_total);
        if (shadow_total > 0U) {
            ImGui::Text("Shadow culled: %u (%.1f%%)",
              shadow_total - shadow_drawn,
              100.0 * double(shadow_total - shadow_drawn) / double(shadow_total));
        }
    }

    ImGui::Separator();

    Kataglyphis::Frontend::renderGpuTimingsPanel(guiRendererSharedVars.gpuTimings.supported,
      guiRendererSharedVars.gpuTimings.pass_ms,
      VulkanRendererInternals::FrontendShared::GPU_TIMED_PASS_NAMES);

    ImGui::End();
}

void GUI::cleanUp()
{
    if (device) {
        ImGui_ImplVulkan_Shutdown();
        device->getLogicalDevice().destroyDescriptorPool(gui_descriptor_pool);
        gui_descriptor_pool = nullptr;
        device.reset();
    }
    // Runs even without a device, or a device-lost shutdown leaks the ImGui context; the guard keeps it idempotent.
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
}

void GUI::create_gui_context(Window *frontend_window,
  const vk::Instance &instance,
  const vk::RenderPass &post_render_pass,
  uint32_t image_count)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;

    Kataglyphis::Frontend::configureKataglyphisImGuiFonts(io);

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;// Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableSetMousePos;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    applyKataglyphisDarkTheme();

    ImGui_ImplGlfw_InitForVulkan(frontend_window->get_window(), false);

    // Create Descriptor Pool
    std::array<vk::DescriptorPoolSize, 11> gui_pool_sizes = { { { vk::DescriptorType::eSampler, 10 },
      { vk::DescriptorType::eCombinedImageSampler, 10 },
      { vk::DescriptorType::eSampledImage, 10 },
      { vk::DescriptorType::eStorageImage, 10 },
      { vk::DescriptorType::eUniformTexelBuffer, 10 },
      { vk::DescriptorType::eStorageTexelBuffer, 10 },
      { vk::DescriptorType::eUniformBuffer, 10 },
      { vk::DescriptorType::eStorageBuffer, 10 },
      { vk::DescriptorType::eUniformBufferDynamic, 10 },
      { vk::DescriptorType::eStorageBufferDynamic, 10 },
      { vk::DescriptorType::eInputAttachment, 100 } } };

    vk::DescriptorPoolCreateInfo gui_pool_info{};
    gui_pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    gui_pool_info.maxSets = 10 * static_cast<uint32_t>(gui_pool_sizes.size());
    gui_pool_info.poolSizeCount = static_cast<uint32_t>(gui_pool_sizes.size());
    gui_pool_info.pPoolSizes = gui_pool_sizes.data();

    auto pool_result = device->getLogicalDevice().createDescriptorPool(gui_pool_info);
    ASSERT_VULKAN(pool_result.result, "Failed to create a gui descriptor pool!");
    gui_descriptor_pool = pool_result.value;

    Kataglyphis::VulkanRendererInternals::QueueFamilyIndices const indices = device->getQueueFamilies();

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = instance;
    init_info.PhysicalDevice = device->getPhysicalDevice();
    init_info.Device = device->getLogicalDevice();
    init_info.QueueFamily = static_cast<uint32_t>(indices.graphics_family);
    init_info.Queue = device->getGraphicsQueue();
    init_info.PipelineInfoMain.RenderPass = post_render_pass;
    init_info.DescriptorPool = gui_descriptor_pool;
    init_info.PipelineCache = device->getPipelineCache();
    init_info.MinImageCount = 2;
    init_info.ImageCount = image_count;
    init_info.Allocator = VK_NULL_HANDLE;
    init_info.CheckVkResultFn = VK_NULL_HANDLE;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    ImGui_ImplVulkan_Init(&init_info);// post_render_pass
}

GUI::~GUI() { cleanUp(); }
