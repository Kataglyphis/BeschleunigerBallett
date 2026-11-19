#include "EngineFrame.hpp"
#include "EngineLoadWait.hpp"
#include "GpuTestGuard.hpp"
#include "ScopedEnvVar.hpp"
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <vulkan/vulkan.hpp>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <memory>
#include <string>
#include <string_view>

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.render_settings;
import kataglyphis.vulkan.renderer;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.scene_config;
import kataglyphis.vulkan.window;

namespace {

using Kataglyphis::TestSupport::ScopedEnvVar;
using Kataglyphis::VulkanRendererInternals::FrontendShared::drawnRenderMode;
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
