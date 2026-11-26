#include "EngineFrame.hpp"
#include "GpuTestGuard.hpp"
#include "common/ExtensionSupport.hpp"
#include <gtest/gtest.h>
#include <vulkan/vulkan.hpp>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <array>
#include <cstdlib>
#include <glm/glm.hpp>
#include <glm/mat4x4.hpp>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.debug;
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.instance;
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

// Profile and Release create no validation layer; their captures still need the labels and the object names.
TEST(Integration, DebugLabelsAndNamesWorkWithoutTheValidationLayer)
{
    SKIP_WITHOUT_GPU();

    Kataglyphis::VulkanInstance instance(false);
    const auto listed = vk::enumerateInstanceExtensionProperties();
    ASSERT_EQ(listed.result, vk::Result::eSuccess);
    const bool loader_lists = Kataglyphis::supportsExtension(listed.value, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    EXPECT_EQ(instance.debugUtilsEnabled(), loader_lists) << "debug_utils must come on exactly where it is listed";
    if (!loader_lists) { return; }

    const auto physical_devices = instance.getVulkanInstance().enumeratePhysicalDevices();
    ASSERT_EQ(physical_devices.result, vk::Result::eSuccess);
    ASSERT_FALSE(physical_devices.value.empty());
    const float priority = 1.0F;
    const vk::DeviceQueueCreateInfo queue_info({}, 0, 1, &priority);
    vk::DeviceCreateInfo device_info{};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    const auto created = physical_devices.value.front().createDevice(device_info);
    ASSERT_EQ(created.result, vk::Result::eSuccess);
    const vk::Device device = created.value;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(device);

    EXPECT_TRUE(Kataglyphis::debug::labelsAvailable()) << "the label calls would no-op in a Profile or Release frame";
    const auto pool = device.createCommandPool(vk::CommandPoolCreateInfo({}, 0));
    ASSERT_EQ(pool.result, vk::Result::eSuccess);
    const auto buffers =
      device.allocateCommandBuffers(vk::CommandBufferAllocateInfo(pool.value, vk::CommandBufferLevel::ePrimary, 1));
    ASSERT_EQ(buffers.result, vk::Result::eSuccess);
    const vk::CommandBuffer command_buffer = buffers.value.front();
    ASSERT_EQ(command_buffer.begin(vk::CommandBufferBeginInfo{}), vk::Result::eSuccess);
    {
        const Kataglyphis::debug::ScopedCmdLabel label(command_buffer, "probe", { 1.0F, 1.0F, 1.0F, 1.0F });
    }
    EXPECT_EQ(command_buffer.end(), vk::Result::eSuccess);
    EXPECT_TRUE(Kataglyphis::debug::nameObject(device, command_buffer, "debug-utils probe"));
    EXPECT_TRUE(Kataglyphis::debug::nameObject(device, pool.value, "debug-utils probe pool"));

    device.destroyCommandPool(pool.value);
    device.destroy();
}
