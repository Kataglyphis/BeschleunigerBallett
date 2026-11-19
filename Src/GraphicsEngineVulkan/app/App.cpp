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
import kataglyphis.vulkan.gltf_loader;
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.render_settings;
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
    double last_time = 0.0;

    std::unique_ptr<Kataglyphis::Frontend::Window> const window =
      std::make_unique<Kataglyphis::Frontend::Window>(window_width, window_height, true);

    if (!window->is_valid()) {
        spdlog::critical("Window creation failed, aborting startup!");
        return EXIT_FAILURE;
    }

    // The editor writes the settings and shows the feedback; the renderer reads one and fills the other per frame.
    Kataglyphis::RenderSettings settings;
    Kataglyphis::RendererFeedback feedback;

    // The environment's loader options are read here once; the loaders themselves read none.
    std::unique_ptr<Scene> const scene = std::make_unique<Scene>(gltfLoadOptionsFromEnvironment());
    std::unique_ptr<Kataglyphis::Frontend::GUI> const gui =
      std::make_unique<Kataglyphis::Frontend::GUI>(window.get(), settings, feedback);
    std::unique_ptr<Camera> const camera = std::make_unique<Camera>();

    // Declared last, so destroyed first: it drains the GPU, and the scene and GUI keep the device alive past it.
    Kataglyphis::VulkanRenderer vulkan_renderer{ window.get(), scene.get(), camera.get(), settings, feedback };
    gui->initializeVulkanContext(vulkan_renderer.sharedDevice(),
      vulkan_renderer.vulkanInstance(),
      vulkan_renderer.swapChainFormat(),
      vulkan_renderer.swapChainImageCount());
    gui->setUserSelectionForRRT(vulkan_renderer.supportsHardwareRaytracing());

    while (!window->get_should_close()) {
        // poll all events incoming from user
        glfwPollEvents();

        Kataglyphis::Frontend::update_frame_timing(delta_time, last_time);

        // handle events for the camera
        Kataglyphis::Frontend::process_camera_input(window.get(), camera.get(), delta_time);

        //// retrieve updates from the UI
        gui->render();

        // Then update Scene/Renderer state with the NEW settings
        vulkan_renderer.updateStateDueToUserInput(settings, feedback);
        Kataglyphis::applyRendererFeedback(settings, feedback);
        settings.time_seconds = glfwGetTime();
        vulkan_renderer.updateUniforms(scene.get(), camera.get(), settings);

        // Finally draw the frame with the updated state
        vulkan_renderer.drawFrame(settings, feedback);
    }

    return Kataglyphis::appExitCode(vulkan_renderer.hasDeviceLost(), vulkan_renderer.hasFatalFrameError());
}

Kataglyphis::App::~App() = default;
