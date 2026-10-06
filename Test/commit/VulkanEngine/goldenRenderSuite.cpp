// Structure only, never exact pixels; see docs/gpu-golden-testing.md § Writing a new golden test.

#include "EngineLoadWait.hpp"
#include "GoldenMetrics.hpp"
#include "common/host_device_shared_vars.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <iostream>
#include <vulkan/vulkan.hpp>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.gui_renderer_shared_vars;
import kataglyphis.vulkan.gui_scene_shared_vars;
import kataglyphis.vulkan.renderer;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.scene_config;
import kataglyphis.vulkan.window;

namespace {

using Kataglyphis::VulkanRendererInternals::FrontendShared::GpuTimedPass;
using Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars;
using Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode;
using Kataglyphis::Test::GoldenMetrics::Crop;
using Kataglyphis::Test::GoldenMetrics::card_crop;
using Kataglyphis::Test::GoldenMetrics::detail_fraction;
using Kataglyphis::Test::GoldenMetrics::luminance_of;
using Kataglyphis::Test::GoldenMetrics::mean_luminance_in_crop;
using Kataglyphis::Test::GoldenMetrics::panel_free_crop;
using Kataglyphis::Test::GoldenMetrics::swung_fraction;

constexpr int WINDOW_WIDTH = 1200;
constexpr int WINDOW_HEIGHT = 768;
// Lets the scene, GUI layout and per-image resources settle before the first capture.
constexpr int WARMUP_FRAMES = 5;
// Frames rendered after a settings change before capturing it.
constexpr int SETTLE_FRAMES = 3;

bool glfw_reports_vulkan_support()
{
    if (glfwInit() == 0) { return false; }
    const bool supports_vulkan = glfwVulkanSupported() != 0;
    glfwTerminate();
    return supports_vulkan;
}

// Pick a scene for what it measures: the debug skeleton's thin bones leave most PCF taps unoccluded.
class ScopedModelOverride
{
  public:
    explicit ScopedModelOverride(const char *relative_path) { set(relative_path); }
    ScopedModelOverride(const ScopedModelOverride &) = delete;
    ScopedModelOverride &operator=(const ScopedModelOverride &) = delete;
    ~ScopedModelOverride() { set(""); }

  private:
    static void set(const char *value)
    {
#ifdef _WIN32
        std::ignore = _putenv_s("KATAGLYPHIS_MODEL_OVERRIDE", value);
#else
        if (*value == '\0') {
            std::ignore = unsetenv("KATAGLYPHIS_MODEL_OVERRIDE");
        } else {
            std::ignore = setenv("KATAGLYPHIS_MODEL_OVERRIDE", value, 1);
        }
#endif
    }
};

// The goldens were calibrated on the Debug app's scene and framing, so the harness pins both, whatever the build type.
constexpr const char *GOLDEN_SCENE_MODEL = "Models/Dinosaurs/dinosaurs.obj";

void frame_golden_view(Camera &camera)
{
    camera.set_camera_position(glm::vec3(0.0F, 6.0F, 26.0F));
    camera.set_orientation(-90.0F, -10.0F);
    camera.set_near_plane(0.1F);
    camera.set_far_plane(150.0F);
    camera.set_fov(45.0F);
}

// Tears the engine down in the same order as the other integration suites.
struct EngineHarness
{
    // Only when the test picked no scene of its own; ScopedModelOverride clears the variable on exit.
    std::optional<ScopedModelOverride> golden_scene;
    std::unique_ptr<Kataglyphis::Frontend::Window> window;
    std::unique_ptr<Kataglyphis::Scene> scene;
    std::unique_ptr<Kataglyphis::Frontend::GUI> gui;
    std::unique_ptr<Camera> camera;
    std::unique_ptr<Kataglyphis::VulkanRenderer> renderer;

    EngineHarness()
      : window(std::make_unique<Kataglyphis::Frontend::Window>(WINDOW_WIDTH, WINDOW_HEIGHT)),
        scene(std::make_unique<Kataglyphis::Scene>()), gui(std::make_unique<Kataglyphis::Frontend::GUI>(window.get())),
        camera(std::make_unique<Camera>())
    {
        if (const char *chosen = std::getenv("KATAGLYPHIS_MODEL_OVERRIDE"); chosen == nullptr || *chosen == '\0') {
            golden_scene.emplace(GOLDEN_SCENE_MODEL);
        }
        frame_golden_view(*camera);
        renderer = std::make_unique<Kataglyphis::VulkanRenderer>(window.get(), scene.get(), gui.get(), camera.get());
    }

    EngineHarness(const EngineHarness &) = delete;
    EngineHarness &operator=(const EngineHarness &) = delete;

    // One iteration of the App::run frame loop (App.cpp) minus camera input.
    void render_frame()
    {
        glfwPollEvents();
        gui->render();
        auto &guiSceneSharedVars = gui->getGuiSceneSharedVars();
        renderer->updateStateDueToUserInput(guiSceneSharedVars);
        renderer->updateUniforms(scene.get(), camera.get(), guiSceneSharedVars);
        renderer->drawFrame(guiSceneSharedVars);
    }

    /// Pumps frames until the asynchronously parsed model is in the scene.
    void wait_for_model()
    {
        Kataglyphis::TestSupport::waitForModelLoad(renderer.get(), [this] { render_frame(); });
    }

    void render_frames(int count)
    {
        wait_for_model();
        for (int frame = 0; frame < count; ++frame) { render_frame(); }
    }

    // takeCapturedFrame fence-syncs the readback.
    std::vector<uint8_t> capture_frame(uint32_t &width, uint32_t &height)
    {
        renderer->requestFrameCapture();
        render_frame();
        return renderer->takeCapturedFrame(width, height);
    }

    [[nodiscard]] bool supportsFrameCapture() const { return renderer->supportsFrameCapture(); }

    GUIRendererSharedVars &useForwardRaster()
    {
        auto &renderer_vars = gui->getGuiRendererSharedVars();
        renderer_vars.raytracing = false;
        renderer_vars.pathTracing = false;
        renderer_vars.rasterizationMode = RasterizationMode::Forward;
        return renderer_vars;
    }

    ~EngineHarness()
    {
        if (renderer && !renderer->hasDeviceLost()) {
            renderer->finishAllRenderCommands();
            scene->cleanUp();
            gui->cleanUp();
        }
        if (renderer) {
            renderer->cleanUp();
            renderer.reset();
        }
        camera.reset();
        gui.reset();
        scene.reset();
        if (window) {
            window->cleanUp();
            window.reset();
        }
    }
};

// Mean luminance over the whole frame, on a 0..255 scale.
double mean_luminance(const std::vector<uint8_t> &rgba)
{
    const size_t pixel_count = rgba.size() / 4U;
    if (pixel_count == 0U) { return 0.0; }

    double sum = 0.0;
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) { sum += luminance_of(rgba, pixel); }
    return sum / static_cast<double>(pixel_count);
}

double luminance_stddev(const std::vector<uint8_t> &rgba)
{
    const size_t pixel_count = rgba.size() / 4U;
    if (pixel_count < 2U) { return 0.0; }

    const double mean = mean_luminance(rgba);
    double accumulated = 0.0;
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
        const double delta = luminance_of(rgba, pixel) - mean;
        accumulated += delta * delta;
    }
    return std::sqrt(accumulated / static_cast<double>(pixel_count));
}

double fraction_above(const std::vector<uint8_t> &rgba, double threshold)
{
    const size_t pixel_count = rgba.size() / 4U;
    if (pixel_count == 0U) { return 0.0; }

    size_t hits = 0;
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
        if (luminance_of(rgba, pixel) > threshold) { ++hits; }
    }
    return static_cast<double>(hits) / static_cast<double>(pixel_count);
}

// A blank or single-colour frame collapses to a handful of buckets.
size_t distinct_luminance_buckets(const std::vector<uint8_t> &rgba)
{
    std::vector<bool> seen(256, false);
    const size_t pixel_count = rgba.size() / 4U;
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
        const int bucket = std::clamp(static_cast<int>(luminance_of(rgba, pixel)), 0, 255);
        seen[static_cast<size_t>(bucket)] = true;
    }
    return static_cast<size_t>(std::count(seen.begin(), seen.end(), true));
}

// A solid box floating over a ground plane, built for shadow measurement.
constexpr const char *SHADOW_RIG_MODEL = "Models/ShadowTest/shadow_rig.obj";
// One mesh with two primitives, which must load as two meshes.
constexpr const char *TWO_PRIMITIVE_MODEL = "Models/GltfTest/two_primitives.gltf";
constexpr const char *MASK_CARD_MODEL = "Models/GltfTest/mask_card.gltf";
// Image 0 is valid base64 but not a decodable image; image 1 is a valid PNG.
constexpr const char *CORRUPT_EMBEDDED_IMAGE_MODEL = "Models/GltfTest/corrupt_embedded_image.gltf";
constexpr const char *UV_TRANSFORM_MODEL = "Models/GltfTest/uv_transform_card.gltf";
// Near-black base colour, emissiveFactor (1,1,1) - glows regardless of light.
constexpr const char *EMISSIVE_CARD_MODEL = "Models/GltfTest/emissive_card.gltf";
// emissive_strength 4.0 overflows an 8-bit UNORM G-buffer channel.
constexpr const char *EMISSIVE_STRENGTH_CARD_MODEL = "Models/GltfTest/emissive_strength_card.gltf";
// The lit-but-not-glowing control for EMISSIVE_CARD_MODEL.
constexpr const char *METALLIC_CARD_MODEL = "Models/GltfTest/metallic_card.gltf";

// Validation errors reach spdlog::error, the only way to see one a driver tolerates without changing pixels.
class ScopedValidationErrorCounter
{
  public:
    ScopedValidationErrorCounter()
      : sink_(std::make_shared<spdlog::sinks::callback_sink_mt>([this](const spdlog::details::log_msg &msg) {
            if (msg.level >= spdlog::level::err) { ++error_count_; }
        }))
    { spdlog::default_logger()->sinks().push_back(sink_); }
    ScopedValidationErrorCounter(const ScopedValidationErrorCounter &) = delete;
    ScopedValidationErrorCounter &operator=(const ScopedValidationErrorCounter &) = delete;
    ~ScopedValidationErrorCounter()
    {
        auto &sinks = spdlog::default_logger()->sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), sink_), sinks.end());
    }

    int errorCount() const { return error_count_.load(); }

  private:
    std::shared_ptr<spdlog::sinks::callback_sink_mt> sink_;
    std::atomic<int> error_count_{ 0 };
};

// The renderer reads KATAGLYPHIS_GPU_TIMING_JSON in cleanUp, so it must outlive the harness.
class ScopedGpuTimingJsonPath
{
  public:
    explicit ScopedGpuTimingJsonPath(const std::string &path) { set(path.c_str()); }
    ScopedGpuTimingJsonPath(const ScopedGpuTimingJsonPath &) = delete;
    ScopedGpuTimingJsonPath &operator=(const ScopedGpuTimingJsonPath &) = delete;
    ~ScopedGpuTimingJsonPath() { set(""); }

  private:
    static void set(const char *value)
    {
#ifdef _WIN32
        std::ignore = _putenv_s("KATAGLYPHIS_GPU_TIMING_JSON", value);
#else
        if (*value == '\0') {
            std::ignore = unsetenv("KATAGLYPHIS_GPU_TIMING_JSON");
        } else {
            std::ignore = setenv("KATAGLYPHIS_GPU_TIMING_JSON", value, 1);
        }
#endif
    }
};

#define SKIP_WITHOUT_GPU()                                                                                            \
    do {                                                                                                              \
        if (!glfw_reports_vulkan_support()) { GTEST_SKIP() << "GLFW/Vulkan runtime is unavailable on this system."; } \
    } while (false)

// Vulkan support does not imply a surface that allows copying out of a swapchain image.
#define SKIP_WITHOUT_FRAME_CAPTURE(harness)                                                      \
    do {                                                                                         \
        if (!(harness).supportsFrameCapture()) {                                                 \
            GTEST_SKIP() << "Surface does not support eTransferSrc; frame capture unavailable."; \
        }                                                                                        \
    } while (false)

}// namespace

// Cheapest guard against the whole render graph silently producing nothing.
TEST(GoldenRender, RendersNonBlankFrame)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.gui->getGuiSceneSharedVars();
    harness.useForwardRaster();
    scene_vars.shadows_enabled = true;

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);

    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
    ASSERT_FALSE(frame.empty()) << "Frame capture returned no pixels.";
    EXPECT_GT(width, 0U);
    EXPECT_GT(height, 0U);
    ASSERT_EQ(frame.size(), static_cast<size_t>(width) * static_cast<size_t>(height) * 4U)
      << "Captured buffer size does not match the reported extent.";

    const double stddev = luminance_stddev(frame);
    const double lit_fraction = fraction_above(frame, 8.0);
    const size_t buckets = distinct_luminance_buckets(frame);

    EXPECT_GT(stddev, 2.0) << "Frame is essentially uniform (luminance stddev " << stddev
                           << "); the render graph likely produced a blank image.";
    EXPECT_GT(lit_fraction, 0.05) << "Only " << (lit_fraction * 100.0)
                                  << "% of pixels are non-black; the frame looks empty.";
    EXPECT_GT(buckets, 8U) << "Only " << buckets
                           << " distinct luminance levels present; the frame looks like a flat fill.";
}

