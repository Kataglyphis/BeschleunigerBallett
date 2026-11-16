// Structure only, never exact pixels; see docs/source/gpu-golden-testing.md § Writing a new golden test.

#include "EngineFrame.hpp"
#include "EngineLoadWait.hpp"
#include "GltfVariant.hpp"
#include "GoldenMetrics.hpp"
#include "ScopedEnvVar.hpp"
#include "common/GuiModelTransform.hpp"
#include "common/SceneUboMarshal.hpp"
#include "common/host_device_shared_vars.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>
#include <iostream>
#include <vulkan/vulkan.hpp>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.cascaded_shadow_map;
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.render_settings;
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
using Kataglyphis::Test::GoldenMetrics::compare_shadow_masks;
using Kataglyphis::Test::GoldenMetrics::GroundPixel;
using Kataglyphis::Test::GoldenMetrics::GroundRect;
using Kataglyphis::Test::GoldenMetrics::GroundShadowPrediction;
using Kataglyphis::Test::GoldenMetrics::post_display_level;
using Kataglyphis::Test::GoldenMetrics::predict_ground_shadow;
using Kataglyphis::Test::GoldenMetrics::ShadowAgreement;
using Kataglyphis::Test::GoldenMetrics::WorldBox;
using Kataglyphis::Test::GoldenMetrics::detail_fraction;
using Kataglyphis::Test::GoldenMetrics::luminance_of;
using Kataglyphis::Test::GoldenMetrics::mean_luminance_in_crop;
using Kataglyphis::Test::GoldenMetrics::panel_free_crop;
using Kataglyphis::Test::GoldenMetrics::sky_silhouette;
using Kataglyphis::Test::GoldenMetrics::SKY_SILHOUETTE_STEP;
using Kataglyphis::Test::GoldenMetrics::SkySilhouette;
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

void frame_golden_view(Camera &camera)
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
        if (const char *chosen = std::getenv("KATAGLYPHIS_MODEL_OVERRIDE"); chosen == nullptr || *chosen == '\0') {
            golden_scene.emplace(GOLDEN_SCENE_MODEL);
        }
        frame_golden_view(*camera);
        // The goldens frame the whole image; the docked editor would shrink the scene into its viewport.
        gui->setOverlayVisible(false);
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
// A card with a normal texture and nothing else, the base for a normal-map control.
constexpr const char *NORMAL_MAP_CARD_MODEL = "Models/GltfTest/uv_transform_normal_only_card.gltf";
// A unit cube, metallic 0: a dielectric the library lists, so the editor's reload can pick it.
constexpr const char *DIELECTRIC_CUBE_MODEL = "Models/GltfTest/cube_textured.gltf";
// One texel (218, 128, 218): a tangent-space normal tilted 45 degrees toward +T, as a glTF data URI.
constexpr const char *TILTED_NORMAL_PNG =
  "data:image/png;base64,"
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mO41XALAARsAjUuHifuAAAAAElFTkSuQmCC";
// One texel (128, 38, 218): a tangent-space normal tilted 45 degrees toward -B, as a glTF data URI.
constexpr const char *TILTED_AGAINST_BITANGENT_PNG =
  "data:image/png;base64,"
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mNoULsFAAKqAYGCutYQAAAAAElFTkSuQmCC";
// One mid-grey texel (128, 128, 128), as a glTF data URI.
constexpr const char *GREY_PNG =
  "data:image/png;base64,"
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR42mNoaGgAAAMEAYF1LgG8AAAAAElFTkSuQmCC";
// One metallic-roughness texel (0, 0, 0): roughness and metallic both 0 whatever their factors.
constexpr const char *ZERO_ROUGHNESS_PNG =
  "data:image/png;base64,"
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGNgYGAAAAAEAAH2FzhVAAAAAElFTkSuQmCC";
// One metallic-roughness texel (0, 255, 0): the roughness factor itself, metallic 0.
constexpr const char *FULL_ROUGHNESS_PNG =
  "data:image/png;base64,"
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGNg+M8AAAICAQB7CYF4AAAAAElFTkSuQmCC";
// Four float32 VEC3 normals (sin 70, 0, cos 70): a card facing +Z whose vertex normals lean 70 degrees toward +X.
constexpr const char *LEANING_VERTEX_NORMALS =
  "data:application/octet-stream;base64,so9wPwAAAABEHa8+so9wPwAAAABEHa8+so9wPwAAAABEHa8+so9wPwAAAABEHa8+";

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
    explicit ScopedGpuTimingJsonPath(const std::string &path) : env_("KATAGLYPHIS_GPU_TIMING_JSON", path.c_str()) {}

  private:
    ScopedEnvVar env_;
};

// "light" is run-riscv64-tests.sh's QEMU frame budget; docs/source/gpu-golden-testing.md § The riscv64 light variant.
int path_traced_history(int full_frames, int light_frames)
{
    const char *value = std::getenv("KATAGLYPHIS_PT_GOLDEN_VARIANT");
    const std::string variant = (value == nullptr || *value == '\0') ? "full" : value;
    EXPECT_TRUE(variant == "full" || variant == "light")
      << "KATAGLYPHIS_PT_GOLDEN_VARIANT='" << variant << "' is neither 'full' nor 'light'";
    const int frames = variant == "light" ? light_frames : full_frames;
    GTEST_LOG_(INFO) << "path-traced variant: " << variant << ", " << frames << "-frame history (full " << full_frames
                     << ", light " << light_frames << ")";
    return frames;
}

// A fixture resolved like any model, copied privately with an edit; keep the handle until the model has loaded.
std::optional<GltfVariant>
  write_gltf_variant(const char *fixture, const char *file_name, const std::function<void(nlohmann::json &)> &edit)
{ return GltfVariant::create(sceneConfig::resolveModelPath(fixture), file_name, edit); }

// Half extents are Scale x 10 in volumetric_clouds.slang, so a test names the box it wants and the density.
void place_cloud_box(GUISceneSharedVars &vars, glm::vec3 centre, glm::vec3 half_extents, float density)
{
    vars.cloud_density_multiplier = density;
    for (int axis = 0; axis < 3; ++axis) {
        vars.cloud_mesh_offset[axis] = centre[axis];
        vars.cloud_mesh_scale[axis] = half_extents[axis] / 10.0F;
    }
}

// Largest per-channel difference of one pixel between two RGBA8 captures.
int channel_delta(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, size_t pixel)
{
    int delta = 0;
    for (size_t c = 0; c < 3U; ++c) {
        delta = std::max(delta, std::abs(static_cast<int>(a[pixel * 4U + c]) - static_cast<int>(b[pixel * 4U + c])));
    }
    return delta;
}

// skybox.slang's disabled sky, linear (0, 0, 0.5), as the UNORM swapchain stores it.
bool is_disabled_sky(const std::vector<uint8_t> &rgba, size_t pixel)
{
    const size_t base = pixel * 4U;
    return rgba[base] <= 2U && rgba[base + 1U] <= 2U && std::abs(static_cast<int>(rgba[base + 2U]) - 188) <= 2;
}

// Pixels whose 3x3 neighbourhood is all `mask`, so an edge a traced pass samples differently drops out.
std::vector<bool> eroded(const std::vector<bool> &mask, uint32_t w, uint32_t h)
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
void dump_cloud_frame(const std::string &name, const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h)
{
    const char *prefix = std::getenv("KATAGLYPHIS_CLOUDS_DUMP");
    if (prefix == nullptr || *prefix == '\0' || rgba.empty()) { return; }
    stbi_write_png((std::string(prefix) + "-" + name + ".png").c_str(),
      static_cast<int>(w),
      static_cast<int>(h),
      4,
      rgba.data(),
      static_cast<int>(w) * 4);
}

// brdf.slang's brdf_sampled (a bounce's BRDF) for a white albedo over the hemisphere, uniform in angle for narrow lobes.
double white_directional_albedo(double mu, double roughness, double metallic)
{
    constexpr double PI = 3.14159265358979323846;
    constexpr int STEPS = 320;
    const double alpha = roughness * roughness;
    const double a2 = alpha * alpha;
    const double k = alpha / 2.0;
    const double f0 = 0.04 + 0.96 * metallic;
    const auto smith = [k](double cosine) { return cosine / std::max(cosine * (1.0 - k) + k, 1e-6); };
    const glm::dvec3 v(std::sqrt(std::max(0.0, 1.0 - mu * mu)), 0.0, mu);
    const double n_dot_v = std::max(mu, 1e-4);
    double sum = 0.0;
    for (int i = 0; i < STEPS; ++i) {
        const double theta = (static_cast<double>(i) + 0.5) / STEPS * PI * 0.5;
        const double n_dot_l = std::cos(theta);
        for (int j = 0; j < STEPS; ++j) {
            const double phi = (static_cast<double>(j) + 0.5) / STEPS * 2.0 * PI;
            const glm::dvec3 l(std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), n_dot_l);
            const glm::dvec3 h = glm::normalize(v + l);
            const double n_dot_h = std::max(h.z, 0.0);
            const double d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
            const double ndf = a2 / std::max(PI * d * d, 1e-6);
            const double fresnel = f0 + (1.0 - f0) * std::pow(std::clamp(1.0 - glm::dot(h, v), 0.0, 1.0), 5.0);
            const double specular =
              ndf * smith(n_dot_v) * smith(n_dot_l) * fresnel / std::max(4.0 * n_dot_v * n_dot_l, 1e-6);
            const double diffuse = (1.0 - fresnel) * (1.0 - metallic) / PI;
            sum += (diffuse + specular) * n_dot_l * std::sin(theta);
        }
    }
    return sum * (PI * 0.5 / STEPS) * (2.0 * PI / STEPS);
}

// Mean red, green or blue radiance of an RGBA float capture over a square around (cx, cy).
double mean_hdr_in_square(const std::vector<float> &rgba, uint32_t w, uint32_t cx, uint32_t cy, uint32_t half)
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

// Every texel finite and at most hdr_safe.slang's HDR_MAX, and the largest one, from an RGBA float capture.
struct HdrRange
{
    size_t non_finite = 0;
    float largest = 0.0F;
};

HdrRange hdr_range(const std::vector<float> &rgba)
{
    HdrRange range;
    for (size_t i = 0; i < rgba.size(); ++i) {
        if (i % 4U == 3U) { continue; }
        if (!std::isfinite(rgba[i])) {
            ++range.non_finite;
            continue;
        }
        range.largest = std::max(range.largest, rgba[i]);
    }
    return range;
}

// Mean Rec. 709 luma of an RGBA8 capture over a square around (cx, cy).
double mean_luminance_in_square(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t cx, uint32_t cy, uint32_t half)
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

    auto &scene_vars = harness.settings.scene;
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

// Disabled: writes PNGs and asserts nothing; see docs/source/gpu-golden-testing.md § Writing a new golden test.
TEST(GoldenRender, DISABLED_DumpsFrameToPng)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    // A screenshot of the editor itself, for looking at the GUI; the pixel comparisons below then differ.
    if (const char *with_ui = std::getenv("KATAGLYPHIS_FRAME_DUMP_WITH_UI"); with_ui != nullptr && *with_ui != '\0') {
        harness.gui->setOverlayVisible(true);
    }
    harness.settings.scene.shadows_enabled = true;
    // Must match ShadowsDarkenSomePixels' configuration, or the two cannot be compared.
    harness.settings.scene.cascaded_shadow_intensity = 1.0F;

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
    auto &scene_vars = harness.settings.scene;

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

// Disabled: measures what each PCF radius changes on the rig (MAX_PCF_RADIUS, docs/source/gpu-golden-testing.md).
TEST(GoldenRender, DISABLED_PcfRadiusSweepMeasuresTheShadowRig)
{
    SKIP_WITHOUT_GPU();

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
    const auto &timings = harness.feedback.gpuTimings;
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

    // A kernel without a receiver-plane bias darkens lit receivers too, so penumbra and lit area are measured apart.
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

    auto &scene_vars = harness.settings.scene;
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
    auto &scene_vars = harness.settings.scene;
    auto &renderer_vars = harness.useForwardRaster();

    scene_vars.shadows_enabled = true;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    ASSERT_GT(harness.feedback.visibility.shadow_casters_total, 0U)
      << "the renderer reported considering no shadow casters at all; "
         "the shadow visibility counters are not being written";
    EXPECT_LE(harness.feedback.visibility.shadow_casters_drawn, harness.feedback.visibility.shadow_casters_total);

    scene_vars.shadows_enabled = false;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    EXPECT_EQ(harness.feedback.visibility.shadow_casters_total, 0U)
      << "shadow caster counters must be zeroed when the shadow pass does not run";
    EXPECT_EQ(harness.feedback.visibility.shadow_casters_drawn, 0U)
      << "shadow caster counters must be zeroed when the shadow pass does not run";
}

// The raster pass samples the cascade array with shadows off too; a pass that never ran left it UNDEFINED.
TEST(GoldenRender, ShadowsOffFromTheFirstFrameLeaveTheCascadeArraySampleable)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    harness.useForwardRaster();
    harness.settings.scene.shadows_enabled = false;
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
    auto &scene_vars = harness.settings.scene;
    auto &renderer_vars = harness.useForwardRaster();

    scene_vars.shadows_enabled = true;

    const glm::mat4 far_placement = glm::translate(glm::mat4(1.0F), glm::vec3(5000.0F, 0.0F, 0.0F));
    const std::optional<uint32_t> far_model = harness.renderer->addModel(SHADOW_RIG_MODEL, far_placement);
    ASSERT_TRUE(far_model.has_value()) << "the far-away caster model failed to load";

    renderer_vars.frustum_culling_enabled = false;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    ASSERT_GT(harness.feedback.visibility.shadow_casters_total, 0U)
      << "the renderer reported considering no shadow casters at all; "
         "the shadow visibility counters are not being written";
    EXPECT_EQ(harness.feedback.visibility.shadow_casters_drawn, harness.feedback.visibility.shadow_casters_total)
      << "with frustum culling disabled, every shadow caster must be drawn regardless of the cascade frusta";

    renderer_vars.frustum_culling_enabled = true;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    EXPECT_LE(harness.feedback.visibility.shadow_casters_drawn, harness.feedback.visibility.shadow_casters_total);
}

// A shadow that darkens but never moves passes ShadowsDarkenSomePixels and fails here.
TEST(GoldenRender, ShadowsMoveWhenTheLightRotates)
{
    SKIP_WITHOUT_GPU();

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
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

// What one shadow-rig placement is expected to show; the cascade is the one its shadow sits in, -1 for any.
struct ShadowRigCase
{
    const char *name;
    glm::vec3 camera_position;
    glm::vec3 light_travel;
    glm::vec3 model_position;
    float model_yaw_degrees;
    int expected_cascade;
};

// Models/ShadowTest/shadow_rig.obj at the identity transform.
constexpr GroundRect SHADOW_RIG_GROUND{ 0.0F, -30.0F, 30.0F, -30.0F, 30.0F };
constexpr WorldBox SHADOW_RIG_SLAB{ glm::vec3(-22.0F, 8.0F, -10.0F), glm::vec3(22.0F, 10.0F, 2.0F) };

// Bounds of the box's corners under `transform`, exact for the quarter-turn yaws the cases use.
WorldBox transformed_box(const WorldBox &box, const glm::mat4 &transform)
{
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 local((corner & 1) != 0 ? box.hi.x : box.lo.x,
          (corner & 2) != 0 ? box.hi.y : box.lo.y,
          (corner & 4) != 0 ? box.hi.z : box.lo.z);
        const glm::vec3 world(transform * glm::vec4(local, 1.0F));
        lo = glm::min(lo, world);
        hi = glm::max(hi, world);
    }
    return WorldBox{ lo, hi };
}

// The renderer's own camera matrices (VulkanRenderer::updateUniforms), so a predicted pixel is the rendered one.
glm::mat4 harness_inverse_view_projection(EngineHarness &harness, uint32_t width, uint32_t height)
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

// Pixels the shadow darkens by more than 4 levels, minus any that move between two identical unshadowed frames.
std::vector<bool> measured_shadow_mask(EngineHarness &harness, uint32_t &width, uint32_t &height)
{
    auto &scene_vars = harness.settings.scene;
    scene_vars.cascaded_shadow_intensity = 0.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> unshadowed = harness.capture_frame(width, height);
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> noise_reference = harness.capture_frame(width, height);
    scene_vars.cascaded_shadow_intensity = 1.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> shadowed = harness.capture_frame(width, height);
    const size_t pixel_count = static_cast<size_t>(width) * height;
    std::vector<bool> mask(pixel_count, false);
    if (unshadowed.size() != pixel_count * 4U || shadowed.size() != unshadowed.size()
        || noise_reference.size() != unshadowed.size()) {
        return mask;
    }
    for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
        if (std::abs(luminance_of(unshadowed, pixel) - luminance_of(noise_reference, pixel)) > 4.0) { continue; }
        mask[pixel] = luminance_of(unshadowed, pixel) - luminance_of(shadowed, pixel) > 4.0;
    }
    return mask;
}

