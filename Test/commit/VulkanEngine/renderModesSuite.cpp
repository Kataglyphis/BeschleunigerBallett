#include "EngineFrame.hpp"
#include "EngineLoadWait.hpp"
#include "GpuTestGuard.hpp"
#include "ScopedEnvVar.hpp"
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.hpp>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.fxaa_stage;
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.render_settings;
import kataglyphis.vulkan.renderer;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.scene_config;
import kataglyphis.vulkan.window;

namespace {

using Kataglyphis::TestSupport::ScopedEnvVar;
using Kataglyphis::VulkanRendererInternals::FxaaStage;
using Kataglyphis::VulkanRendererInternals::FrontendShared::drawnRenderMode;
using Kataglyphis::VulkanRendererInternals::FrontendShared::FXAA_MODE_LABELS;
using Kataglyphis::VulkanRendererInternals::FrontendShared::FxaaMode;
using Kataglyphis::VulkanRendererInternals::FrontendShared::fxaaFilters;
using Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars;
using Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode;
using Kataglyphis::VulkanRendererInternals::FrontendShared::renderModeLogName;

// A missing model never loads, so the scene keeps no TLAS for as long as a test needs one to be missing.
constexpr const char *MISSING_MODEL = "Models/ShadowTest/no_such_model.obj";

// One engine with the editor showing, driven like App::run minus camera input and torn down in the usual order.
struct EditorEngine
{
    Kataglyphis::RenderSettings settings;
    Kataglyphis::RendererFeedback feedback;
    std::unique_ptr<Kataglyphis::Frontend::Window> window;
    std::unique_ptr<Kataglyphis::Scene> scene;
    std::unique_ptr<Kataglyphis::Frontend::GUI> gui;
    std::unique_ptr<Camera> camera;
    std::unique_ptr<Kataglyphis::VulkanRenderer> renderer;

    EditorEngine()
      : window(std::make_unique<Kataglyphis::Frontend::Window>(1200, 768)),
        scene(std::make_unique<Kataglyphis::Scene>()),
        gui(std::make_unique<Kataglyphis::Frontend::GUI>(window.get(), settings, feedback)),
        camera(std::make_unique<Camera>())
    {
        renderer =
          std::make_unique<Kataglyphis::VulkanRenderer>(window.get(), scene.get(), camera.get(), settings, feedback);
        Kataglyphis::TestSupport::attachEditor(*gui, *renderer);
    }
    EditorEngine(const EditorEngine &) = delete;
    EditorEngine &operator=(const EditorEngine &) = delete;

    void frame()
    {
        glfwPollEvents();
        Kataglyphis::TestSupport::renderEditorFrame(*gui, *renderer, scene.get(), camera.get(), settings, feedback);
    }

    void waitForModel()
    {
        Kataglyphis::TestSupport::waitForModelLoad(renderer.get(), [this] { frame(); });
    }

    // Members go in reverse declaration order, the renderer first, as App::run's locals do.
    ~EditorEngine() = default;
};

// One iteration of the App::run frame loop (App.cpp) minus camera input.
void run_frames(EditorEngine &engine, int frame_count, const std::string &mode_label)
{
    // The model parses on a worker thread, so the first frames show an empty scene.
    engine.waitForModel();

    for (int frame = 0; frame < frame_count; ++frame) {
        engine.frame();

        ASSERT_FALSE(engine.renderer->hasDeviceLost())
          << "Device lost in mode '" << mode_label << "' at frame " << frame;
    }
}

}// namespace

