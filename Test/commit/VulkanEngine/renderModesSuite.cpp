#include "EngineLoadWait.hpp"
#include <gtest/gtest.h>
#include <vulkan/vulkan.hpp>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <memory>
#include <string>

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.gui_renderer_shared_vars;
import kataglyphis.vulkan.renderer;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.window;

namespace {

bool glfw_reports_vulkan_support()
{
    if (glfwInit() == 0) { return false; }
    const bool supports_vulkan = glfwVulkanSupported() != 0;
    glfwTerminate();
    return supports_vulkan;
}

using Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode;

// One iteration of the App::run frame loop (App.cpp) minus camera input.
void run_frames(Kataglyphis::Frontend::Window *window,
  Kataglyphis::Scene *scene,
  Kataglyphis::Frontend::GUI *gui,
  Camera *camera,
  Kataglyphis::VulkanRenderer *renderer,
  int frame_count,
  const std::string &mode_label)
{
    // The model parses on a worker thread, so the first frames show an empty scene.
    Kataglyphis::TestSupport::waitForModelLoad(renderer, [&] {
        glfwPollEvents();
        gui->render();
        auto &guiSceneSharedVars = gui->getGuiSceneSharedVars();
        renderer->updateStateDueToUserInput(guiSceneSharedVars);
        renderer->updateUniforms(scene, camera, guiSceneSharedVars);
        renderer->drawFrame(guiSceneSharedVars);
    });

    for (int frame = 0; frame < frame_count; ++frame) {
        glfwPollEvents();
        gui->render();
        auto &guiSceneSharedVars = gui->getGuiSceneSharedVars();
        renderer->updateStateDueToUserInput(guiSceneSharedVars);
        renderer->updateUniforms(scene, camera, guiSceneSharedVars);
        renderer->drawFrame(guiSceneSharedVars);

        ASSERT_FALSE(renderer->hasDeviceLost()) << "Device lost in mode '" << mode_label << "' at frame " << frame;
    }
}

}// namespace

TEST(Integration, RenderModesSelectableInGui)
{
    if (!glfw_reports_vulkan_support()) { GTEST_SKIP() << "GLFW/Vulkan runtime is unavailable on this system."; }

    constexpr int window_width = 1200;
    constexpr int window_height = 768;
    constexpr int frames_per_mode = 3;

    using namespace Kataglyphis;
    auto window = std::make_unique<Kataglyphis::Frontend::Window>(window_width, window_height);
    auto scene = std::make_unique<Scene>();
    auto gui = std::make_unique<Kataglyphis::Frontend::GUI>(window.get());
    auto camera = std::make_unique<Camera>();

    auto renderer = std::make_unique<VulkanRenderer>(window.get(), scene.get(), gui.get(), camera.get());

    auto &renderer_vars = gui->getGuiRendererSharedVars();

    // Rasterizer, forward shading (the startup default).
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = false;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    run_frames(window.get(), scene.get(), gui.get(), camera.get(), renderer.get(), frames_per_mode, "forward");

    // Rasterizer, deferred shading.
    renderer_vars.rasterizationMode = RasterizationMode::Deferred;
    run_frames(window.get(), scene.get(), gui.get(), camera.get(), renderer.get(), frames_per_mode, "deferred");

    // Back to forward: mode switches must be repeatable in both directions.
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    run_frames(window.get(), scene.get(), gui.get(), camera.get(), renderer.get(), frames_per_mode, "forward-again");

    if (renderer->supportsHardwareRaytracing()) {
        renderer_vars.raytracing = true;
        renderer_vars.pathTracing = false;
        run_frames(window.get(), scene.get(), gui.get(), camera.get(), renderer.get(), frames_per_mode, "raytracing");

        renderer_vars.raytracing = false;
        renderer_vars.pathTracing = true;
        run_frames(window.get(), scene.get(), gui.get(), camera.get(), renderer.get(), frames_per_mode, "path-tracing");

        renderer_vars.pathTracing = false;
        run_frames(
          window.get(), scene.get(), gui.get(), camera.get(), renderer.get(), frames_per_mode, "back-to-raster");
    } else {
        GTEST_LOG_(INFO) << "Hardware raytracing not supported - raytracing/path tracing modes not exercised.";
    }

    if (!renderer->hasDeviceLost()) { renderer->finishAllRenderCommands(); }

    if (!renderer->hasDeviceLost()) {
        scene->cleanUp();
        gui->cleanUp();
    }

    renderer->cleanUp();
    renderer.reset();
    camera.reset();
    gui.reset();
    scene.reset();
    window->cleanUp();
    window.reset();
}
