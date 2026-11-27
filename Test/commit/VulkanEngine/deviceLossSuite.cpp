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
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <vector>

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

// The renderer's device-loss block: every line it logs at error level that starts "Device loss" or is indented.
class ScopedDeviceLossReportCapture
{
  public:
    ScopedDeviceLossReportCapture()
      : sink_(std::make_shared<spdlog::sinks::callback_sink_mt>([this](const spdlog::details::log_msg &msg) {
            const std::string_view text(msg.payload.data(), msg.payload.size());
            if (msg.level >= spdlog::level::err && (text.starts_with("Device loss") || text.starts_with("  "))) {
                const std::scoped_lock lock(mutex_);
                lines_.emplace_back(text);
            }
        }))
    { spdlog::default_logger()->sinks().push_back(sink_); }
    ScopedDeviceLossReportCapture(const ScopedDeviceLossReportCapture &) = delete;
    ScopedDeviceLossReportCapture &operator=(const ScopedDeviceLossReportCapture &) = delete;
    ~ScopedDeviceLossReportCapture()
    {
        auto &sinks = spdlog::default_logger()->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), sink_), sinks.end());
    }

    [[nodiscard]] std::vector<std::string> lines() const
    {
        const std::scoped_lock lock(mutex_);
        return lines_;
    }

  private:
    std::shared_ptr<spdlog::sinks::callback_sink_mt> sink_;
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

std::size_t countStartingWith(const std::vector<std::string> &lines, std::string_view prefix)
{
    return static_cast<std::size_t>(std::count_if(
      lines.begin(), lines.end(), [prefix](const std::string &line) { return line.starts_with(prefix); }));
}

struct ReportedFrame
{
    std::uint64_t serial = 0;
    std::uint32_t image = 0;
};

// "  frame <serial> (image <index>): ...", or nullopt for any other line.
std::optional<ReportedFrame> reportedFrame(std::string_view line)
{
    constexpr std::string_view kFrame = "  frame ";
    constexpr std::string_view kImage = " (image ";
    if (!line.starts_with(kFrame)) { return std::nullopt; }
    line.remove_prefix(kFrame.size());
    ReportedFrame frame{};
    const auto serial_end = std::from_chars(line.data(), line.data() + line.size(), frame.serial);
    if (serial_end.ec != std::errc{}) { return std::nullopt; }
    line.remove_prefix(static_cast<std::size_t>(serial_end.ptr - line.data()));
    if (!line.starts_with(kImage)) { return std::nullopt; }
    line.remove_prefix(kImage.size());
    const auto image_end = std::from_chars(line.data(), line.data() + line.size(), frame.image);
    if (image_end.ec != std::errc{}) { return std::nullopt; }
    line.remove_prefix(static_cast<std::size_t>(image_end.ptr - line.data()));
    if (!line.starts_with("): ")) { return std::nullopt; }
    return frame;
}

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

