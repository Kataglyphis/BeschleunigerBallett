module;

#include <cstdlib>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <format>
#include <memory>

#include "spdlog/spdlog.h"

#include "app/AppExitCode.hpp"

module kataglyphis.vulkan.app;

import kataglyphis.shared.frontend.frame_input;

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.renderer;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.window;

Kataglyphis::App::App() = default;

auto Kataglyphis::App::run() -> int
{
    // Room for the editor's docks around the scene; Window scales it to the monitor and keeps it on screen.
    int const window_width = 1600;
    int const window_height = 900;

    float delta_time = 0.0F;
    float last_time = 0.0F;

    std::unique_ptr<Kataglyphis::Frontend::Window> const window =
      std::make_unique<Kataglyphis::Frontend::Window>(window_width, window_height, true);

    if (!window->is_valid()) {
        spdlog::critical("Window creation failed, aborting startup!");
        return EXIT_FAILURE;
    }

    std::unique_ptr<Scene> const scene = std::make_unique<Scene>();
    std::unique_ptr<Kataglyphis::Frontend::GUI> const gui = std::make_unique<Kataglyphis::Frontend::GUI>(window.get());
    std::unique_ptr<Camera> const camera = std::make_unique<Camera>();

    Kataglyphis::VulkanRenderer vulkan_renderer{ window.get(), scene.get(), gui.get(), camera.get() };

    while (!window->get_should_close()) {
        // poll all events incoming from user
        glfwPollEvents();

        Kataglyphis::Frontend::update_frame_timing(delta_time, last_time);

        // handle events for the camera
        Kataglyphis::Frontend::process_camera_input(window.get(), camera.get(), delta_time);

        //// retrieve updates from the UI
        gui->render();

        // Then update Scene/Renderer state with the NEW GUI values
        auto &guiSceneSharedVars = gui->getGuiSceneSharedVars();
        vulkan_renderer.updateStateDueToUserInput(guiSceneSharedVars);
        vulkan_renderer.updateUniforms(scene.get(), camera.get(), guiSceneSharedVars);

        // Finally draw the frame with the updated state
        vulkan_renderer.drawFrame(guiSceneSharedVars);
    }

    if (!vulkan_renderer.hasDeviceLost()) { vulkan_renderer.finishAllRenderCommands(); }

    if (!vulkan_renderer.hasDeviceLost()) {
        scene->cleanUp();
        gui->cleanUp();
    } else {
        spdlog::warn("Skipping scene/gui Vulkan teardown because device is lost.");
    }

    vulkan_renderer.cleanUp();
    window->cleanUp();

    return Kataglyphis::appExitCode(vulkan_renderer.hasDeviceLost(), vulkan_renderer.hasFatalFrameError());
}

Kataglyphis::App::~App() = default;