// Disabled: writes PNGs and asserts nothing; see docs/gpu-golden-testing.md § Writing a new golden test.
TEST(GoldenRender, DISABLED_DumpsFrameToPng)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    harness.gui->getGuiSceneSharedVars().shadows_enabled = true;
    // Must match ShadowsDarkenSomePixels' configuration, or the two cannot be compared.
    harness.gui->getGuiSceneSharedVars().cascaded_shadow_intensity = 1.0F;

    harness.render_frames(WARMUP_FRAMES);
    harness.render_frames(SETTLE_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(frame.empty()) << "Capture returned no pixels.";

    const char *out_path = std::getenv("KATAGLYPHIS_FRAME_DUMP");
    const std::string base = (out_path != nullptr) ? out_path : "frame-dump";
    const auto write = [&](const std::string &suffix, const std::vector<uint8_t> &pixels) {
        const std::string path = base + suffix + ".png";
        ASSERT_NE(stbi_write_png(path.c_str(),
                    static_cast<int>(width),
                    static_cast<int>(height),
                    4,
                    pixels.data(),
                    static_cast<int>(width) * 4),
          0)
          << "Failed to write " << path;
        std::cout << "[  INFO ] wrote " << path << " (" << width << "x" << height << ")\n";
    };

    write("-shadows-on", frame);

    // ShadowsDarkenSomePixels' exact sequence, with the PNG of the very bytes each mean was taken over.
    auto &scene_vars = harness.gui->getGuiSceneSharedVars();

    scene_vars.cascaded_shadow_intensity = 0.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> golden_order_off = harness.capture_frame(width, height);

    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> golden_order_noise = harness.capture_frame(width, height);

    scene_vars.cascaded_shadow_intensity = 1.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> golden_order_on = harness.capture_frame(width, height);

    ASSERT_EQ(golden_order_off.size(), golden_order_on.size());
    GTEST_LOG_(INFO) << "golden-order means: intensity 0.0 = " << mean_luminance(golden_order_off)
                     << ", noise reference (same 0.0 state) = " << mean_luminance(golden_order_noise)
                     << ", intensity 1.0 = " << mean_luminance(golden_order_on);

    write("-golden-order-off", golden_order_off);
    write("-golden-order-on", golden_order_on);

    // One PCF tap makes the term binary, separating a lacy occluder's weak shadow from a defect.
    const int restore_pcf = scene_vars.pcf_radius;
    scene_vars.pcf_radius = 0;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> single_tap_on = harness.capture_frame(width, height);
    scene_vars.cascaded_shadow_intensity = 0.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> single_tap_off = harness.capture_frame(width, height);
    scene_vars.pcf_radius = restore_pcf;
    scene_vars.cascaded_shadow_intensity = 1.0F;

    GTEST_LOG_(INFO) << "single-tap PCF means: intensity 1.0 = " << mean_luminance(single_tap_on)
                     << ", intensity 0.0 = " << mean_luminance(single_tap_off);
    write("-singletap-on", single_tap_on);

    std::vector<uint8_t> single_tap_delta(single_tap_on.size(), 255);
    for (size_t i = 0; i + 3 < single_tap_on.size(); i += 4) {
        for (size_t channel = 0; channel < 3; ++channel) {
            const int d = static_cast<int>(single_tap_off[i + channel]) - static_cast<int>(single_tap_on[i + channel]);
            single_tap_delta[i + channel] = static_cast<uint8_t>(255 - std::clamp(d * 4, 0, 255));
        }
    }
    write("-singletap-delta", single_tap_delta);

    // A settle far past SETTLE_FRAMES rules out slow GUI-to-UBO propagation.
    scene_vars.cascaded_shadow_intensity = 0.0F;
    harness.render_frames(20);
    uint32_t off_width = 0;
    uint32_t off_height = 0;
    const std::vector<uint8_t> unshadowed = harness.capture_frame(off_width, off_height);
    ASSERT_EQ(unshadowed.size(), frame.size());

    GTEST_LOG_(INFO) << "dump-order means: intensity 1.0 (first capture) = " << mean_luminance(frame)
                     << ", intensity 0.0 (after 20 frames) = " << mean_luminance(unshadowed);

    write("-shadows-off", unshadowed);

    std::vector<uint8_t> difference(frame.size(), 255);
    for (size_t i = 0; i + 3 < frame.size(); i += 4) {
        for (size_t channel = 0; channel < 3; ++channel) {
            const int delta = static_cast<int>(unshadowed[i + channel]) - static_cast<int>(frame[i + channel]);
            // Inverted 32x gain, because this scene's shadow is genuinely faint.
            difference[i + channel] = static_cast<uint8_t>(255 - std::clamp(delta * 32, 0, 255));
        }
    }
    write("-shadow-delta", difference);

    // Delta of the golden-order pair too, at the same gain.
    std::vector<uint8_t> golden_delta(golden_order_on.size(), 255);
    for (size_t i = 0; i + 3 < golden_order_on.size(); i += 4) {
        for (size_t channel = 0; channel < 3; ++channel) {
            const int d =
              static_cast<int>(golden_order_off[i + channel]) - static_cast<int>(golden_order_on[i + channel]);
            golden_delta[i + channel] = static_cast<uint8_t>(255 - std::clamp(d * 32, 0, 255));
        }
    }
    write("-golden-order-delta", golden_delta);
}

// Disabled: measures what each PCF radius changes on the rig, behind MAX_PCF_RADIUS (docs/gpu-golden-testing.md).
TEST(GoldenRender, DISABLED_PcfRadiusSweepMeasuresTheShadowRig)
{
    SKIP_WITHOUT_GPU();

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.gui->getGuiSceneSharedVars();
    const auto &timings = harness.gui->getGuiRendererSharedVars().gpuTimings;
    harness.useForwardRaster();
    scene_vars.shadows_enabled = true;
    harness.render_frames(WARMUP_FRAMES);

    // Past GpuPassAverage's 30-frame window, so each radius's pass times are its own.
    constexpr int TIMED_FRAMES = 35;
    using Frame = std::vector<uint8_t>;
    struct Sample
    {
        int radius;
        Frame on;
        Frame off;
        double main_ms;
        double shadow_ms;
        double wall_ms;
    };
    std::vector<Sample> samples;
    uint32_t width = 0;
    uint32_t height = 0;
    for (int radius = 0; radius <= MAX_PCF_RADIUS; ++radius) {
        scene_vars.pcf_radius = radius;
        scene_vars.cascaded_shadow_intensity = 1.0F;
        const auto start = std::chrono::steady_clock::now();
        harness.render_frames(TIMED_FRAMES);
        const double wall_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / TIMED_FRAMES;
        const double main_ms = timings.pass_ms[static_cast<size_t>(GpuTimedPass::Main)];
        const double shadow_ms = timings.pass_ms[static_cast<size_t>(GpuTimedPass::ShadowCascades)];
        std::vector<uint8_t> on = harness.capture_frame(width, height);
        // The off frame holds this radius's GUI text, so overlay pixels that move with the slider can be masked.
        scene_vars.cascaded_shadow_intensity = 0.0F;
        harness.render_frames(SETTLE_FRAMES);
        std::vector<uint8_t> off = harness.capture_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost at radius " << radius;
        samples.push_back({ radius, std::move(on), std::move(off), main_ms, shadow_ms, wall_ms });
    }
    ASSERT_GE(samples.size(), 2U);

    // Big kernels darken lit receivers too (one reference depth per tap), so penumbra and lit area are measured apart.
    const size_t pixel_count = static_cast<size_t>(width) * height;
    const std::vector<uint8_t> &unshadowed = samples.front().off;
    const std::vector<uint8_t> &hard = samples.front().on;
    std::vector<int> shadowed_prefix((static_cast<size_t>(width) + 1U) * (height + 1U), 0);
    std::vector<bool> noisy(pixel_count, false);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t pixel = static_cast<size_t>(y) * width + x;
            for (const Sample &sample : samples) {
                noisy[pixel] =
                  noisy[pixel] || std::abs(luminance_of(sample.off, pixel) - luminance_of(unshadowed, pixel)) >= 1.0;
            }
            const int shadowed = luminance_of(unshadowed, pixel) - luminance_of(hard, pixel) >= 2.0 ? 1 : 0;
            const size_t at = (static_cast<size_t>(y) + 1U) * (width + 1U) + x + 1U;
            shadowed_prefix[at] = shadowed + shadowed_prefix[at - 1U] + shadowed_prefix[at - (width + 1U)]
                                  - shadowed_prefix[at - (width + 1U) - 1U];
        }
    }
    // Near: within PENUMBRA_PX of the smallest radius's shadow, where softening shows; far: lit pixels beyond it.
    constexpr uint32_t PENUMBRA_PX = 16U;
    const auto shadow_within = [&](uint32_t x, uint32_t y) {
        const size_t x0 = x > PENUMBRA_PX ? x - PENUMBRA_PX : 0U;
        const size_t y0 = y > PENUMBRA_PX ? y - PENUMBRA_PX : 0U;
        const size_t x1 = std::min<size_t>(width, x + PENUMBRA_PX + 1U);
        const size_t y1 = std::min<size_t>(height, y + PENUMBRA_PX + 1U);
        const auto row = [&](size_t yy) { return yy * (width + 1U); };
        return shadowed_prefix[row(y1) + x1] - shadowed_prefix[row(y0) + x1] - shadowed_prefix[row(y1) + x0]
                 + shadowed_prefix[row(y0) + x0]
               > 0;
    };
    std::vector<int> region(pixel_count, 0);
    size_t near_pixels = 0;
    size_t far_pixels = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t pixel = static_cast<size_t>(y) * width + x;
            if (noisy[pixel]) { continue; }
            region[pixel] = shadow_within(x, y) ? 1 : 2;
            ++(region[pixel] == 1 ? near_pixels : far_pixels);
        }
    }
    ASSERT_GT(near_pixels, 0U) << "No shadow at the smallest radius; the sweep measured nothing.";
    ASSERT_GT(far_pixels, 0U);

    // Mean change and share moving >= 3 levels (0..255 luma); signed_delta: how much darker a is than b.
    const auto compare = [&](const Frame &a, const Frame &b, int which, bool signed_delta) {
        double sum = 0.0;
        size_t moved = 0;
        size_t count = 0;
        for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
            if (region[pixel] != which) { continue; }
            const double raw = luminance_of(b, pixel) - luminance_of(a, pixel);
            const double delta = signed_delta ? raw : std::abs(raw);
            sum += delta;
            if (delta >= 3.0) { ++moved; }
            ++count;
        }
        return std::pair{ sum / static_cast<double>(count), static_cast<double>(moved) / static_cast<double>(count) };
    };

    std::cout << "[  INFO ] PCF sweep on " << SHADOW_RIG_MODEL << ", " << width << "x" << height << "; near "
              << near_pixels << " px, far-lit " << far_pixels << " px\n"
              << "[  INFO ] radius taps | near vs prev: MAD >=3lvl | far-lit darkening: mean >=3lvl | main_ms "
                 "shadow_ms wall_ms\n";
    for (size_t i = 0; i < samples.size(); ++i) {
        const Sample &sample = samples[i];
        const auto [near_mad, near_moved] =
          i == 0 ? std::pair{ 0.0, 0.0 } : compare(sample.on, samples[i - 1].on, 1, false);
        const auto [far_dark, far_moved] = compare(sample.on, unshadowed, 2, true);
        const int taps = (2 * sample.radius + 1) * (2 * sample.radius + 1);
        std::cout << "[  INFO ] " << sample.radius << " " << taps << " | " << near_mad << " " << near_moved << " | "
                  << far_dark << " " << far_moved << " | " << sample.main_ms << " " << sample.shadow_ms << " "
                  << sample.wall_ms << "\n";
    }

    if (const char *out_path = std::getenv("KATAGLYPHIS_FRAME_DUMP"); out_path != nullptr) {
        for (const Sample &sample : samples) {
            const std::string path = std::string(out_path) + "-pcf" + std::to_string(sample.radius) + ".png";
            EXPECT_NE(stbi_write_png(path.c_str(),
                        static_cast<int>(width),
                        static_cast<int>(height),
                        4,
                        sample.on.data(),
                        static_cast<int>(width) * 4),
              0)
              << "Failed to write " << path;
        }
    }
}

// Guards against a shadow map nothing samples; the rig, not the debug scene, is what makes it assertable.
TEST(GoldenRender, ShadowsDarkenSomePixels)
{
    SKIP_WITHOUT_GPU();

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.gui->getGuiSceneSharedVars();
    harness.useForwardRaster();

    // Only intensity varies, isolating "the shadow map is sampled" from "the shadow pass runs".
    scene_vars.shadows_enabled = true;

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    uint32_t width = 0;
    uint32_t height = 0;

    scene_vars.cascaded_shadow_intensity = 0.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> without_shadows = harness.capture_frame(width, height);

    // The overlay's FPS readout flips hundreds of pixels, so mask out whatever moves between identical states.
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> noise_reference = harness.capture_frame(width, height);
    ASSERT_FALSE(without_shadows.empty()) << "Capture without shadows returned no pixels.";

    scene_vars.cascaded_shadow_intensity = 1.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> with_shadows = harness.capture_frame(width, height);
    ASSERT_FALSE(with_shadows.empty()) << "Capture with shadows returned no pixels.";

    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
    ASSERT_EQ(without_shadows.size(), with_shadows.size());

    // Skybox and overlay swamp a whole-frame mean; per pixel, shadowing can only darken.
    constexpr double CHANGE_THRESHOLD = 4.0;

    const size_t pixel_count = with_shadows.size() / 4U;
    size_t darkened = 0;
    size_t brightened = 0;
    size_t unstable = 0;
    double shadowed_region_sum_with = 0.0;
    double shadowed_region_sum_without = 0.0;

    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
        // Moved between identical captures: overlay or dither, not shadow.
        if (std::abs(luminance_of(without_shadows, pixel) - luminance_of(noise_reference, pixel)) > CHANGE_THRESHOLD) {
            ++unstable;
            continue;
        }

        const double lum_without = luminance_of(without_shadows, pixel);
        const double lum_with = luminance_of(with_shadows, pixel);
        const double delta = lum_without - lum_with;

        if (delta > CHANGE_THRESHOLD) {
            ++darkened;
            shadowed_region_sum_with += lum_with;
            shadowed_region_sum_without += lum_without;
        } else if (delta < -CHANGE_THRESHOLD) {
            ++brightened;
        }
    }

    const double darkened_fraction =
      pixel_count == 0U ? 0.0 : static_cast<double>(darkened) / static_cast<double>(pixel_count);

    GTEST_LOG_(INFO) << "whole-frame mean luminance: intensity 0.0 = " << mean_luminance(without_shadows)
                     << ", intensity 1.0 = " << mean_luminance(with_shadows) << "; pixels darkened = " << darkened
                     << " (" << (darkened_fraction * 100.0) << "%), brightened = " << brightened
                     << ", unstable/overlay skipped = " << unstable << " of " << pixel_count;

    // 1. A shadowed region must exist at all.
    ASSERT_GT(darkened, 0U) << "Not a single pixel changed when the shadow intensity went from 0.0 to 1.0. "
                               "The cascaded shadow map is rendered but its result never reaches the lighting "
                               "shaders.";
    // Measured, not tuned: shadow-pass culling only halves the signal, so never lower this to pass.
    EXPECT_GT(darkened_fraction, 0.04)
      << "Only " << (darkened_fraction * 100.0)
      << "% of pixels were meaningfully darkened by shadows; expected a visible shadowed region.";

    // 2. Over that region the mean luminance with shadows must be lower.
    const double mean_shadowed = shadowed_region_sum_with / static_cast<double>(darkened);
    const double mean_unshadowed = shadowed_region_sum_without / static_cast<double>(darkened);
    EXPECT_LT(mean_shadowed, mean_unshadowed)
      << "Shadows did not darken the shadowed region: mean luminance " << mean_shadowed
      << " at shadow intensity 1.0 vs " << mean_unshadowed << " at intensity 0.0.";

    // 3. Shadowing only attenuates, so brightened pixels are overlay noise and must stay a minority.
    EXPECT_GT(darkened, brightened * 5U) << darkened << " pixels darkened but " << brightened
                                         << " brightened - raising the shadow intensity must not brighten the image.";
}

