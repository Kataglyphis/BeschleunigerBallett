// Device loss through VulkanRenderer's test seam, then App::run's teardown: reverse declaration order.

#include "EngineFrame.hpp"
#include "EngineLoadWait.hpp"
#include "GpuTestGuard.hpp"
#include "ScopedEnvVar.hpp"
#include "app/AppExitCode.hpp"
#include <gtest/gtest.h>
#include <vulkan/vulkan.hpp>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.instance;
import kataglyphis.vulkan.render_settings;
import kataglyphis.vulkan.renderer;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.window;

namespace {

using Kataglyphis::TestSupport::ScopedEnvVar;

// A textured card: the scene then owns VMA buffers and images that must be released on a live allocator.
constexpr const char *TEXTURED_CARD_MODEL = "Models/GltfTest/uv_transform_card.gltf";
constexpr int WARMUP_FRAMES = 3;

// Validation messages only: the renderer logs an error for the lost frame, the loader one for an ICD without a device.
class ScopedValidationErrorCounter
{
  public:
    ScopedValidationErrorCounter()
      : sink_(std::make_shared<spdlog::sinks::callback_sink_mt>([this](const spdlog::details::log_msg &msg) {
            const std::string_view text(msg.payload.data(), msg.payload.size());
            if (msg.level >= spdlog::level::err && text.starts_with("ERROR: [") && !text.contains("[Loader Message]")) {
                ++error_count_;
            }
        }))
    { spdlog::default_logger()->sinks().push_back(sink_); }
    ScopedValidationErrorCounter(const ScopedValidationErrorCounter &) = delete;
    ScopedValidationErrorCounter &operator=(const ScopedValidationErrorCounter &) = delete;
    ~ScopedValidationErrorCounter()
    {
        auto &sinks = spdlog::default_logger()->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), sink_), sinks.end());
    }

    [[nodiscard]] int errorCount() const { return error_count_.load(); }

  private:
    std::shared_ptr<spdlog::sinks::callback_sink_mt> sink_;
    std::atomic<int> error_count_{ 0 };
};

// App::run's objects in App::run's declaration order, driven by its frame loop minus camera input.
struct AppObjects
{
    ScopedEnvVar model{ "KATAGLYPHIS_MODEL_OVERRIDE", TEXTURED_CARD_MODEL };
    std::unique_ptr<Kataglyphis::Frontend::Window> window{ std::make_unique<Kataglyphis::Frontend::Window>(1200, 768) };
    Kataglyphis::RenderSettings settings;
    Kataglyphis::RendererFeedback feedback;
    std::unique_ptr<Kataglyphis::Scene> scene{ std::make_unique<Kataglyphis::Scene>() };
    std::unique_ptr<Kataglyphis::Frontend::GUI> gui{
        std::make_unique<Kataglyphis::Frontend::GUI>(window.get(), settings, feedback)
    };
    std::unique_ptr<Camera> camera{ std::make_unique<Camera>() };
    std::unique_ptr<Kataglyphis::VulkanRenderer> renderer{
        std::make_unique<Kataglyphis::VulkanRenderer>(window.get(), scene.get(), camera.get(), settings, feedback)
    };

    AppObjects() { Kataglyphis::TestSupport::attachEditor(*gui, *renderer); }

    void frame()
    {
        glfwPollEvents();
        Kataglyphis::TestSupport::renderEditorFrame(*gui, *renderer, scene.get(), camera.get(), settings, feedback);
    }

    void warmUp()
    {
        Kataglyphis::TestSupport::waitForModelLoad(renderer.get(), [this] { frame(); });
        for (int i = 0; i < WARMUP_FRAMES; ++i) { frame(); }
    }

    // Exactly the destruction App::run's scope exit performs, with the device's lifetime checked between steps.
    void tearDownInAppOrder()
    {
        const std::weak_ptr<Kataglyphis::VulkanDevice> device = renderer->deviceLifetime();
        ASSERT_FALSE(device.expired()) << "the renderer has no device to observe";
        const std::weak_ptr<Kataglyphis::VulkanInstance> instance = device.lock()->getInstance();

        renderer.reset();
        if (const auto alive = device.lock()) {
            EXPECT_TRUE(alive->getLogicalDevice())
              << "the renderer destroyed the logical device while the scene and the GUI still own resources on it";
        } else {
            ADD_FAILURE() << "the device went with the renderer, before the scene and the GUI released theirs";
        }

        camera.reset();
        gui.reset();
        scene.reset();
        EXPECT_TRUE(device.expired()) << "the device outlived its last user: something still holds a reference";
        EXPECT_TRUE(instance.expired()) << "the instance outlived the device, its last child";

        window.reset();
    }
};

}// namespace

TEST(Integration, SimulatedDeviceLossEndsTheRunAsAFailureAndTearsDownCleanly)
{
    SKIP_WITHOUT_GPU();

    ScopedValidationErrorCounter validation_errors;
    AppObjects app;
    app.warmUp();
    ASSERT_FALSE(app.renderer->hasDeviceLost()) << "the device was lost before the simulated loss";
    ASSERT_FALSE(app.scene->isModelLoadPending()) << "the card never loaded, so the scene owns nothing to release";

    app.renderer->simulateDeviceLossForTesting();
    app.frame();

    EXPECT_TRUE(app.renderer->hasDeviceLost()) << "the failed fence wait did not take the device-lost path";
    EXPECT_TRUE(app.renderer->hasFatalFrameError());
    EXPECT_EQ(glfwWindowShouldClose(app.window->get_window()), GLFW_TRUE) << "App::run's loop would not end";
    EXPECT_EQ(
      Kataglyphis::appExitCode(app.renderer->hasDeviceLost(), app.renderer->hasFatalFrameError()), EXIT_FAILURE);

    // A lost device stays lost; a frame drawn anyway must fail the same way, not crash.
    app.frame();
    EXPECT_TRUE(app.renderer->hasDeviceLost());

    app.tearDownInAppOrder();
    EXPECT_EQ(validation_errors.errorCount(), 0) << "the run or its teardown logged a Vulkan validation error";
}

TEST(Integration, TeardownAfterACleanRunReleasesTheDeviceAfterItsLastUser)
{
    SKIP_WITHOUT_GPU();

    ScopedValidationErrorCounter validation_errors;
    AppObjects app;
    app.warmUp();
    ASSERT_FALSE(app.renderer->hasDeviceLost()) << "the device was lost during a clean run";
    ASSERT_FALSE(app.scene->isModelLoadPending()) << "the card never loaded, so the scene owns nothing to release";

    app.tearDownInAppOrder();
    EXPECT_EQ(validation_errors.errorCount(), 0) << "the run or its teardown logged a Vulkan validation error";
}