// The ShadowsDarkenSomePixels ratio says a shadow exists; this says it is the caster's, where geometry puts it.
TEST(GoldenRender, ShadowFootprintMatchesTheAnalyticCaster)
{
    SKIP_WITHOUT_GPU();

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
    harness.useForwardRaster();
    scene_vars.shadows_enabled = true;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    const glm::vec3 default_light(scene_vars.directional_light_direction[0],
      scene_vars.directional_light_direction[1],
      scene_vars.directional_light_direction[2]);
    // Opposite light tilts put the shadow on opposite sides of the slab; the camera moves pick each cascade.
    const std::array<ShadowRigCase, 5> cases{ {
      { "default light", glm::vec3(0.0F, 6.0F, 26.0F), default_light, glm::vec3(0.0F), 0.0F, 1 },
      { "light toward the camera",
        glm::vec3(0.0F, 6.0F, 26.0F),
        glm::vec3(0.55F, -1.0F, 0.6F),
        glm::vec3(0.0F),
        0.0F,
        1 },
      { "near camera", glm::vec3(0.0F, 6.0F, 10.0F), default_light, glm::vec3(0.0F), 0.0F, 0 },
      { "far camera", glm::vec3(0.0F, 6.0F, 44.0F), default_light, glm::vec3(0.0F), 0.0F, 2 },
      { "model moved and turned",
        glm::vec3(0.0F, 6.0F, 26.0F),
        default_light,
        glm::vec3(4.0F, 0.0F, -3.0F),
        90.0F,
        -1 },
    } };

    std::vector<ShadowAgreement> results;
    for (const ShadowRigCase &rig_case : cases) {
        SCOPED_TRACE(rig_case.name);
        harness.camera->set_camera_position(rig_case.camera_position);
        for (int axis = 0; axis < 3; ++axis) {
            scene_vars.directional_light_direction[axis] = rig_case.light_travel[axis];
            scene_vars.model_position[axis] = rig_case.model_position[axis];
            scene_vars.model_rotation[axis] = axis == 1 ? rig_case.model_yaw_degrees : 0.0F;
        }
        scene_vars.selected_model_index = 0;
        scene_vars.model_transform_changed = true;
        harness.render_frame();
        scene_vars.model_transform_changed = false;

        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<bool> measured = measured_shadow_mask(harness, width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
        ASSERT_EQ(measured.size(), static_cast<size_t>(width) * height);

        const glm::mat4 model = Kataglyphis::makeGuiModelTransform(
          std::span<const float, 3>(scene_vars.model_position), std::span<const float, 3>(scene_vars.model_rotation));
        const WorldBox slab = transformed_box(SHADOW_RIG_SLAB, model);
        const WorldBox ground_box =
          transformed_box(WorldBox{ glm::vec3(SHADOW_RIG_GROUND.x0, 0.0F, SHADOW_RIG_GROUND.z0),
                            glm::vec3(SHADOW_RIG_GROUND.x1, 0.0F, SHADOW_RIG_GROUND.z1) },
            model);
        const GroundRect ground{ ground_box.lo.y, ground_box.lo.x, ground_box.hi.x, ground_box.lo.z, ground_box.hi.z };
        const GroundShadowPrediction prediction = predict_ground_shadow(
          harness_inverse_view_projection(harness, width, height), width, height, ground, slab, rig_case.light_travel);

        // The case is only about its cascade if its predicted shadow sits there (CascadedShadowMap's splits).
        const std::vector<Kataglyphis::CascadeData> cascades =
          Kataglyphis::computeCascadeData(static_cast<uint32_t>(scene_vars.num_shadow_cascades),
            Kataglyphis::CascadeFitParams{ .cameraView = harness.camera->calculate_viewmatrix(),
              .cameraFov = harness.camera->get_fov(),
              .aspect = static_cast<float>(width) / static_cast<float>(height),
              .nearPlane = harness.camera->get_near_plane(),
              .farPlane = harness.camera->get_far_plane(),
              .lightDir = glm::normalize(rig_case.light_travel),
              .shadowDistance = scene_vars.shadow_distance,
              .splitLambda = scene_vars.cascade_split_lambda });
        std::vector<size_t> per_cascade(cascades.size() + 1U, 0U);
        for (size_t pixel = 0; pixel < prediction.pixels.size(); ++pixel) {
            if (prediction.pixels[pixel] != GroundPixel::Shadowed) { continue; }
            const float distance = glm::dot(
              prediction.world[pixel] - harness.camera->get_camera_position(), harness.camera->get_camera_direction());
            size_t index = 0;
            while (index < cascades.size() && distance >= cascades[index].splitDepth) { ++index; }
            ++per_cascade[index];
        }

        // PCF and the map's texels blur a few pixels either side of the analytic edge.
        constexpr uint32_t EDGE_BAND_PX = 6U;
        const ShadowAgreement agreement =
          compare_shadow_masks(prediction.pixels, measured, width, height, EDGE_BAND_PX);
        results.push_back(agreement);

        std::ostringstream cascade_log;
        for (size_t i = 0; i < per_cascade.size(); ++i) { cascade_log << " c" << i << "=" << per_cascade[i]; }
        GTEST_LOG_(INFO) << rig_case.name << ": compared " << agreement.compared << " px, agreement "
                         << agreement.agreement() << ", shadowed predicted " << agreement.predicted_shadowed
                         << " measured " << agreement.measured_shadowed << ", centroid predicted ("
                         << agreement.predicted_centroid.x << ", " << agreement.predicted_centroid.y << ") measured ("
                         << agreement.measured_centroid.x << ", " << agreement.measured_centroid.y
                         << "); predicted shadow per cascade:" << cascade_log.str();

        const size_t predicted_total = std::accumulate(per_cascade.begin(), per_cascade.end(), static_cast<size_t>(0));
        ASSERT_GT(predicted_total, 0U) << "The prediction puts no shadow in view; the case measures nothing.";
        if (rig_case.expected_cascade >= 0) {
            EXPECT_GT(static_cast<double>(per_cascade[static_cast<size_t>(rig_case.expected_cascade)]),
              0.6 * static_cast<double>(predicted_total))
              << "The case's shadow is not mostly in cascade " << rig_case.expected_cascade << "; move its camera.";
        }
        ASSERT_GT(agreement.predicted_shadowed, 2000U) << "Too little predicted shadow away from its edges.";
        EXPECT_GT(agreement.agreement(), 0.97) << "The rendered shadow is not where the caster and the light put it.";
        EXPECT_LT(glm::distance(agreement.predicted_centroid, agreement.measured_centroid), 8.0)
          << "The rendered shadow's centroid is off the predicted one.";
    }

    // Opposite tilts must move the shadow the way the geometry does, not merely somewhere.
    const glm::dvec2 predicted_shift = results[1].predicted_centroid - results[0].predicted_centroid;
    const glm::dvec2 measured_shift = results[1].measured_centroid - results[0].measured_centroid;
    EXPECT_GT(glm::length(predicted_shift), 20.0) << "The two tilts barely move the predicted shadow.";
    EXPECT_GT(glm::dot(predicted_shift, measured_shift), 0.9 * glm::dot(predicted_shift, predicted_shift))
      << "The shadow moved (" << measured_shift.x << ", " << measured_shift.y << ") px, the geometry says ("
      << predicted_shift.x << ", " << predicted_shift.y << ").";
}

// The receiver-plane kernel's point: a lit receiver far from any shadow stays lit at the widest PCF radius.
TEST(GoldenRender, WidestPcfLeavesLitGroundLit)
{
    SKIP_WITHOUT_GPU();

    const ScopedModelOverride use_shadow_rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
    harness.useForwardRaster();
    scene_vars.shadows_enabled = true;
    scene_vars.pcf_radius = MAX_PCF_RADIUS;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    uint32_t width = 0;
    uint32_t height = 0;
    scene_vars.cascaded_shadow_intensity = 0.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> unshadowed = harness.capture_frame(width, height);
    scene_vars.cascaded_shadow_intensity = 1.0F;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> shadowed = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
    ASSERT_EQ(unshadowed.size(), static_cast<size_t>(width) * height * 4U);
    ASSERT_EQ(shadowed.size(), unshadowed.size());

    const glm::vec3 light(scene_vars.directional_light_direction[0],
      scene_vars.directional_light_direction[1],
      scene_vars.directional_light_direction[2]);
    const GroundShadowPrediction prediction =
      predict_ground_shadow(harness_inverse_view_projection(harness, width, height),
        width,
        height,
        SHADOW_RIG_GROUND,
        SHADOW_RIG_SLAB,
        light);

    // Lit ground 16 px or more from any predicted shadow or non-ground pixel, past the widest penumbra.
    constexpr uint32_t FAR_PX = 16U;
    std::vector<bool> darkened(unshadowed.size() / 4U, false);
    for (size_t pixel = 0; pixel < darkened.size(); ++pixel) {
        darkened[pixel] = luminance_of(unshadowed, pixel) - luminance_of(shadowed, pixel) >= 3.0;
    }
    std::vector<GroundPixel> lit_only = prediction.pixels;
    for (GroundPixel &kind : lit_only) {
        if (kind == GroundPixel::Shadowed) { kind = GroundPixel::NotGround; }
    }
    const ShadowAgreement far_lit = compare_shadow_masks(lit_only, darkened, width, height, FAR_PX);
    const double darkened_fraction =
      far_lit.compared > 0U ? static_cast<double>(far_lit.measured_shadowed) / static_cast<double>(far_lit.compared)
                            : 1.0;
    GTEST_LOG_(INFO) << "PCF radius " << MAX_PCF_RADIUS << ": " << far_lit.measured_shadowed << " of "
                     << far_lit.compared << " far-lit ground pixels darkened by 3+ levels ("
                     << darkened_fraction * 100.0 << " %)";

    ASSERT_GT(far_lit.compared, 20000U) << "Too little lit ground in view; the test measures nothing.";
    // Measured 0 % with the receiver plane; the kernel before it darkened 62 % here (gpu-golden-testing.md).
    EXPECT_LT(darkened_fraction, 0.01) << "PCF darkens lit ground far from any shadow: shadow acne from the kernel.";
}

// Post's whole chain against its formula, texel by texel of one frame: the HDR capture is exactly what post read.
TEST(GoldenRender, PostEncodesEachHdrTexelThroughAcesAndSrgb)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
    harness.useForwardRaster();
    scene_vars.shadows_enabled = true;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while warming up.";

    // A dim and the default exposure of one view: the dim one fills the toe and the middle, the default the shoulder.
    const std::array<float, 2> radiances{ 0.1F * scene_vars.directional_light_radiance,
        scene_vars.directional_light_radiance };
    size_t dark = 0;
    size_t mid_grey = 0;
    size_t shoulder = 0;
    double mid_grey_level_sum = 0.0;
    for (const float radiance : radiances) {
        SCOPED_TRACE("directional radiance " + std::to_string(radiance));
        scene_vars.directional_light_radiance = radiance;
        harness.render_frames(SETTLE_FRAMES);

        harness.renderer->requestFrameCapture();
        harness.renderer->requestHdrCapture();
        harness.render_frame();
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t hdr_width = 0;
        uint32_t hdr_height = 0;
        const std::vector<uint8_t> display = harness.renderer->takeCapturedFrame(width, height);
        const std::vector<uint8_t> raw_hdr = harness.renderer->takeCapturedHdrFrame(hdr_width, hdr_height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost during capture.";
        ASSERT_EQ(width, hdr_width);
        ASSERT_EQ(height, hdr_height);
        ASSERT_EQ(display.size(), static_cast<size_t>(width) * height * 4U);
        ASSERT_EQ(raw_hdr.size(), display.size() * 2U);

        // Opaque texels only: post blends a sky texel (alpha 0) over the sky pass, outside the formula.
        size_t opaque = 0;
        size_t matching = 0;
        int worst_delta = 0;
        for (size_t pixel = 0; pixel < display.size() / 4U; ++pixel) {
            const auto half_at = [&](size_t channel) {
                const size_t at = (pixel * 4U + channel) * 2U;
                return static_cast<double>(
                  glm::unpackHalf1x16(static_cast<uint16_t>(raw_hdr[at] | (raw_hdr[at + 1U] << 8U))));
            };
            if (half_at(3U) < 0.999) { continue; }
            ++opaque;
            int pixel_delta = 0;
            for (size_t channel = 0; channel < 3U; ++channel) {
                const double linear = half_at(channel);
                const double expected = post_display_level(linear);
                pixel_delta = std::max(pixel_delta,
                  static_cast<int>(
                    std::lround(std::abs(expected - static_cast<double>(display[pixel * 4U + channel])))));
                if (channel != 1U) { continue; }
                if (linear < 0.05) { ++dark; }
                if (linear > 1.0) { ++shoulder; }
                // Narrow enough that the band's own spread (about one level each side) cannot move the mean.
                if (std::abs(linear - 0.18) < 0.004) {
                    ++mid_grey;
                    mid_grey_level_sum += static_cast<double>(display[pixel * 4U + channel]);
                }
            }
            worst_delta = std::max(worst_delta, pixel_delta);
            // One level for the 8-bit rounding, one for the GPU's pow and division.
            if (pixel_delta <= 2) { ++matching; }
        }
        const double match_fraction = opaque > 0U ? static_cast<double>(matching) / static_cast<double>(opaque) : 0.0;
        GTEST_LOG_(INFO) << opaque << " opaque texels, " << match_fraction * 100.0 << " % within 2 levels (worst "
                         << worst_delta << ")";
        ASSERT_GT(opaque, 100000U) << "Too few opaque texels; the frame shows mostly sky.";
        EXPECT_GT(match_fraction, 0.999) << "Post's output is not ACES then sRGB of what it read.";
    }

    // Both frames together must exercise the toe, the middle and the shoulder, or a match proves one segment only.
    const double mid_grey_level = mid_grey > 0U ? mid_grey_level_sum / static_cast<double>(mid_grey) : 0.0;
    GTEST_LOG_(INFO) << "green texels: below 0.05 " << dark << ", near 0.18 " << mid_grey << " (mean level "
                     << mid_grey_level << ", formula " << post_display_level(0.18) << "), above 1.0 " << shoulder;
    EXPECT_GT(dark, 1000U) << "No dark texels: the toe goes unchecked.";
    EXPECT_GT(mid_grey, 100U) << "No texels near 0.18: the middle goes unchecked.";
    EXPECT_GT(shoulder, 1000U) << "No texels above 1.0: the shoulder goes unchecked.";
    EXPECT_NEAR(mid_grey_level, post_display_level(0.18), 2.0)
      << "Linear 0.18 should display at level 141: 46 is no tonemap and no encode, 68 no encode, 118 no tonemap.";
}

// Forward and deferred never match exactly, but a broken path diverges far beyond this.
TEST(GoldenRender, DeferredMatchesForwardRoughly)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
    auto &renderer_vars = harness.settings.renderer;
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

    auto &scene_vars = harness.settings.scene;
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
        harness.settings.scene.shadows_enabled = true;
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
    harness.settings.scene.shadows_enabled = false;

    uint32_t width = 0;
    uint32_t height = 0;
    const auto capture_all = [&] {
        std::vector<std::vector<uint8_t>> frames;
        for (const Mode &mode : modes) {
            auto &renderer_vars = harness.settings.renderer;
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

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = false;
    harness.settings.scene.shadows_enabled = true;

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
    const unsigned int considered = harness.feedback.visibility.meshes_total;
    ASSERT_GT(considered, 0U) << "the renderer reported considering no meshes at all; "
                                 "the visibility counters are not being written";
    EXPECT_EQ(harness.feedback.visibility.meshes_drawn, considered)
      << "culling dropped geometry the camera is looking straight at";

    // Far past the far plane, with the scene behind the camera.
    harness.camera->set_camera_position(glm::vec3(0.0F, 0.0F, 5000.0F));
    harness.render_frames(SETTLE_FRAMES);

    EXPECT_EQ(harness.feedback.visibility.meshes_total, considered)
      << "the number of meshes considered must not change when the camera moves";
    EXPECT_EQ(harness.feedback.visibility.meshes_drawn, 0U)
      << "a scene entirely outside the view should be culled completely, but "
      << harness.feedback.visibility.meshes_drawn << " mesh(es) were still drawn";

    // Separates "culling works" from "the renderer stopped drawing".
    renderer_vars.frustum_culling_enabled = false;
    harness.render_frames(SETTLE_FRAMES);
    EXPECT_EQ(harness.feedback.visibility.meshes_drawn, considered)
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

    const unsigned int meshes_with_one_model = harness.feedback.visibility.meshes_total;
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
    EXPECT_GT(harness.feedback.visibility.meshes_total, meshes_with_one_model)
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

    EXPECT_EQ(harness.feedback.visibility.meshes_total, 2U)
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
        harness.settings.scene.shadows_enabled = true;

        // Fresh query pools are unreadable until recorded once, so the first readbacks measure nothing.
        harness.render_frames(30);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost while rendering.";
    }// the renderer's destructor writes the dump

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

    auto &renderer_vars = harness.settings.renderer;
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

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES);

    // History-reset nudge, as in PathTracingAccumulatesAndConverges.
    harness.camera->set_camera_position(harness.camera->get_camera_position() + glm::vec3(0.001F, 0.0F, 0.0F));
    harness.render_frame();

    // Shallow history looks "in between" from noise alone, which would pass vacuously.
    harness.render_frames(path_traced_history(80, 25));

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(frame.empty());

    // Only the slab's top edge against the sky: penumbrae and the ground's 1-px far-edge row sit in between unjittered.
    const Crop crop = panel_free_crop(width, height);
    const SkySilhouette silhouette = sky_silhouette(frame, width, crop);
    const size_t crop_columns = crop.x1 - crop.x0;
    GTEST_LOG_(INFO) << "slab-vs-sky silhouette: rows " << silhouette.first_row << "-" << silhouette.last_row << ", "
                     << silhouette.columns << " of " << crop_columns << " columns, " << silhouette.blended
                     << " with a blended pixel";

    ASSERT_GE(silhouette.columns * 2U, crop_columns)
      << "The slab's top edge against the sky (a step of more than " << SKY_SILHOUETTE_STEP
      << " levels below the sky at the top of the panel-free crop) crosses only " << silhouette.columns << " of "
      << crop_columns << " columns; check the crop, the rig or the golden framing.";

    const double blended_fraction = static_cast<double>(silhouette.blended) / static_cast<double>(silhouette.columns);
    // Measured on x64 lavapipe: about 1.0 jittered, 0.0 through the pixel centre, in both variants.
    EXPECT_GT(blended_fraction, 0.5)
      << "Only " << (blended_fraction * 100.0)
      << "% of the slab's silhouette against the sky has a blended pixel between sky and slab - the primary ray "
         "lands on the exact pixel centre every sample instead of being jittered across the pixel's area.";
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

    auto &renderer_vars = harness.settings.renderer;
    auto &scene_vars = harness.settings.scene;
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

    auto &renderer_vars = harness.settings.renderer;
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

// metallic_card's 0.75 comes without a metallic-roughness texture; the path tracer once lit it like a dielectric.
TEST(GoldenRender, PathTracingHonorsTheUntexturedMetallicFactor)
{
    SKIP_WITHOUT_GPU();

    const auto dielectric_card = write_gltf_variant(METALLIC_CARD_MODEL,
      "kataglyphis_dielectric_card.gltf",
      [](nlohmann::json &gltf) { gltf["materials"][0]["pbrMetallicRoughness"]["metallicFactor"] = 0.0; });
    ASSERT_TRUE(dielectric_card.has_value()) << "could not write the dielectric twin of " << METALLIC_CARD_MODEL;

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    // Two short paths a pixel: the card means over thousands of pixels average the noise out.
    renderer_vars.pathTracingSamplesPerPixel = 2;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);

    // Mirror images of one placement, so the sun meets both cards alike.
    const glm::mat4 right = panel_free_card_placement();
    const glm::mat4 left = glm::translate(glm::mat4(1.0F), glm::vec3(-2.0F * right[3].x, 0.0F, 0.0F)) * right;
    ASSERT_TRUE(harness.renderer->addModel(METALLIC_CARD_MODEL, right).has_value())
      << "adding the metallic card failed";
    ASSERT_TRUE(harness.renderer->addModel(dielectric_card->string(), left).has_value())
      << "adding the dielectric card failed";
    harness.render_frames(SETTLE_FRAMES + 16);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    // Linear radiance: the specular lobe gives the metal back part of its light, which ACES would squeeze near white.
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(radiance.empty());

    const Crop metallic_box = panel_free_card_box(width, height);
    const uint32_t centre_y = (metallic_box.y0 + metallic_box.y1) / 2U;
    const uint32_t metallic_x = (metallic_box.x0 + metallic_box.x1) / 2U;
    const double metallic_mean = mean_hdr_in_square(radiance, width, metallic_x, centre_y, 40U);
    const double dielectric_mean = mean_hdr_in_square(radiance, width, width - metallic_x, centre_y, 40U);
    GTEST_LOG_(INFO) << "path-traced card radiance: metallic 0.75 " << metallic_mean << ", dielectric "
                     << dielectric_mean;

    // A quarter of the diffuse plus a rough GGX lobe at F0 near the albedo comes to about half the dielectric's.
    ASSERT_GT(dielectric_mean, 0.05) << "the dielectric card is unlit - check the placement";
    EXPECT_LT(metallic_mean, 0.8 * dielectric_mean)
      << "the untextured metallicFactor does not reach the path tracer: the metal shades like its dielectric twin";
}