TEST(Integration, RenderModesSelectableInGui)
{
    SKIP_WITHOUT_GPU();

    constexpr int frames_per_mode = 3;

    EditorEngine engine;
    auto &renderer_vars = engine.settings.renderer;

    // Rasterizer, forward shading (the startup default).
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = false;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    run_frames(engine, frames_per_mode, "forward");

    // The menu and status bars always stay, so the visible editor must hand the renderer an inner rect.
    const auto &scene_viewport = renderer_vars.scene_viewport;
    EXPECT_GT(scene_viewport.y0, 0.0F) << "the menu bar must push the scene's top edge down";
    EXPECT_LT(scene_viewport.y1, 1.0F) << "the status bar must lift the scene's bottom edge";
    EXPECT_GE(scene_viewport.x0, 0.0F);
    EXPECT_LE(scene_viewport.x1, 1.0F);
    EXPECT_LT(scene_viewport.x0, scene_viewport.x1);
    EXPECT_LT(scene_viewport.y0, scene_viewport.y1);

    // Hidden, the editor must give the scene the whole image again, as the goldens rely on.
    engine.gui->setOverlayVisible(false);
    run_frames(engine, 1, "editor-hidden");
    EXPECT_FLOAT_EQ(scene_viewport.x0, 0.0F);
    EXPECT_FLOAT_EQ(scene_viewport.y0, 0.0F);
    EXPECT_FLOAT_EQ(scene_viewport.x1, 1.0F);
    EXPECT_FLOAT_EQ(scene_viewport.y1, 1.0F);
    engine.gui->setOverlayVisible(true);

    // Rasterizer, deferred shading.
    renderer_vars.rasterizationMode = RasterizationMode::Deferred;
    run_frames(engine, frames_per_mode, "deferred");

    // Back to forward: mode switches must be repeatable in both directions.
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    run_frames(engine, frames_per_mode, "forward-again");

    if (engine.renderer->supportsHardwareRaytracing()) {
        renderer_vars.raytracing = true;
        renderer_vars.pathTracing = false;
        run_frames(engine, frames_per_mode, "raytracing");

        renderer_vars.raytracing = false;
        renderer_vars.pathTracing = true;
        run_frames(engine, frames_per_mode, "path-tracing");

        renderer_vars.pathTracing = false;
        run_frames(engine, frames_per_mode, "back-to-raster");
    } else {
        GTEST_LOG_(INFO) << "Hardware raytracing not supported - raytracing/path tracing modes not exercised.";
    }

    // EditorEngine tears down in App::run's order: the renderer first, while the scene and GUI keep its device alive.
}

// The renderer draws by this rule and the editor shows it, so an RT/PT pick reads raster until a TLAS exists.
TEST(RenderModeUnit, DrawnModeIsRasterUntilTracingIsAvailable)
{
    EXPECT_EQ(drawnRenderMode(false, false, true), 0);
    EXPECT_EQ(drawnRenderMode(true, false, true), 1);
    EXPECT_EQ(drawnRenderMode(false, true, true), 2);
    EXPECT_EQ(drawnRenderMode(true, false, false), 0) << "ray tracing picked before the TLAS exists draws raster";
    EXPECT_EQ(drawnRenderMode(false, true, false), 0) << "path tracing picked before the TLAS exists draws raster";
    // The editor never sets both; when it does not hold, path tracing wins, as GUI::renderMode() reads them.
    EXPECT_EQ(drawnRenderMode(true, true, true), 2);
}

// Raster filters what the raster paths drew, Always every frame; post and the fxaa stage both decide by this rule.
TEST(RenderModeUnit, FxaaFiltersRasterFramesOrEveryFrame)
{
    EXPECT_FALSE(fxaaFilters(FxaaMode::Off, false, true));
    EXPECT_FALSE(fxaaFilters(FxaaMode::Off, true, true));
    EXPECT_TRUE(fxaaFilters(FxaaMode::Raster, false, true));
    EXPECT_FALSE(fxaaFilters(FxaaMode::Raster, true, true)) << "Raster filtered a traced frame";
    EXPECT_TRUE(fxaaFilters(FxaaMode::Always, false, true));
    EXPECT_TRUE(fxaaFilters(FxaaMode::Always, true, true));
    // A stage without its set or pipeline would leave the swapchain image unwritten and the GUI undrawn.
    for (const FxaaMode mode : { FxaaMode::Raster, FxaaMode::Always }) {
        EXPECT_FALSE(fxaaFilters(mode, false, false))
          << "post rendered into the intermediate of an FXAA that cannot run";
    }
    EXPECT_FALSE(FxaaStage{}.ready()) << "a stage without its sets and pipeline reported ready";
    // Forward has no MSAA yet, so the editor starts with FXAA on every frame.
    EXPECT_EQ(GUIRendererSharedVars{}.fxaaMode, FxaaMode::Always);
    // The labels the editor shows, in the enum's order.
    EXPECT_STREQ(FXAA_MODE_LABELS[static_cast<std::size_t>(FxaaMode::Off)], "Off");
    EXPECT_STREQ(FXAA_MODE_LABELS[static_cast<std::size_t>(FxaaMode::Raster)], "Raster");
    EXPECT_STREQ(FXAA_MODE_LABELS[static_cast<std::size_t>(FxaaMode::Always)], "Always");
}