// A counter assertion, not a pixel oracle, so no classifier or tonemap hazard.
TEST(GoldenRender, ShadowCasterStatsAreReportedAndZeroWhenShadowsAreOff)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    auto &scene_vars = harness.gui->getGuiSceneSharedVars();
    auto &renderer_vars = harness.useForwardRaster();

    scene_vars.shadows_enabled = true;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    ASSERT_GT(renderer_vars.visibility.shadow_casters_total, 0U)
      << "the renderer reported considering no shadow casters at all; "
         "the shadow visibility counters are not being written";
    EXPECT_LE(renderer_vars.visibility.shadow_casters_drawn, renderer_vars.visibility.shadow_casters_total);

    scene_vars.shadows_enabled = false;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    EXPECT_EQ(renderer_vars.visibility.shadow_casters_total, 0U)
      << "shadow caster counters must be zeroed when the shadow pass does not run";
    EXPECT_EQ(renderer_vars.visibility.shadow_casters_drawn, 0U)
      << "shadow caster counters must be zeroed when the shadow pass does not run";
}

// The raster pass samples the cascade array with shadows off too; a pass that never ran left it UNDEFINED.
TEST(GoldenRender, ShadowsOffFromTheFirstFrameLeaveTheCascadeArraySampleable)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    harness.useForwardRaster();
    harness.gui->getGuiSceneSharedVars().shadows_enabled = false;
    // After the harness: the arm64 runner's PowerVR ICD logs loader errors while devices are enumerated.
    ScopedValidationErrorCounter validation_errors;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost with shadows off.";

    EXPECT_EQ(validation_errors.errorCount(), 0)
      << "a frame with the shadow pass skipped logged a validation error - VUID-vkCmdDraw-None-09600 "
         "if the cascade depth array was sampled before anything moved it out of UNDEFINED";
}

// The default scene's casters all sit inside every cascade, so only a far-off caster makes the flag observable.
TEST(GoldenRender, DisablingFrustumCullingAlsoDisablesShadowCasterCulling)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    auto &scene_vars = harness.gui->getGuiSceneSharedVars();
    auto &renderer_vars = harness.useForwardRaster();

    scene_vars.shadows_enabled = true;

    const glm::mat4 far_placement = glm::translate(glm::mat4(1.0F), glm::vec3(5000.0F, 0.0F, 0.0F));
    const std::optional<uint32_t> far_model = harness.renderer->addModel(SHADOW_RIG_MODEL, far_placement);
    ASSERT_TRUE(far_model.has_value()) << "the far-away caster model failed to load";

    renderer_vars.frustum_culling_enabled = false;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    ASSERT_GT(renderer_vars.visibility.shadow_casters_total, 0U)
      << "the renderer reported considering no shadow casters at all; "
         "the shadow visibility counters are not being written";
    EXPECT_EQ(renderer_vars.visibility.shadow_casters_drawn, renderer_vars.visibility.shadow_casters_total)
      << "with frustum culling disabled, every shadow caster must be drawn regardless of the cascade frusta";

    renderer_vars.frustum_culling_enabled = true;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    EXPECT_LE(renderer_vars.visibility.shadow_casters_drawn, renderer_vars.visibility.shadow_casters_total);
}

// A shadow that darkens but never moves passes ShadowsDarkenSomePixels and fails here.
TEST(GoldenRender, ShadowsMoveWhenTheLightRotates)
{
    SKIP_WITHOUT_GPU();

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.gui->getGuiSceneSharedVars();
    harness.useForwardRaster();
    scene_vars.shadows_enabled = true;

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    // -51 degrees about Y keeps both shadows on the ground; the band mostly slides in depth, so the masks overlap.
    const glm::vec3 direction_a(scene_vars.directional_light_direction[0],
      scene_vars.directional_light_direction[1],
      scene_vars.directional_light_direction[2]);
    const float rotation_radians = glm::radians(-51.0F);
    const glm::vec3 direction_b(
      glm::rotate(glm::mat4(1.0F), rotation_radians, glm::vec3(0.0F, 1.0F, 0.0F)) * glm::vec4(direction_a, 0.0F));

    const auto set_direction = [&scene_vars](const glm::vec3 &direction) {
        scene_vars.directional_light_direction[0] = direction.x;
        scene_vars.directional_light_direction[1] = direction.y;
        scene_vars.directional_light_direction[2] = direction.z;
    };

    uint32_t width = 0;
    uint32_t height = 0;

    // Direction A: unshadowed, a same-state noise reference, then shadowed.
    set_direction(direction_a);
    scene_vars.cascaded_shadow_intensity = 0.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> a_unshadowed = harness.capture_frame(width, height);
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> a_noise_reference = harness.capture_frame(width, height);
    scene_vars.cascaded_shadow_intensity = 1.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> a_shadowed = harness.capture_frame(width, height);

    // Direction B: same three-capture pattern.
    set_direction(direction_b);
    scene_vars.cascaded_shadow_intensity = 0.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> b_unshadowed = harness.capture_frame(width, height);
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> b_noise_reference = harness.capture_frame(width, height);
    scene_vars.cascaded_shadow_intensity = 1.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> b_shadowed = harness.capture_frame(width, height);

    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
    ASSERT_FALSE(a_unshadowed.empty()) << "Capture under direction A returned no pixels.";
    ASSERT_EQ(a_unshadowed.size(), a_shadowed.size());
    ASSERT_EQ(a_unshadowed.size(), b_unshadowed.size());
    ASSERT_EQ(a_unshadowed.size(), b_shadowed.size());

    constexpr double CHANGE_THRESHOLD = 4.0;
    const size_t pixel_count = a_unshadowed.size() / 4U;

    // ShadowsDarkenSomePixels' mask, for one light direction.
    const auto build_mask = [pixel_count](const std::vector<uint8_t> &unshadowed,
                              const std::vector<uint8_t> &noise_reference,
                              const std::vector<uint8_t> &shadowed) {
        std::vector<bool> mask(pixel_count, false);
        size_t darkened = 0;
        for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
            if (std::abs(luminance_of(unshadowed, pixel) - luminance_of(noise_reference, pixel)) > CHANGE_THRESHOLD) {
                continue;
            }
            if (luminance_of(unshadowed, pixel) - luminance_of(shadowed, pixel) > CHANGE_THRESHOLD) {
                mask[pixel] = true;
                ++darkened;
            }
        }
        return std::make_pair(mask, darkened);
    };

    const auto [mask_a, darkened_a] = build_mask(a_unshadowed, a_noise_reference, a_shadowed);
    const auto [mask_b, darkened_b] = build_mask(b_unshadowed, b_noise_reference, b_shadowed);

    const double area_a = static_cast<double>(darkened_a) / static_cast<double>(pixel_count);
    const double area_b = static_cast<double>(darkened_b) / static_cast<double>(pixel_count);

    // Two empty masks would otherwise pass by trivially agreeing.
    constexpr double AREA_FLOOR = 0.04;
    EXPECT_GT(area_a, AREA_FLOOR) << "Direction A cast a shadow over only " << (area_a * 100.0)
                                  << "% of pixels; expected a visible shadowed region.";
    EXPECT_GT(area_b, AREA_FLOOR) << "Direction B cast a shadow over only " << (area_b * 100.0)
                                  << "% of pixels; expected a visible shadowed region.";

    size_t intersection = 0;
    size_t mask_union = 0;
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
        if (mask_a[pixel] || mask_b[pixel]) {
            ++mask_union;
            if (mask_a[pixel] && mask_b[pixel]) { ++intersection; }
        }
    }
    const size_t symmetric_difference = mask_union - intersection;
    const double moved_fraction =
      mask_union == 0U ? 0.0 : static_cast<double>(symmetric_difference) / static_cast<double>(mask_union);

    GTEST_LOG_(INFO) << "direction A shadowed " << darkened_a << " px (" << (area_a * 100.0)
                     << "%), direction B shadowed " << darkened_b << " px (" << (area_b * 100.0) << "%); mask union "
                     << mask_union << ", symmetric difference " << symmetric_difference << " ("
                     << (moved_fraction * 100.0) << "% of union)";

    // 2026-10-01: RTX 2080 0.370, llvmpipe 0.339; 0.011 with the shadow pass pinned to direction A.
    constexpr double MOVED_FRACTION_THRESHOLD = 0.15;
    EXPECT_GT(moved_fraction, MOVED_FRACTION_THRESHOLD)
      << "Only " << (moved_fraction * 100.0)
      << "% of the shadowed-pixel union differs between the two light directions; the shadow does not appear to "
         "move.";
}

// Forward and deferred never match exactly, but a broken path diverges far beyond this.
TEST(GoldenRender, DeferredMatchesForwardRoughly)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.gui->getGuiSceneSharedVars();
    auto &renderer_vars = harness.gui->getGuiRendererSharedVars();
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = false;
    scene_vars.shadows_enabled = true;

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    uint32_t width = 0;
    uint32_t height = 0;

    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> forward = harness.capture_frame(width, height);
    ASSERT_FALSE(forward.empty()) << "Forward capture returned no pixels.";

    renderer_vars.rasterizationMode = RasterizationMode::Deferred;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> deferred = harness.capture_frame(width, height);
    ASSERT_FALSE(deferred.empty()) << "Deferred capture returned no pixels.";

    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
    ASSERT_EQ(forward.size(), deferred.size());

    const double mean_forward = mean_luminance(forward);
    const double mean_deferred = mean_luminance(deferred);

    // Both paths must produce a non-degenerate image in the first place.
    EXPECT_GT(luminance_stddev(forward), 2.0) << "Forward frame is uniform.";
    EXPECT_GT(luminance_stddev(deferred), 2.0) << "Deferred frame is uniform.";

    // Generous on purpose: catches a broken path, not slightly different shading.
    constexpr double LUMINANCE_TOLERANCE = 60.0;
    EXPECT_NEAR(mean_deferred, mean_forward, LUMINANCE_TOLERANCE)
      << "Forward and deferred disagree far more than expected (forward " << mean_forward << ", deferred "
      << mean_deferred << "); one of the two lighting paths is likely broken.";

    // Mean luminance cannot tell forward-vs-forward from parity; absolute channel means expose a probe shader.
    const auto channel_means = [](const std::vector<uint8_t> &px, const char *label) {
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        const size_t n = px.size() / 4;
        for (size_t i = 0; i < px.size(); i += 4) {
            r += px[i];
            g += px[i + 1];
            b += px[i + 2];
        }
        GTEST_LOG_(INFO) << label << " channel means R " << r / n << " G " << g / n << " B " << b / n;
    };
    channel_means(forward, "forward");
    channel_means(deferred, "deferred");

    // Per-pixel difference separates "shade alike" from "means happen to agree"; the limit is measured.
    double abs_diff_sum = 0.0;
    for (size_t i = 0; i < forward.size(); ++i) {
        abs_diff_sum += std::abs(static_cast<int>(forward[i]) - static_cast<int>(deferred[i]));
    }
    const double mean_abs_diff = abs_diff_sum / static_cast<double>(forward.size());
    GTEST_LOG_(INFO) << "deferred-vs-forward mean abs channel diff: " << mean_abs_diff;
    constexpr double MEAN_ABS_DIFF_LIMIT = 1.0;
    EXPECT_LT(mean_abs_diff, MEAN_ABS_DIFF_LIMIT)
      << "Deferred diverges from forward per-pixel; the paths no longer shade alike.";
}

// The pixels shadows darken by more than 15 luma levels; acne alone stays under that.
static std::vector<bool> shadow_mask(const std::vector<uint8_t> &unshadowed, const std::vector<uint8_t> &shadowed)
{
    std::vector<bool> mask(unshadowed.size() / 4U);
    for (size_t pixel = 0; pixel < mask.size(); ++pixel) {
        mask[pixel] = luminance_of(unshadowed, pixel) - luminance_of(shadowed, pixel) > 15.0;
    }
    return mask;
}

static double mask_iou(const std::vector<bool> &a, const std::vector<bool> &b, uint32_t w, uint32_t h, bool flip_b)
{
    size_t both = 0;
    size_t either = 0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const bool in_a = a[static_cast<size_t>(y) * w + x];
            const bool in_b = b[static_cast<size_t>(flip_b ? h - 1U - y : y) * w + x];
            both += (in_a && in_b) ? 1U : 0U;
            either += (in_a || in_b) ? 1U : 0U;
        }
    }
    return either > 0U ? static_cast<double>(both) / static_cast<double>(either) : 0.0;
}

// Deferred rebuilds world positions from the fullscreen uv; a mirrored rebuild puts its shadows in the other half.
TEST(GoldenRender, DeferredShadowsLandWhereForwardShadowsLand)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.gui->getGuiSceneSharedVars();
    auto &renderer_vars = harness.useForwardRaster();
    scene_vars.shadows_enabled = true;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    uint32_t width = 0;
    uint32_t height = 0;
    const float default_intensity = scene_vars.cascaded_shadow_intensity;
    const auto mask_for = [&](RasterizationMode mode) {
        renderer_vars.rasterizationMode = mode;
        scene_vars.cascaded_shadow_intensity = 0.0F;
        harness.render_frames(SETTLE_FRAMES);
        const std::vector<uint8_t> unshadowed = harness.capture_frame(width, height);
        scene_vars.cascaded_shadow_intensity = default_intensity;
        harness.render_frames(SETTLE_FRAMES);
        const std::vector<uint8_t> shadowed = harness.capture_frame(width, height);
        return shadow_mask(unshadowed, shadowed);
    };
    const std::vector<bool> forward = mask_for(RasterizationMode::Forward);
    const std::vector<bool> deferred = mask_for(RasterizationMode::Deferred);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(forward.size(), static_cast<size_t>(width) * height);
    ASSERT_EQ(deferred.size(), forward.size());

    const double forward_coverage =
      static_cast<double>(std::count(forward.begin(), forward.end(), true)) / static_cast<double>(forward.size());
    const double iou = mask_iou(deferred, forward, width, height, false);
    const double iou_flipped = mask_iou(deferred, forward, width, height, true);
    GTEST_LOG_(INFO) << "forward shadow coverage " << forward_coverage << ", deferred-vs-forward IoU " << iou
                     << ", against the flipped forward mask " << iou_flipped;

    ASSERT_GT(forward_coverage, 0.01) << "forward casts no measurable shadow - the comparison would be vacuous";
    ASSERT_LT(forward_coverage, 0.5) << "forward darkens most of the frame - not a shadow";
    EXPECT_GT(iou, 0.5) << "deferred shadows do not land where forward's do";
    EXPECT_GT(iou, iou_flipped + 0.25) << "deferred shadows match forward's vertical mirror at least as well";
}

// x = 5 frames the card at 78-92% of the width, inside card_crop; the mask cards' 2.5 straddles the panel edge.
static glm::mat4 panel_free_card_placement()
{
    const auto env_f = [](const char *name, float fallback) {
        const char *value = std::getenv(name);
        return (value != nullptr) ? std::strtof(value, nullptr) : fallback;
    };
    return glm::translate(
             glm::mat4(1.0F), glm::vec3(env_f("CARD_X", 5.0F), env_f("MASK_Y", 4.0F), env_f("MASK_Z", 15.0F)))
           * glm::scale(glm::mat4(1.0F), glm::vec3(env_f("MASK_SCALE", 2.0F)));
}

// The inside of the card panel_free_card_placement frames, so a mean measures the card and not the sky around it.
static Crop panel_free_card_box(uint32_t w, uint32_t h)
{ return Crop{ (w * 80U) / 100U, (w * 90U) / 100U, (h * 42U) / 100U, (h * 58U) / 100U }; }