// The light must scale diffuse as well as specular, or only sparse highlights respond to radiance.
TEST(GoldenRender, ForwardLightingRespondsToTheDirectionalLight)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &scene_vars = harness.settings.scene;
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

    auto &renderer_vars = harness.settings.renderer;
    auto &scene_vars = harness.settings.scene;
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

    auto &renderer_vars = harness.settings.renderer;
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

    const unsigned int forward_meshes_drawn = harness.feedback.visibility.meshes_drawn;
    ASSERT_GT(forward_meshes_drawn, 0U) << "the renderer reported drawing no meshes at all in forward mode";
    const float forward_main_ms =
      harness.feedback.gpuTimings
        .pass_ms[static_cast<size_t>(Kataglyphis::VulkanRendererInternals::FrontendShared::GpuTimedPass::Main)];

    renderer_vars.raytracing = true;
    // Flush again so the average holds RT-only samples.
    harness.render_frames(30);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    EXPECT_EQ(harness.feedback.visibility.meshes_drawn, 0U)
      << "the raster pass must not run - and must not report stale mesh counts - while RT owns the frame";
    EXPECT_EQ(harness.feedback.visibility.meshes_total, 0U)
      << "the raster pass must not run - and must not report stale mesh counts - while RT owns the frame";

    if (harness.feedback.gpuTimings.supported) {
        const float rt_main_ms =
          harness.feedback.gpuTimings
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

    auto &renderer_vars = harness.settings.renderer;
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

    // The fixture leaves metallicFactor at glTF's default 1, which the Lambertian path tracer renders black.
    const auto card = write_gltf_variant(MASK_CARD_MODEL,
      "kataglyphis_dielectric_mask_card.gltf",
      [](nlohmann::json &gltf) { gltf["materials"][0]["pbrMetallicRoughness"]["metallicFactor"] = 0.0; });
    ASSERT_TRUE(card.has_value()) << "could not write the dielectric twin of " << MASK_CARD_MODEL;

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    // Both captures get the same accumulated history, or PT noise alone changes pixels across the card's band.
    const int history = path_traced_history(60, 24);
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES + history);
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
    const auto added = harness.renderer->addModel(card->string(), placement);
    ASSERT_TRUE(added.has_value()) << "adding the mask card failed";
    harness.render_frames(SETTLE_FRAMES + history);
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

    auto &renderer_vars = harness.settings.renderer;
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

// Mean linear radiance of each channel of an RGBA float capture over a square around (cx, cy).
static glm::dvec4
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

// The emissive card's quad as one glTF node per entry, each with its own material and so its own mesh.
static void make_card_scene(nlohmann::json &gltf,
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

// Pixel column of world-space x on a camera looking along -Z (or +Z, mirrored) from `distance`, at the golden fov.
static uint32_t card_column(float world_x, float distance, uint32_t width, uint32_t height)
{
    const float half_width =
      distance * std::tan(glm::radians(45.0F) * 0.5F) * static_cast<float>(width) / static_cast<float>(height);
    return static_cast<uint32_t>((0.5F + 0.5F * world_x / half_width) * static_cast<float>(width));
}

// Two half-transparent cards over an opaque one, seen from both sides, so each order is right once and wrong once.
TEST(GoldenRender, BlendCardsCompositeInDepthOrderFromBothSides)
{
    SKIP_WITHOUT_GPU();

    const auto unlit = [](std::array<double, 4> rgba, bool blend) {
        nlohmann::json material;
        material["pbrMetallicRoughness"] = { { "baseColorFactor", rgba } };
        material["doubleSided"] = true;
        material["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
        if (blend) { material["alphaMode"] = "BLEND"; }
        return material;
    };
    // Red and green overlap at x in [-0.25, 0.25] before grey backdrops; at y = 8 a red pair stands against the sky.
    const auto scene = write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_blend_cards.gltf", [&](auto &gltf) {
        make_card_scene(gltf,
          { unlit({ 0.5, 0.5, 0.5, 1.0 }, false),
            unlit({ 1.0, 0.0, 0.0, 0.5 }, true),
            unlit({ 0.0, 1.0, 0.0, 0.5 }, true),
            unlit({ 1.0, 0.0, 0.0, 1.0 }, false) },
          { { 0, { { "translation", { 0.0, 0.0, -3.0 } }, { "scale", { 8.0, 8.0, 1.0 } } } },
            { 0, { { "translation", { 0.0, 0.0, 3.0 } }, { "scale", { 8.0, 8.0, 1.0 } } } },
            { 1, { { "translation", { -0.25, 0.0, 0.0 } } } },
            { 2, { { "translation", { 0.25, 0.0, 0.5 } } } },
            { 1, { { "translation", { -0.6, 8.0, 0.0 } } } },
            { 3, { { "translation", { 0.6, 8.0, 0.0 } } } } });
        gltf["extensionsUsed"] = nlohmann::json::array({ "KHR_materials_unlit" });
    });
    ASSERT_TRUE(scene.has_value()) << "could not write the blend cards";

    ScopedModelOverride cards(scene->string().c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.shadows_enabled = false;
    // The disabled sky is one flat colour, so the card over it has one expected value.
    scene_vars.skybox_enabled = false;
    harness.render_frames(WARMUP_FRAMES);

    struct View
    {
        const char *name;
        float z;
        float yaw;
        // The colour channel of the nearer card; the other card's is 1 - near.
        int near_channel;
    };
    // Looking +Z mirrors x, so the farther card is on the left from both sides and the overlap stays centred.
    constexpr std::array<View, 2> VIEWS = { { { "front", 1.8F, -90.0F, 1 }, { "back", -1.3F, 90.0F, 0 } } };
    constexpr std::array<RasterizationMode, 2> MODES = { RasterizationMode::Forward, RasterizationMode::Deferred };

    for (const RasterizationMode mode : MODES) {
        renderer_vars.rasterizationMode = mode;
        const char *mode_name = mode == RasterizationMode::Forward ? "forward" : "deferred";
        for (const View &view : VIEWS) {
            harness.camera->set_camera_position(glm::vec3(0.0F, 0.0F, view.z));
            harness.camera->set_orientation(view.yaw, 0.0F);
            harness.render_frames(SETTLE_FRAMES);
            uint32_t width = 0;
            uint32_t height = 0;
            const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
            ASSERT_FALSE(harness.renderer->hasDeviceLost());
            ASSERT_FALSE(radiance.empty());

            // Near card 1.3 away, far card 1.8: only the far one covers x -0.55 at 1.8, only the near one 0.47 at 1.3.
            const uint32_t y = height / 2U;
            const uint32_t far_x = card_column(-0.55F, 1.8F, width, height);
            const uint32_t near_x = card_column(0.47F, 1.3F, width, height);
            const glm::dvec4 overlap = mean_hdr_rgba_in_square(radiance, width, width / 2U, y, 12U);
            const glm::dvec4 far_only = mean_hdr_rgba_in_square(radiance, width, far_x, y, 12U);
            const glm::dvec4 near_only = mean_hdr_rgba_in_square(radiance, width, near_x, y, 12U);
            const int near_channel = view.near_channel;
            const int far_channel = 1 - view.near_channel;
            GTEST_LOG_(INFO) << mode_name << " " << view.name << ": overlap (" << overlap.r << ", " << overlap.g << ", "
                             << overlap.b << ", " << overlap.a << "), far only (" << far_only.r << ", " << far_only.g
                             << ", " << far_only.b << "), near only (" << near_only.r << ", " << near_only.g << ", "
                             << near_only.b << ")";

            // Linear over: one card over the 0.5 grey gives 0.75 / 0.25; near over far over grey 0.625 / 0.375.
            constexpr double TOLERANCE = 0.03;
            EXPECT_NEAR(far_only[far_channel], 0.75, TOLERANCE)
              << mode_name << " " << view.name << ": the far card alone does not blend over the grey";
            EXPECT_NEAR(far_only.b, 0.25, TOLERANCE)
              << mode_name << " " << view.name << ": the grey does not show through the far card";
            EXPECT_NEAR(near_only[near_channel], 0.75, TOLERANCE)
              << mode_name << " " << view.name << ": the near card alone does not blend over the grey";
            EXPECT_NEAR(overlap[near_channel], 0.625, TOLERANCE)
              << mode_name << " " << view.name << ": the nearer card does not lie on top - the BLEND sort is off";
            EXPECT_NEAR(overlap[far_channel], 0.375, TOLERANCE)
              << mode_name << " " << view.name << ": the farther card does not show through the nearer one";
            EXPECT_NEAR(overlap.b, 0.125, TOLERANCE)
              << mode_name << " " << view.name << ": the grey does not show through both cards";
            EXPECT_GT(overlap.a, 0.99) << mode_name << " " << view.name
                                       << ": blending over an opaque pixel lowered its coverage, so post would "
                                          "composite the sky through it";
        }

        // Over the sky, post composites the tonemapped card at its coverage: half the opaque card's encoded red.
        harness.camera->set_camera_position(glm::vec3(0.0F, 8.0F, 1.8F));
        harness.camera->set_orientation(-90.0F, 0.0F);
        harness.render_frames(SETTLE_FRAMES);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<uint8_t> frame = harness.capture_frame(width, height);
        ASSERT_FALSE(frame.empty());
        const auto encoded_at = [&](float world_x, size_t channel) {
            const uint32_t x = card_column(world_x, 1.8F, width, height);
            double sum = 0.0;
            for (uint32_t y = height / 2U - 8U; y < height / 2U + 8U; ++y) {
                for (uint32_t dx = x - 8U; dx < x + 8U; ++dx) {
                    sum += frame[(static_cast<size_t>(y) * width + dx) * 4U + channel];
                }
            }
            return sum / 256.0;
        };
        const double blend_red = encoded_at(-0.6F, 0U);
        const double blend_blue = encoded_at(-0.6F, 2U);
        const double opaque_red = encoded_at(0.6F, 0U);
        GTEST_LOG_(INFO) << mode_name << " over the sky: BLEND card (" << blend_red << ", -, " << blend_blue
                         << "), opaque card red " << opaque_red;
        ASSERT_GT(opaque_red, 150.0) << mode_name << ": the opaque red card is not in view - check the placement";
        EXPECT_NEAR(blend_red, 0.5 * opaque_red, 3.0)
          << mode_name << ": a half-transparent card over the sky is not composited at half its tonemapped colour";
        // skybox.slang's disabled sky stores blue 188 (is_disabled_sky).
        EXPECT_NEAR(blend_blue, 0.5 * 188.0, 3.0) << mode_name << ": the sky does not show through the card";
    }
}

// A 2x1 emissive texture, white then black, nearest-filtered, so each half of a card shows which UV set it sampled.
constexpr const char *WHITE_BLACK_PNG =
  "data:image/png;base64,"
  "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAIAAAB7QOjdAAAAD0lEQVR4nGP4//8/AwMDAA74Av7Ji4P1AAAAAElFTkSuQmCC";
// The emissive card's four TEXCOORD_0 values with u mirrored, as float32 VEC2: (1,1) (0,1) (0,0) (1,0).
constexpr const char *MIRRORED_U_TEXCOORDS =
  "data:application/octet-stream;base64,AACAPwAAgD8AAAAAAACAPwAAAAAAAAAAAACAPwAAAAA=";

// TEXCOORD_1 mirrors u, so an emissive texture on set 1 lights the other half of the card in every mode.
TEST(GoldenRender, EmissiveOnTexCoord1FollowsTheSecondUvSetInEveryMode)
{
    SKIP_WITHOUT_GPU();

    const auto emissive = [](nlohmann::json texture_info) {
        nlohmann::json material;
        material["pbrMetallicRoughness"] = {
            { "baseColorFactor", { 0.0, 0.0, 0.0, 1.0 } }, { "metallicFactor", 0.0 }, { "roughnessFactor", 1.0 }
        };
        material["emissiveFactor"] = { 1.0, 1.0, 1.0 };
        material["emissiveTexture"] = texture_info;
        material["doubleSided"] = true;
        return material;
    };
    nlohmann::json on_set1;
    on_set1["index"] = 0;
    on_set1["texCoord"] = 1;
    nlohmann::json override_to_set1;
    override_to_set1["index"] = 0;
    override_to_set1["extensions"]["KHR_texture_transform"] = { { "texCoord", 1 } };
    // Left: the TEXCOORD_0 control; centre: texCoord 1; right: texCoord 0 that KHR_texture_transform moves to 1.
    constexpr std::array<float, 3> CARD_X = { -1.2F, 0.0F, 1.2F };
    const auto scene = write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_uv1_emissive.gltf", [&](auto &gltf) {
        make_card_scene(gltf,
          { emissive({ { "index", 0 } }), emissive(on_set1), emissive(override_to_set1) },
          { { 0, { { "translation", { CARD_X[0], 0.0F, 0.0F } } } },
            { 1, { { "translation", { CARD_X[1], 0.0F, 0.0F } } } },
            { 2, { { "translation", { CARD_X[2], 0.0F, 0.0F } } } } });
        nlohmann::json buffer;
        buffer["uri"] = MIRRORED_U_TEXCOORDS;
        buffer["byteLength"] = 32;
        gltf["buffers"].push_back(buffer);
        nlohmann::json view;
        view["buffer"] = gltf["buffers"].size() - 1U;
        view["byteOffset"] = 0;
        view["byteLength"] = 32;
        view["target"] = 34962;
        gltf["bufferViews"].push_back(view);
        nlohmann::json accessor;
        accessor["bufferView"] = gltf["bufferViews"].size() - 1U;
        accessor["componentType"] = 5126;
        accessor["count"] = 4;
        accessor["type"] = "VEC2";
        gltf["accessors"].push_back(accessor);
        for (auto &mesh : gltf["meshes"]) {
            mesh["primitives"][0]["attributes"]["TEXCOORD_1"] = gltf["accessors"].size() - 1U;
        }
        gltf["images"] = nlohmann::json::array({ { { "uri", WHITE_BLACK_PNG } } });
        gltf["samplers"] = nlohmann::json::array(
          { { { "magFilter", 9728 }, { "minFilter", 9728 }, { "wrapS", 33071 }, { "wrapT", 33071 } } });
        gltf["textures"] = nlohmann::json::array({ { { "source", 0 }, { "sampler", 0 } } });
        gltf["extensionsUsed"] = nlohmann::json::array({ "KHR_texture_transform" });
    });
    ASSERT_TRUE(scene.has_value()) << "could not write the TEXCOORD_1 cards";

    ScopedModelOverride cards(scene->string().c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.shadows_enabled = false;
    // Emission only: a lit black card would add a highlight to either half.
    scene_vars.directional_light_radiance = 0.0F;
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 1;
    constexpr float DISTANCE = 3.0F;
    harness.camera->set_camera_position(glm::vec3(0.0F, 0.0F, DISTANCE));
    harness.camera->set_orientation(-90.0F, 0.0F);
    harness.render_frames(WARMUP_FRAMES);

    struct Mode
    {
        const char *name;
        RasterizationMode raster;
        bool raytracing;
        bool path_tracing;
    };
    std::vector<Mode> modes = { { "forward", RasterizationMode::Forward, false, false },
        { "deferred", RasterizationMode::Deferred, false, false } };
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back({ "ray traced", RasterizationMode::Forward, true, false });
        modes.push_back({ "path traced", RasterizationMode::Forward, false, true });
    } else {
        GTEST_LOG_(INFO) << "Hardware raytracing unsupported; checking the raster modes only.";
    }

    for (const Mode &mode : modes) {
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        renderer_vars.pathTracing = mode.path_tracing;
        harness.render_frames(SETTLE_FRAMES);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost());
        ASSERT_FALSE(radiance.empty());

        // u < 0.5 is the white texel: the -x half on TEXCOORD_0, the +x half on the mirrored TEXCOORD_1.
        std::array<double, 3> white_minus_black{};
        for (size_t card = 0; card < CARD_X.size(); ++card) {
            const uint32_t minus_x = card_column(CARD_X[card] - 0.25F, DISTANCE, width, height);
            const uint32_t plus_x = card_column(CARD_X[card] + 0.25F, DISTANCE, width, height);
            const double minus_half = mean_hdr_in_square(radiance, width, minus_x, height / 2U, 16U);
            const double plus_half = mean_hdr_in_square(radiance, width, plus_x, height / 2U, 16U);
            white_minus_black[card] = card == 0 ? minus_half - plus_half : plus_half - minus_half;
            GTEST_LOG_(INFO) << mode.name << " card " << card << ": -x half " << minus_half << ", +x half "
                             << plus_half;
        }
        EXPECT_GT(white_minus_black[0], 0.5)
          << mode.name << ": the TEXCOORD_0 control does not glow on its -x half - check the placement";
        EXPECT_GT(white_minus_black[1], 0.5)
          << mode.name << ": an emissive texture on texCoord 1 samples TEXCOORD_0 - the second UV set is ignored";
        EXPECT_GT(white_minus_black[2], 0.5) << mode.name
                                             << ": KHR_texture_transform's texCoord 1 does not move the emissive "
                                                "slot onto the second UV set";
    }
}