// The renderer's submit-failure log once named the GUI's pick, ray tracing first, even while raster was drawn.
TEST(RenderModeUnit, SubmitLogNamesTheDrawnMode)
{
    EXPECT_STREQ(renderModeLogName(drawnRenderMode(true, false, false)), "rasterizer")
      << "a ray-tracing pick without a TLAS draws raster, so the log must say so";
    EXPECT_STREQ(renderModeLogName(drawnRenderMode(false, true, false)), "rasterizer");
    EXPECT_STREQ(renderModeLogName(drawnRenderMode(true, false, true)), "raytracing");
    EXPECT_STREQ(renderModeLogName(drawnRenderMode(false, true, true)), "path_tracing");
    EXPECT_STREQ(renderModeLogName(drawnRenderMode(true, true, true)), "path_tracing")
      << "path tracing wins over ray tracing, as drawnRenderMode decides";
}

// Picked while the scene holds nothing to trace, ray tracing must show raster and wait; a model then makes it draw.
TEST(Integration, EditorShowsRasterUntilTheSceneCanBeTraced)
{
    SKIP_WITHOUT_GPU();

    const ScopedEnvVar missing("KATAGLYPHIS_MODEL_OVERRIDE", MISSING_MODEL);
    EditorEngine engine;
    auto &renderer_vars = engine.settings.renderer;
    renderer_vars.raytracing = true;
    renderer_vars.pathTracing = false;

    engine.waitForModel();
    engine.frame();
    ASSERT_FALSE(engine.renderer->hasDeviceLost());
    ASSERT_EQ(engine.scene->getModelCount(), 0U) << MISSING_MODEL << " loaded after all";
    EXPECT_FALSE(engine.feedback.tracingReady) << "no model, no TLAS: nothing can be traced yet";
    EXPECT_EQ(engine.gui->displayedRenderMode(), 0) << "the editor claims ray tracing while raster frames are drawn";
    EXPECT_TRUE(renderer_vars.raytracing) << "the pick must wait for the TLAS, not be dropped";

    // The library's load: a model arrives, and with it a TLAS.
    const int rig_row =
      sceneConfig::modelIndexOf(sceneConfig::getAvailableModelPaths(), "Models/ShadowTest/shadow_rig.obj");
    ASSERT_GE(rig_row, 0) << "the shadow rig is not in the model library";
    auto &scene_vars = engine.settings.scene;
    scene_vars.selected_model_index = rig_row;
    scene_vars.model_reload_requested = true;
    engine.frame();
    engine.frame();
    ASSERT_FALSE(engine.renderer->hasDeviceLost());
    ASSERT_EQ(engine.scene->getModelCount(), 1U) << "the shadow rig did not load";
    if (engine.renderer->supportsHardwareRaytracing()) {
        EXPECT_TRUE(engine.feedback.tracingReady);
        EXPECT_EQ(engine.gui->displayedRenderMode(), 1) << "the model has loaded, so ray tracing now draws";
        EXPECT_EQ(engine.feedback.visibility.meshes_drawn, 0U) << "a traced frame must not run the raster pass";
    } else {
        EXPECT_FALSE(engine.feedback.tracingReady);
        EXPECT_EQ(engine.gui->displayedRenderMode(), 0) << "without hardware ray tracing every frame is raster";
    }
}

// The switch shows Raster while a tracing pick waits for a TLAS; a press on that Raster segment must drop the pick.
TEST(Integration, RasterSegmentCancelsAPendingTracingPick)
{
    SKIP_WITHOUT_GPU();

    const ScopedEnvVar missing("KATAGLYPHIS_MODEL_OVERRIDE", MISSING_MODEL);
    EditorEngine engine;
    auto &renderer_vars = engine.settings.renderer;
    renderer_vars.raytracing = true;
    renderer_vars.pathTracing = false;
    engine.waitForModel();
    engine.frame();
    ASSERT_FALSE(engine.renderer->hasDeviceLost());
    ASSERT_EQ(engine.gui->displayedRenderMode(), 0) << "with no TLAS the switch must show Raster";
    ASSERT_TRUE(renderer_vars.raytracing);

    // The menu bar's switch: segment 0 of "##renderer_switch", inside BeginMenuBar's "##MenuBar" ID scope.
    ImGuiWindow *menu_bar = ImGui::FindWindowByName("##MainMenuBar");
    ASSERT_NE(menu_bar, nullptr) << "the editor drew no main menu bar";
    const ImGuiID menu_scope = ImGui::GetIDWithSeed("##MenuBar", nullptr, menu_bar->ID);
    const ImGuiID raster_segment =
      ImGui::GetIDWithSeed(0, ImGui::GetIDWithSeed("##renderer_switch", nullptr, menu_scope));
    ImGui::ActivateItemByID(raster_segment);
    engine.frame();
    ASSERT_FALSE(engine.renderer->hasDeviceLost());

    EXPECT_EQ(ImGui::GetCurrentContext()->LastActiveId, raster_segment)
      << "the press reached no item: the switch's ID scope moved, so this test presses nothing";
    EXPECT_FALSE(renderer_vars.raytracing) << "pressing the shown Raster segment left the pending ray-tracing pick";
    EXPECT_FALSE(renderer_vars.pathTracing);
}