// The block must not depend on the driver: llvmpipe lists the recorded scopes, a marker device says how far each got.
TEST(Integration, SimulatedDeviceLossLogsTheBreadcrumbsOfEachImagesLastFrameOnce)
{
    SKIP_WITHOUT_GPU();

    ScopedDeviceLossReportCapture report;
    AppObjects app;
    app.warmUp();
    ASSERT_FALSE(app.renderer->hasDeviceLost()) << "the device was lost before the simulated loss";
    ASSERT_TRUE(report.lines().empty()) << "a device-loss block was logged before any loss";

    app.renderer->simulateDeviceLossForTesting();
    app.frame();
    // The second failed frame is the same loss and must not log the block again.
    app.frame();
    ASSERT_TRUE(app.renderer->hasDeviceLost());

    const std::vector<std::string> lines = report.lines();
    const auto header =
      std::ranges::find_if(lines, [](const std::string &line) { return line.starts_with("Device loss breadcrumbs"); });
    ASSERT_NE(header, lines.end()) << "the loss logged no breadcrumbs";
    EXPECT_EQ(countStartingWith(lines, "Device loss breadcrumbs"), 1U) << "the second failed frame logged them again";
    const bool lists_scopes = header->find("the scopes of each image's last submitted frame") != std::string::npos;

    std::vector<std::string> frames;
    for (const std::string &line : lines) {
        if (line.starts_with("  frame ")) { frames.push_back(line); }
    }
    ASSERT_FALSE(frames.empty()) << "the warm-up submitted frames, but the block names none";
    // One line per image, oldest first: an image named twice or a newer frame before an older one misreads the order.
    std::vector<std::uint32_t> images;
    std::optional<std::uint64_t> previous_serial;
    for (const std::string &frame : frames) {
        const std::optional<ReportedFrame> reported = reportedFrame(frame);
        ASSERT_TRUE(reported.has_value()) << "not a frame line: " << frame;
        EXPECT_LT(reported->image, app.renderer->swapChainImageCount()) << frame;
        EXPECT_EQ(std::ranges::count(images, reported->image), 0) << "an image reported twice: " << frame;
        images.push_back(reported->image);
        if (previous_serial) { EXPECT_GT(reported->serial, *previous_serial) << "not oldest first: " << frame; }
        previous_serial = reported->serial;
    }
    for (const std::string &frame : frames) {
        if (lists_scopes) {
            // main has no label, so its breadcrumb is the stage bracket's; the editor's label nests in post or fxaa.
            EXPECT_NE(frame.find("main, main > "), std::string::npos) << frame;
            EXPECT_NE(frame.find(" > gui"), std::string::npos) << frame;
        } else {
            const bool says_progress = frame.ends_with(": completed") || frame.ends_with(": not started")
                                       || frame.find(": last completed ") != std::string::npos;
            EXPECT_TRUE(says_progress) << frame;
        }
    }

    // A live device: the fault query would be invalid usage, so it is skipped or unavailable, never run.
    EXPECT_EQ(countStartingWith(lines, "Device loss fault info: "), 1U);
    const bool fault_line_is_honest = std::ranges::any_of(lines, [](const std::string &line) {
        return line == "Device loss fault info: not queried, the loss is simulated"
               || line == "Device loss fault info: VK_EXT_device_fault is not enabled";
    });
    EXPECT_TRUE(fault_line_is_honest);

    app.tearDownInAppOrder();
}

// A resize while the GPU hangs: the recreation's idle wait sees the loss first, before it resets any trail.
TEST(Integration, ALossFirstSeenBySwapchainRecreationLogsTheBreadcrumbsAndRebuildsNothing)
{
    SKIP_WITHOUT_GPU();

    ScopedValidationErrorCounter validation_errors;
    ScopedDeviceLossReportCapture report;
    AppObjects app;
    app.warmUp();
    ASSERT_FALSE(app.renderer->hasDeviceLost()) << "the device was lost before the simulated loss";
    const std::uint32_t image_count = app.renderer->swapChainImageCount();

    app.renderer->simulateDeviceLossForTesting();
    EXPECT_FALSE(app.renderer->recreateSwapChain()) << "the swapchain was rebuilt on a lost device";
    EXPECT_TRUE(app.renderer->hasDeviceLost()) << "the failed idle wait did not take the device-lost path";
    EXPECT_TRUE(app.renderer->hasFatalFrameError());
    EXPECT_EQ(glfwWindowShouldClose(app.window->get_window()), GLFW_TRUE) << "App::run's loop would not end";
    EXPECT_EQ(app.renderer->swapChainImageCount(), image_count);

    const std::vector<std::string> lines = report.lines();
    EXPECT_EQ(countStartingWith(lines, "Device loss breadcrumbs"), 1U) << "the recreation did not report the loss";
    EXPECT_GT(countStartingWith(lines, "  frame "), 0U) << "the trails were reset before the block named their frames";
    EXPECT_EQ(countStartingWith(lines, "  no frame was submitted"), 0U);

    // The next frame meets the same loss at its fence wait and must not report it again.
    app.frame();
    EXPECT_EQ(countStartingWith(report.lines(), "Device loss breadcrumbs"), 1U);

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