// The build-integrity check only sees material.emission as text; this proves it actually brightens pixels.
TEST(GoldenRender, EmissiveMaterialBrightensTheFrame)
{
    SKIP_WITHOUT_GPU();

    {
        // Probe here: the skip macro's `return` only leaves the TEST from this depth, not from the lambda.
        ScopedModelOverride rig(SHADOW_RIG_MODEL);
        EngineHarness probe;
        SKIP_WITHOUT_FRAME_CAPTURE(probe);
    }

    const glm::mat4 placement = panel_free_card_placement();

    // One harness per card: addModel only adds, so a swap would measure both cards.
    const auto capture_card_mean = [&placement](const char *card_model, const char *label) {
        ScopedModelOverride rig(SHADOW_RIG_MODEL);
        EngineHarness harness;

        harness.useForwardRaster();
        harness.gui->getGuiSceneSharedVars().shadows_enabled = true;
        harness.render_frames(WARMUP_FRAMES);
        if (harness.renderer->hasDeviceLost()) {
            ADD_FAILURE() << "Device lost while warming up (" << label << ").";
            return std::optional<double>();
        }

        const auto added = harness.renderer->addModel(card_model, placement);
        if (!added.has_value()) {
            ADD_FAILURE() << "adding the " << label << " card failed";
            return std::optional<double>();
        }
        harness.render_frames(SETTLE_FRAMES);
        if (harness.renderer->hasDeviceLost()) {
            ADD_FAILURE() << "Device lost after adding the " << label << " card.";
            return std::optional<double>();
        }

        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<uint8_t> frame = harness.capture_frame(width, height);
        if (frame.empty()) {
            ADD_FAILURE() << label << " card capture returned no pixels.";
            return std::optional<double>();
        }
        return std::optional<double>(mean_luminance_in_crop(frame, width, height, panel_free_card_box(width, height)));
    };

    const std::optional<double> emissive_mean = capture_card_mean(EMISSIVE_CARD_MODEL, "emissive");
    ASSERT_TRUE(emissive_mean.has_value());
    const std::optional<double> metallic_mean = capture_card_mean(METALLIC_CARD_MODEL, "metallic");
    ASSERT_TRUE(metallic_mean.has_value());

    GTEST_LOG_(INFO) << "card-region mean luminance: emissive " << *emissive_mean << ", metallic " << *metallic_mean;

    // RTX 2080, 2026-10-01: emissive 232 vs metallic 174, and 40 vs 174 with emission dropped from rasterizer.slang.
    constexpr double MARGIN = 10.0;
    EXPECT_GT(*emissive_mean, *metallic_mean + MARGIN)
      << "The emissive card's own region is not meaningfully brighter than the non-emissive control; "
         "material.emission is likely not reaching this shading path.";
}

// Mean row of the pixels at x >= 68% where `after` moves any channel more than 12 levels from `before`.
static std::optional<double> changed_centroid_row(const std::vector<uint8_t> &before,
  const std::vector<uint8_t> &after,
  uint32_t w,
  uint32_t h,
  size_t &count)
{
    double row_sum = 0.0;
    count = 0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = (w * 68U) / 100U; x < w; ++x) {
            const size_t base = (static_cast<size_t>(y) * w + x) * 4U;
            for (size_t c = 0; c < 3U; ++c) {
                if (std::abs(static_cast<int>(after[base + c]) - static_cast<int>(before[base + c])) > 12) {
                    row_sum += static_cast<double>(y);
                    ++count;
                    break;
                }
            }
        }
    }
    return count > 200U ? std::optional<double>(row_sum / static_cast<double>(count)) : std::nullopt;
}

// The camera aims at y = 4 where the red/green cards stand: y = 6.5 belongs in the upper half, 1.5 in the lower.
TEST(GoldenRender, WorldUpIsScreenUp)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    struct Mode
    {
        const char *name;
        bool raytracing;
        RasterizationMode raster;
    };
    std::vector<Mode> modes{ { "forward", false, RasterizationMode::Forward },
        { "deferred", false, RasterizationMode::Deferred } };
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back({ "raytracing", true, RasterizationMode::Forward });
    }

    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";
    // A card's shadow on the ground would pull its changed-pixel centroid down.
    harness.gui->getGuiSceneSharedVars().shadows_enabled = false;

    uint32_t width = 0;
    uint32_t height = 0;
    const auto capture_all = [&] {
        std::vector<std::vector<uint8_t>> frames;
        for (const Mode &mode : modes) {
            auto &renderer_vars = harness.gui->getGuiRendererSharedVars();
            renderer_vars.raytracing = mode.raytracing;
            renderer_vars.pathTracing = false;
            renderer_vars.rasterizationMode = mode.raster;
            harness.render_frames(SETTLE_FRAMES);
            frames.push_back(harness.capture_frame(width, height));
        }
        return frames;
    };
    const auto add_card = [&](float y) {
        const glm::mat4 placement =
          glm::translate(glm::mat4(1.0F), glm::vec3(5.0F, y, 15.0F)) * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
        return harness.renderer->addModel(TWO_PRIMITIVE_MODEL, placement).has_value();
    };

    const auto base = capture_all();
    ASSERT_TRUE(add_card(6.5F)) << "adding the high card failed";
    const auto high = capture_all();
    ASSERT_TRUE(add_card(1.5F)) << "adding the low card failed";
    const auto low = capture_all();
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    const double half = static_cast<double>(height) / 2.0;
    for (size_t i = 0; i < modes.size(); ++i) {
        ASSERT_FALSE(base[i].empty() || high[i].empty() || low[i].empty()) << modes[i].name;
        size_t high_count = 0;
        size_t low_count = 0;
        const auto high_row = changed_centroid_row(base[i], high[i], width, height, high_count);
        const auto low_row = changed_centroid_row(high[i], low[i], width, height, low_count);
        GTEST_LOG_(INFO) << modes[i].name << ": high card " << high_count << " px at mean row "
                         << high_row.value_or(-1.0) << ", low card " << low_count << " px at mean row "
                         << low_row.value_or(-1.0) << " of " << height;
        ASSERT_TRUE(high_row.has_value() && low_row.has_value())
          << modes[i].name << ": a card did not brighten the right of the frame - framing changed";
        EXPECT_LT(*high_row, half) << modes[i].name << ": the card above the aim point is in the lower half";
        EXPECT_GT(*low_row, half) << modes[i].name << ": the card below the aim point is in the upper half";
    }
}

// An 8-bit UNORM G-buffer clips emission to 1.0; the debug scene, since the rig's sky diverges on its own.
TEST(GoldenRender, EmissiveStrengthSurvivesTheDeferredGBuffer)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.gui->getGuiRendererSharedVars();
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = false;
    harness.gui->getGuiSceneSharedVars().shadows_enabled = true;

    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    const auto added = harness.renderer->addModel(EMISSIVE_STRENGTH_CARD_MODEL, panel_free_card_placement());
    ASSERT_TRUE(added.has_value()) << "adding the emissive-strength card failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> forward = harness.capture_frame(width, height);
    ASSERT_FALSE(forward.empty()) << "Forward capture returned no pixels.";

    renderer_vars.rasterizationMode = RasterizationMode::Deferred;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> deferred = harness.capture_frame(width, height);
    ASSERT_FALSE(deferred.empty()) << "Deferred capture returned no pixels.";

    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
    ASSERT_EQ(forward.size(), deferred.size());

    double abs_diff_sum = 0.0;
    for (size_t i = 0; i < forward.size(); ++i) {
        abs_diff_sum += std::abs(static_cast<int>(forward[i]) - static_cast<int>(deferred[i]));
    }
    const double mean_abs_diff = abs_diff_sum / static_cast<double>(forward.size());
    GTEST_LOG_(INFO) << "emissive-strength deferred-vs-forward mean abs channel diff: " << mean_abs_diff;

    if (const char *dump = std::getenv("KATAGLYPHIS_EMISSIVE_DUMP")) {
        const std::string base = dump;
        const auto stride = static_cast<int>(width) * 4;
        stbi_write_png((base + "-forward.png").c_str(), int(width), int(height), 4, forward.data(), stride);
        stbi_write_png((base + "-deferred.png").c_str(), int(width), int(height), 4, deferred.data(), stride);
    }

    // Measured: sits between the shipped float format and an eR8G8B8A8Unorm revert.
    constexpr double MEAN_ABS_DIFF_LIMIT = 0.7;
    EXPECT_LT(mean_abs_diff, MEAN_ABS_DIFF_LIMIT)
      << "Deferred diverges from forward on the emissive-strength card; the G-buffer attachment is likely "
         "clipping a strength>1 emitter again.";
}

// Culling that works has no visual signature, so the wiring is checked through drawn/considered counters.
TEST(GoldenRender, FrustumCullingDropsOffscreenMeshesOnly)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    auto &renderer_vars = harness.useForwardRaster();
    renderer_vars.frustum_culling_enabled = true;

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    // The default debug camera frames the scene, so everything is visible.
    const unsigned int considered = renderer_vars.visibility.meshes_total;
    ASSERT_GT(considered, 0U) << "the renderer reported considering no meshes at all; "
                                 "the visibility counters are not being written";
    EXPECT_EQ(renderer_vars.visibility.meshes_drawn, considered)
      << "culling dropped geometry the camera is looking straight at";

    // Far past the far plane, with the scene behind the camera.
    harness.camera->set_camera_position(glm::vec3(0.0F, 0.0F, 5000.0F));
    harness.render_frames(SETTLE_FRAMES);

    EXPECT_EQ(renderer_vars.visibility.meshes_total, considered)
      << "the number of meshes considered must not change when the camera moves";
    EXPECT_EQ(renderer_vars.visibility.meshes_drawn, 0U)
      << "a scene entirely outside the view should be culled completely, but " << renderer_vars.visibility.meshes_drawn
      << " mesh(es) were still drawn";

    // Separates "culling works" from "the renderer stopped drawing".
    renderer_vars.frustum_culling_enabled = false;
    harness.render_frames(SETTLE_FRAMES);
    EXPECT_EQ(renderer_vars.visibility.meshes_drawn, considered)
      << "with culling disabled every mesh must be submitted regardless of the camera";

    ASSERT_FALSE(harness.renderer->hasDeviceLost());
}

// With one model objectIndex is always 0; this proves a second one renders, not the index arithmetic.
TEST(GoldenRender, SecondModelLoadsAndRenders)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.useForwardRaster();
    renderer_vars.frustum_culling_enabled = true;

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    const unsigned int meshes_with_one_model = renderer_vars.visibility.meshes_total;
    ASSERT_GT(meshes_with_one_model, 0U);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    // The large untextured rig cannot fail to change the frame if it renders.
    const glm::mat4 placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 2.0F, 12.0F)) * glm::scale(glm::mat4(1.0F), glm::vec3(0.15F));
    const std::optional<uint32_t> second = harness.renderer->addModel("Models/ShadowTest/shadow_rig.obj", placement);

    ASSERT_TRUE(second.has_value()) << "the second model failed to load";
    EXPECT_EQ(*second, 1U) << "the second model must be index 1 - this is the objectIndex the raster path pushes";

    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost after adding a model.";

    // The raster path must now consider more meshes than before.
    EXPECT_GT(renderer_vars.visibility.meshes_total, meshes_with_one_model)
      << "adding a model did not change how many meshes the raster path considered; "
         "the new model never reached the draw loop";

    const std::vector<uint8_t> after = harness.capture_frame(width, height);
    ASSERT_EQ(after.size(), before.size());

    size_t changed = 0;
    for (size_t pixel = 0; pixel + 3 < after.size(); pixel += 4) {
        if (std::abs(luminance_of(after, pixel / 4) - luminance_of(before, pixel / 4)) > 4.0) { ++changed; }
    }
    EXPECT_GT(changed, 500U) << "the second model loaded and was counted, but changed only " << changed
                             << " pixels - it is being drawn somewhere invisible, or not drawn at all";
}

// The mesh count is the framing-independent proof; pixels depend on the camera.
TEST(GoldenRender, MultiPrimitiveGltfLoadsAsMultipleMeshes)
{
    SKIP_WITHOUT_GPU();

    const ScopedModelOverride model_override(TWO_PRIMITIVE_MODEL);

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.useForwardRaster();
    renderer_vars.frustum_culling_enabled = false;// count every mesh, framing aside

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost())
      << "Device lost rendering the two-primitive glTF - the split produced an invalid mesh.";

    EXPECT_EQ(renderer_vars.visibility.meshes_total, 2U)
      << "the two-primitive glTF must build two meshes (one per primitive), not one flattened mesh";

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(frame.empty()) << "the two-primitive glTF captured no frame";
}

// Covers the whole loop: env var, accumulation over real frames, the write in cleanUp, and the schema.
TEST(GoldenRender, GpuTimingJsonDumpIsWrittenAndSane)
{
    SKIP_WITHOUT_GPU();

    const std::filesystem::path json_path =
      std::filesystem::temp_directory_path() / "kataglyphis-gpu-timing-dump-test.json";
    std::error_code filesystem_error;
    std::filesystem::remove(json_path, filesystem_error);
    ASSERT_FALSE(std::filesystem::exists(json_path, filesystem_error))
      << "Stale dump at " << json_path << " could not be removed; the test would read a lie.";

    const ScopedGpuTimingJsonPath scoped_path(json_path.string());

    {
        EngineHarness harness;
        harness.useForwardRaster();
        harness.gui->getGuiSceneSharedVars().shadows_enabled = true;

        // Fresh query pools are unreadable until recorded once, so the first readbacks measure nothing.
        harness.render_frames(30);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while rendering.";
    }// harness teardown calls renderer->cleanUp(), which writes the dump

    ASSERT_TRUE(std::filesystem::exists(json_path, filesystem_error))
      << "Renderer teardown did not write " << json_path;

    std::ifstream file(json_path);
    ASSERT_TRUE(file.is_open()) << "Cannot read back " << json_path;
    std::stringstream content;
    content << file.rdbuf();

    // Non-throwing parse: exceptions are disabled project-wide.
    const nlohmann::json dump = nlohmann::json::parse(content.str(), nullptr, false);
    ASSERT_FALSE(dump.is_discarded()) << "GPU timing dump is not valid JSON: " << content.str();

    ASSERT_TRUE(dump.contains("frames_measured")) << dump.dump(2);
    ASSERT_TRUE(dump.contains("timestamps_supported")) << dump.dump(2);
    ASSERT_TRUE(dump.contains("passes")) << dump.dump(2);
    ASSERT_TRUE(dump["passes"].is_object()) << dump.dump(2);

    if (!dump["timestamps_supported"].get<bool>()) {
        // A parsing file is what separates "cannot measure" from "export broken" here.
        EXPECT_EQ(dump["frames_measured"].get<std::uint64_t>(), 0U)
          << "A device without timestamps cannot have measured frames.";
        EXPECT_TRUE(dump["passes"].empty()) << dump.dump(2);
        GTEST_SKIP() << "GPU timestamps unsupported on this device; average checks not possible.";
    }

    EXPECT_GT(dump["frames_measured"].get<std::uint64_t>(), 0U)
      << "Timestamps are supported but no frame produced a valid sample: " << dump.dump(2);

    // These are bracketed every frame, so absence means broken accumulation, not a disabled feature.
    for (const char *always_recorded : { "Main", "Sky", "Post" }) {
        EXPECT_TRUE(dump["passes"].contains(always_recorded))
          << "Pass '" << always_recorded << "' is recorded every frame but missing: " << dump.dump(2);
    }

    for (const auto &[pass_name, average_ms] : dump["passes"].items()) {
        ASSERT_TRUE(average_ms.is_number()) << "Pass '" << pass_name << "' is not a number: " << dump.dump(2);
        const double value = average_ms.get<double>();
        EXPECT_TRUE(std::isfinite(value)) << "Pass '" << pass_name << "' average is not finite.";
        EXPECT_GE(value, 0.0) << "Pass '" << pass_name << "' average is negative.";
    }
}