// RT/PT see only the acceleration structure, so addModel must rebuild it or the model is invisible to them.
TEST(GoldenRender, AddedModelAppearsInPathTracing)
{
    SKIP_WITHOUT_GPU();

    // The fixture leaves metallicFactor at glTF's default 1, which the Lambertian path tracer renders black.
    const auto card = write_gltf_variant(UV_TRANSFORM_MODEL,
      "kataglyphis_dielectric_uv_card.gltf",
      [](nlohmann::json &gltf) { gltf["materials"][0]["pbrMetallicRoughness"]["metallicFactor"] = 0.0; });
    ASSERT_TRUE(card.has_value()) << "could not write the dielectric twin of " << UV_TRANSFORM_MODEL;

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    // The control gets the same accumulated history as the capture after the add, so noise reads alike in both.
    const int history = path_traced_history(60, 24);
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES + history);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> before = harness.capture_frame(width, height);
    ASSERT_FALSE(before.empty());

    const auto added = harness.renderer->addModel(card->string(), panel_free_card_placement());
    ASSERT_TRUE(added.has_value()) << "adding the card failed";
    // The AS rebuild resets history; deepen it so noise does not swamp the detail.
    harness.render_frames(SETTLE_FRAMES);
    harness.render_frames(history);
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

// The layer reads them at instance creation, so one harness gets sync validation whatever the lane's settings say.
class ScopedSyncValidation
{
  public:
    ScopedSyncValidation() { setAll("1"); }
    ScopedSyncValidation(const ScopedSyncValidation &) = delete;
    ScopedSyncValidation &operator=(const ScopedSyncValidation &) = delete;
    ~ScopedSyncValidation() { setAll(""); }

  private:
    // Both extras default off and hide a whole class: a load op after another pass's store, and a shader's reads.
    static constexpr std::array<const char *, 3> kSettings = { "VK_KHRONOS_VALIDATION_VALIDATE_SYNC",
        "VK_KHRONOS_VALIDATION_SYNCVAL_LOAD_OP_AFTER_STORE_OP_VALIDATION",
        "VK_KHRONOS_VALIDATION_SYNCVAL_SHADER_ACCESSES_HEURISTIC" };

    static void setAll(const char *value)
    {
        for (const char *name : kSettings) { set(name, value); }
    }

    static void set(const char *name, const char *value)
    {
#ifdef _WIN32
        std::ignore = _putenv_s(name, value);
#else
        if (*value == '\0') {
            std::ignore = unsetenv(name);
        } else {
            std::ignore = setenv(name, value, 1);
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

// Default sync validation misses both edges into post: the sky pass's stored image and the clouds' compute output.
TEST(GoldenRender, CloudCompositeAndSkyLoadAreFreeOfSyncHazards)
{
    SKIP_WITHOUT_GPU();

    ScopedSyncValidation sync_validation;
    EngineHarness harness;
    harness.useForwardRaster();
    harness.settings.scene.clouds_enabled = true;
    // From the first frame: the layer reports one message id ten times, then goes quiet, so warmup would spend them.
    ScopedValidationErrorCounter validation_errors;
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost()) << "Device lost compositing the clouds.";

    EXPECT_EQ(validation_errors.errorCount(), 0)
      << "synchronization validation logged an error - SYNC-HAZARD-READ-AFTER-WRITE if post lost its barrier after "
         "the sky pass (PostStage.cpp) or after the clouds' dispatch (VulkanRenderer.cpp)";
}

namespace {
struct CheckerFractions
{
    double green = 0.0;
    double magenta = 0.0;
};

// cube_textured.gltf's green and magenta cells over the centre crop its front face fills from 2.2 units away.
CheckerFractions cube_checker_fractions(const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height)
{
    size_t green = 0;
    size_t magenta = 0;
    size_t total = 0;
    for (uint32_t y = height / 2U - 150U; y < height / 2U + 150U; ++y) {
        for (uint32_t x = width / 2U - 150U; x < width / 2U + 150U; ++x) {
            const size_t base = (static_cast<size_t>(y) * width + x) * 4U;
            const int red = rgba[base];
            const int grn = rgba[base + 1U];
            if (grn > red + 40) { ++green; }
            if (red > grn + 40) { ++magenta; }
            ++total;
        }
    }
    return { static_cast<double>(green) / static_cast<double>(total),
        static_cast<double>(magenta) / static_cast<double>(total) };
}
}// namespace

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

    auto &renderer_vars = harness.settings.renderer;
    auto &scene_vars = harness.settings.scene;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES + SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    // A dielectric, so the frame measures the new model shaded; a metal card went black here, the rig still vanished.
    const int reload_index = sceneConfig::modelIndexOf(sceneConfig::getAvailableModelPaths(), DIELECTRIC_CUBE_MODEL);
    ASSERT_GE(reload_index, 0) << DIELECTRIC_CUBE_MODEL << " not found among the scanned models";

    ScopedValidationErrorCounter validation_errors;
    // The same flags the editor's library sets.
    scene_vars.selected_model_index = reload_index;
    scene_vars.model_reload_requested = true;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    // The cube sits at the origin with the identity; from here its front face fills the image's centre.
    harness.camera->set_camera_position(glm::vec3(0.0F, 0.6F, 2.2F));
    harness.camera->set_orientation(-90.0F, -15.0F);
    harness.render_frames(30);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    EXPECT_EQ(validation_errors.errorCount(), 0)
      << "the reload logged a Vulkan validation error - almost certainly the "
         "raytracing descriptor set still referencing the TLAS the AS rebuild destroyed";

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> after = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(after.empty());

    // A stale TLAS shows the rig's grey, an empty one sky.
    const CheckerFractions checker = cube_checker_fractions(after, width, height);
    GTEST_LOG_(INFO) << "reloaded cube, centre crop: " << checker.green << " green and " << checker.magenta
                     << " magenta";

    EXPECT_GT(checker.green, 0.05) << "no green checker texels: the reloaded cube is not shaded in path tracing";
    EXPECT_GT(checker.magenta, 0.05) << "no magenta checker texels: the reloaded cube is not shaded in path tracing";
}

// A full rebuild counts two builds for the add and replaces the rig's BLAS; BlasReuse.hpp decides what stays.
TEST(GoldenRender, AddedModelBuildsOnlyItsOwnBlas)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.settings.renderer;
    auto &scene_vars = harness.settings.scene;
    renderer_vars.raytracing = true;
    renderer_vars.pathTracing = false;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    const std::vector<vk::AccelerationStructureKHR> loaded = harness.renderer->bottomLevelHandles();
    ASSERT_EQ(loaded.size(), 1U) << "the loaded rig has no BLAS";
    EXPECT_EQ(harness.renderer->bottomLevelBuildCount(), 1U);

    ScopedValidationErrorCounter validation_errors;
    const glm::mat4 beside = glm::translate(glm::mat4(1.0F), glm::vec3(3.0F, 1.0F, 8.0F));
    ASSERT_TRUE(harness.renderer->addModel(DIELECTRIC_CUBE_MODEL, beside).has_value()) << "adding the cube failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    const std::vector<vk::AccelerationStructureKHR> added = harness.renderer->bottomLevelHandles();
    ASSERT_EQ(added.size(), 2U);
    EXPECT_EQ(harness.renderer->bottomLevelBuildCount(), 2U)
      << "adding one model rebuilt the BLAS of the model already in the scene";
    EXPECT_EQ(added[0], loaded[0]) << "the rig's BLAS was replaced by an addModel";

    // A reload replaces model 0, so neither BLAS may stay: one build, for the reloaded model.
    const int reload_index = sceneConfig::modelIndexOf(sceneConfig::getAvailableModelPaths(), DIELECTRIC_CUBE_MODEL);
    ASSERT_GE(reload_index, 0) << DIELECTRIC_CUBE_MODEL << " not found among the scanned models";
    scene_vars.selected_model_index = reload_index;
    scene_vars.model_reload_requested = true;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    const std::vector<vk::AccelerationStructureKHR> reloaded = harness.renderer->bottomLevelHandles();
    ASSERT_EQ(reloaded.size(), 1U) << "the reload kept a BLAS of a model the scene no longer holds";
    EXPECT_EQ(harness.renderer->bottomLevelBuildCount(), 3U);

    // The next add appends to the reloaded scene, not to the BLAS the reload discarded.
    ASSERT_TRUE(harness.renderer->addModel(MASK_CARD_MODEL, beside).has_value()) << "adding the card failed";
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    const std::vector<vk::AccelerationStructureKHR> appended = harness.renderer->bottomLevelHandles();
    ASSERT_EQ(appended.size(), 2U);
    EXPECT_EQ(harness.renderer->bottomLevelBuildCount(), 4U);
    EXPECT_EQ(appended[0], reloaded[0]) << "the reloaded cube's BLAS was replaced by an addModel";
    EXPECT_EQ(validation_errors.errorCount(), 0) << "a traced frame or an AS build logged an error";
}

// Nothing rewrites the traced sets per frame, so each change they depend on must reach updateAllDescriptorSets.
TEST(GoldenRender, TracedFramesFollowTheRasterModeAndTheSwapchain)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = true;
    renderer_vars.pathTracing = false;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    ScopedValidationErrorCounter validation_errors;
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> forward = harness.capture_frame(width, height);
    ASSERT_FALSE(forward.empty());
    ASSERT_GT(distinct_luminance_buckets(forward), 16U) << "the traced rig frame is blank - check the scene";

    const auto differing = [&](const std::vector<uint8_t> &other) {
        size_t count = 0;
        for (size_t pixel = 0; pixel < forward.size() / 4U; ++pixel) {
            if (channel_delta(forward, other, pixel) > 8) { ++count; }
        }
        return static_cast<double>(count) / static_cast<double>(forward.size() / 4U);
    };

    // The traced pass writes the mode's offscreen image, which post then reads.
    renderer_vars.rasterizationMode = RasterizationMode::Deferred;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> deferred = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(deferred.size(), forward.size());
    const double deferred_differing = differing(deferred);

    // A recreated swapchain recreates every image the traced sets name.
    harness.renderer->recreateSwapChain();
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<uint8_t> recreated = harness.capture_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(recreated.size(), forward.size());
    const double recreated_differing = differing(recreated);

    GTEST_LOG_(INFO) << "traced rig, pixels differing from the forward-mode frame: " << deferred_differing
                     << " in deferred mode, " << recreated_differing << " after the swapchain recreation";
    EXPECT_LT(deferred_differing, 0.01) << "the traced frame did not follow the switch to deferred mode: the traced "
                                           "pass still writes the forward image while post reads the deferred one";
    EXPECT_LT(recreated_differing, 0.01) << "the traced frame changed across a swapchain recreation";
    EXPECT_EQ(validation_errors.errorCount(), 0)
      << "a traced frame logged an error - a descriptor still naming an image the recreation destroyed";
}

