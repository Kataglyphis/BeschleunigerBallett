#pragma once

// The goldens' engine harness: it imports the engine, so it is a file's last include (gpu-golden-testing.md).

#include "EngineFrame.hpp"
#include "EngineLoadWait.hpp"
#include "GltfVariant.hpp"
#include "GoldenMetrics.hpp"
#include "GpuTestGuard.hpp"
#include "RepoFiles.hpp"
#include "ScopedEnvVar.hpp"
#include "common/EnvironmentVariable.hpp"
#include "common/GuiModelTransform.hpp"
#include "common/ImageBarrierHelper.hpp"
#include "common/RenderingHelper.hpp"
#include "common/SceneUboMarshal.hpp"
#include "common/host_device_shared_vars.hpp"
#include "scene/TextureFit.hpp"
#include "scene/sky_box/AnalyticSky.hpp"
#include "scene/sky_box/CloudAmbient.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>
#include <iostream>
#include <vulkan/vulkan.hpp>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <numbers>
#include <numeric>
#include <optional>
#include <span>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.cascaded_shadow_map;
import kataglyphis.vulkan.gpu_timing;
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.render_settings;
import kataglyphis.vulkan.render_stage_registry;
import kataglyphis.vulkan.renderer;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.scene_config;
import kataglyphis.vulkan.window;

namespace Kataglyphis::TestSupport::GoldenRender {

using Kataglyphis::VulkanRendererInternals::FrontendShared::GpuTimedPass;
using Kataglyphis::VulkanRendererInternals::FrontendShared::FxaaMode;
using Kataglyphis::VulkanRendererInternals::FrontendShared::GUIRendererSharedVars;
using Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode;
using Kataglyphis::Test::GoldenMetrics::Crop;
using Kataglyphis::Test::GoldenMetrics::GroundPixel;
using Kataglyphis::Test::GoldenMetrics::GroundRect;
using Kataglyphis::Test::GoldenMetrics::GroundShadowPrediction;
using Kataglyphis::Test::GoldenMetrics::predict_ground_shadow;
using Kataglyphis::Test::GoldenMetrics::WorldBox;
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

using Kataglyphis::TestSupport::GltfVariant;
using Kataglyphis::TestSupport::ScopedEnvVar;

// Pick a scene for what it measures: the debug skeleton's thin bones leave most PCF taps unoccluded.
class ScopedModelOverride
{
  public:
    explicit ScopedModelOverride(const char *relative_path) : env_("KATAGLYPHIS_MODEL_OVERRIDE", relative_path) {}

  private:
    ScopedEnvVar env_;
};

// The goldens were calibrated on the Debug app's scene and framing, so the harness pins both, whatever the build type.
constexpr const char *GOLDEN_SCENE_MODEL = "Models/Dinosaurs/dinosaurs.obj";

inline void frame_golden_view(Camera &camera)
{
    camera.set_camera_position(glm::vec3(0.0F, 6.0F, 26.0F));
    camera.set_orientation(-90.0F, -10.0F);
    camera.set_near_plane(0.1F);
    camera.set_far_plane(150.0F);
    camera.set_fov(45.0F);
}

// Declared in App::run's order, so the default destruction tears down as App::run does.
struct EngineHarness
{
    // Only when the test picked no scene of its own; ScopedModelOverride clears the variable on exit.
    std::optional<ScopedModelOverride> golden_scene;
    // App-owned in App::run: tests drive the settings and read the feedback as the editor does.
    Kataglyphis::RenderSettings settings;
    Kataglyphis::RendererFeedback feedback;
    std::unique_ptr<Kataglyphis::Frontend::Window> window;
    std::unique_ptr<Kataglyphis::Scene> scene;
    std::unique_ptr<Kataglyphis::Frontend::GUI> gui;
    std::unique_ptr<Camera> camera;
    std::unique_ptr<Kataglyphis::VulkanRenderer> renderer;

    EngineHarness()
      : window(std::make_unique<Kataglyphis::Frontend::Window>(WINDOW_WIDTH, WINDOW_HEIGHT)),
        scene(std::make_unique<Kataglyphis::Scene>()),
        gui(std::make_unique<Kataglyphis::Frontend::GUI>(window.get(), settings, feedback)),
        camera(std::make_unique<Camera>())
    {
        if (!Kataglyphis::environmentFlag("KATAGLYPHIS_MODEL_OVERRIDE")) { golden_scene.emplace(GOLDEN_SCENE_MODEL); }
        frame_golden_view(*camera);
        // The goldens frame the whole image; the docked editor would shrink the scene into its viewport.
        gui->setOverlayVisible(false);
        // The goldens grade pixels FXAA would move; FxaaSmoothsTheRasterStaircaseAndLeavesFlatRegionsAlone turns it on.
        settings.renderer.fxaaMode = FxaaMode::Off;
        // Frames unpaced by the display, as before; VsyncTogglesBetweenFifoAndAnUncappedMode turns VSync on.
        settings.renderer.vsync = false;
        renderer =
          std::make_unique<Kataglyphis::VulkanRenderer>(window.get(), scene.get(), camera.get(), settings, feedback);
        Kataglyphis::TestSupport::attachEditor(*gui, *renderer);
    }