// New samples every frame must make early frames differ and late frames converge (1/N running mean).
TEST(GoldenRender, PathTracingAccumulatesAndConverges)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.gui->getGuiRendererSharedVars();
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES);

    // Nudge to reset history, or the early pair measures the mean healing from pre-load frames.
    harness.camera->set_camera_position(harness.camera->get_camera_position() + glm::vec3(0.001F, 0.0F, 0.0F));
    harness.render_frame();

    // Changed-pixel fraction in the GUI-free right edge; means and centre crops measured GUI noise.
    const auto changed_fraction =
      [](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, uint32_t w, uint32_t h) {
          const uint32_t x0 = (w * 3U) / 4U;
          const uint32_t x1 = (w * 49U) / 50U;
          const uint32_t y0 = h / 20U;
          const uint32_t y1 = (h * 19U) / 20U;
          size_t changed = 0;
          size_t total = 0;
          for (uint32_t y = y0; y < y1; ++y) {
              for (uint32_t x = x0; x < x1; ++x) {
                  const size_t base = (static_cast<size_t>(y) * w + x) * 4U;
                  for (size_t c = 0; c < 3U; ++c) {
                      if (std::abs(static_cast<int>(a[base + c]) - static_cast<int>(b[base + c])) > 2) {
                          ++changed;
                          break;
                      }
                  }
                  ++total;
              }
          }
          return static_cast<double>(changed) / static_cast<double>(total);
      };

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> early_a = harness.capture_frame(width, height);
    const std::vector<uint8_t> early_b = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(early_a.empty());
    ASSERT_EQ(early_a.size(), early_b.size());

    // Deepen the history, then measure the per-frame movement again.
    harness.render_frames(40);
    const std::vector<uint8_t> late_a = harness.capture_frame(width, height);
    const std::vector<uint8_t> late_b = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(late_a.size(), late_b.size());

    // Logged, not asserted: autocorrelation shows a future seed change's noise character.
    {
        const uint32_t cx0 = (width * 18U) / 25U, cx1 = (width * 49U) / 50U;
        const uint32_t cy0 = height / 4U, cy1 = (height * 3U) / 4U;
        auto corr = [&](uint32_t lag) {
            double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
            size_t n = 0;
            for (uint32_t y = cy0; y < cy1; ++y)
                for (uint32_t x = cx0; x + lag < cx1; ++x) {
                    const double a =
                      luminance_of(early_a, (size_t)y * width + x) - luminance_of(early_b, (size_t)y * width + x);
                    const double b = luminance_of(early_a, (size_t)y * width + x + lag)
                                     - luminance_of(early_b, (size_t)y * width + x + lag);
                    sx += a;
                    sy += b;
                    sxx += a * a;
                    syy += b * b;
                    sxy += a * b;
                    ++n;
                }
            const double cov = sxy / n - (sx / n) * (sy / n);
            const double va = sxx / n - (sx / n) * (sx / n);
            const double vb = syy / n - (sy / n) * (sy / n);
            return (va > 0 && vb > 0) ? cov / std::sqrt(va * vb) : 0.0;
        };
        GTEST_LOG_(INFO) << "noise autocorrelation lag-1: " << corr(1) << ", lag-16: " << corr(16);
    }

    const double early_delta = changed_fraction(early_a, early_b, width, height);
    const double late_delta = changed_fraction(late_a, late_b, width, height);
    GTEST_LOG_(INFO) << "consecutive-frame changed fraction (right-edge crop): early = " << early_delta
                     << ", late = " << late_delta;

    // (2) Not blank: the accumulated image reaches the screen at all.
    EXPECT_GT(fraction_above(late_b, 8.0), 0.01)
      << "Path-traced frame is (nearly) black - accumulated image not reaching the screen.";

    // (1) The crop is GUI-free, so frozen sampling gives exactly 0; the limit is measured.
    EXPECT_GT(early_delta, 1.0e-4) << "Consecutive path-traced frames are (near) identical in the scene region - "
                                      "per-frame seeding inactive.";

    // (3) Half the early fraction leaves room for driver variance but still demands convergence.
    EXPECT_LT(late_delta, early_delta / 2.0)
      << "Per-frame movement did not shrink with accumulation depth - history not converging.";
}

// Pixel jitter lets accumulation blend straddling pixels; without it every edge pixel is fully fg or bg.
TEST(GoldenRender, PathTracingAntiAliasesGeometricEdges)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.gui->getGuiRendererSharedVars();
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES);

    // History-reset nudge, as in PathTracingAccumulatesAndConverges.
    harness.camera->set_camera_position(harness.camera->get_camera_position() + glm::vec3(0.001F, 0.0F, 0.0F));
    harness.render_frame();

    // Shallow history looks "in between" from noise alone, which would pass vacuously.
    harness.render_frames(80);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(frame.empty());

    // Triples take fg and bg from the same local edge; columns too, since the rig's edges are mostly horizontal.
    const Crop crop = panel_free_crop(width, height);
    constexpr double EDGE_JUMP = 40.0;
    constexpr double PARTIAL_MARGIN = 6.0;
    size_t edges_found = 0;
    size_t edges_with_partial_pixel = 0;
    const auto classify_triple = [&](double before, double mid, double after) {
        const double lo = std::min(before, after);
        const double hi = std::max(before, after);
        if (hi - lo <= EDGE_JUMP) { return; }
        ++edges_found;
        if (mid > lo + PARTIAL_MARGIN && mid < hi - PARTIAL_MARGIN) { ++edges_with_partial_pixel; }
    };
    for (uint32_t y = crop.y0; y < crop.y1; ++y) {
        for (uint32_t x = crop.x0 + 1U; x + 1U < crop.x1; ++x) {
            classify_triple(luminance_of(frame, static_cast<size_t>(y) * width + (x - 1U)),
              luminance_of(frame, static_cast<size_t>(y) * width + x),
              luminance_of(frame, static_cast<size_t>(y) * width + (x + 1U)));
        }
    }
    for (uint32_t x = crop.x0; x < crop.x1; ++x) {
        for (uint32_t y = crop.y0 + 1U; y + 1U < crop.y1; ++y) {
            classify_triple(luminance_of(frame, static_cast<size_t>(y - 1U) * width + x),
              luminance_of(frame, static_cast<size_t>(y) * width + x),
              luminance_of(frame, static_cast<size_t>(y + 1U) * width + x));
        }
    }

    ASSERT_GT(edges_found, 0U)
      << "No silhouette edge (a >40-level luminance jump between neighbours) found in the panel-free "
         "crop - the shadow rig's box/ground/sky boundary should always produce one; check the crop or rig.";

    const double partial_fraction = static_cast<double>(edges_with_partial_pixel) / static_cast<double>(edges_found);
    GTEST_LOG_(INFO) << "silhouette edges found: " << edges_found
                     << ", with a partial (intermediate-luminance) pixel: " << edges_with_partial_pixel << " ("
                     << (partial_fraction * 100.0) << "%)";

    // Measured: wide margin between the jittered kernel and a pixel-centre-only revert.
    EXPECT_GT(partial_fraction, 0.15)
      << "Almost none of this frame's silhouette edges have a partial (blended) pixel between the "
         "foreground and background luminance - the primary ray is landing on the exact pixel centre "
         "every sample instead of being jittered across the pixel's area.";
}

// Path tracing does NEE toward the GUI light, so zero radiance must darken the GUI-free right edge hard.
TEST(GoldenRender, PathTracingRespondsToTheDirectionalLight)
{
    SKIP_WITHOUT_GPU();

    // The skeleton scene's right-edge crop is nearly all sky, which the light does not touch.
    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.gui->getGuiRendererSharedVars();
    auto &scene_vars = harness.gui->getGuiSceneSharedVars();
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES);

    // Count swung pixels, not a mean: the lit ground clamps at the UNORM ceiling and the band hugs the panel.
    harness.render_frames(SETTLE_FRAMES);
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> lit = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(lit.empty());

    // History resets on camera/scene changes, not light changes, so wait for the mean to wash out.
    scene_vars.directional_light_radiance = 0.0F;
    harness.render_frames(30);
    const std::vector<uint8_t> unlit = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(lit.size(), unlit.size());

    const double response = swung_fraction(lit, unlit, width, height, panel_free_crop(width, height));
    // Logged: with NEE's 1/pi, PT should match forward's brightness, not sit pi times above it.
    const auto crop_mean_luminance = [](const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h) {
        const uint32_t x0 = (w * 18U) / 25U;
        const uint32_t x1 = (w * 49U) / 50U;
        const uint32_t y0 = h / 20U;
        const uint32_t y1 = (h * 19U) / 20U;
        double sum = 0.0;
        size_t count = 0;
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = x0; x < x1; ++x) {
                sum += luminance_of(rgba, static_cast<size_t>(y) * w + x);
                ++count;
            }
        }
        return sum / static_cast<double>(count);
    };
    GTEST_LOG_(INFO) << "PT lit-vs-unlit swung-pixel fraction (panel-free crop): " << response
                     << ", lit crop mean luminance: " << crop_mean_luminance(lit, width, height);

    // Measured with margin; a light-blind kernel leaves only accumulation drift, well below it.
    EXPECT_GT(response, 5.0e-3) << "Path-traced pixels did not respond to the directional light radiance.";
}

// One bounce means no indirect sky light at all, so a kernel honouring the cap changes large regions.
TEST(GoldenRender, PathTracingHonorsTheQualityControls)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.gui->getGuiRendererSharedVars();
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> full_quality = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(full_quality.empty());

    renderer_vars.pathTracingMaxBounces = 1;
    harness.render_frames(10);
    const std::vector<uint8_t> single_bounce = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(full_quality.size(), single_bounce.size());

    const double response = swung_fraction(full_quality, single_bounce, width, height, panel_free_crop(width, height));
    GTEST_LOG_(INFO) << "PT bounces 8-vs-1 swung-pixel fraction (panel-free crop): " << response;

    // Measured: removing all indirect light must change a real share of scene pixels.
    EXPECT_GT(response, 5.0e-3) << "The bounce-cap slider did not change the path-traced image - "
                                   "quality push constants not reaching the kernel.";
}

// The light must scale diffuse as well as specular, or only sparse highlights respond to radiance.
TEST(GoldenRender, ForwardLightingRespondsToTheDirectionalLight)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.gui->getGuiSceneSharedVars();
    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> lit = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(lit.empty());

    scene_vars.directional_light_radiance = 0.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> unlit = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(lit.size(), unlit.size());

    const double response = swung_fraction(lit, unlit, width, height, panel_free_crop(width, height));

    // Specular alone swings many pixels, so only an unlit crop mean catches a light-blind diffuse.
    const auto crop_mean_luminance = [](const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h) {
        const uint32_t x0 = (w * 18U) / 25U;
        const uint32_t x1 = (w * 49U) / 50U;
        const uint32_t y0 = h / 20U;
        const uint32_t y1 = (h * 19U) / 20U;
        double sum = 0.0;
        size_t count = 0;
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = x0; x < x1; ++x) {
                sum += luminance_of(rgba, static_cast<size_t>(y) * w + x);
                ++count;
            }
        }
        return sum / static_cast<double>(count);
    };
    const double unlit_luma = crop_mean_luminance(unlit, width, height);
    const double lit_luma = crop_mean_luminance(lit, width, height);
    GTEST_LOG_(INFO) << "forward lit-vs-unlit swung-pixel fraction (panel-free crop): " << response
                     << ", lit crop mean luminance: " << lit_luma << ", unlit crop mean luminance: " << unlit_luma;

    EXPECT_LT(unlit_luma, 30.0) << "The scene stays lit with the directional light at zero radiance - "
                                   "the light factors are not reaching the diffuse term.";

    // Vacuous alone; a cheap sanity floor.
    EXPECT_GT(response, 0.05) << "Forward lighting did not respond to the directional light radiance.";
}

// A transform change must rebuild the TLAS; deterministic RT over a stale one is bit-identical.
TEST(GoldenRender, RaytracedWorldFollowsTheModelTransform)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.gui->getGuiRendererSharedVars();
    auto &scene_vars = harness.gui->getGuiSceneSharedVars();
    renderer_vars.raytracing = true;
    renderer_vars.pathTracing = false;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(before.empty());

    // Raise the model the way the GUI does it.
    scene_vars.selected_model_index = 0;
    scene_vars.model_position[1] += 3.0F;
    scene_vars.model_transform_changed = true;
    harness.render_frames(SETTLE_FRAMES);

    const std::vector<uint8_t> after = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(before.size(), after.size());

    const double moved = swung_fraction(before, after, width, height, panel_free_crop(width, height));
    GTEST_LOG_(INFO) << "RT transform-move swung-pixel fraction (panel-free crop): " << moved;

    // Measured; a stale TLAS gives exactly zero.
    EXPECT_GT(moved, 5.0e-3) << "The traced image did not follow the model transform - stale TLAS.";
}

// Seeding the payload with albedo adds an ambient term forward/deferred lack, so shadows never go dark.
TEST(GoldenRender, RaytracedShadowsAreDarkerThanLitGround)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.gui->getGuiRendererSharedVars();
    renderer_vars.raytracing = true;
    renderer_vars.pathTracing = false;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(frame.empty());

    // Darkest vs brightest decile, since a whole-crop mean averages shadow and light together.
    const Crop crop = panel_free_crop(width, height);
    std::vector<double> luminances;
    luminances.reserve(static_cast<size_t>(crop.x1 - crop.x0) * static_cast<size_t>(crop.y1 - crop.y0));
    for (uint32_t y = crop.y0; y < crop.y1; ++y) {
        for (uint32_t x = crop.x0; x < crop.x1; ++x) {
            luminances.push_back(luminance_of(frame, static_cast<size_t>(y) * width + x));
        }
    }
    std::sort(luminances.begin(), luminances.end());
    const size_t decile = std::max<size_t>(1U, luminances.size() / 10U);
    double dark_sum = 0.0;
    for (size_t i = 0; i < decile; ++i) { dark_sum += luminances[i]; }
    double lit_sum = 0.0;
    for (size_t i = luminances.size() - decile; i < luminances.size(); ++i) { lit_sum += luminances[i]; }
    const double dark_mean = dark_sum / static_cast<double>(decile);
    const double lit_mean = lit_sum / static_cast<double>(decile);

    GTEST_LOG_(INFO) << "RT shadow darkest-decile mean luminance: " << dark_mean
                     << ", lit-decile mean luminance: " << lit_mean;

    // Measured: between the fix (exactly black) and an albedo-seeded payload in raytrace.rchit.slang.
    EXPECT_LT(dark_mean, lit_mean * 0.05) << "Ray-traced shadow did not go dark relative to the lit ground - the "
                                             "closest-hit shader is still seeding an ambient/unlit-albedo term.";
}