// Model 0 has two meshes and one texture, so the second model's hits need its mesh base and its texture offset.
TEST(GoldenRender, TracedSecondModelResolvesItsOwnMeshAndTextures)
{
    SKIP_WITHOUT_GPU();

    // The second primitive makes the cube's mesh base 2 against an instance index of 1; the grey texel fills slot 0.
    const auto first = write_gltf_variant(DIELECTRIC_CUBE_MODEL, "kataglyphis_grey_two_mesh_cube.gltf", [](auto &gltf) {
        gltf["images"][0]["uri"] = GREY_PNG;
        gltf["meshes"][0]["primitives"].push_back(gltf["meshes"][0]["primitives"][0]);
    });
    ASSERT_TRUE(first.has_value()) << "could not write the grey two-mesh cube";

    ScopedModelOverride first_model(first->string().c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.useForwardRaster();
    renderer_vars.pathTracingSamplesPerPixel = 2;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_EQ(harness.scene->getMeshCountPerModel(), (std::vector<uint32_t>{ 2U }))
      << "the first cube is not two meshes";
    ASSERT_EQ(harness.scene->getTextureCount(0), 1U) << "the first cube must own texture slot 0";

    // Far enough along +X that the first cube stays out of the frame.
    const glm::mat4 placement = glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, 0.0F, 0.0F));
    ASSERT_TRUE(harness.renderer->addModel(DIELECTRIC_CUBE_MODEL, placement).has_value()) << "adding the cube failed";
    harness.camera->set_camera_position(glm::vec3(10.0F, 0.6F, 2.2F));
    harness.camera->set_orientation(-90.0F, -15.0F);

    const auto measure = [&](const char *mode, int frames) {
        harness.render_frames(frames);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<uint8_t> frame = harness.capture_frame(width, height);
        EXPECT_FALSE(harness.renderer->hasDeviceLost());
        if (frame.empty()) { return CheckerFractions{}; }
        const CheckerFractions checker = cube_checker_fractions(frame, width, height);
        GTEST_LOG_(INFO) << mode << ": second cube, centre crop: " << checker.green << " green and " << checker.magenta
                         << " magenta";
        return checker;
    };

    const CheckerFractions forward = measure("forward", SETTLE_FRAMES);
    ASSERT_GT(forward.green, 0.05) << "the forward frame shows no green checker - check the framing";
    ASSERT_GT(forward.magenta, 0.05) << "the forward frame shows no magenta checker - check the framing";

    renderer_vars.raytracing = true;
    const CheckerFractions traced = measure("ray traced", SETTLE_FRAMES);
    EXPECT_GT(traced.green, 0.05) << "ray traced: the second cube shades with the first's grey - its object index "
                                     "lost InstanceID()'s mesh base, or its texture its offset";
    EXPECT_GT(traced.magenta, 0.05) << "ray traced: the second cube's magenta checker cells are missing";

    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    const CheckerFractions path_traced = measure("path traced", 8);
    EXPECT_GT(path_traced.green, 0.05) << "path traced: the second cube shades with the first's grey - its object "
                                          "index lost CommittedInstanceID()'s mesh base, or its texture its offset";
    EXPECT_GT(path_traced.magenta, 0.05) << "path traced: the second cube's magenta checker cells are missing";
}

// The library marks what the startup load opened; it once marked the default scene whatever the override said.
TEST(GoldenRender, EditorMarksTheModelTheSceneLoaded)
{
    SKIP_WITHOUT_GPU();

    {
        ScopedModelOverride rig(SHADOW_RIG_MODEL);
        EngineHarness harness;
        // Visible, so the editor runs the code that marks the loaded row.
        harness.gui->setOverlayVisible(true);
        harness.render_frames(SETTLE_FRAMES);
        ASSERT_FALSE(harness.renderer->hasDeviceLost());

        const int rig_row = sceneConfig::modelIndexOf(sceneConfig::getAvailableModelPaths(), SHADOW_RIG_MODEL);
        ASSERT_GE(rig_row, 0) << SHADOW_RIG_MODEL << " is not in the model library";
        EXPECT_EQ(harness.settings.scene.selected_model_index, rig_row)
          << "KATAGLYPHIS_MODEL_OVERRIDE loaded the shadow rig, but the library marks another row";
        EXPECT_EQ(harness.gui->loadedModelName(), std::string_view("ShadowTest/shadow_rig"))
          << "the library row's name";
    }
    {
        // A failed load leaves the scene empty, so no row may claim to be loaded.
        ScopedModelOverride missing("Models/ShadowTest/no_such_model.obj");
        EngineHarness harness;
        harness.gui->setOverlayVisible(true);
        harness.render_frames(SETTLE_FRAMES);
        ASSERT_FALSE(harness.renderer->hasDeviceLost());

        EXPECT_EQ(harness.scene->getModelCount(), 0U);
        EXPECT_EQ(harness.settings.scene.selected_model_index, -1)
          << "nothing loaded, yet the library marks a row as loaded";
        EXPECT_TRUE(harness.gui->loadedModelName().empty()) << "nothing loaded, yet the editor names a model";
    }
}

// An absolute override loads a model the library lists under no row; the inspector once disabled its transform.
TEST(GoldenRender, EditorTransformsAModelTheLibraryDoesNotList)
{
    SKIP_WITHOUT_GPU();

    std::error_code absolute_error;
    const std::string unlisted =
      std::filesystem::absolute(sceneConfig::resolveModelPath(SHADOW_RIG_MODEL), absolute_error).string();
    ASSERT_FALSE(absolute_error) << "could not make the shadow rig's path absolute";
    ScopedModelOverride rig(unlisted.c_str());
    EngineHarness harness;
    harness.gui->setOverlayVisible(true);
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(harness.scene->getModelCount(), 1U) << "the absolute override " << unlisted << " did not load";

    auto &scene_vars = harness.settings.scene;
    EXPECT_EQ(scene_vars.selected_model_index, -1) << "no library row holds an absolute path";
    // The inspector enables Position, Rotation and Reset exactly when the editor names a model.
    EXPECT_EQ(harness.gui->loadedModelName(), std::string_view("shadow_rig"))
      << "the editor names no model for the scene's unlisted one, so its transform controls stay disabled";

    // What those controls write; the renderer must move the unlisted model with it.
    const std::array<float, 3> position = { 1.0F, 2.0F, 3.0F };
    std::copy(position.begin(), position.end(), std::begin(scene_vars.model_position));
    scene_vars.model_transform_changed = true;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    EXPECT_EQ(harness.scene->getModelMatrix(0),
      Kataglyphis::makeGuiModelTransform(
        std::span<const float, 3>(scene_vars.model_position), std::span<const float, 3>(scene_vars.model_rotation)))
      << "the transform did not reach the model the library does not list";
}

// A load places the model at the identity, so the inspector must too; it once kept the previous model's transform.
TEST(GoldenRender, ReloadResetsTheInspectorTransform)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    harness.render_frames(WARMUP_FRAMES);

    const std::array<float, 3> position = { 1.0F, 2.0F, 3.0F };
    const std::array<float, 3> rotation = { 10.0F, 20.0F, 30.0F };
    std::copy(position.begin(), position.end(), std::begin(scene_vars.model_position));
    std::copy(rotation.begin(), rotation.end(), std::begin(scene_vars.model_rotation));
    scene_vars.model_transform_changed = true;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_NE(harness.scene->getModelMatrix(0), glm::mat4(1.0F)) << "the transform never reached the model";

    const int reload_row = sceneConfig::modelIndexOf(sceneConfig::getAvailableModelPaths(), UV_TRANSFORM_MODEL);
    ASSERT_GE(reload_row, 0) << UV_TRANSFORM_MODEL << " is not in the model library";
    scene_vars.selected_model_index = reload_row;
    scene_vars.model_reload_requested = true;
    harness.render_frames(SETTLE_FRAMES);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(harness.scene->getModelCount(), 1U) << "the reload failed";

    const glm::mat4 shown = Kataglyphis::makeGuiModelTransform(
      std::span<const float, 3>(scene_vars.model_position), std::span<const float, 3>(scene_vars.model_rotation));
    EXPECT_EQ(shown, harness.scene->getModelMatrix(0))
      << "the inspector shows position (" << scene_vars.model_position[0] << ", " << scene_vars.model_position[1]
      << ", " << scene_vars.model_position[2] << ") for a model the reload placed elsewhere";
    EXPECT_EQ(scene_vars.selected_model_index, reload_row) << "the library does not mark the reloaded model";
}

// Ray tracing traces a binary shadow ray and path tracing uses NEE; the cascade pass once ran under both anyway.
TEST(GoldenRender, TracedFramesSkipTheShadowPass)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.useForwardRaster();
    harness.settings.scene.shadows_enabled = true;
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 1;
    const auto shadow_pass_ms = [&harness] {
        return harness.feedback.gpuTimings.pass_ms[static_cast<size_t>(GpuTimedPass::ShadowCascades)];
    };

    // A fresh query pool reads back nothing for its first frames, so the raster control runs long enough to sample.
    harness.render_frames(WARMUP_FRAMES + 10);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_GT(harness.feedback.visibility.shadow_casters_total, 0U) << "the raster frame drew no shadow casters";
    if (harness.feedback.gpuTimings.supported) {
        EXPECT_GE(shadow_pass_ms(), 0.0F) << "the raster frame's shadow pass reported no GPU time";
    }

    for (const bool raytracing : { true, false }) {
        renderer_vars.raytracing = raytracing;
        renderer_vars.pathTracing = !raytracing;
        // Past every swapchain image's timing slice, so no raster sample is left.
        harness.render_frames(6);
        ASSERT_FALSE(harness.renderer->hasDeviceLost());

        const char *mode = raytracing ? "ray traced" : "path traced";
        EXPECT_EQ(harness.feedback.visibility.shadow_casters_total, 0U)
          << mode << " frames still record the shadow pass";
        if (harness.feedback.gpuTimings.supported) {
            EXPECT_LT(shadow_pass_ms(), 0.0F) << mode << " frames still time a \"Shadow cascades\" pass";
        }
    }
}

// A white dielectric under a uniform sky of radiance 1, graded in linear radiance, where ACES hides a few percent.
TEST(GoldenRender, PathTracingPassesTheWhiteFurnaceTest)
{
    SKIP_WITHOUT_GPU();

    // The furnace metal golden covers metallic 1.
    const ScopedEnvVar furnace("KATAGLYPHIS_PT_FURNACE", "1.0");
    const ScopedEnvVar furnace_metallic("KATAGLYPHIS_PT_FURNACE_METALLIC", "0");
    std::optional<ScopedEnvVar> furnace_roughness;

    // A lone card returns E(mu) of the sky after one bounce: the first-bounce oracle for both lobes and their mixture.
    {
        ScopedModelOverride card(METALLIC_CARD_MODEL);
        EngineHarness harness;
        SKIP_WITHOUT_FRAME_CAPTURE(harness);
        if (!harness.renderer->supportsHardwareRaytracing()) {
            GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
        }

        auto &renderer_vars = harness.settings.renderer;
        harness.settings.scene.directional_light_radiance = 0.0F;
        renderer_vars.raytracing = false;
        renderer_vars.pathTracing = true;
        renderer_vars.pathTracingSamplesPerPixel = 4;
        renderer_vars.pathTracingMaxBounces = 2;
        harness.render_frames(WARMUP_FRAMES);

        struct View
        {
            float mu;
            glm::vec3 position;
            float yaw;
        };
        const std::array<View, 2> views = { View{ 1.0F, glm::vec3(0.0F, 0.0F, 2.0F), -90.0F },
            View{ 0.5F, glm::vec3(1.7320508F, 0.0F, 1.0F), -150.0F } };
        const int frames = path_traced_history(8, 4);
        // 0.25 narrows the GGX lobe until the 0.1 floor on its selection chance decides the mixture.
        for (const char *roughness : { "0.5", "0.25" }) {
            furnace_roughness.reset();
            furnace_roughness.emplace("KATAGLYPHIS_PT_FURNACE_ROUGHNESS", roughness);
            for (const View &view : views) {
                harness.camera->set_camera_position(view.position);
                harness.camera->set_orientation(view.yaw, 0.0F);
                harness.render_frames(frames);
                uint32_t width = 0;
                uint32_t height = 0;
                const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
                ASSERT_FALSE(harness.renderer->hasDeviceLost());
                ASSERT_FALSE(radiance.empty());

                const double measured = mean_hdr_in_square(radiance, width, width / 2U, height / 2U, 24U);
                const double expected = white_directional_albedo(view.mu, std::strtod(roughness, nullptr), 0.0);
                GTEST_LOG_(INFO) << "white dielectric card, roughness " << roughness << ", mu " << view.mu << ": "
                                 << measured << " of a sky of 1, E(mu) = " << expected;
                EXPECT_NEAR(measured, expected, 0.01)
                  << "the lobe mixture's sampling weight is off: roughness " << roughness << ", mu " << view.mu;
            }
        }
    }

    // Many bounces compound a per-bounce error; at 0.8 E(mu) stays in 0.981-0.997, capping the mean below 1.
    furnace_roughness.reset();
    furnace_roughness.emplace("KATAGLYPHIS_PT_FURNACE_ROUGHNESS", "0.8");
    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    auto &renderer_vars = harness.settings.renderer;
    auto &scene_vars = harness.settings.scene;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    // NEE would add energy on top of the furnace; many bounces keep truncation loss small.
    scene_vars.directional_light_radiance = 0.0F;
    renderer_vars.pathTracingMaxBounces = 16;
    harness.render_frames(WARMUP_FRAMES);
    harness.render_frames(path_traced_history(80, 40));

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(radiance.empty());

    // Measured 0.9823 in both variants; albedo 0.99 gives 0.9715 and 1.01 gives 0.9933, each past its bound.
    constexpr double FURNACE_PIXEL_BAND = 0.1;
    constexpr double FURNACE_RIG_MEAN_MIN = 0.977;
    constexpr double FURNACE_RIG_MEAN_MAX = 0.988;
    const uint32_t x0 = (width * 18U) / 25U;
    const uint32_t x1 = (width * 49U) / 50U;
    const uint32_t y0 = height / 20U;
    const uint32_t y1 = (height * 19U) / 20U;
    double sum = 0.0;
    size_t within = 0;
    size_t total = 0;
    for (uint32_t y = y0; y < y1; ++y) {
        for (uint32_t x = x0; x < x1; ++x) {
            const size_t base = (static_cast<size_t>(y) * width + x) * 4U;
            const double value = (radiance[base] + radiance[base + 1U] + radiance[base + 2U]) / 3.0;
            sum += value;
            if (std::abs(value - 1.0) <= FURNACE_PIXEL_BAND) { ++within; }
            ++total;
        }
    }
    const double mean = sum / static_cast<double>(total);
    const double uniform_fraction = static_cast<double>(within) / static_cast<double>(total);
    GTEST_LOG_(INFO) << "furnace rig crop mean radiance: " << mean << " (sky 1), fraction within +-"
                     << FURNACE_PIXEL_BAND << ": " << uniform_fraction;

    EXPECT_GT(mean, FURNACE_RIG_MEAN_MIN) << "Furnace converges LOW - the estimator is losing energy.";
    EXPECT_LT(mean, FURNACE_RIG_MEAN_MAX) << "Furnace converges HIGH - the estimator is gaining energy.";
    EXPECT_GT(uniform_fraction, 0.98) << "The furnace image is not uniform - geometry is visible, so some path "
                                         "class is biased.";
}

// A white metal reflects through its GGX lobe alone, so in a uniform sky it must return E(mu) of that sky, no more.
TEST(GoldenRender, PathTracingPassesTheWhiteMetalFurnaceTest)
{
    SKIP_WITHOUT_GPU();

    const ScopedEnvVar furnace("KATAGLYPHIS_PT_FURNACE", "1.0");
    const ScopedEnvVar furnace_metallic("KATAGLYPHIS_PT_FURNACE_METALLIC", "1");
    std::optional<ScopedEnvVar> furnace_roughness;
    // A lone card: every reflected ray reaches the sky, so a pixel's radiance is exactly E(mu) times its radiance.
    ScopedModelOverride card(METALLIC_CARD_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.settings.renderer;
    harness.settings.scene.directional_light_radiance = 0.0F;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.pathTracingSamplesPerPixel = 8;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);

    struct View
    {
        float mu;
        glm::vec3 position;
        float yaw;
    };
    // Two units from the card's centre, head-on and at 60 degrees; the measured square spans a few degrees.
    const std::array<View, 2> views = { View{ 1.0F, glm::vec3(0.0F, 0.0F, 2.0F), -90.0F },
        View{ 0.5F, glm::vec3(1.7320508F, 0.0F, 1.0F), -150.0F } };
    for (const char *roughness : { "0.3", "0.7" }) {
        furnace_roughness.reset();
        furnace_roughness.emplace("KATAGLYPHIS_PT_FURNACE_ROUGHNESS", roughness);
        for (const View &view : views) {
            // A camera move restarts the history, so each pair accumulates on its own.
            harness.camera->set_camera_position(view.position);
            harness.camera->set_orientation(view.yaw, 0.0F);
            harness.render_frames(12);
            uint32_t width = 0;
            uint32_t height = 0;
            const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
            ASSERT_FALSE(harness.renderer->hasDeviceLost());
            ASSERT_FALSE(radiance.empty());

            const double measured = mean_hdr_in_square(radiance, width, width / 2U, height / 2U, 24U);
            const double expected = white_directional_albedo(view.mu, std::strtod(roughness, nullptr), 1.0);
            GTEST_LOG_(INFO) << "white metal, roughness " << roughness << ", mu " << view.mu << ": " << measured
                             << " of a sky of 1, E(mu) = " << expected;
            // Single scattering only: a microfacet lobe keeps E(mu) <= 1, and the estimator must reproduce it.
            EXPECT_LE(expected, 1.0);
            EXPECT_NEAR(measured, expected, 0.03)
              << "the GGX lobe's sampling weight is off: roughness " << roughness << ", mu " << view.mu;
        }
    }
}