    EngineHarness(const EngineHarness &) = delete;
    EngineHarness &operator=(const EngineHarness &) = delete;

    // One iteration of the App::run frame loop (App.cpp) minus camera input.
    void render_frame()
    {
        glfwPollEvents();
        Kataglyphis::TestSupport::renderEditorFrame(*gui, *renderer, scene.get(), camera.get(), settings, feedback);
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

    // Linear RGBA radiance as post read it, before the tonemap compresses what a bound has to tell apart.
    std::vector<float> capture_hdr_frame(uint32_t &width, uint32_t &height)
    {
        renderer->requestHdrCapture();
        render_frame();
        const std::vector<uint8_t> raw = renderer->takeCapturedHdrFrame(width, height);
        std::vector<float> rgba(raw.size() / 2U);
        for (size_t i = 0; i < rgba.size(); ++i) {
            const auto half = static_cast<uint16_t>(raw[2U * i] | (raw[2U * i + 1U] << 8U));
            rgba[i] = glm::unpackHalf1x16(half);
        }
        return rgba;
    }

    [[nodiscard]] bool supportsFrameCapture() const { return renderer->supportsFrameCapture(); }

    GUIRendererSharedVars &useForwardRaster()
    {
        auto &renderer_vars = settings.renderer;
        renderer_vars.raytracing = false;
        renderer_vars.pathTracing = false;
        renderer_vars.rasterizationMode = RasterizationMode::Forward;
        return renderer_vars;
    }

    // Members go in reverse declaration order, the renderer first, as App::run's locals do.
    ~EngineHarness() = default;
};

// Mean luminance over the whole frame, on a 0..255 scale.
inline double mean_luminance(const std::vector<uint8_t> &rgba)
{
    const size_t pixel_count = rgba.size() / 4U;
    if (pixel_count == 0U) { return 0.0; }

    double sum = 0.0;
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) { sum += luminance_of(rgba, pixel); }
    return sum / static_cast<double>(pixel_count);
}

inline double luminance_stddev(const std::vector<uint8_t> &rgba)
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

inline double fraction_above(const std::vector<uint8_t> &rgba, double threshold)
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
inline size_t distinct_luminance_buckets(const std::vector<uint8_t> &rgba)
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
constexpr const char *UV_TRANSFORM_MODEL = "Models/GltfTest/uv_transform_card.gltf";
// Near-black base colour, emissiveFactor (1,1,1) - glows regardless of light.
constexpr const char *EMISSIVE_CARD_MODEL = "Models/GltfTest/emissive_card.gltf";
// emissive_strength 4.0 overflows an 8-bit UNORM G-buffer channel.
constexpr const char *EMISSIVE_STRENGTH_CARD_MODEL = "Models/GltfTest/emissive_strength_card.gltf";
// One metallic-roughness texel (0, 0, 0): roughness and metallic both 0 whatever their factors.
constexpr const char *ZERO_ROUGHNESS_PNG =
  "data:image/png;base64,"
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGNgYGAAAAAEAAH2FzhVAAAAAElFTkSuQmCC";

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

// "light" is run-riscv64-tests.sh's QEMU frame budget; docs/source/gpu-golden-testing.md § The riscv64 light variant.
inline int path_traced_history(int full_frames, int light_frames)
{
    std::string variant = Kataglyphis::environmentVariable("KATAGLYPHIS_PT_GOLDEN_VARIANT").value_or("");
    if (variant.empty()) { variant = "full"; }
    EXPECT_TRUE(variant == "full" || variant == "light")
      << "KATAGLYPHIS_PT_GOLDEN_VARIANT='" << variant << "' is neither 'full' nor 'light'";
    const int frames = variant == "light" ? light_frames : full_frames;
    GTEST_LOG_(INFO) << "path-traced variant: " << variant << ", " << frames << "-frame history (full " << full_frames
                     << ", light " << light_frames << ")";
    return frames;
}

// A fixture resolved like any model, copied privately with an edit; keep the handle until the model has loaded.
inline std::optional<GltfVariant>
  write_gltf_variant(const char *fixture, const char *file_name, const std::function<void(nlohmann::json &)> &edit)
{ return GltfVariant::create(sceneConfig::resolveModelPath(fixture), file_name, edit); }

// Largest per-channel difference of one pixel between two RGBA8 captures.
inline int channel_delta(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, size_t pixel)
{
    int delta = 0;
    for (size_t c = 0; c < 3U; ++c) {
        delta = std::max(delta, std::abs(static_cast<int>(a[pixel * 4U + c]) - static_cast<int>(b[pixel * 4U + c])));
    }
    return delta;
}

// sky.slang's disabled sky, linear (0, 0, 0.5) on screen, as the UNORM swapchain stores it.
inline bool is_disabled_sky(const std::vector<uint8_t> &rgba, size_t pixel)
{
    const size_t base = pixel * 4U;
    return rgba[base] <= 2U && rgba[base + 1U] <= 2U && std::abs(static_cast<int>(rgba[base + 2U]) - 188) <= 2;
}

// Pixels whose 3x3 neighbourhood is all `mask`, so an edge a traced pass samples differently drops out.
inline std::vector<bool> eroded(const std::vector<bool> &mask, uint32_t w, uint32_t h)
{
    std::vector<bool> result(mask.size(), false);
    for (uint32_t y = 1; y + 1U < h; ++y) {
        for (uint32_t x = 1; x + 1U < w; ++x) {
            bool all = true;
            for (int dy = -1; dy <= 1 && all; ++dy) {
                for (int dx = -1; dx <= 1 && all; ++dx) {
                    all = mask[static_cast<size_t>(static_cast<int>(y) + dy) * w
                               + static_cast<size_t>(static_cast<int>(x) + dx)];
                }
            }
            result[static_cast<size_t>(y) * w + x] = all;
        }
    }
    return result;
}

// KATAGLYPHIS_CLOUDS_DUMP=<prefix> writes <prefix>-<name>.png, for looking at what a cloud golden measured.
inline void dump_cloud_frame(const std::string &name, const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h)
{
    const std::string prefix = Kataglyphis::environmentVariable("KATAGLYPHIS_CLOUDS_DUMP").value_or("");
    if (prefix.empty() || rgba.empty()) { return; }
    stbi_write_png((prefix + "-" + name + ".png").c_str(),
      static_cast<int>(w),
      static_cast<int>(h),
      4,
      rgba.data(),
      static_cast<int>(w) * 4);
}

// Mean red, green or blue radiance of an RGBA float capture over a square around (cx, cy).
inline double mean_hdr_in_square(const std::vector<float> &rgba, uint32_t w, uint32_t cx, uint32_t cy, uint32_t half)
{
    double sum = 0.0;
    size_t count = 0;
    for (uint32_t y = cy - half; y < cy + half; ++y) {
        for (uint32_t x = cx - half; x < cx + half; ++x) {
            const size_t base = (static_cast<size_t>(y) * w + x) * 4U;
            sum += static_cast<double>(rgba[base] + rgba[base + 1U] + rgba[base + 2U]) / 3.0;
            ++count;
        }
    }
    return count > 0U ? sum / static_cast<double>(count) : 0.0;
}

// Mean linear radiance of each channel of an RGBA float capture over a square around (cx, cy).
inline glm::dvec4
  mean_hdr_rgba_in_square(const std::vector<float> &rgba, uint32_t w, uint32_t cx, uint32_t cy, uint32_t half)
{
    glm::dvec4 sum(0.0);
    size_t count = 0;
    for (uint32_t y = cy - half; y < cy + half; ++y) {
        for (uint32_t x = cx - half; x < cx + half; ++x) {
            const size_t base = (static_cast<size_t>(y) * w + x) * 4U;
            sum += glm::dvec4(rgba[base], rgba[base + 1U], rgba[base + 2U], rgba[base + 3U]);
            ++count;
        }
    }
    return count > 0U ? sum / static_cast<double>(count) : sum;
}

// Pixel column of world-space x on a camera looking along -Z (or +Z, mirrored) from `distance`, at the golden fov.
inline uint32_t card_column(float world_x, float distance, uint32_t width, uint32_t height)
{
    const float half_width =
      distance * std::tan(glm::radians(45.0F) * 0.5F) * static_cast<float>(width) / static_cast<float>(height);
    return static_cast<uint32_t>((0.5F + 0.5F * world_x / half_width) * static_cast<float>(width));
}

// Mean Rec. 709 luma of an RGBA8 capture over a square around (cx, cy).
inline double
  mean_luminance_in_square(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t cx, uint32_t cy, uint32_t half)
{
    double sum = 0.0;
    size_t count = 0;
    for (uint32_t y = cy - half; y < cy + half; ++y) {
        for (uint32_t x = cx - half; x < cx + half; ++x) {
            sum += luminance_of(rgba, static_cast<size_t>(y) * w + x);
            ++count;
        }
    }
    return count > 0U ? sum / static_cast<double>(count) : 0.0;
}

// Models/ShadowTest/shadow_rig.obj at the identity transform.
constexpr GroundRect SHADOW_RIG_GROUND{ 0.0F, -30.0F, 30.0F, -30.0F, 30.0F };
constexpr WorldBox SHADOW_RIG_SLAB{ glm::vec3(-22.0F, 8.0F, -10.0F), glm::vec3(22.0F, 10.0F, 2.0F) };

// The renderer's own camera matrices (VulkanRenderer::updateUniforms), so a predicted pixel is the rendered one.
inline glm::mat4 harness_inverse_view_projection(EngineHarness &harness, uint32_t width, uint32_t height)
{
    const auto &viewport = harness.settings.renderer.scene_viewport;
    const Kataglyphis::ViewportFraction rect =
      Kataglyphis::sanitizeViewportFraction(viewport.x0, viewport.y0, viewport.x1, viewport.y1);
    Camera &camera = *harness.camera;
    const glm::mat4 projection = Kataglyphis::makeViewportRemap(rect)
                                 * Kataglyphis::makeVulkanProjection(camera.get_fov(),
                                   Kataglyphis::viewportAspectRatio(width, height, rect),
                                   camera.get_near_plane(),
                                   camera.get_far_plane());
    return glm::inverse(projection * camera.calculate_viewmatrix());
}

// x = 5 frames the card at 78-92% of the width, inside card_crop; the mask cards' 2.5 straddles the panel edge.
inline glm::mat4 panel_free_card_placement()
{
    const auto env_f = [](const char *name, float fallback) {
        const auto value = Kataglyphis::environmentVariable(name);
        return value ? std::strtof(value->c_str(), nullptr) : fallback;
    };
    return glm::translate(
             glm::mat4(1.0F), glm::vec3(env_f("CARD_X", 5.0F), env_f("MASK_Y", 4.0F), env_f("MASK_Z", 15.0F)))
           * glm::scale(glm::mat4(1.0F), glm::vec3(env_f("MASK_SCALE", 2.0F)));
}

// The inside of the card panel_free_card_placement frames, so a mean measures the card and not the sky around it.
inline Crop panel_free_card_box(uint32_t w, uint32_t h)
{ return Crop{ (w * 80U) / 100U, (w * 90U) / 100U, (h * 42U) / 100U, (h * 58U) / 100U }; }

// The emissive card's quad as one glTF node per entry, each with its own material and so its own mesh.
inline void make_card_scene(nlohmann::json &gltf,
  const std::vector<nlohmann::json> &materials,
  const std::vector<std::pair<size_t, nlohmann::json>> &nodes)
{
    const nlohmann::json primitive = gltf["meshes"][0]["primitives"][0];
    gltf["materials"] = materials;
    gltf["meshes"] = nlohmann::json::array();
    for (size_t m = 0; m < materials.size(); ++m) {
        nlohmann::json mesh_primitive = primitive;
        mesh_primitive["material"] = m;
        gltf["meshes"].push_back({ { "primitives", nlohmann::json::array({ mesh_primitive }) } });
    }
    gltf["nodes"] = nlohmann::json::array();
    gltf["scenes"][0]["nodes"] = nlohmann::json::array();
    for (const auto &[mesh, transform] : nodes) {
        nlohmann::json node = transform;
        node["mesh"] = mesh;
        gltf["scenes"][0]["nodes"].push_back(gltf["nodes"].size());
        gltf["nodes"].push_back(node);
    }
}

// The display byte post writes for an HDR radiance: ACES, then sRGB, as aces.slang and post.slang encode it.
inline int displayed_byte(float radiance)
{
    const float x = std::max(radiance, 0.0F);
    const float tonemapped = std::clamp((x * (2.51F * x + 0.03F)) / (x * (2.43F * x + 0.59F) + 0.14F), 0.0F, 1.0F);
    const float encoded =
      tonemapped <= 0.0031308F ? tonemapped * 12.92F : 1.055F * std::pow(tonemapped, 1.0F / 2.4F) - 0.055F;
    return static_cast<int>(std::lround(encoded * 255.0F));
}

}// namespace Kataglyphis::TestSupport::GoldenRender