// One set of scene targets serves every frame in flight, and only the drawn raster path's exist, as Rust's pool keeps.
TEST(Integration, SceneTargetsAreOneSetOfTheDrawnRasterPath)
{
    SKIP_WITHOUT_GPU();

    // A small scene: the targets are measured, and its TLAS lets the traced frames run.
    const ScopedEnvVar rig("KATAGLYPHIS_MODEL_OVERRIDE", "Models/ShadowTest/shadow_rig.obj");
    EditorEngine engine;
    auto &renderer_vars = engine.settings.renderer;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    run_frames(engine, 2, "forward");
    ASSERT_GE(engine.renderer->swapChainImageCount(), 2U) << "one swapchain image cannot tell one set from one each";

    const auto expect_targets_of = [&engine](RasterizationMode drawn, const std::string &when) {
        const Kataglyphis::SceneTargetFootprint footprint = engine.renderer->sceneTargetFootprint();
        const vk::Extent2D extent = engine.renderer->sceneExtent();
        EXPECT_EQ(footprint.extent, extent) << when;
        const vk::DeviceSize pixels = static_cast<vk::DeviceSize>(extent.width) * extent.height;
        // RGBA16F, so one image holds at least 8 bytes a pixel and a second one would double it.
        EXPECT_EQ(footprint.color.images, 1U) << when << ": the HDR target is not one for every frame in flight";
        EXPECT_GE(footprint.color.bytes, pixels * 8U) << when;
        EXPECT_LT(footprint.color.bytes, pixels * 16U) << when;
        EXPECT_EQ(footprint.fxaa.images, 1U) << when << ": FXAA's intermediate is not one for every frame in flight";
        const bool forward = drawn == RasterizationMode::Forward;
        // The depth and the SSAO prepass; the three G-buffer targets and their depth.
        EXPECT_EQ(footprint.forward.images, forward ? 2U : 0U) << when << ": the forward targets";
        EXPECT_EQ(footprint.deferred.images, forward ? 0U : 4U) << when << ": the deferred targets";
        if (forward) {
            EXPECT_GE(footprint.forward.bytes, pixels * 8U) << when;
        } else {
            EXPECT_GE(footprint.deferred.bytes, pixels * 24U) << when;
        }
    };
    expect_targets_of(RasterizationMode::Forward, "forward start");

    // The first deferred frame allocates the G-buffer, and the forward depth goes.
    renderer_vars.rasterizationMode = RasterizationMode::Deferred;
    run_frames(engine, 2, "deferred");
    expect_targets_of(RasterizationMode::Deferred, "deferred");

    // A resize recreates only the drawn path's.
    engine.renderer->recreateSwapChain();
    run_frames(engine, 1, "deferred-resized");
    expect_targets_of(RasterizationMode::Deferred, "deferred after a swapchain recreation");

    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    run_frames(engine, 2, "forward-again");
    expect_targets_of(RasterizationMode::Forward, "forward again");

    // The footprint holds every extent-sized image, so a target made per swapchain image grows the allocator past it.
    const auto allocated = [&engine] {
        VmaTotalStatistics stats{};
        vmaCalculateStatistics(engine.renderer->sharedDevice()->getVmaAllocator(), &stats);
        return static_cast<double>(stats.total.statistics.allocationBytes);
    };
    const double allocated_before = allocated();
    const double footprint_before = static_cast<double>(engine.renderer->sceneTargetFootprint().totalBytes());
    glfwSetWindowSize(engine.window->get_window(), 1000, 640);
    run_frames(engine, 2, "forward-smaller");
    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetFramebufferSize(engine.window->get_window(), &framebuffer_width, &framebuffer_height);
    ASSERT_EQ(engine.renderer->sceneExtent(),
      (vk::Extent2D{ static_cast<uint32_t>(framebuffer_width), static_cast<uint32_t>(framebuffer_height) }))
      << "the scene extent did not follow the window";
    const double footprint_shrink =
      footprint_before - static_cast<double>(engine.renderer->sceneTargetFootprint().totalBytes());
    ASSERT_GT(footprint_shrink, 0.0) << "a smaller window freed no target memory";
    EXPECT_NEAR(allocated_before - allocated(), footprint_shrink, 0.01 * footprint_shrink + 65536.0)
      << "the allocator shrank by more or less than the scene targets did: an extent-sized image is missing from "
         "sceneTargetFootprint(), or is made once per swapchain image";
    expect_targets_of(RasterizationMode::Forward, "forward at a smaller extent");

    // A traced frame keeps the picked raster path's: without a TLAS the same pick draws raster again.
    if (engine.renderer->supportsHardwareRaytracing()) {
        // One path per pixel and frame: the targets are measured, not the image.
        renderer_vars.pathTracingSamplesPerPixel = 1;
        renderer_vars.pathTracingMaxBounces = 1;
        renderer_vars.raytracing = true;
        run_frames(engine, 2, "ray traced");
        ASSERT_TRUE(engine.feedback.tracingReady) << "the shadow rig has no TLAS";
        expect_targets_of(RasterizationMode::Forward, "ray traced");
        EXPECT_EQ(engine.renderer->sceneTargetFootprint().traced.images, 2U) << "the path tracer's history and depth";
        renderer_vars.raytracing = false;
        renderer_vars.pathTracing = true;
        renderer_vars.rasterizationMode = RasterizationMode::Deferred;
        run_frames(engine, 2, "path traced");
        expect_targets_of(RasterizationMode::Deferred, "path traced with deferred picked");
        renderer_vars.pathTracing = false;
    }
    ASSERT_FALSE(engine.renderer->hasDeviceLost());
}