// A metal reflects only through its specular lobe; the path tracer once had none and rendered every metal black.
TEST(GoldenRender, PathTracedMetalCardReflectsTheSkyAndTheSun)
{
    SKIP_WITHOUT_GPU();

    const auto metal = write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_white_metal_card.gltf", [](auto &gltf) {
        auto &material = gltf["materials"][0];
        material["pbrMetallicRoughness"] = {
            { "baseColorFactor", { 0.9, 0.9, 0.9, 1.0 } }, { "metallicFactor", 1.0 }, { "roughnessFactor", 0.3 }
        };
        material["emissiveFactor"] = { 0.0, 0.0, 0.0 };
    });
    ASSERT_TRUE(metal.has_value()) << "could not write the white metal card";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.settings.renderer;
    auto &scene_vars = harness.settings.scene;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.pathTracingSamplesPerPixel = 4;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);

    // Mirror images of one placement; only the right card turns its normal halfway between the sun and the camera.
    const glm::vec3 right_centre(5.0F, 4.0F, 15.0F);
    const glm::vec3 to_sun = glm::normalize(-glm::vec3(scene_vars.directional_light_direction[0],
      scene_vars.directional_light_direction[1],
      scene_vars.directional_light_direction[2]));
    const glm::vec3 to_camera = glm::normalize(harness.camera->get_camera_position() - right_centre);
    const glm::vec3 mirror_normal = glm::normalize(to_sun + to_camera);
    const glm::vec3 card_normal(0.0F, 0.0F, 1.0F);
    const glm::mat4 highlight = glm::translate(glm::mat4(1.0F), right_centre)
                                * glm::rotate(glm::mat4(1.0F),
                                  std::acos(glm::dot(card_normal, mirror_normal)),
                                  glm::normalize(glm::cross(card_normal, mirror_normal)))
                                * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    const glm::vec3 left_centre(-right_centre.x, right_centre.y, right_centre.z);
    const glm::mat4 plain = glm::translate(glm::mat4(1.0F), left_centre) * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    ASSERT_TRUE(harness.renderer->addModel(metal->string(), highlight).has_value()) << "adding the tilted card failed";
    ASSERT_TRUE(harness.renderer->addModel(metal->string(), plain).has_value()) << "adding the plain card failed";
    harness.render_frames(SETTLE_FRAMES + 12);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = harness.capture_frame(width, height);
    ASSERT_FALSE(frame.empty());
    const Crop card_box = panel_free_card_box(width, height);
    const uint32_t centre_x = (card_box.x0 + card_box.x1) / 2U;
    const uint32_t centre_y = (card_box.y0 + card_box.y1) / 2U;
    const double highlight_mean = mean_luminance_in_square(frame, width, centre_x, centre_y, 20U);
    const double plain_mean = mean_luminance_in_square(frame, width, width - centre_x, centre_y, 20U);
    GTEST_LOG_(INFO) << "path-traced white metal: " << highlight_mean << " where the sun mirrors into the camera, "
                     << plain_mean << " facing it";

    // Facing the camera, the card mirrors the sky behind it at F0 0.9: lit, if far from the sun's highlight.
    EXPECT_GT(plain_mean, 60.0) << "the metal card is (nearly) black: no specular lobe reflects the sky";
    // The sun's GGX peak is a hundredfold the sky; tonemapped it is white.
    EXPECT_GT(highlight_mean, 245.0) << "no specular highlight where the sun mirrors into the camera";
    EXPECT_GT(highlight_mean, plain_mean + 30.0) << "the highlight is not brighter than the reflected sky";
}

// An unlit glTF material that omits metallicFactor (1 by the spec) once went black in PT: metallic darkened it first.
TEST(GoldenRender, PathTracedUnlitCardKeepsItsAlbedo)
{
    SKIP_WITHOUT_GPU();

    const auto unlit = write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_unlit_orange_card.gltf", [](auto &gltf) {
        auto &material = gltf["materials"][0];
        material["pbrMetallicRoughness"] = { { "baseColorFactor", { 0.8, 0.4, 0.1, 1.0 } } };
        material["emissiveFactor"] = { 0.0, 0.0, 0.0 };
        material["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
        gltf["extensionsUsed"] = nlohmann::json::array({ "KHR_materials_unlit" });
    });
    ASSERT_TRUE(unlit.has_value()) << "could not write the unlit card";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.useForwardRaster();
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_TRUE(harness.renderer->addModel(unlit->string(), panel_free_card_placement()).has_value())
      << "adding the unlit card failed";
    harness.render_frames(SETTLE_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<float> forward = harness.capture_hdr_frame(width, height);
    renderer_vars.pathTracing = true;
    // Unlit ends every path at its first hit, so the card interior converges in one frame.
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<float> traced = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(forward.empty());
    ASSERT_EQ(traced.size(), forward.size());

    const Crop card_box = panel_free_card_box(width, height);
    const uint32_t centre_x = (card_box.x0 + card_box.x1) / 2U;
    const uint32_t centre_y = (card_box.y0 + card_box.y1) / 2U;
    const size_t centre = (static_cast<size_t>(centre_y) * width + centre_x) * 4U;
    GTEST_LOG_(INFO) << "unlit card centre: forward (" << forward[centre] << ", " << forward[centre + 1U] << ", "
                     << forward[centre + 2U] << "), path traced (" << traced[centre] << ", " << traced[centre + 1U]
                     << ", " << traced[centre + 2U] << ")";

    // Raster returns the albedo for an unlit material; the path tracer must add it as the path's last radiance.
    constexpr std::array<float, 3> ALBEDO = { 0.8F, 0.4F, 0.1F };
    for (size_t channel = 0; channel < 3U; ++channel) {
        EXPECT_NEAR(forward[centre + channel], ALBEDO[channel], 0.01F)
          << "forward does not show the unlit card's albedo in channel " << channel << " - check the placement";
        EXPECT_NEAR(traced[centre + channel], forward[centre + channel], 0.01F)
          << "the path tracer darkens an unlit material that omits metallicFactor, channel " << channel;
    }
}

// A metallic-roughness texel with G = 0 once made GGX alpha 0: the visible-normal density's 0/0 broke whole samples.
TEST(GoldenRender, PathTracingFloorsAZeroRoughnessTexel)
{
    SKIP_WITHOUT_GPU();

    // White dielectrics: one at roughness factor 1 times a G of 0, one at the 0.045 floor times a G of 1.
    const auto roughness_card = [](const char *file_name, const char *texel, double roughness_factor) {
        return write_gltf_variant(EMISSIVE_CARD_MODEL, file_name, [texel, roughness_factor](nlohmann::json &gltf) {
            auto &material = gltf["materials"][0];
            nlohmann::json texture_ref;
            texture_ref["index"] = 0;
            material["pbrMetallicRoughness"] = { { "baseColorFactor", { 0.8, 0.8, 0.8, 1.0 } },
                { "metallicFactor", 1.0 },
                { "roughnessFactor", roughness_factor },
                { "metallicRoughnessTexture", texture_ref } };
            material["emissiveFactor"] = { 0.0, 0.0, 0.0 };
            nlohmann::json image;
            image["uri"] = texel;
            nlohmann::json texture;
            texture["source"] = 0;
            gltf["images"] = nlohmann::json::array({ image });
            gltf["textures"] = nlohmann::json::array({ texture });
        });
    };
    const auto zero_card = roughness_card("kataglyphis_zero_roughness_card.gltf", ZERO_ROUGHNESS_PNG, 1.0);
    const auto floor_card = roughness_card("kataglyphis_floor_roughness_card.gltf", FULL_ROUGHNESS_PNG, 0.045);
    ASSERT_TRUE(zero_card.has_value() && floor_card.has_value()) << "could not write the roughness cards";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; path tracing unavailable.";
    }

    auto &renderer_vars = harness.settings.renderer;
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    renderer_vars.rasterizationMode = RasterizationMode::Forward;
    renderer_vars.pathTracingSamplesPerPixel = 2;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);

    const glm::mat4 right = panel_free_card_placement();
    const glm::mat4 left = glm::translate(glm::mat4(1.0F), glm::vec3(-2.0F * right[3].x, 0.0F, 0.0F)) * right;
    ASSERT_TRUE(harness.renderer->addModel(zero_card->string(), right).has_value()) << "adding the G = 0 card failed";
    ASSERT_TRUE(harness.renderer->addModel(floor_card->string(), left).has_value()) << "adding the floor card failed";
    harness.render_frames(SETTLE_FRAMES + 16);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(radiance.empty());
    const Crop zero_box = panel_free_card_box(width, height);
    const uint32_t centre_y = (zero_box.y0 + zero_box.y1) / 2U;
    const uint32_t zero_x = (zero_box.x0 + zero_box.x1) / 2U;
    const double zero_mean = mean_hdr_in_square(radiance, width, zero_x, centre_y, 40U);
    const double floor_mean = mean_hdr_in_square(radiance, width, width - zero_x, centre_y, 40U);
    GTEST_LOG_(INFO) << "path-traced card radiance: texel G 0 " << zero_mean << ", roughness floor " << floor_mean;

    ASSERT_GT(floor_mean, 0.05) << "the floor card is unlit - check the placement";
    // The floor makes the two materials one; NaN-zeroed or 1e-6-divided samples move the mean by far more.
    EXPECT_NEAR(zero_mean, floor_mean, 0.03 * floor_mean)
      << "a roughness texel of 0 shades unlike the 0.045 floor: GGX alpha 0 reached the path tracer";
}

// A smooth low-poly mesh's vertex normal can face away where its triangle faces the camera; raster sides by winding.
TEST(GoldenRender, TracedHitsTakeTheirSideFromTheTriangleWinding)
{
    SKIP_WITHOUT_GPU();

    const auto grey_card = [](nlohmann::json &gltf) {
        auto &material = gltf["materials"][0];
        material["pbrMetallicRoughness"] = {
            { "baseColorFactor", { 0.8, 0.8, 0.8, 1.0 } }, { "metallicFactor", 0.0 }, { "roughnessFactor", 0.5 }
        };
        material["emissiveFactor"] = { 0.0, 0.0, 0.0 };
        material["doubleSided"] = false;
    };
    const auto flat = write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_flat_grey_card.gltf", grey_card);
    const auto leaning =
      write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_leaning_normal_card.gltf", [&](auto &gltf) {
          grey_card(gltf);
          nlohmann::json buffer;
          buffer["uri"] = LEANING_VERTEX_NORMALS;
          buffer["byteLength"] = 48;
          gltf["buffers"].push_back(buffer);
          nlohmann::json view;
          view["buffer"] = gltf["buffers"].size() - 1U;
          view["byteOffset"] = 0;
          view["byteLength"] = 48;
          view["target"] = 34962;
          gltf["bufferViews"].push_back(view);
          nlohmann::json accessor;
          accessor["bufferView"] = gltf["bufferViews"].size() - 1U;
          accessor["componentType"] = 5126;
          accessor["count"] = 4;
          accessor["type"] = "VEC3";
          gltf["accessors"].push_back(accessor);
          gltf["meshes"][0]["primitives"][0]["attributes"]["NORMAL"] = gltf["accessors"].size() - 1U;
      });
    const auto emitter =
      write_gltf_variant(EMISSIVE_STRENGTH_CARD_MODEL, "kataglyphis_strong_emitter.gltf", [](auto &gltf) {
          gltf["materials"][0]["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] = 50.0;
      });
    ASSERT_TRUE(flat.has_value() && leaning.has_value() && emitter.has_value()) << "could not write the cards";

    ScopedModelOverride card(leaning->string().c_str());
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) {
        GTEST_SKIP() << "Hardware raytracing unsupported; ray tracing unavailable.";
    }

    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.shadows_enabled = false;
    // Lit along the leaning normal: raster's N.L is 0.96, a normal flipped by the ray's side gets none.
    scene_vars.directional_light_direction[0] = -0.9F;
    scene_vars.directional_light_direction[1] = -0.3F;
    scene_vars.directional_light_direction[2] = -0.33F;
    renderer_vars.pathTracingSamplesPerPixel = 4;
    renderer_vars.pathTracingMaxBounces = 2;
    // 60 degrees off the card's axis toward -X: the triangle faces the camera, the vertex normal (N.V -0.64) does not.
    harness.camera->set_camera_position(glm::vec3(-1.7320508F, 0.0F, 1.0F));
    harness.camera->set_orientation(-30.0F, 0.0F);
    harness.render_frames(WARMUP_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<float> forward = harness.capture_hdr_frame(width, height);
    renderer_vars.raytracing = true;
    harness.render_frames(SETTLE_FRAMES);
    const std::vector<float> traced = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_FALSE(forward.empty());
    ASSERT_EQ(traced.size(), forward.size());
    const double forward_mean = mean_hdr_in_square(forward, width, width / 2U, height / 2U, 24U);
    const double traced_mean = mean_hdr_in_square(traced, width, width / 2U, height / 2U, 24U);
    GTEST_LOG_(INFO) << "leaning-normal card: forward " << forward_mean << ", ray traced " << traced_mean;
    ASSERT_GT(forward_mean, 0.05) << "forward leaves the card unlit - check the light or the placement";
    EXPECT_NEAR(traced_mean, forward_mean, 0.05 * forward_mean)
      << "the ray-traced hit flips its normal by the vertex normal's side, not the triangle's winding";

    // Sun off: the front sees only the sky, and an emitter wholly behind the card's plane must add nothing.
    renderer_vars.raytracing = false;
    renderer_vars.pathTracing = true;
    scene_vars.directional_light_radiance = 0.0F;
    harness.render_frames(8);
    const std::vector<float> before = harness.capture_hdr_frame(width, height);
    const glm::mat4 behind = glm::translate(glm::mat4(1.0F), glm::vec3(-1.5F, 0.0F, -1.2F))
                             * glm::rotate(glm::mat4(1.0F), glm::half_pi<float>(), glm::vec3(0.0F, 1.0F, 0.0F))
                             * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    ASSERT_TRUE(harness.renderer->addModel(emitter->string(), behind).has_value()) << "adding the emitter failed";
    harness.render_frames(8);
    const std::vector<float> after = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(after.size(), before.size());
    const double before_mean = mean_hdr_in_square(before, width, width / 2U, height / 2U, 24U);
    const double after_mean = mean_hdr_in_square(after, width, width / 2U, height / 2U, 24U);
    GTEST_LOG_(INFO) << "path-traced leaning-normal card: " << before_mean << " alone, " << after_mean
                     << " with an emitter behind it";
    ASSERT_GT(before_mean, 0.02) << "the path-traced card does not see the sky - check the placement";
    EXPECT_NEAR(after_mean, before_mean, 0.05 * before_mean)
      << "bounces leave through the triangle toward an emitter behind it: the path bound is the vertex normal";

    // A vertex normal facing away from the viewer leaves no lobe, so the triangle's normal shades, as on a flat twin.
    const glm::mat4 in_front = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, 0.001F));
    ASSERT_TRUE(harness.renderer->addModel(flat->string(), in_front).has_value()) << "adding the flat card failed";
    harness.render_frames(8);
    const std::vector<float> flat_frame = harness.capture_hdr_frame(width, height);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    ASSERT_EQ(flat_frame.size(), before.size());
    const double flat_mean = mean_hdr_in_square(flat_frame, width, width / 2U, height / 2U, 24U);
    GTEST_LOG_(INFO) << "path-traced flat twin: " << flat_mean;
    EXPECT_NEAR(before_mean, flat_mean, 0.05 * flat_mean)
      << "the path-traced card shades unlike its flat twin: its side or its path bound follows the vertex normal";
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

    auto &s = harness.settings.scene;
    auto &r = harness.settings.renderer;

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