// RT overwrites the raster image, so pixels prove nothing: the zeroed counters are the oracle, timing is logged.
TEST(GoldenRender, RaytracingFrameSkipsTheRasterPass)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.useForwardRaster();
    renderer_vars.frustum_culling_enabled = true;

    // 30 frames flush GpuPassAverage::WINDOW with forward-only samples.
    harness.render_frames(WARMUP_FRAMES + 30);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    const unsigned int forward_meshes_drawn = renderer_vars.visibility.meshes_drawn;
    ASSERT_GT(forward_meshes_drawn, 0U) << "the renderer reported drawing no meshes at all in forward mode";
    const float forward_main_ms =
      renderer_vars.gpuTimings
        .pass_ms[static_cast<size_t>(Kataglyphis::VulkanRendererInternals::FrontendShared::GpuTimedPass::Main)];

    renderer_vars.raytracing = true;
    // Flush again so the average holds RT-only samples.
    harness.render_frames(30);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    EXPECT_EQ(renderer_vars.visibility.meshes_drawn, 0U)
      << "the raster pass must not run - and must not report stale mesh counts - while RT owns the frame";
    EXPECT_EQ(renderer_vars.visibility.meshes_total, 0U)
      << "the raster pass must not run - and must not report stale mesh counts - while RT owns the frame";

    if (renderer_vars.gpuTimings.supported) {
        const float rt_main_ms =
          renderer_vars.gpuTimings
            .pass_ms[static_cast<size_t>(Kataglyphis::VulkanRendererInternals::FrontendShared::GpuTimedPass::Main)];
        GTEST_LOG_(INFO) << "Main-pass GPU time: forward(raster only)=" << forward_main_ms
                         << "ms, RT(dispatch only)=" << rt_main_ms << "ms";
    }
}

// textureIDs are model-local, so a second model needs its offset or it samples the first model's slots.
TEST(GoldenRender, SecondModelShadesWithItsOwnTextures)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);

    // Sponza is the one bundled model with texture files; the others would sample the white default.
    const std::string sponza = sceneConfig::resolveModelPath("Models/crytek-sponza/sponza_triag.obj");
    if (!std::filesystem::exists(sponza)) { GTEST_SKIP() << "textured second model not present"; }

    // Sponza spans about +-1800 units; 0.01 fits it next to the rig inside the frame.
    const auto added = harness.renderer->addModel(sponza, glm::scale(glm::mat4(1.0F), glm::vec3(0.01F)));
    ASSERT_TRUE(added.has_value()) << "adding the second model failed";
    harness.render_frames(SETTLE_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(frame.empty());

    // Detail, not colour: the bricks are near-greyscale, but a flat white default has no detail.
    const double detail = detail_fraction(frame, width, height, card_crop(width, height));
    GTEST_LOG_(INFO) << "second-model texture-detail fraction (panel-free crop): " << detail;

    EXPECT_GT(detail, 0.02) << "The added model shows no texture detail - it is sampling the first "
                               "model's (flat white) texture slots.";
}

// Sponza is the release default scene, so all its textures must fit under MAX_TEXTURE_COUNT.
TEST(GoldenRender, SponzaBindsEveryTextureSlot)
{
    SKIP_WITHOUT_GPU();

    const std::string sponza = sceneConfig::resolveModelPath("Models/crytek-sponza/sponza_triag.obj");
    if (!std::filesystem::exists(sponza)) { GTEST_SKIP() << "Sponza model not present"; }

    ScopedModelOverride sponza_override(sponza.c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);

    EXPECT_LE(harness.scene->getTextureCount(0), static_cast<uint32_t>(MAX_TEXTURE_COUNT))
      << "Sponza needs more texture slots than MAX_TEXTURE_COUNT provides.";

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while binding Sponza's textures.";
    ASSERT_FALSE(frame.empty()) << "Frame capture returned no pixels.";
}

// A failed decode must still fill its slot, or later textureIDs index past the descriptor array.
TEST(GoldenRender, CorruptEmbeddedImageKeepsTextureSlotsAligned)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride corrupt_override(CORRUPT_EMBEDDED_IMAGE_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost())
      << "Device lost loading a model with a corrupt embedded image - the failed decode was not handled.";

    EXPECT_EQ(harness.scene->getTextureCount(0), 2U)
      << "a skipped slot would shift material 1's textureID down and index past the descriptor array";
}

// A 50/50 checkerboard changes about half its bounding box when discard works; KATAGLYPHIS_MASK_DUMP dumps PNGs.
TEST(GoldenRender, MaskCardDiscardsCutoutTexelsVisually)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    // A: the base scene, before the card is added.
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    // Upper right, clear of the ImGui panel and the card's floor shadow; MASK_X/Y/Z/SCALE tune it without a rebuild.
    const auto env_f = [](const char *name, float fallback) {
        const char *value = std::getenv(name);
        return (value != nullptr) ? std::strtof(value, nullptr) : fallback;
    };
    const glm::mat4 placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(env_f("MASK_X", 2.5F), env_f("MASK_Y", 4.0F), env_f("MASK_Z", 15.0F)))
      * glm::scale(glm::mat4(1.0F), glm::vec3(env_f("MASK_SCALE", 2.0F)));
    const auto added = harness.renderer->addModel(MASK_CARD_MODEL, placement);
    ASSERT_TRUE(added.has_value()) << "adding the mask card failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    // B: with the card.
    uint32_t w2 = 0;
    uint32_t h2 = 0;
    const std::vector<uint8_t> after = harness.capture_frame(w2, h2);
    ASSERT_FALSE(after.empty());
    ASSERT_EQ(width, w2);
    ASSERT_EQ(height, h2);

    const auto changed_at = [&](uint32_t x, uint32_t y) {
        const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
        return std::abs(static_cast<int>(before[b]) - static_cast<int>(after[b])) > 12
               || std::abs(static_cast<int>(before[b + 1U]) - static_cast<int>(after[b + 1U])) > 12
               || std::abs(static_cast<int>(before[b + 2U]) - static_cast<int>(after[b + 2U])) > 12;
    };

    // The FPS text and the floor shadow would otherwise pollute the bounding box.
    const uint32_t scan_x0 = (width * 68U) / 100U;
    const uint32_t scan_y1 = (height * 62U) / 100U;

    uint32_t minx = width;
    uint32_t miny = height;
    uint32_t maxx = 0;
    uint32_t maxy = 0;
    size_t changed_total = 0;
    for (uint32_t y = 0; y < scan_y1; ++y) {
        for (uint32_t x = scan_x0; x < width; ++x) {
            if (changed_at(x, y)) {
                ++changed_total;
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            }
        }
    }

    if (const char *dump = std::getenv("KATAGLYPHIS_MASK_DUMP")) {
        const std::string base = dump;
        std::vector<uint8_t> diff(before.size(), 0U);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
                const bool in_scan = (x >= scan_x0 && y < scan_y1);
                const uint8_t v = changed_at(x, y) ? 255U : 0U;
                diff[b] = v;
                diff[b + 1U] = in_scan ? v : static_cast<uint8_t>(v / 3U);// dim outside the scan box
                diff[b + 2U] = in_scan ? v : static_cast<uint8_t>(v / 3U);
                diff[b + 3U] = 255U;
            }
        }
        const auto stride = static_cast<int>(width) * 4;
        stbi_write_png((base + "-before.png").c_str(), int(width), int(height), 4, before.data(), stride);
        stbi_write_png((base + "-after.png").c_str(), int(width), int(height), 4, after.data(), stride);
        stbi_write_png((base + "-diff.png").c_str(), int(width), int(height), 4, diff.data(), stride);
    }

    ASSERT_GT(changed_total, 200U)
      << "the card barely changed the upper-right region - not visible there (framing/culling)";

    const double box_area = static_cast<double>(maxx - minx + 1U) * static_cast<double>(maxy - miny + 1U);
    const double changed_fraction = static_cast<double>(changed_total) / box_area;
    GTEST_LOG_(INFO) << "mask card: changed " << changed_total << " px in upper-right, bbox [" << minx << "," << miny
                     << ".." << maxx << "," << maxy << "] fraction-in-box " << changed_fraction;

    // Measured between discard on and off; grey cut-out texels keep "off" below 1.0.
    EXPECT_LT(changed_fraction, 0.55)
      << "the card footprint changed too fully - cut-out texels are NOT being discarded";
    EXPECT_GT(changed_fraction, 0.20) << "the changed pixels are too sparse to be the checkerboard - check framing";
}

// Any of FORCE_OPAQUE on either ray or an eOpaque BLAS skips any-hit, so all three must stay relaxed.
TEST(GoldenRender, MaskCardDiscardsCutoutTexelsInRaytracing)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.gui->getGuiRendererSharedVars();
    renderer_vars.raytracing = true;
    renderer_vars.pathTracing = false;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    // A: the base scene, before the card is added.
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    // Same env-tunable placement as the forward discard golden.
    const auto env_f = [](const char *name, float fallback) {
        const char *value = std::getenv(name);
        return (value != nullptr) ? std::strtof(value, nullptr) : fallback;
    };
    const glm::mat4 placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(env_f("MASK_X", 2.5F), env_f("MASK_Y", 4.0F), env_f("MASK_Z", 15.0F)))
      * glm::scale(glm::mat4(1.0F), glm::vec3(env_f("MASK_SCALE", 2.0F)));
    const auto added = harness.renderer->addModel(MASK_CARD_MODEL, placement);
    ASSERT_TRUE(added.has_value()) << "adding the mask card failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    // B: with the card.
    uint32_t w2 = 0;
    uint32_t h2 = 0;
    const std::vector<uint8_t> after = harness.capture_frame(w2, h2);
    ASSERT_FALSE(after.empty());
    ASSERT_EQ(width, w2);
    ASSERT_EQ(height, h2);

    const auto changed_at = [&](uint32_t x, uint32_t y) {
        const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
        return std::abs(static_cast<int>(before[b]) - static_cast<int>(after[b])) > 12
               || std::abs(static_cast<int>(before[b + 1U]) - static_cast<int>(after[b + 1U])) > 12
               || std::abs(static_cast<int>(before[b + 2U]) - static_cast<int>(after[b + 2U])) > 12;
    };

    // Same GUI-free upper-right scan region as the forward golden.
    const uint32_t scan_x0 = (width * 68U) / 100U;
    const uint32_t scan_y1 = (height * 62U) / 100U;

    uint32_t minx = width;
    uint32_t miny = height;
    uint32_t maxx = 0;
    uint32_t maxy = 0;
    size_t changed_total = 0;
    for (uint32_t y = 0; y < scan_y1; ++y) {
        for (uint32_t x = scan_x0; x < width; ++x) {
            if (changed_at(x, y)) {
                ++changed_total;
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            }
        }
    }

    if (const char *dump = std::getenv("KATAGLYPHIS_MASK_DUMP")) {
        const std::string base = dump;
        std::vector<uint8_t> diff(before.size(), 0U);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
                const bool in_scan = (x >= scan_x0 && y < scan_y1);
                const uint8_t v = changed_at(x, y) ? 255U : 0U;
                diff[b] = v;
                diff[b + 1U] = in_scan ? v : static_cast<uint8_t>(v / 3U);
                diff[b + 2U] = in_scan ? v : static_cast<uint8_t>(v / 3U);
                diff[b + 3U] = 255U;
            }
        }
        const auto stride = static_cast<int>(width) * 4;
        stbi_write_png((base + "-before.png").c_str(), int(width), int(height), 4, before.data(), stride);
        stbi_write_png((base + "-after.png").c_str(), int(width), int(height), 4, after.data(), stride);
        stbi_write_png((base + "-diff.png").c_str(), int(width), int(height), 4, diff.data(), stride);
    }

    ASSERT_GT(changed_total, 200U)
      << "the card barely changed the upper-right region - not visible there (framing/culling)";

    const double box_area = static_cast<double>(maxx - minx + 1U) * static_cast<double>(maxy - miny + 1U);
    const double changed_fraction = static_cast<double>(changed_total) / box_area;
    GTEST_LOG_(INFO) << "RT mask card: changed " << changed_total << " px in upper-right, bbox [" << minx << "," << miny
                     << ".." << maxx << "," << maxy << "] fraction-in-box " << changed_fraction;

    // RTX 2080, 2026-10-01: 0.479 with any-hit discarding, 0.941 with the primary ray forced opaque.
    EXPECT_LT(changed_fraction, 0.70)
      << "the card footprint changed too fully in RT - cut-out texels are NOT being discarded";
    EXPECT_GT(changed_fraction, 0.15) << "the changed pixels are too sparse to be the checkerboard - check framing";
}

