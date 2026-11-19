#include "EngineFrame.hpp"
#include "GpuTestGuard.hpp"
#include <gtest/gtest.h>
#include <vulkan/vulkan.hpp>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <cstdlib>
#include <glm/glm.hpp>
#include <glm/mat4x4.hpp>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.render_settings;
import kataglyphis.vulkan.renderer;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.window;

// Demonstrate some basic assertions.
TEST(HelloTestCommit, BasicAssertions)
{

    // Expect two strings not to be equal.
    EXPECT_STRNE("hello", "world");
    // Expect equality.
    EXPECT_EQ(7 * 6, 42);
}

TEST(Integration, VulkanEngine)
{
    EXPECT_EQ(7 * 6, 42);

    SKIP_WITHOUT_GPU();

    int window_width = 1200;
    int window_height = 768;

    using namespace Kataglyphis;
    std::unique_ptr<Kataglyphis::Frontend::Window> window =
      std::make_unique<Kataglyphis::Frontend::Window>(window_width, window_height);
    RenderSettings settings;
    RendererFeedback feedback;
    std::unique_ptr<Scene> scene = std::make_unique<Scene>();
    std::unique_ptr<Kataglyphis::Frontend::GUI> gui =
      std::make_unique<Kataglyphis::Frontend::GUI>(window.get(), settings, feedback);
    std::unique_ptr<Camera> camera = std::make_unique<Camera>();

    auto vulkan_renderer =
      std::make_unique<Kataglyphis::VulkanRenderer>(window.get(), scene.get(), camera.get(), settings, feedback);
    Kataglyphis::TestSupport::attachEditor(*gui, *vulkan_renderer);

    // App::run's order: the renderer first, while the scene and GUI it drew for keep its device alive.
    vulkan_renderer.reset();
    camera.reset();
    gui.reset();
    scene.reset();
    window.reset();
}