// A cross-frame hazard shows as a hang, not a pixel; see docs/source/clouds.md § Queue ownership and barriers.
TEST(GoldenRender, CloudsAcrossManyFramesDoesNotLoseTheDevice)
{
    SKIP_WITHOUT_GPU();

    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
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
    auto &scene_vars = harness.settings.scene;
    scene_vars.clouds_enabled = false;

    // Test-only: the default slab sits far above the camera's eye line, and default wisps are too faint to measure.
    place_cloud_box(scene_vars,
      glm::vec3(scene_vars.cloud_mesh_offset[0], 6.0F, scene_vars.cloud_mesh_offset[2]),
      glm::vec3(20000.0F, 100.0F, 20000.0F),
      2.0F);
    scene_vars.cloud_coverage_threshold = 0.60F;

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

// Pixels a clouds-on capture changes against a clouds-off one, per image half; "drifted" counts any 3-level move.
struct CloudFootprint
{
    size_t changed = 0;
    double centroid_x = 0.0;
    std::array<size_t, 2> changed_per_half{};
    std::array<size_t, 2> drifted_per_half{};
};

static CloudFootprint
  cloud_footprint(const std::vector<uint8_t> &off, const std::vector<uint8_t> &on, uint32_t w, uint32_t h)
{
    CloudFootprint footprint;
    double column_sum = 0.0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const int delta = channel_delta(off, on, static_cast<size_t>(y) * w + x);
            const size_t half = x < w / 2U ? 0U : 1U;
            if (delta > 2) { ++footprint.drifted_per_half[half]; }
            if (delta <= 5) { continue; }
            ++footprint.changed;
            ++footprint.changed_per_half[half];
            column_sum += static_cast<double>(x);
        }
    }
    footprint.centroid_x = footprint.changed > 0U ? column_sum / static_cast<double>(footprint.changed) : 0.0;
    return footprint;
}

// Offset places the box and Scale sizes it; the box was once a sheared slab that Offset could not move.
TEST(GoldenRender, CloudOffsetMovesTheCloudVolume)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.clouds_enabled = false;
    scene_vars.cloud_coverage_threshold = 0.3F;
    harness.render_frames(WARMUP_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> baseline = harness.capture_frame(width, height);
    ASSERT_FALSE(baseline.empty());
    dump_cloud_frame("offset-baseline", baseline, width, height);

    // 3-unit boxes between the camera and the rig, either side of the view axis; each projects inside its own half.
    const auto footprint_of_box_at = [&](float x) {
        place_cloud_box(scene_vars, glm::vec3(x, 6.0F, 12.0F), glm::vec3(3.0F), 2.0F);
        scene_vars.clouds_enabled = true;
        harness.render_frames(SETTLE_FRAMES);
        uint32_t w = 0;
        uint32_t h = 0;
        const std::vector<uint8_t> frame = harness.capture_frame(w, h);
        dump_cloud_frame(x < 0.0F ? "offset-left" : "offset-right", frame, w, h);
        EXPECT_EQ(frame.size(), baseline.size());
        return frame.size() == baseline.size() ? cloud_footprint(baseline, frame, width, height) : CloudFootprint{};
    };
    const CloudFootprint left_box = footprint_of_box_at(-6.0F);
    const CloudFootprint right_box = footprint_of_box_at(6.0F);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());

    GTEST_LOG_(INFO) << "cloud box at x=-6: " << left_box.changed << " px, centroid column " << left_box.centroid_x
                     << "; at x=+6: " << right_box.changed << " px, centroid column " << right_box.centroid_x << " of "
                     << width;

    const double pixels = static_cast<double>(width) * static_cast<double>(height);
    const double half_pixels = pixels / 2.0;
    ASSERT_GT(static_cast<double>(left_box.changed), 0.005 * pixels) << "the box at x=-6 drew no clouds";
    ASSERT_GT(static_cast<double>(right_box.changed), 0.005 * pixels) << "the box at x=+6 drew no clouds";
    EXPECT_LT(left_box.centroid_x, 0.4 * width) << "the box at x=-6 does not show left of the view axis";
    EXPECT_GT(right_box.centroid_x, 0.6 * width) << "the box at x=+6 does not show right of the view axis";
    // Each box leaves the far half alone, the sky included: there post's HDR sky must reproduce the sky pass.
    EXPECT_LT(static_cast<double>(left_box.changed_per_half[1]), 0.001 * half_pixels)
      << "the box at x=-6 changed the right half, outside its 3-unit extent";
    EXPECT_LT(static_cast<double>(right_box.changed_per_half[0]), 0.001 * half_pixels)
      << "the box at x=+6 changed the left half, outside its 3-unit extent";
    EXPECT_LT(static_cast<double>(left_box.drifted_per_half[1]), 0.002 * half_pixels)
      << "with clouds on, cloud-free pixels drifted from the clouds-off frame: the sky's inverse tonemap is off";
    EXPECT_LT(static_cast<double>(right_box.drifted_per_half[0]), 0.002 * half_pixels)
      << "with clouds on, cloud-free pixels drifted from the clouds-off frame: the sky's inverse tonemap is off";
}

// A wall in front of the box hides it: a box behind the rig's slab shows around the slab, never on it.
TEST(GoldenRender, CloudsStopAtTheSceneDepth)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    // The disabled sky is one known colour, so a pixel that is not it is geometry.
    scene_vars.skybox_enabled = false;
    scene_vars.cloud_coverage_threshold = 0.3F;
    // Behind the slab (z -10..2) and above the ground; rays over and under the slab reach it.
    place_cloud_box(scene_vars, glm::vec3(0.0F, 9.0F, -20.0F), glm::vec3(40.0F, 6.0F, 4.0F), 1.0F);
    harness.render_frames(WARMUP_FRAMES);

    for (const RasterizationMode path : { RasterizationMode::Forward, RasterizationMode::Deferred }) {
        const char *name = path == RasterizationMode::Forward ? "forward" : "deferred";
        renderer_vars.rasterizationMode = path;

        scene_vars.clouds_enabled = false;
        harness.render_frames(SETTLE_FRAMES);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<uint8_t> off = harness.capture_frame(width, height);
        scene_vars.clouds_enabled = true;
        harness.render_frames(SETTLE_FRAMES);
        uint32_t w2 = 0;
        uint32_t h2 = 0;
        const std::vector<uint8_t> on = harness.capture_frame(w2, h2);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << name;
        ASSERT_FALSE(off.empty()) << name;
        ASSERT_EQ(off.size(), on.size()) << name;
        dump_cloud_frame(std::string("depth-") + name + "-off", off, width, height);
        dump_cloud_frame(std::string("depth-") + name + "-on", on, width, height);

        size_t geometry = 0;
        size_t geometry_changed = 0;
        size_t sky = 0;
        size_t sky_changed = 0;
        for (size_t pixel = 0; pixel < off.size() / 4U; ++pixel) {
            const int delta = channel_delta(off, on, pixel);
            if (is_disabled_sky(off, pixel)) {
                ++sky;
                if (delta > 5) { ++sky_changed; }
            } else {
                ++geometry;
                if (delta > 2) { ++geometry_changed; }
            }
        }
        GTEST_LOG_(INFO) << name << ": " << geometry_changed << " of " << geometry << " geometry px changed, "
                         << sky_changed << " of " << sky << " sky px";

        ASSERT_GT(geometry, off.size() / 4U / 10U) << name << ": the rig fills too little of the frame to judge";
        EXPECT_GT(static_cast<double>(sky_changed), 0.1 * static_cast<double>(sky))
          << name << ": the box behind the slab drew no clouds around it - nothing to hide";
        EXPECT_LT(static_cast<double>(geometry_changed), 0.001 * static_cast<double>(geometry))
          << name << ": clouds behind the slab drew over it - the march ignores the scene depth";
    }
}

// RT and PT write no depth buffer, so their own primary hits must hide the box, as the raster depth does.
TEST(GoldenRender, CloudsStopAtTracedGeometry)
{
    SKIP_WITHOUT_GPU();

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.skybox_enabled = false;
    scene_vars.cloud_coverage_threshold = 0.3F;
    place_cloud_box(scene_vars, glm::vec3(0.0F, 9.0F, -20.0F), glm::vec3(40.0F, 6.0F, 4.0F), 1.0F);
    // One path per pixel and frame: the depth is what is measured here, not the image.
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 1;
    harness.render_frames(WARMUP_FRAMES);

    // Raster names the geometry; eroded, as RT's pixel centres and PT's jitter may disagree with it on an edge.
    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> raster = harness.capture_frame(width, height);
    ASSERT_FALSE(raster.empty());
    std::vector<bool> geometry_mask(raster.size() / 4U);
    std::vector<bool> sky_mask(raster.size() / 4U);
    for (size_t pixel = 0; pixel < geometry_mask.size(); ++pixel) {
        sky_mask[pixel] = is_disabled_sky(raster, pixel);
        geometry_mask[pixel] = !sky_mask[pixel];
    }
    geometry_mask = eroded(geometry_mask, width, height);
    sky_mask = eroded(sky_mask, width, height);

    struct Mode
    {
        const char *name;
        bool raytracing;
        int frames;
    };
    for (const Mode mode : { Mode{ "ray traced", true, SETTLE_FRAMES }, Mode{ "path traced", false, 8 } }) {
        renderer_vars.raytracing = mode.raytracing;
        renderer_vars.pathTracing = !mode.raytracing;

        scene_vars.clouds_enabled = false;
        harness.render_frames(mode.frames);
        uint32_t w1 = 0;
        uint32_t h1 = 0;
        const std::vector<uint8_t> off = harness.capture_frame(w1, h1);
        scene_vars.clouds_enabled = true;
        harness.render_frames(mode.frames);
        uint32_t w2 = 0;
        uint32_t h2 = 0;
        const std::vector<uint8_t> on = harness.capture_frame(w2, h2);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << mode.name;
        ASSERT_EQ(off.size(), raster.size()) << mode.name;
        ASSERT_EQ(on.size(), raster.size()) << mode.name;

        size_t geometry = 0;
        size_t geometry_changed = 0;
        size_t sky = 0;
        size_t sky_changed = 0;
        for (size_t pixel = 0; pixel < geometry_mask.size(); ++pixel) {
            const int delta = channel_delta(off, on, pixel);
            if (geometry_mask[pixel]) {
                ++geometry;
                // PT keeps accumulating across the toggle, so a penumbra pixel may still settle by a few levels.
                if (delta > (mode.raytracing ? 2 : 16)) { ++geometry_changed; }
            } else if (sky_mask[pixel]) {
                ++sky;
                if (delta > 5) { ++sky_changed; }
            }
        }
        GTEST_LOG_(INFO) << mode.name << ": " << geometry_changed << " of " << geometry << " geometry px changed, "
                         << sky_changed << " of " << sky << " sky px";

        ASSERT_GT(geometry, geometry_mask.size() / 10U) << mode.name;
        EXPECT_GT(static_cast<double>(sky_changed), 0.1 * static_cast<double>(sky))
          << mode.name << ": the box behind the slab drew no clouds around it - nothing to hide";
        EXPECT_LT(static_cast<double>(geometry_changed), 0.005 * static_cast<double>(geometry))
          << mode.name << ": clouds behind the slab drew over it - the traced depth does not reach the march";
    }
}

// Over a sky of zero red, a cloud's red is its own tonemapped light, as over a black card; once it was scaled by alpha.
TEST(GoldenRender, CloudsCompositeOverTheSkyAsOverGeometry)
{
    SKIP_WITHOUT_GPU();

    const auto black_card =
      write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_black_unlit_card.gltf", [](auto &gltf) {
          auto &material = gltf["materials"][0];
          material["pbrMetallicRoughness"]["baseColorFactor"] = { 0.0, 0.0, 0.0, 1.0 };
          material["emissiveFactor"] = { 0.0, 0.0, 0.0 };
          material["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
          gltf["extensionsUsed"] = nlohmann::json::array({ "KHR_materials_unlit" });
      });
    ASSERT_TRUE(black_card.has_value()) << "could not write the black unlit card";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    // The disabled sky is linear (0, 0, 0.5): no red, like the black card.
    scene_vars.skybox_enabled = false;
    scene_vars.cloud_coverage_threshold = 0.3F;
    // Powder darkens thin cloud to almost nothing, and thin, lit cloud is what tells the two composites apart.
    scene_vars.cloud_powder_effect = false;
    // Beyond the ground's far edge, in the band of sky under the slab; the card stands behind them.
    place_cloud_box(scene_vars, glm::vec3(0.0F, 4.0F, -40.0F), glm::vec3(40.0F, 4.0F, 5.0F), 0.6F);
    harness.render_frames(WARMUP_FRAMES);

    uint32_t width = 0;
    uint32_t height = 0;
    const auto capture = [&](bool clouds) {
        scene_vars.clouds_enabled = clouds;
        harness.render_frames(SETTLE_FRAMES);
        return harness.capture_frame(width, height);
    };
    const std::vector<uint8_t> sky_off = capture(false);
    const std::vector<uint8_t> sky_on = capture(true);

    const glm::mat4 card_placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 5.0F, -60.0F)) * glm::scale(glm::mat4(1.0F), glm::vec3(20.0F));
    ASSERT_TRUE(harness.renderer->addModel(black_card->string(), card_placement).has_value())
      << "adding the black card failed";
    const std::vector<uint8_t> card_off = capture(false);
    const std::vector<uint8_t> card_on = capture(true);
    ASSERT_FALSE(harness.renderer->hasDeviceLost());
    for (const auto &[name, frame] : { std::pair{ "sky-off", &sky_off },
           std::pair{ "sky-on", &sky_on },
           std::pair{ "card-off", &card_off },
           std::pair{ "card-on", &card_on } }) {
        dump_cloud_frame(std::string("composite-") + name, *frame, width, height);
    }
    ASSERT_FALSE(sky_off.empty());
    ASSERT_EQ(sky_on.size(), sky_off.size());
    ASSERT_EQ(card_off.size(), sky_off.size());
    ASSERT_EQ(card_on.size(), sky_off.size());

    // Pixels that were sky before the card and are the card after it: the same cloud over both backgrounds.
    size_t region = 0;
    size_t clouded = 0;
    size_t mismatched = 0;
    for (size_t pixel = 0; pixel < sky_off.size() / 4U; ++pixel) {
        const size_t base = pixel * 4U;
        const bool card = card_off[base] <= 2U && card_off[base + 1U] <= 2U && card_off[base + 2U] <= 2U;
        if (!is_disabled_sky(sky_off, pixel) || !card) { continue; }
        ++region;
        if (sky_on[base] >= 10U) { ++clouded; }
        const int red = std::abs(static_cast<int>(sky_on[base]) - static_cast<int>(card_on[base]));
        const int green = std::abs(static_cast<int>(sky_on[base + 1U]) - static_cast<int>(card_on[base + 1U]));
        if (std::max(red, green) > 3) { ++mismatched; }
    }
    GTEST_LOG_(INFO) << "sky-then-card region: " << region << " px, " << clouded << " clouded, " << mismatched
                     << " with red or green off by more than 3 levels between sky and card";

    ASSERT_GT(region, 2000U) << "the card does not cover enough former sky - check the framing";
    ASSERT_GT(static_cast<double>(clouded), 0.2 * static_cast<double>(region))
      << "the clouds in front of the card are too thin to measure";
    EXPECT_LT(static_cast<double>(mismatched), 0.01 * static_cast<double>(region))
      << "the same cloud shows a different red over the sky than over a black card: the sky composite is not the "
         "linear HDR premultiplied-over that geometry gets";
}