// A ray query has no any-hit stage, so path_tracing.slang alpha-tests each candidate itself.
TEST(GoldenRender, PathTracedMaskCardShowsItsCutout)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.gui->getGuiRendererSharedVars();
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    // Both captures get the same accumulated history, or PT noise alone changes pixels across the card's band.
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES + 60);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    // A: the base scene, before the card is added.
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    // Same env-tunable placement as the forward/RT discard goldens.
    const auto env_f = [](const char *name, float fallback) {
        const char *value = std::getenv(name);
        return (value != nullptr) ? std::strtof(value, nullptr) : fallback;
    };
    const glm::mat4 placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(env_f("MASK_X", 2.5F), env_f("MASK_Y", 4.0F), env_f("MASK_Z", 15.0F)))
      * glm::scale(glm::mat4(1.0F), glm::vec3(env_f("MASK_SCALE", 2.0F)));
    const auto added = harness.renderer->addModel(MASK_CARD_MODEL, placement);
    ASSERT_TRUE(added.has_value()) << "adding the mask card failed";
    harness.render_frames(SETTLE_FRAMES + 60);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    // B: with the card.
    uint32_t w2 = 0;
    uint32_t h2 = 0;
    const std::vector<uint8_t> after = harness.capture_frame(w2, h2);
    ASSERT_FALSE(after.empty());
    ASSERT_EQ(width, w2);
    ASSERT_EQ(height, h2);

    const auto changed_at = [&](uint32_t x, uint32_t y) {
        const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
        return std::abs(static_cast<int>(before[b]) - static_cast<int>(after[b])) > 12
               || std::abs(static_cast<int>(before[b + 1U]) - static_cast<int>(after[b + 1U])) > 12
               || std::abs(static_cast<int>(before[b + 2U]) - static_cast<int>(after[b + 2U])) > 12;
    };

    // Same GUI-free upper-right scan region as the forward/RT goldens.
    const uint32_t scan_x0 = (width * 68U) / 100U;
    const uint32_t scan_y1 = (height * 62U) / 100U;

    uint32_t minx = width;
    uint32_t miny = height;
    uint32_t maxx = 0;
    uint32_t maxy = 0;
    size_t changed_total = 0;
    for (uint32_t y = 0; y < scan_y1; ++y) {
        for (uint32_t x = scan_x0; x < width; ++x) {
            if (changed_at(x, y)) {
                ++changed_total;
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            }
        }
    }

    if (const char *dump = std::getenv("KATAGLYPHIS_MASK_DUMP")) {
        const std::string base = dump;
        std::vector<uint8_t> diff(before.size(), 0U);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
                const bool in_scan = (x >= scan_x0 && y < scan_y1);
                const uint8_t v = changed_at(x, y) ? 255U : 0U;
                diff[b] = v;
                diff[b + 1U] = in_scan ? v : static_cast<uint8_t>(v / 3U);
                diff[b + 2U] = in_scan ? v : static_cast<uint8_t>(v / 3U);
                diff[b + 3U] = 255U;
            }
        }
        const auto stride = static_cast<int>(width) * 4;
        stbi_write_png((base + "-before.png").c_str(), int(width), int(height), 4, before.data(), stride);
        stbi_write_png((base + "-after.png").c_str(), int(width), int(height), 4, after.data(), stride);
        stbi_write_png((base + "-diff.png").c_str(), int(width), int(height), 4, diff.data(), stride);
    }

    ASSERT_GT(changed_total, 200U)
      << "the card barely changed the upper-right region - not visible there (framing/culling)";

    const double box_area = static_cast<double>(maxx - minx + 1U) * static_cast<double>(maxy - miny + 1U);
    const double changed_fraction = static_cast<double>(changed_total) / box_area;
    GTEST_LOG_(INFO) << "PT mask card: changed " << changed_total << " px in upper-right, bbox [" << minx << "," << miny
                     << ".." << maxx << "," << maxy << "] fraction-in-box " << changed_fraction;

    // RTX 2080, 2026-10-01: 0.346 with the ray query's alpha test, 0.627 with the query forced opaque.
    EXPECT_LT(changed_fraction, 0.49)
      << "the card footprint changed too fully in PT - cut-out texels are NOT being discarded";
    EXPECT_GT(changed_fraction, 0.15) << "the changed pixels are too sparse to be the checkerboard - check framing";
}

// The card shows its back face, so it is visible only if doubleSided reaches the per-draw cull mode.
TEST(GoldenRender, MaskCardDoubleSidedRendersFromBehind)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    const auto env_f = [](const char *name, float fallback) {
        const char *value = std::getenv(name);
        return (value != nullptr) ? std::strtof(value, nullptr) : fallback;
    };
    // Rotated 180 degrees about Y so the back face faces the camera.
    const glm::mat4 placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(env_f("MASK_X", 2.5F), env_f("MASK_Y", 4.0F), env_f("MASK_Z", 15.0F)))
      * glm::rotate(glm::mat4(1.0F), glm::radians(180.0F), glm::vec3(0.0F, 1.0F, 0.0F))
      * glm::scale(glm::mat4(1.0F), glm::vec3(env_f("MASK_SCALE", 2.0F)));
    const auto added = harness.renderer->addModel(MASK_CARD_MODEL, placement);
    ASSERT_TRUE(added.has_value()) << "adding the mask card failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t w2 = 0;
    uint32_t h2 = 0;
    const std::vector<uint8_t> after = harness.capture_frame(w2, h2);
    ASSERT_FALSE(after.empty());
    ASSERT_EQ(width, w2);
    ASSERT_EQ(height, h2);

    const auto changed_at = [&](uint32_t x, uint32_t y) {
        const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
        return std::abs(static_cast<int>(before[b]) - static_cast<int>(after[b])) > 12
               || std::abs(static_cast<int>(before[b + 1U]) - static_cast<int>(after[b + 1U])) > 12
               || std::abs(static_cast<int>(before[b + 2U]) - static_cast<int>(after[b + 2U])) > 12;
    };

    const uint32_t scan_x0 = (width * 68U) / 100U;
    const uint32_t scan_y1 = (height * 62U) / 100U;
    uint32_t minx = width;
    uint32_t miny = height;
    uint32_t maxx = 0;
    uint32_t maxy = 0;
    size_t changed_total = 0;
    for (uint32_t y = 0; y < scan_y1; ++y) {
        for (uint32_t x = scan_x0; x < width; ++x) {
            if (changed_at(x, y)) {
                ++changed_total;
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            }
        }
    }

    // Single-sided culling would leave changed_total near 0.
    ASSERT_GT(changed_total, 1500U) << "the back-facing doubleSided card is not visible - back-face culled, so "
                                       "the doubleSided flag is not reaching the per-draw cull mode";

    const double box_area = static_cast<double>(maxx - minx + 1U) * static_cast<double>(maxy - miny + 1U);
    const double changed_fraction = static_cast<double>(changed_total) / box_area;
    GTEST_LOG_(INFO) << "doubleSided card (from behind): changed " << changed_total << " px, fraction-in-box "
                     << changed_fraction;
    // And it is the cut-out checkerboard, not a solid blob or artifact.
    EXPECT_GT(changed_fraction, 0.20);
    EXPECT_LT(changed_fraction, 0.80);
}

// Culled out of the G-buffer, the card would never reach the lighting pass.
TEST(GoldenRender, MaskCardDoubleSidedRendersFromBehindDeferred)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.gui->getGuiRendererSharedVars();
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = false;
    renderer_vars.rasterizationMode = RasterizationMode::Deferred;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    const auto env_f = [](const char *name, float fallback) {
        const char *value = std::getenv(name);
        return (value != nullptr) ? std::strtof(value, nullptr) : fallback;
    };
    const glm::mat4 placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(env_f("MASK_X", 2.5F), env_f("MASK_Y", 4.0F), env_f("MASK_Z", 15.0F)))
      * glm::rotate(glm::mat4(1.0F), glm::radians(180.0F), glm::vec3(0.0F, 1.0F, 0.0F))
      * glm::scale(glm::mat4(1.0F), glm::vec3(env_f("MASK_SCALE", 2.0F)));
    const auto added = harness.renderer->addModel(MASK_CARD_MODEL, placement);
    ASSERT_TRUE(added.has_value()) << "adding the mask card failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t w2 = 0;
    uint32_t h2 = 0;
    const std::vector<uint8_t> after = harness.capture_frame(w2, h2);
    ASSERT_FALSE(after.empty());
    ASSERT_EQ(width, w2);
    ASSERT_EQ(height, h2);

    const auto changed_at = [&](uint32_t x, uint32_t y) {
        const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
        return std::abs(static_cast<int>(before[b]) - static_cast<int>(after[b])) > 12
               || std::abs(static_cast<int>(before[b + 1U]) - static_cast<int>(after[b + 1U])) > 12
               || std::abs(static_cast<int>(before[b + 2U]) - static_cast<int>(after[b + 2U])) > 12;
    };

    const uint32_t scan_x0 = (width * 68U) / 100U;
    const uint32_t scan_y1 = (height * 62U) / 100U;
    size_t changed_total = 0;
    for (uint32_t y = 0; y < scan_y1; ++y) {
        for (uint32_t x = scan_x0; x < width; ++x) {
            if (changed_at(x, y)) { ++changed_total; }
        }
    }
    GTEST_LOG_(INFO) << "doubleSided card (deferred, from behind): changed " << changed_total << " px";
    ASSERT_GT(changed_total, 1500U) << "the back-facing doubleSided card is not in the deferred G-buffer - the "
                                       "geometry pass back-face culled it despite doubleSided";
}

// Sampling repeats, so a 4x UV scale tiles the 8x8 checkerboard to 32x32, far more edges per area.
TEST(GoldenRender, KhrTextureTransformTilesTheTexture)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    const auto env_f = [](const char *name, float fallback) {
        const char *value = std::getenv(name);
        return (value != nullptr) ? std::strtof(value, nullptr) : fallback;
    };
    const glm::mat4 placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(env_f("MASK_X", 2.5F), env_f("MASK_Y", 4.0F), env_f("MASK_Z", 15.0F)))
      * glm::scale(glm::mat4(1.0F), glm::vec3(env_f("MASK_SCALE", 2.0F)));
    const auto added = harness.renderer->addModel(UV_TRANSFORM_MODEL, placement);
    ASSERT_TRUE(added.has_value()) << "adding the uv-transform card failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t w2 = 0;
    uint32_t h2 = 0;
    const std::vector<uint8_t> after = harness.capture_frame(w2, h2);
    ASSERT_FALSE(after.empty());
    ASSERT_EQ(width, w2);
    ASSERT_EQ(height, h2);

    // Locate the card: bounding box of changed pixels in the GUI-free upper-right.
    const auto changed_at = [&](uint32_t x, uint32_t y) {
        const size_t b = (static_cast<size_t>(y) * width + x) * 4U;
        return std::abs(static_cast<int>(before[b]) - static_cast<int>(after[b])) > 12
               || std::abs(static_cast<int>(before[b + 1U]) - static_cast<int>(after[b + 1U])) > 12
               || std::abs(static_cast<int>(before[b + 2U]) - static_cast<int>(after[b + 2U])) > 12;
    };
    const uint32_t scan_x0 = (width * 68U) / 100U;
    const uint32_t scan_y1 = (height * 62U) / 100U;
    uint32_t minx = width;
    uint32_t miny = height;
    uint32_t maxx = 0;
    uint32_t maxy = 0;
    size_t changed_total = 0;
    for (uint32_t y = 0; y < scan_y1; ++y) {
        for (uint32_t x = scan_x0; x < width; ++x) {
            if (changed_at(x, y)) {
                ++changed_total;
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            }
        }
    }
    ASSERT_GT(changed_total, 500U) << "the uv-transform card is not visible in the upper-right (framing)";

    // A finely tiled checkerboard is nearly all edges; an untransformed one is not.
    const double detail = detail_fraction(after, width, height, Crop{ minx, maxx + 1U, miny, maxy + 1U });
    GTEST_LOG_(INFO) << "uv-transform card: box [" << minx << "," << miny << ".." << maxx << "," << maxy
                     << "] detail-fraction " << detail;

    // The untransformed 8x8 card measures far lower.
    EXPECT_GT(detail, 0.15)
      << "the card is not finely tiled - KHR_texture_transform scale is not reaching the sampled UV";
}

// RT/PT see only the acceleration structure, so addModel must rebuild it or the model is invisible to them.
TEST(GoldenRender, AddedModelAppearsInPathTracing)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.gui->getGuiRendererSharedVars();
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    // The control gets the same accumulated history as the capture after the add, so noise reads alike in both.
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES + 60);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    const auto added = harness.renderer->addModel(UV_TRANSFORM_MODEL, panel_free_card_placement());
    ASSERT_TRUE(added.has_value()) << "adding the card failed";
    // The AS rebuild resets history; deepen it so noise does not swamp the detail.
    harness.render_frames(SETTLE_FRAMES);
    harness.render_frames(60);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(frame.empty());
    ASSERT_EQ(frame.size(), before.size());

    if (const char *dump = std::getenv("KATAGLYPHIS_MASK_DUMP")) {
        const std::string base = dump;
        stbi_write_png(
          (base + "-ptadd.png").c_str(), int(width), int(height), 4, frame.data(), static_cast<int>(width) * 4);
    }

    const Crop box = panel_free_card_box(width, height);
    const double detail_before = detail_fraction(before, width, height, box);
    const double detail = detail_fraction(frame, width, height, box);
    GTEST_LOG_(INFO) << "added-model PT card-box detail fraction: before the add " << detail_before << ", after "
                     << detail;

    // Before the add stands in for a missing AS rebuild. RTX 2080, 2026-10-01: 0.037 before, 0.328 after.
    constexpr double DETAIL_THRESHOLD = 0.15;
    EXPECT_LT(detail_before, DETAIL_THRESHOLD) << "the card box already has detail without the card - check framing";
    EXPECT_GT(detail, DETAIL_THRESHOLD)
      << "the runtime-added card is not visible in path tracing - the AS was not rebuilt to include it";
}

// The layer reads it at instance creation, so one harness gets sync validation whatever the lane's settings say.
class ScopedSyncValidation
{
  public:
    ScopedSyncValidation() { set("1"); }
    ScopedSyncValidation(const ScopedSyncValidation &) = delete;
    ScopedSyncValidation &operator=(const ScopedSyncValidation &) = delete;
    ~ScopedSyncValidation() { set(""); }

  private:
    static void set(const char *value)
    {
#ifdef _WIN32
        std::ignore = _putenv_s("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", value);
#else
        if (*value == '\0') {
            std::ignore = unsetenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC");
        } else {
            std::ignore = setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", value, 1);
        }
#endif
    }
};

// Both wrote without a barrier before: the deferred pass's G-buffer stores and back-to-back BLAS builds on one scratch.
TEST(GoldenRender, DeferredFramesAndBlasRebuildsAreFreeOfSyncHazards)
{
    SKIP_WITHOUT_GPU();

    ScopedSyncValidation sync_validation;
    EngineHarness harness;
    harness.useForwardRaster().rasterizationMode = RasterizationMode::Deferred;
    // From the first frame: the layer reports one message id ten times, then goes quiet, so warmup would spend them.
    ScopedValidationErrorCounter validation_errors;
    harness.render_frames(WARMUP_FRAMES);
    // A second model rebuilds every BLAS in one command buffer through one scratch buffer.
    const auto added = harness.renderer->addModel(UV_TRANSFORM_MODEL, panel_free_card_placement());
    ASSERT_TRUE(added.has_value()) << "adding the second model failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost in the deferred path.";

    EXPECT_EQ(validation_errors.errorCount(), 0)
      << "synchronization validation logged an error - SYNC-HAZARD-WRITE-AFTER-WRITE if a deferred G-buffer store "
         "or a BLAS build's scratch write lost its barrier (DeferredRasterizer.cpp, ASManager.cpp)";
}