// Every scene target follows the render scale in every mode, the output's stay, and the path tracer starts over.
TEST(Integration, SceneTargetsFollowTheRenderScale)
{
    SKIP_WITHOUT_GPU();

    const ScopedEnvVar rig("KATAGLYPHIS_MODEL_OVERRIDE", "Models/ShadowTest/shadow_rig.obj");
    EditorEngine engine;
    auto &renderer_vars = engine.settings.renderer;
    auto &scene_vars = engine.settings.scene;
    // Bloom and SSAO draw at half the scene extent, so both run.
    renderer_vars.bloom_strength = 0.6F;
    renderer_vars.ssao_strength = 0.7F;

    // Each path's targets at full scale, which a scaled group is measured against: no format or alignment is assumed.
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    run_frames(engine, 2, "forward at full scale");
    const Kataglyphis::SceneTargetFootprint full_forward = engine.renderer->sceneTargetFootprint();
    renderer_vars.rasterizationMode = RasterizationMode::Deferred;
    run_frames(engine, 2, "deferred at full scale");
    const Kataglyphis::SceneTargetFootprint full_deferred = engine.renderer->sceneTargetFootprint();
    ASSERT_EQ(full_forward.extent, full_forward.outputExtent);
    ASSERT_GT(full_forward.forward.images, 0U);
    ASSERT_GT(full_deferred.deferred.images, 0U);
    ASSERT_GT(full_forward.clouds.images, 0U);

    const auto expect_scaled = [&engine](
                                 float scale, const Kataglyphis::SceneTargetFootprint &full, const std::string &when) {
        const vk::Extent2D output = engine.renderer->outputExtent();
        ASSERT_EQ(output, full.outputExtent) << when;
        const auto side = [scale](uint32_t length) {
            return std::max<uint32_t>(1U, static_cast<uint32_t>(std::lround(static_cast<float>(length) * scale)));
        };
        const vk::Extent2D scene{ side(output.width), side(output.height) };
        ASSERT_EQ(engine.renderer->sceneExtent(), scene) << when;
        const Kataglyphis::SceneTargetFootprint footprint = engine.renderer->sceneTargetFootprint();
        EXPECT_EQ(footprint.extent, scene) << when;
        EXPECT_EQ(footprint.outputExtent, output) << when;
        const double share = (static_cast<double>(scene.width) * static_cast<double>(scene.height))
                             / (static_cast<double>(output.width) * static_cast<double>(output.height));
        // A group left at the output's extent keeps its full-scale bytes: 4x the scaled ones at 0.5, 1.8x at 0.75.
        const auto expect_group = [&when](const char *name,
                                    const Kataglyphis::SceneTargetGroup &scaled,
                                    const Kataglyphis::SceneTargetGroup &unscaled,
                                    double expected_share) {
            EXPECT_EQ(scaled.images, unscaled.images) << when << ": " << name;
            const double expected = expected_share * static_cast<double>(unscaled.bytes);
            EXPECT_NEAR(static_cast<double>(scaled.bytes),
              expected,
              0.05 * expected + 65536.0 * static_cast<double>(scaled.images))
              << when << ": " << name << " hold " << scaled.bytes << " B of " << unscaled.bytes << " at full scale";
        };
        expect_group("the HDR target", footprint.color, full.color, share);
        expect_group("the forward depths", footprint.forward, full.forward, share);
        expect_group("the G-buffer and its depth", footprint.deferred, full.deferred, share);
        expect_group("the traced history and depth", footprint.traced, full.traced, share);
        expect_group("bloom's and SSAO's half targets", footprint.post, full.post, share);
        expect_group("the clouds' full and half targets", footprint.clouds, full.clouds, share);
        expect_group("FXAA's intermediate, the output's", footprint.fxaa, full.fxaa, 1.0);
    };

    renderer_vars.render_scale = 0.5F;
    run_frames(engine, 2, "deferred at half scale");
    expect_scaled(0.5F, full_deferred, "deferred at half scale");
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    run_frames(engine, 2, "forward at half scale");
    expect_scaled(0.5F, full_forward, "forward at half scale");

    // The clouds march the scaled depth and post filters them up through its sampler; their resize runs drawn.
    scene_vars.clouds_enabled = true;
    run_frames(engine, 2, "forward with clouds at half scale");
    const std::vector<std::string> recorded = engine.renderer->renderStageRegistry().lastRecorded();
    EXPECT_NE(std::find(recorded.begin(), recorded.end(), "clouds"), recorded.end()) << "the clouds did not draw";
    expect_scaled(0.5F, full_forward, "forward with clouds at half scale");

    // A new scale resizes every scene target without a swapchain recreation.
    renderer_vars.rasterizationMode = RasterizationMode::Deferred;
    renderer_vars.render_scale = 0.75F;
    run_frames(engine, 2, "deferred with clouds at three quarters");
    expect_scaled(0.75F, full_deferred, "deferred with clouds at three quarters");

    // A swapchain recreation keeps the scale.
    engine.renderer->recreateSwapChain();
    run_frames(engine, 1, "deferred with clouds at three quarters, recreated");
    expect_scaled(0.75F, full_deferred, "deferred with clouds at three quarters after a swapchain recreation");
    scene_vars.clouds_enabled = false;

    if (engine.renderer->supportsHardwareRaytracing()) {
        renderer_vars.rasterizationMode = RasterizationMode::Forward;
        renderer_vars.render_scale = 0.5F;
        renderer_vars.raytracing = true;
        run_frames(engine, 2, "ray traced at half scale");
        ASSERT_TRUE(engine.feedback.tracingReady) << "the shadow rig has no TLAS";
        expect_scaled(0.5F, full_forward, "ray traced at half scale");

        // One path per pixel and frame: the history's length is measured, not the image.
        renderer_vars.pathTracingSamplesPerPixel = 1;
        renderer_vars.pathTracingMaxBounces = 1;
        renderer_vars.raytracing = false;
        renderer_vars.pathTracing = true;
        run_frames(engine, 3, "path traced at half scale");
        expect_scaled(0.5F, full_forward, "path traced at half scale");
        EXPECT_EQ(engine.feedback.pathTracingAccumulatedSamples, 3U);
        // Another extent is another image: the running mean starts over, though camera and settings stayed.
        renderer_vars.render_scale = 0.75F;
        run_frames(engine, 1, "path traced at three quarters");
        expect_scaled(0.75F, full_forward, "path traced at three quarters");
        EXPECT_EQ(engine.feedback.pathTracingAccumulatedSamples, 1U)
          << "the path tracer kept averaging a history of another extent";
        renderer_vars.pathTracing = false;
    }

    renderer_vars.render_scale = 1.0F;
    run_frames(engine, 1, "back at full scale");
    EXPECT_EQ(engine.renderer->sceneExtent(), engine.renderer->outputExtent());
    ASSERT_FALSE(engine.renderer->hasDeviceLost());
}