// A box in front of the slab and an unlit card must cloud them in every mode; RT once put an unlit hit at the eye.
TEST(GoldenRender, CloudsInFrontOfGeometryCoverItInEveryMode)
{
    SKIP_WITHOUT_GPU();

    // Unlit, so its colour is the same in every mode and the path tracer adds no noise over it.
    const auto green_card =
      write_gltf_variant(EMISSIVE_CARD_MODEL, "kataglyphis_green_unlit_card.gltf", [](auto &gltf) {
          auto &material = gltf["materials"][0];
          material["pbrMetallicRoughness"]["baseColorFactor"] = { 0.0, 1.0, 0.0, 1.0 };
          material["emissiveFactor"] = { 0.0, 0.0, 0.0 };
          material["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
          gltf["extensionsUsed"] = nlohmann::json::array({ "KHR_materials_unlit" });
      });
    ASSERT_TRUE(green_card.has_value()) << "could not write the green unlit card";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    const bool traced = harness.renderer->supportsHardwareRaytracing();

    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.skybox_enabled = false;
    // Threshold 0 fills the box with fog, so every ray through it meets cloud.
    scene_vars.cloud_coverage_threshold = 0.0F;
    // z 4..8, between the camera (26) and the card (3), which stands one unit before the slab's face (2).
    place_cloud_box(scene_vars, glm::vec3(0.0F, 9.0F, 6.0F), glm::vec3(8.0F, 2.5F, 2.0F), 2.0F);
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 1;
    harness.render_frames(WARMUP_FRAMES);
    const glm::mat4 card_placement =
      glm::translate(glm::mat4(1.0F), glm::vec3(2.0F, 9.0F, 3.0F)) * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    ASSERT_TRUE(harness.renderer->addModel(green_card->string(), card_placement).has_value())
      << "adding the green card failed";

    uint32_t width = 0;
    uint32_t height = 0;
    const auto capture = [&](bool clouds) {
        scene_vars.clouds_enabled = clouds;
        harness.render_frames(SETTLE_FRAMES);
        return harness.capture_frame(width, height);
    };

    // Forward names the regions: the card by its green, the geometry the clouds cover by what they change.
    const std::vector<uint8_t> forward_off = capture(false);
    const std::vector<uint8_t> forward_on = capture(true);
    ASSERT_FALSE(forward_off.empty());
    ASSERT_EQ(forward_on.size(), forward_off.size());
    std::vector<bool> card_mask(forward_off.size() / 4U);
    std::vector<bool> covered_mask(forward_off.size() / 4U);
    for (size_t pixel = 0; pixel < card_mask.size(); ++pixel) {
        const size_t base = pixel * 4U;
        card_mask[pixel] = forward_off[base] < 40U && forward_off[base + 1U] > 200U && forward_off[base + 2U] < 40U;
        covered_mask[pixel] = !is_disabled_sky(forward_off, pixel) && !card_mask[pixel]
                              && channel_delta(forward_off, forward_on, pixel) > 5;
    }
    // Eroded, as RT's pixel centres and PT's jitter may disagree with raster on an edge.
    card_mask = eroded(card_mask, width, height);
    covered_mask = eroded(covered_mask, width, height);
    const size_t card_pixels = static_cast<size_t>(std::count(card_mask.begin(), card_mask.end(), true));
    const size_t covered_pixels = static_cast<size_t>(std::count(covered_mask.begin(), covered_mask.end(), true));
    ASSERT_GT(card_pixels, 1000U) << "the green card fills too little of the frame - check the placement";
    ASSERT_GT(covered_pixels, 2000U) << "forward shows no cloud over the lit geometry behind the box";

    struct Mode
    {
        const char *name;
        RasterizationMode raster;
        bool raytracing;
        bool pathTracing;
    };
    std::vector<Mode> modes = { Mode{ "forward", RasterizationMode::Forward, false, false },
        Mode{ "deferred", RasterizationMode::Deferred, false, false } };
    if (traced) {
        modes.push_back(Mode{ "ray traced", RasterizationMode::Forward, true, false });
        modes.push_back(Mode{ "path traced", RasterizationMode::Forward, false, true });
    }
    for (const Mode &mode : modes) {
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        renderer_vars.pathTracing = mode.pathTracing;
        const std::vector<uint8_t> off = capture(false);
        const std::vector<uint8_t> on = capture(true);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << mode.name;
        ASSERT_EQ(off.size(), forward_off.size()) << mode.name;
        ASSERT_EQ(on.size(), forward_off.size()) << mode.name;

        size_t card_clouded = 0;
        size_t covered_clouded = 0;
        for (size_t pixel = 0; pixel < card_mask.size(); ++pixel) {
            if (card_mask[pixel] && channel_delta(off, on, pixel) > 5) { ++card_clouded; }
            if (covered_mask[pixel] && channel_delta(off, on, pixel) > 5) { ++covered_clouded; }
        }
        GTEST_LOG_(INFO) << mode.name << ": clouds change " << card_clouded << " of " << card_pixels << " card px and "
                         << covered_clouded << " of " << covered_pixels << " lit px behind the box";

        EXPECT_GT(static_cast<double>(card_clouded), 0.9 * static_cast<double>(card_pixels))
          << mode.name << ": clouds in front of the unlit card vanish - its depth reaches the march too near";
        // The path tracer's lit pixels move by noise alone, so its depth is judged on the noise-free card.
        if (!mode.pathTracing) {
            EXPECT_GT(static_cast<double>(covered_clouded), 0.9 * static_cast<double>(covered_pixels))
              << mode.name << ": clouds in front of lit geometry vanish - the depth reaches the march too near";
        }
    }
}

// Radiance past fp16's 65504 stores as +Inf (lavapipe: 65504), and the tonemap turns Inf into black NaN.
TEST(GoldenRender, HdrTargetStaysBelowTheHalfFloatMaximum)
{
    SKIP_WITHOUT_GPU();

    const auto lamp =
      write_gltf_variant(EMISSIVE_STRENGTH_CARD_MODEL, "kataglyphis_overbright_card.gltf", [](auto &gltf) {
          gltf["materials"][0]["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] = 1.0e6;
      });
    ASSERT_TRUE(lamp.has_value()) << "could not write the overbright card";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.useForwardRaster();
    renderer_vars.pathTracingSamplesPerPixel = 1;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);
    ASSERT_TRUE(harness.renderer->addModel(lamp->string(), panel_free_card_placement()).has_value())
      << "adding the overbright card failed";

    struct Mode
    {
        const char *name;
        RasterizationMode raster;
        bool raytracing;
        bool pathTracing;
    };
    std::vector<Mode> modes = { Mode{ "forward", RasterizationMode::Forward, false, false },
        Mode{ "deferred", RasterizationMode::Deferred, false, false } };
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back(Mode{ "ray traced", RasterizationMode::Forward, true, false });
        modes.push_back(Mode{ "path traced", RasterizationMode::Forward, false, true });
    }
    // hdr_safe.slang's HDR_MAX, as fp16 rounds it; every pass that writes the HDR target clamps to it.
    constexpr float HDR_MAX = 65000.0F;
    for (const Mode &mode : modes) {
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        renderer_vars.pathTracing = mode.pathTracing;
        harness.render_frames(SETTLE_FRAMES);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
        const std::vector<uint8_t> frame = harness.capture_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << mode.name;
        ASSERT_FALSE(radiance.empty()) << mode.name;
        ASSERT_FALSE(frame.empty()) << mode.name;

        const HdrRange range = hdr_range(radiance);
        const double lamp_luma = mean_luminance_in_crop(frame, width, height, panel_free_card_box(width, height));
        GTEST_LOG_(INFO) << mode.name << ": " << range.non_finite << " non-finite HDR channels, largest "
                         << range.largest << ", lamp luma " << lamp_luma;

        EXPECT_EQ(range.non_finite, 0U) << mode.name << " wrote Inf or NaN to the HDR target";
        EXPECT_LE(range.largest, HDR_MAX) << mode.name << " wrote radiance past the fp16-safe maximum";
        EXPECT_GE(range.largest, 0.9F * HDR_MAX) << mode.name << ": the lamp never reached the clamp - check it";
        EXPECT_GT(lamp_luma, 240.0) << mode.name << ": the overbright lamp does not tonemap to white";
    }
}

// glTF 3.9.2: a doubleSided back face reverses its whole tangent frame; flipping only N and B mirrored the relief.
TEST(GoldenRender, NormalMappedBackFaceFlipsItsWholeTangentFrame)
{
    SKIP_WITHOUT_GPU();

    // A white, rough dielectric whose normal map tilts every texel 45 degrees toward +T, +X on an unrotated card.
    const auto tilted =
      write_gltf_variant(NORMAL_MAP_CARD_MODEL, "kataglyphis_tilted_normal_card.gltf", [](auto &gltf) {
          auto &material = gltf["materials"][0];
          material["normalTexture"] = { { "index", 0 } };
          material["pbrMetallicRoughness"] = {
              { "baseColorFactor", { 1.0, 1.0, 1.0, 1.0 } }, { "metallicFactor", 0.0 }, { "roughnessFactor", 1.0 }
          };
          gltf["images"][0]["uri"] = TILTED_NORMAL_PNG;
      });
    ASSERT_TRUE(tilted.has_value()) << "could not write the tilted normal card";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.shadows_enabled = false;
    // From +X and the camera's side: the front card's mapped normal (0.71, 0, 0.71) faces it almost squarely.
    scene_vars.directional_light_direction[0] = -1.0F;
    scene_vars.directional_light_direction[1] = -0.3F;
    scene_vars.directional_light_direction[2] = -1.0F;
    renderer_vars.pathTracingSamplesPerPixel = 4;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);

    // Turned about Y, the right card shows its back face; reversed whole, its mapped normal points as the left one's.
    const glm::mat4 front =
      glm::translate(glm::mat4(1.0F), glm::vec3(-2.5F, 4.0F, 15.0F)) * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    const glm::mat4 back = glm::translate(glm::mat4(1.0F), glm::vec3(2.5F, 4.0F, 15.0F))
                           * glm::rotate(glm::mat4(1.0F), glm::pi<float>(), glm::vec3(0.0F, 1.0F, 0.0F))
                           * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    ASSERT_TRUE(harness.renderer->addModel(tilted->string(), front).has_value()) << "adding the front card failed";
    ASSERT_TRUE(harness.renderer->addModel(tilted->string(), back).has_value()) << "adding the back card failed";

    struct Mode
    {
        const char *name;
        RasterizationMode raster;
        bool raytracing;
        bool pathTracing;
        int frames;
    };
    std::vector<Mode> modes = { Mode{ "forward", RasterizationMode::Forward, false, false, SETTLE_FRAMES },
        Mode{ "deferred", RasterizationMode::Deferred, false, false, SETTLE_FRAMES } };
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back(Mode{ "ray traced", RasterizationMode::Forward, true, false, SETTLE_FRAMES });
        modes.push_back(Mode{ "path traced", RasterizationMode::Forward, false, true, 12 });
    }
    for (const Mode &mode : modes) {
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        renderer_vars.pathTracing = mode.pathTracing;
        harness.render_frames(mode.frames);
        uint32_t width = 0;
        uint32_t height = 0;
        const std::vector<uint8_t> frame = harness.capture_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << mode.name;
        ASSERT_FALSE(frame.empty()) << mode.name;

        // The cards' centres project 207 px either side of the middle column.
        const uint32_t offset = (width * 207U) / WINDOW_WIDTH;
        const double front_luma = mean_luminance_in_square(frame, width, width / 2U - offset, height / 2U, 30U);
        const double back_luma = mean_luminance_in_square(frame, width, width / 2U + offset, height / 2U, 30U);
        GTEST_LOG_(INFO) << mode.name << ": front face " << front_luma << ", back face " << back_luma;

        ASSERT_GT(front_luma, 80.0) << mode.name << ": the front card is unlit - check the light or the placement";
        // A back face that kept the front's tangent turns its relief away from this light: N.L 0 against 0.98.
        EXPECT_GT(back_luma, 0.85 * front_luma)
          << mode.name << ": the back face's normal map is mirrored along its tangent - only N and B were flipped";
    }
}

// glTF 3.7.4: a mirroring model matrix reverses the winding and the tangent frame's handedness, not the relief.
TEST(GoldenRender, MirroredModelKeepsItsWindingAndNormalMapRelief)
{
    SKIP_WITHOUT_GPU();

    // A white, rough, single-sided dielectric whose normal map tilts every texel 45 degrees toward -B, +Y on this card.
    const auto tilted =
      write_gltf_variant(NORMAL_MAP_CARD_MODEL, "kataglyphis_bitangent_tilted_card.gltf", [](auto &gltf) {
          auto &material = gltf["materials"][0];
          material["normalTexture"] = { { "index", 0 } };
          material["pbrMetallicRoughness"] = {
              { "baseColorFactor", { 1.0, 1.0, 1.0, 1.0 } }, { "metallicFactor", 0.0 }, { "roughnessFactor", 1.0 }
          };
          material["doubleSided"] = false;
          gltf["images"][0]["uri"] = TILTED_AGAINST_BITANGENT_PNG;
      });
    ASSERT_TRUE(tilted.has_value()) << "could not write the tilted normal card";

    ScopedModelOverride rig(SHADOW_RIG_MODEL);
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);

    auto &renderer_vars = harness.useForwardRaster();
    auto &scene_vars = harness.settings.scene;
    scene_vars.shadows_enabled = false;
    // From above and behind the camera, in the mirror plane: the mapped normal (0, 0.71, 0.71) faces it squarely.
    scene_vars.directional_light_direction[0] = 0.0F;
    scene_vars.directional_light_direction[1] = -1.0F;
    scene_vars.directional_light_direction[2] = -1.0F;
    renderer_vars.pathTracingSamplesPerPixel = 4;
    renderer_vars.pathTracingMaxBounces = 2;
    harness.render_frames(WARMUP_FRAMES);

    // The right card is the left one scaled by -1 in x: same facing, same relief, reversed winding.
    const glm::mat4 upright =
      glm::translate(glm::mat4(1.0F), glm::vec3(-2.5F, 4.0F, 15.0F)) * glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    const glm::mat4 mirrored = glm::translate(glm::mat4(1.0F), glm::vec3(2.5F, 4.0F, 15.0F))
                               * glm::scale(glm::mat4(1.0F), glm::vec3(-2.0F, 2.0F, 2.0F));
    ASSERT_TRUE(harness.renderer->addModel(tilted->string(), upright).has_value()) << "adding the upright card failed";
    ASSERT_TRUE(harness.renderer->addModel(tilted->string(), mirrored).has_value())
      << "adding the mirrored card failed";

    struct Mode
    {
        const char *name;
        RasterizationMode raster;
        bool raytracing;
        bool pathTracing;
        int frames;
    };
    std::vector<Mode> modes = { Mode{ "forward", RasterizationMode::Forward, false, false, SETTLE_FRAMES },
        Mode{ "deferred", RasterizationMode::Deferred, false, false, SETTLE_FRAMES } };
    if (harness.renderer->supportsHardwareRaytracing()) {
        modes.push_back(Mode{ "ray traced", RasterizationMode::Forward, true, false, SETTLE_FRAMES });
        modes.push_back(Mode{ "path traced", RasterizationMode::Forward, false, true, 12 });
    }
    for (const Mode &mode : modes) {
        renderer_vars.rasterizationMode = mode.raster;
        renderer_vars.raytracing = mode.raytracing;
        renderer_vars.pathTracing = mode.pathTracing;
        harness.render_frames(mode.frames);
        uint32_t width = 0;
        uint32_t height = 0;
        // HDR: the tonemap compresses the sun's share, and the path tracer's sky bounce alone reached 0.92 of it.
        const std::vector<float> radiance = harness.capture_hdr_frame(width, height);
        ASSERT_FALSE(harness.renderer->hasDeviceLost()) << mode.name;
        ASSERT_FALSE(radiance.empty()) << mode.name;

        // The cards' centres project 207 px either side of the middle column.
        const uint32_t offset = (width * 207U) / WINDOW_WIDTH;
        const double upright_mean = mean_hdr_in_square(radiance, width, width / 2U - offset, height / 2U, 30U);
        const double mirrored_mean = mean_hdr_in_square(radiance, width, width / 2U + offset, height / 2U, 30U);
        GTEST_LOG_(INFO) << mode.name << ": upright card " << upright_mean << ", mirrored card " << mirrored_mean;

        ASSERT_GT(upright_mean, 0.05) << mode.name << ": the upright card is unlit - check the light or the placement";
        // Culled, the mirrored card shows what lies behind it; with its bitangent reversed, N.L is 0 against 1.
        EXPECT_NEAR(mirrored_mean, upright_mean, 0.1 * upright_mean)
          << mode.name << ": the mirrored card is culled by its reversed winding or its normal map's relief inverts";
    }
}

// Keep LAST in the file: a device-lost abort here kills every later test in the process.
TEST(GoldenRender, RaytracedLargeMeshDoesNotLoseTheDevice)
{
    SKIP_WITHOUT_GPU();

    // No override: the debug scene is the large dinosaur mesh this diagnoses.
    EngineHarness harness;
    SKIP_WITHOUT_FRAME_CAPTURE(harness);
    if (!harness.renderer->supportsHardwareRaytracing()) { GTEST_SKIP() << "Hardware raytracing unsupported."; }

    auto &renderer_vars = harness.settings.renderer;
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