// Drivers tolerate a descriptor on the destroyed TLAS, so validation errors are the primary oracle.
TEST(GoldenRender, ReloadedModelIsVisibleInPathTracing)
{
    SKIP_WITHOUT_GPU();

    // A small scene known good in PT; the dinosaur's large BLAS is not what this measures.
    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.gui->getGuiRendererSharedVars();
    auto &scene_vars = harness.gui->getGuiSceneSharedVars();
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(before.empty());

    // The same flags the GUI's model combo box sets.
    const auto model_paths = sceneConfig::getAvailableModelPaths();
    int reload_index = -1;
    for (size_t i = 0; i < model_paths.size(); ++i) {
        if (std::filesystem::path(model_paths[i]) == std::filesystem::path(UV_TRANSFORM_MODEL)) {
            reload_index = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(reload_index, 0) << UV_TRANSFORM_MODEL << " not found among the scanned models";

    ScopedValidationErrorCounter validation_errors;
    scene_vars.selected_model_index = reload_index;
    scene_vars.model_reload_requested = true;
    harness.render_frames(SETTLE_FRAMES);
    // The AS rebuild resets history; deepen it so noise does not swamp the measurement.
    harness.render_frames(60);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    EXPECT_EQ(validation_errors.errorCount(), 0)
      << "the reload logged a Vulkan validation error - almost certainly the "
         "raytracing descriptor set still referencing the TLAS the AS rebuild destroyed";

    const std::vector<uint8_t> after = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(before.size(), after.size());

    const double swung = swung_fraction(before, after, width, height, panel_free_crop(width, height));
    GTEST_LOG_(INFO) << "reload PT before/after swung-pixel fraction (panel-free crop): " << swung;

    EXPECT_GT(swung, 0.05) << "the reloaded model is not visible in path tracing - the scene "
                              "looks unchanged after the reload";
}

// Unbiased means every pixel hits linear_to_srgb(aces(1.0)) * 255; post.slang encodes for the UNORM swapchain.
TEST(GoldenRender, PathTracingPassesTheWhiteFurnaceTest)
{
    SKIP_WITHOUT_GPU();

    // Same env-scoping pattern as ScopedModelOverride.
    struct ScopedFurnace
    {
        ScopedFurnace() { set("1.0"); }
        ~ScopedFurnace() { set(""); }
        // _putenv_s is Windows-only, hence ScopedModelOverride's portable split.
        static void set(const char *value)
        {
#ifdef _WIN32
            std::ignore = _putenv_s("KATAGLYPHIS_PT_FURNACE", value);
#else
            if (*value == '\0') {
                std::ignore = unsetenv("KATAGLYPHIS_PT_FURNACE");
            } else {
                std::ignore = setenv("KATAGLYPHIS_PT_FURNACE", value, 1);
            }
#endif
        }
    } furnace;

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.gui->getGuiRendererSharedVars();
    auto &scene_vars = harness.gui->getGuiSceneSharedVars();
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    // NEE would add energy on top of the furnace; many bounces keep truncation loss small.
    scene_vars.directional_light_radiance = 0.0F;
    renderer_vars.pathTracingMaxBounces = 16;
    harness.render_frames(WARMUP_FRAMES);

    // Accumulate deep so per-frame noise is averaged well below the band.
    harness.render_frames(80);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(frame.empty());

    const uint32_t x0 = (width * 18U) / 25U;
    const uint32_t x1 = (width * 49U) / 50U;
    const uint32_t y0 = height / 20U;
    const uint32_t y1 = (height * 19U) / 20U;
    double sum = 0.0;
    size_t within = 0;
    size_t total = 0;
    for (uint32_t y = y0; y < y1; ++y) {
        for (uint32_t x = x0; x < x1; ++x) {
            const double lum = luminance_of(frame, static_cast<size_t>(y) * width + x);
            sum += lum;
            if (std::abs(lum - 231.6) <= 6.0) { ++within; }
            ++total;
        }
    }
    const double mean = sum / static_cast<double>(total);
    const double uniform_fraction = static_cast<double>(within) / static_cast<double>(total);
    GTEST_LOG_(INFO) << "furnace crop mean luminance: " << mean
                     << " (ideal 231.6), fraction within +-6: " << uniform_fraction;

    EXPECT_GT(mean, 225.6) << "Furnace converges LOW - the estimator is losing energy "
                              "(beyond the known bounce-cap truncation).";
    EXPECT_LT(mean, 237.6) << "Furnace converges HIGH - the estimator is gaining energy.";
    EXPECT_GT(uniform_fraction, 0.98) << "The furnace image is not uniform - geometry is visible, so some path "
                                         "class is biased.";
}

namespace {
// Fixed-seed PRNG so a failing sweep iteration can be re-hit exactly.
struct SweepRng
{
    uint64_t state;
    uint32_t next()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(state >> 32U);
    }
    float uf(float lo, float hi) { return lo + (hi - lo) * (static_cast<float>(next()) / 4294967296.0F); }
    int ui(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1)); }
    bool ub() { return (next() & 1U) != 0U; }
};
}// namespace

// Random combinations within GUI.cpp's slider limits; no pixels asserted, since every state differs.
TEST(GoldenRender, GuiInputSweepNeverCrashesOrLosesTheDevice)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    const bool rt_supported = harness.renderer->supportsHardwareRaytracing();

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    auto &s = harness.gui->getGuiSceneSharedVars();
    auto &r = harness.gui->getGuiRendererSharedVars();

    // Random sampling almost never hits the all-maximum corner, the state most likely to exhaust the GPU.
    if (rt_supported) {
        r.pathTracing = true;
        r.raytracing = false;
    }
    r.rasterizationMode = RasterizationMode::Deferred;
    r.pathTracingSamplesPerPixel = 64;
    r.pathTracingMaxBounces = 16;
    r.frustum_culling_enabled = false;
    s.skybox_enabled = true;
    s.directional_light_radiance = 50.0F;
    s.shadows_enabled = true;
    s.num_shadow_cascades = 8;
    s.shadow_map_res_index = 3;// 4096
    s.pcf_radius = MAX_PCF_RADIUS;
    s.cascaded_shadow_intensity = 1.0F;
    s.shadow_distance = 200.0F;
    s.shadow_resolution_changed = true;
    s.clouds_enabled = true;
    s.cloud_num_march_steps = 128;
    s.cloud_num_march_steps_to_light = 128;
    s.cloud_density_multiplier = 1.0F;
    s.cloud_coverage_threshold = 1.0F;
    s.cloud_pillowness = 1.0F;
    s.cloud_cirrus_effect = 1.0F;
    s.cloud_powder_effect = true;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost on the all-maximum GUI state";
    {
        uint32_t bw = 0;
        uint32_t bh = 0;
        const std::vector<uint8_t> heavy_frame = harness.capture_frame(bw, bh);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost capturing the all-maximum GUI state";
        ASSERT_FALSE(heavy_frame.empty()) << "Empty frame on the all-maximum GUI state";
    }

    SweepRng rng{ 0x9E3779B97F4A7C15ULL };// fixed seed -> deterministic sweep

    constexpr int SWEEP_ITERATIONS = 16;
    for (int iter = 0; iter < SWEEP_ITERATIONS; ++iter) {
        // Modes are mutually exclusive, as the engine drives them.
        const int mode = rng.ui(0, rt_supported ? 3 : 1);
        r.raytracing = (mode == 2);
        r.pathTracing = (mode == 3);
        r.rasterizationMode = (mode == 1) ? RasterizationMode::Deferred : RasterizationMode::Forward;
        r.frustum_culling_enabled = rng.ub();
        r.pathTracingSamplesPerPixel = rng.ui(1, 64);
        r.pathTracingMaxBounces = rng.ui(1, 16);

        s.skybox_enabled = rng.ub();
        s.directional_light_radiance = rng.uf(0.0F, 50.0F);
        for (float &c : s.directional_light_color) { c = rng.uf(0.0F, 1.0F); }
        for (float &d : s.directional_light_direction) { d = rng.uf(-1.0F, 1.0F); }

        s.shadows_enabled = rng.ub();
        s.num_shadow_cascades = rng.ui(1, 8);// slider allows 1..8 (clamped to MAX_CASCADES in use)
        s.shadow_map_res_index = rng.ui(0, 3);// combo: 512 / 1024 / 2048 / 4096
        s.pcf_radius = rng.ui(1, MAX_PCF_RADIUS);
        s.cascaded_shadow_intensity = rng.uf(0.0F, 1.0F);
        s.shadow_distance = rng.uf(1.0F, 200.0F);
        // Force the shadow map to honour the new cascade count / resolution.
        s.shadow_resolution_changed = true;

        s.clouds_enabled = rng.ub();
        s.cloud_num_march_steps = rng.ui(1, 128);
        s.cloud_num_march_steps_to_light = rng.ui(1, 128);
        s.cloud_density_multiplier = rng.uf(0.0F, 1.0F);
        s.cloud_coverage_threshold = rng.uf(0.0F, 1.0F);
        s.cloud_pillowness = rng.uf(0.0F, 1.0F);
        s.cloud_cirrus_effect = rng.uf(0.0F, 1.0F);
        s.cloud_powder_effect = rng.ub();

        const std::string desc = "iter " + std::to_string(iter) + ": mode=" + std::to_string(mode)
                                 + " shadows=" + std::to_string(static_cast<int>(s.shadows_enabled))
                                 + " cascades=" + std::to_string(s.num_shadow_cascades)
                                 + " res=" + std::to_string(s.shadow_map_res_index)
                                 + " clouds=" + std::to_string(static_cast<int>(s.clouds_enabled))
                                 + " radiance=" + std::to_string(s.directional_light_radiance);

        // A few frames let a shadow-map re-init settle.
        harness.render_frames(SETTLE_FRAMES);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while rendering " << desc;

        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<uint8_t> frame = harness.capture_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while capturing " << desc;
        ASSERT_FALSE(frame.empty()) << "Empty frame for " << desc;
    }
}

// Nothing else recreates the swapchain, so a dangling fence or stale framebuffer would pass every other test.
TEST(GoldenRender, SwapchainRecreationKeepsRendering)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());
    const double before_luma = mean_luminance(before);

    // The window does not resize, so the recreated frame must be equivalent.
    harness.renderer->recreateSwapChain();
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during swapchain recreation.";

    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost after swapchain recreation.";

    uint32_t width2 = 0;
    uint32_t height2 = 0;
    const std::vector<uint8_t> after = harness.capture_frame(width2, height2);
    ASSERT_FALSE(after.empty());
    EXPECT_EQ(width, width2);
    EXPECT_EQ(height, height2);

    // The tolerance absorbs overlay jitter; a black or half-torn frame swings far beyond it.
    const double after_luma = mean_luminance(after);
    EXPECT_NEAR(after_luma, before_luma, 20.0)
      << "Mean luminance changed too much across swapchain recreation (before=" << before_luma
      << ", after=" << after_luma << ") - the recreated swapchain is not rendering the same scene.";
}

// A cross-frame hazard shows as a hang, not a pixel; see docs/clouds.md § Queue ownership and barriers.
TEST(GoldenRender, CloudsAcrossManyFramesDoesNotLoseTheDevice)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    auto &scene_vars = harness.gui->getGuiSceneSharedVars();
    scene_vars.clouds_enabled = true;

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up with clouds enabled.";

    // Well past MAX_FRAME_DRAWS, so a cross-frame hazard gets several chances to surface.
    constexpr int CLOUD_FRAMES = 30;
    harness.render_frames(CLOUD_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost rendering repeated frames with clouds enabled.";

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
    ASSERT_FALSE(frame.empty()) << "Frame capture returned no pixels.";
}

// A transparent or flat-wash cloud buffer passes the device-survival test; this requires added detail.
TEST(GoldenRender, EnablingCloudsChangesTheFrameAndAddsDetail)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    auto &scene_vars = harness.gui->getGuiSceneSharedVars();
    scene_vars.clouds_enabled = false;

    // Test-only: the default slab sits far above the camera's eye line, and default wisps are too faint to measure.
    scene_vars.cloud_mesh_offset[1] = 6.0F;
    scene_vars.cloud_coverage_threshold = 0.60F;
    scene_vars.cloud_density_multiplier = 2.0F;

    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> baseline = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost capturing the clouds-off baseline.";
    ASSERT_FALSE(baseline.empty()) << "Baseline capture returned no pixels.";

    // Same harness, same camera: only the clouds toggle changes.
    scene_vars.clouds_enabled = true;
    constexpr int CLOUD_SETTLE_FRAMES = 30;
    harness.render_frames(CLOUD_SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost rendering with clouds enabled.";

    uint32_t width2 = 0;
    uint32_t height2 = 0;
    const std::vector<uint8_t> clouds = harness.capture_frame(width2, height2);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost capturing the clouds-on frame.";
    ASSERT_FALSE(clouds.empty()) << "Clouds-on capture returned no pixels.";
    ASSERT_EQ(width, width2);
    ASSERT_EQ(height, height2);

    if (const char *dump = std::getenv("KATAGLYPHIS_CLOUDS_DUMP")) {
        const std::string base = dump;
        const auto stride = static_cast<int>(width) * 4;
        stbi_write_png((base + "-baseline.png").c_str(), int(width), int(height), 4, baseline.data(), stride);
        stbi_write_png((base + "-clouds.png").c_str(), int(width), int(height), 4, clouds.data(), stride);
    }

    // Upper half only: below is geometry whose sharp skeleton edges would inflate the clouds-off baseline.
    const Crop panel_free = panel_free_crop(width, height);
    const Crop crop{
        panel_free.x0, panel_free.x1, panel_free.y0, panel_free.y0 + (panel_free.y1 - panel_free.y0) / 2U
    };
    const double swung = swung_fraction(baseline, clouds, width, height, crop);
    const double baseline_detail = detail_fraction(baseline, width, height, crop);
    const double clouds_detail = detail_fraction(clouds, width, height, crop);

    GTEST_LOG_(INFO) << "clouds toggle: swung fraction " << swung << ", detail baseline " << baseline_detail
                     << ", detail clouds " << clouds_detail;

    // Measured; re-measure if the rig, camera or cloud_mesh_offset default changes.
    constexpr double SWUNG_FRACTION_MIN = 0.05;
    EXPECT_GT(swung, SWUNG_FRACTION_MIN)
      << "Enabling clouds barely changed the panel-free crop; the cloud box may be out of view "
         "(scene_vars.cloud_mesh_offset) or the clouds pass is not writing pixels at all.";

    // A flat noise volume composites as a smooth wash and lowers detail, with no driver-specific value pinned.
    EXPECT_GT(clouds_detail, baseline_detail)
      << "Clouds did not add pixel-to-pixel detail over the clouds-off baseline (baseline=" << baseline_detail
      << ", clouds=" << clouds_detail << "); the noise volume may be flat or uniformly filled again.";
}

// Keep LAST in the file: a device-lost abort here kills every later test in the process.
TEST(GoldenRender, RaytracedLargeMeshDoesNotLoseTheDevice)
{
    SKIP_WITHOUT_GPU();

    // No override: the debug scene is the large dinosaur mesh this diagnoses.
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.gui->getGuiRendererSharedVars();
    renderer_vars.raytracing = true;
    renderer_vars.pathTracing = false;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    // The PT reproducer device-loses within 0-2 frames.
    constexpr int DIAGNOSTIC_FRAMES = 10;
    harness.render_frames(DIAGNOSTIC_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost())
      << "RT pipeline device-lost on the large dinosaur mesh - the bug is in the shared "
         "vertex-upload/BDA path, not path_tracing.slang/RayQuery specifically.";

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
    ASSERT_FALSE(frame.empty()) << "Frame capture returned no pixels.";
    EXPECT_GT(fraction_above(frame, 8.0), 0.05)
      << "Captured frame looks blank - the large-mesh RT diagnostic needs a real image to "
         "trust the no-device-lost result.";
}
