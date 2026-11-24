// Only BM_RecordCommands* needs a device, and it skips (not errors) without one; optimized timings alone mean anything.

#include <benchmark/benchmark.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include <vulkan/vulkan.hpp>
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>


#include "oxidant_bridge/gltf.h"
#include "oxidant_bridge/parity.h"
#include "oxidant_bridge/settings.h"

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.scene_config;
import kataglyphis.vulkan.cascaded_shadow_map;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.gltf_loader;
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.gui_renderer_shared_vars;
import kataglyphis.vulkan.mesh_range;
import kataglyphis.vulkan.render_settings;
import kataglyphis.vulkan.renderer;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.vertex;
import kataglyphis.vulkan.window;

namespace {

// SkipWithError alone still exits 0, so main() counts it: a benchmark that cannot run fails the suite.
std::atomic<int> skipped_with_error{ 0 };

void skipWithError(benchmark::State &state, const char *reason)
{
    ++skipped_with_error;
    state.SkipWithError(reason);
}

// Camera: runs once per frame from the input handler, so regressions show up as input latency.

void BM_CameraKeyControl(benchmark::State &state)
{
    Camera camera;
    std::array<bool, 1024> keys{};
    keys[static_cast<size_t>('W')] = true;

    for (auto _ : state) {
        camera.key_control(keys, 0.016F);
        benchmark::DoNotOptimize(camera.get_camera_position());
    }
}
BENCHMARK(BM_CameraKeyControl);

void BM_CameraMouseControl(benchmark::State &state)
{
    Camera camera;
    float delta = 1.0F;

    for (auto _ : state) {
        camera.mouse_control(delta, -delta);
        delta = -delta;// stay near the starting orientation
        benchmark::DoNotOptimize(camera.get_camera_direction());
    }
}
BENCHMARK(BM_CameraMouseControl);

void BM_CameraViewMatrix(benchmark::State &state)
{
    Camera camera;
    for (auto _ : state) { benchmark::DoNotOptimize(camera.calculate_viewmatrix()); }
}
BENCHMARK(BM_CameraViewMatrix);

// Projection: rebuilt every frame, and the cloud shader consumes these CPU-side inverses.

void BM_ProjectionAndInverses(benchmark::State &state)
{
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0F, 2.0F, 5.0F), glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    for (auto _ : state) {
        glm::mat4 projection = glm::perspective(glm::radians(60.0F), 16.0F / 9.0F, 0.1F, 1000.0F);
        projection[1][1] *= -1.0F;
        benchmark::DoNotOptimize(projection);
        benchmark::DoNotOptimize(glm::inverse(projection));
        benchmark::DoNotOptimize(glm::inverse(view));
    }
}
BENCHMARK(BM_ProjectionAndInverses);

// Cascaded shadows: per-frame CPU math; arguments mirror cascadedShadowMapSuite.cpp and the GUI defaults.

void BM_ComputeCascadeData(benchmark::State &state)
{
    const auto numCascades = static_cast<uint32_t>(state.range(0));
    const glm::mat4 view =
      glm::lookAt(glm::vec3(0.0F, 6.0F, 26.0F), glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::vec3 lightDir(-0.55F, -1.0F, -0.35F);
    constexpr float kFov = 45.0F;
    constexpr float kAspect = 16.0F / 9.0F;
    constexpr float kNear = 0.1F;
    constexpr float kFar = 150.0F;
    constexpr float kShadowDistance = 60.0F;
    constexpr uint32_t kShadowMapResolution = 2048;

    const Kataglyphis::CascadeFitParams params{
        .cameraView = view,
        .cameraFov = kFov,
        .aspect = kAspect,
        .nearPlane = kNear,
        .farPlane = kFar,
        .lightDir = lightDir,
        .shadowDistance = kShadowDistance,
        .splitLambda = 0.5F,
        .shadowMapResolution = kShadowMapResolution,
    };
    for (auto _ : state) { benchmark::DoNotOptimize(Kataglyphis::computeCascadeData(numCascades, params)); }
}
BENCHMARK(BM_ComputeCascadeData)->Arg(1)->Arg(3);

// Frustum culling: runs per mesh per frame in every record loop; camera constants mirror BM_ComputeCascadeData's.

std::vector<Kataglyphis::AABB> makeRandomAABBs(std::size_t count)
{
    // Fixed seed: deterministic across runs, no Date.now-style nondeterminism.
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> centreXY(-100.0F, 100.0F);
    std::uniform_real_distribution<float> centreZ(-160.0F, 40.0F);
    std::uniform_real_distribution<float> halfExtentDist(0.25F, 5.0F);

    std::vector<Kataglyphis::AABB> boxes;
    boxes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const glm::vec3 centre{ centreXY(rng), centreXY(rng), centreZ(rng) };
        const float halfExtent = halfExtentDist(rng);
        boxes.push_back(Kataglyphis::AABB{ centre - glm::vec3(halfExtent), centre + glm::vec3(halfExtent) });
    }
    return boxes;
}

Kataglyphis::FrustumPlanes representativeFrustumPlanes()
{
    const glm::mat4 view =
      glm::lookAt(glm::vec3(0.0F, 6.0F, 26.0F), glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    constexpr float kFov = 45.0F;
    constexpr float kAspect = 16.0F / 9.0F;
    constexpr float kNear = 0.1F;
    constexpr float kFar = 150.0F;
    const glm::mat4 projection = glm::perspective(glm::radians(kFov), kAspect, kNear, kFar);
    return Kataglyphis::extractFrustumPlanes(projection * view);
}

void BM_FrustumCull(benchmark::State &state)
{
    const Kataglyphis::FrustumPlanes planes = representativeFrustumPlanes();
    const auto boxes = makeRandomAABBs(static_cast<std::size_t>(state.range(0)));

    for (auto _ : state) {
        for (const auto &box : boxes) { benchmark::DoNotOptimize(Kataglyphis::isVisible(planes, box)); }
    }
}
BENCHMARK(BM_FrustumCull)->Arg(64)->Arg(512);

// The shadow-caster variant, to see whether the two diverge in cost.
void BM_FrustumCullShadowCaster(benchmark::State &state)
{
    const Kataglyphis::FrustumPlanes planes = representativeFrustumPlanes();
    const auto boxes = makeRandomAABBs(static_cast<std::size_t>(state.range(0)));

    for (auto _ : state) {
        for (const auto &box : boxes) { benchmark::DoNotOptimize(Kataglyphis::isVisibleAsShadowCaster(planes, box)); }
    }
}
BENCHMARK(BM_FrustumCullShadowCaster)->Arg(64)->Arg(512);

// Scene config

// A miss probes each cached Resources root once (one per tree, not one per parent directory).
void BM_ResolveModelPath_Hit(benchmark::State &state)
{
    for (auto _ : state) { benchmark::DoNotOptimize(sceneConfig::resolveModelPath("Models/plane.obj")); }
}
BENCHMARK(BM_ResolveModelPath_Hit);

void BM_ResolveModelPath_Miss(benchmark::State &state)
{
    for (auto _ : state) { benchmark::DoNotOptimize(sceneConfig::resolveModelPath("Models/does_not_exist.obj")); }
}
BENCHMARK(BM_ResolveModelPath_Miss);

void BM_AvailableModelPaths(benchmark::State &state)
{
    for (auto _ : state) { benchmark::DoNotOptimize(sceneConfig::getAvailableModelPaths()); }
}
BENCHMARK(BM_AvailableModelPaths);

// The models the loader benchmarks parse, found from the repo root or a build directory.

std::string find_model(const char *name)
{
    namespace fs = std::filesystem;
    for (const auto *prefix : { "", "../", "../../", "../../../" }) {
        fs::path candidate = fs::path(prefix) / "Resources" / "Models" / name;
        if (fs::exists(candidate)) { return candidate.string(); }
    }
    return {};
}

// OxidANT's glTF fixtures, which both renderers load in place.
std::string find_oxidant_fixture(const char *name)
{
    namespace fs = std::filesystem;
    for (const auto *prefix : { "", "../", "../../", "../../../" }) {
        fs::path candidate =
          fs::path(prefix) / "third_party" / "OxidANT" / "crates" / "assets" / "tests" / "assets" / name;
        if (fs::exists(candidate)) { return candidate.string(); }
    }
    return {};
}

// transformAABB: runs per mesh in both the camera and shadow passes; a non-trivial matrix denies compiler shortcuts.

void BM_TransformAABB(benchmark::State &state)
{
    const auto boxes = makeRandomAABBs(static_cast<std::size_t>(state.range(0)));

    glm::mat4 model(1.0F);
    model = glm::translate(model, glm::vec3(12.0F, -4.0F, 7.0F));
    model = glm::rotate(model, glm::radians(37.0F), glm::normalize(glm::vec3(0.3F, 1.0F, 0.2F)));
    model = glm::scale(model, glm::vec3(1.5F, 0.5F, 2.25F));

    for (auto _ : state) {
        for (const auto &box : boxes) { benchmark::DoNotOptimize(Kataglyphis::transformAABB(model, box)); }
    }
}
BENCHMARK(BM_TransformAABB)->Arg(64)->Arg(512);

// The engine's loader: the whole CPU parse AsyncModelParse runs off-thread.

// The loader's whole CPU parse (OxidANT's Rust loader): .glb and base64 .gltf take different buffer decode paths.
void gltfLoaderParseCpu(benchmark::State &state, const std::string &path)
{
    for (auto _ : state) {
        Kataglyphis::GltfLoader loader;
        if (!loader.parseCpu(path)) {
            skipWithError(state, "GltfLoader::parseCpu failed");
            return;
        }
        benchmark::DoNotOptimize(loader.getVertices().data());
    }
}

void BM_GltfLoaderParseCpu_CubeGlb(benchmark::State &state)
{
    const std::string path = find_oxidant_fixture("cube.glb");
    if (path.empty()) {
        skipWithError(state, "gltf model not found (run from the repo root)");
        return;
    }
    gltfLoaderParseCpu(state, path);
}
BENCHMARK(BM_GltfLoaderParseCpu_CubeGlb)->Unit(benchmark::kMicrosecond);

void BM_GltfLoaderParseCpu_CubeTextured(benchmark::State &state)
{
    const std::string path = find_model("GltfTest/cube_textured.gltf");
    if (path.empty()) {
        skipWithError(state, "gltf model not found (run from the repo root)");
        return;
    }
    gltfLoaderParseCpu(state, path);
}
BENCHMARK(BM_GltfLoaderParseCpu_CubeTextured)->Unit(benchmark::kMicrosecond);

// An OBJ through the same bridge call: OxidANT's OBJ reader, parse, tangents and flat arrays.
void BM_GltfLoaderParseCpu_SuzanneObj(benchmark::State &state)
{
    const std::string path = find_model("suzanne.obj");
    if (path.empty()) {
        skipWithError(state, "model not found (run from the repo root)");
        return;
    }
    gltfLoaderParseCpu(state, path);
}
BENCHMARK(BM_GltfLoaderParseCpu_SuzanneObj)->Unit(benchmark::kMillisecond);

// Colosseum-gltf is fetched only with KATAGLYPHIS_FETCH_ALL_ASSETS, so its absence skips rather than errs.
void BM_GltfLoaderParseCpu_Colosseum(benchmark::State &state)
{
    const std::string path = find_model("Colosseum-gltf/Colosseum-gltf.gltf");
    if (path.empty()) {
        state.SkipWithMessage("Colosseum-gltf not fetched (KATAGLYPHIS_FETCH_ALL_ASSETS=ON)");
        return;
    }
    gltfLoaderParseCpu(state, path);
}
BENCHMARK(BM_GltfLoaderParseCpu_Colosseum)->Unit(benchmark::kMillisecond);

// Upload, CPU half: glTF stages a range's vertices straight from the parse and re-bases its indices as staged.
void BM_UploadCpuHalf(benchmark::State &state)
{
    const auto quadCount = static_cast<std::size_t>(state.range(0));
    const glm::vec3 normal(0.0F, 0.0F, 1.0F);

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    vertices.reserve(quadCount * 4);
    indices.reserve(quadCount * 6);
    for (std::size_t q = 0; q < quadCount; ++q) {
        const auto base = static_cast<unsigned int>(vertices.size());
        const float ox = static_cast<float>(q) * 2.0F;
        vertices.emplace_back(glm::vec3(ox + 0.0F, 0.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(0.0F, 0.0F));
        vertices.emplace_back(glm::vec3(ox + 1.0F, 0.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(1.0F, 0.0F));
        vertices.emplace_back(glm::vec3(ox + 1.0F, 1.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(1.0F, 1.0F));
        vertices.emplace_back(glm::vec3(ox + 0.0F, 1.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(0.0F, 1.0F));
        for (const unsigned int corner : { 0U, 1U, 2U, 0U, 2U, 3U }) { indices.push_back(base + corner); }
    }

    Kataglyphis::MeshRange range{};
    range.vertexCount = vertices.size();
    range.indexCount = indices.size();

    const std::span<const Vertex> range_vertices = std::span(vertices).subspan(range.vertexBase, range.vertexCount);
    const std::span<const unsigned int> range_indices = std::span(indices).subspan(range.indexStart, range.indexCount);
    std::vector<std::byte> vertex_staging(range_vertices.size_bytes());
    std::vector<unsigned int> index_staging(range_indices.size());

    for (auto _ : state) {
        std::memcpy(vertex_staging.data(), range_vertices.data(), range_vertices.size_bytes());
        Kataglyphis::rebaseIndices(range_indices, static_cast<unsigned int>(range.vertexBase), index_staging);
        benchmark::DoNotOptimize(vertex_staging.data());
        benchmark::DoNotOptimize(index_staging.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations())
                            * static_cast<int64_t>(range_vertices.size_bytes() + range_indices.size_bytes()));
}
BENCHMARK(BM_UploadCpuHalf)->Arg(64)->Arg(4096)->Arg(65536);

// Each bridged OxidANT entry the engine calls, through cxx and guarded(), so a slower bridge shows beside the Rust half.

namespace ox_settings = kataglyphis::oxidant::settings;
namespace ox_parity = kataglyphis::oxidant::parity;
namespace ox_gltf = kataglyphis::oxidant::gltf;

void BM_BridgeDefaultSceneSettings(benchmark::State &state)
{
    for (auto _ : state) {
        ox_settings::SceneSettings settings = ox_settings::default_scene_settings();
        benchmark::DoNotOptimize(settings);
    }
}
BENCHMARK(BM_BridgeDefaultSceneSettings);

void BM_BridgeSettingRanges(benchmark::State &state)
{
    for (auto _ : state) {
        ox_settings::SettingRanges ranges = ox_settings::setting_ranges();
        benchmark::DoNotOptimize(ranges);
    }
}
BENCHMARK(BM_BridgeSettingRanges);

// A NaN takes the field's default and a far value its clamp, so both of sanitize's branches run.
void BM_BridgeSanitize(benchmark::State &state)
{
    ox_settings::SceneSettings settings = ox_settings::default_scene_settings();
    settings.clouds.density = std::numeric_limits<float>::quiet_NaN();
    settings.clouds.coverage = 1.0e6F;
    for (auto _ : state) {
        ox_settings::SceneSettings sanitized = ox_settings::sanitize(settings);
        benchmark::DoNotOptimize(sanitized);
    }
}
BENCHMARK(BM_BridgeSanitize);

void BM_BridgeParityViewportRemap(benchmark::State &state)
{
    const ox_parity::ViewportRect rect{ 0.2F, 0.05F, 0.8F, 0.75F };
    for (auto _ : state) {
        ox_parity::ViewportRemap remap = ox_parity::parity_viewport_remap(rect);
        benchmark::DoNotOptimize(remap);
    }
}
BENCHMARK(BM_BridgeParityViewportRemap);

void BM_BridgeParityLens(benchmark::State &state)
{
    const ox_parity::Lens lens{
        45.0F, 1200.0F / 768.0F, 0.1F, 150.0F, ox_parity::ViewportRect{ 0.2F, 0.05F, 0.8F, 0.75F }
    };
    for (auto _ : state) {
        ox_parity::Matrix4 projection = ox_parity::parity_lens(lens);
        benchmark::DoNotOptimize(projection);
    }
}
BENCHMARK(BM_BridgeParityLens);

void BM_BridgeParityModelTransform(benchmark::State &state)
{
    const ox_parity::ModelPose pose{ { 1.0F, -2.0F, 3.0F }, { 10.0F, 45.0F, -30.0F } };
    for (auto _ : state) {
        ox_parity::Matrix4 transform = ox_parity::parity_model_transform(pose);
        benchmark::DoNotOptimize(transform);
    }
}
BENCHMARK(BM_BridgeParityModelTransform);

// The bridge call alone; BM_GltfLoaderParseCpu_CubeTextured adds GltfLoader's copy into engine types.
void BM_BridgeLoadGltfFlatWith(benchmark::State &state)
{
    const std::string path = find_model("GltfTest/cube_textured.gltf");
    if (path.empty()) {
        skipWithError(state, "gltf model not found (run from the repo root)");
        return;
    }
    const ox_gltf::GltfLoadOptions options{};
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(path.data());
    for (auto _ : state) {
        ox_gltf::GltfModel model =
          ox_gltf::load_gltf_flat_with(rust::Slice<const std::uint8_t>(bytes, path.size()), options);
        if (!model.ok) {
            skipWithError(state, "load_gltf_flat_with reported an error");
            return;
        }
        benchmark::DoNotOptimize(model.vertices.data());
    }
}
BENCHMARK(BM_BridgeLoadGltfFlatWith)->Unit(benchmark::kMicrosecond);

// record_commands per render mode, on the golden scene and camera; the GPU never runs what is recorded.

enum class RecordedMode : std::uint8_t { Forward, Deferred, Raytracing, PathTracing };

// The CI lanes have no display or Vulkan device: SkipWithMessage reports "skipped", never error_occurred.
bool vulkanWindowAvailable()
{
    if (glfwInit() == 0) { return false; }
    bool supported = glfwVulkanSupported() != 0;
    // windows-11-arm has a loader but no driver: the renderer's instance creation would abort the suite.
    if (supported) {
        const auto create_instance =
          reinterpret_cast<PFN_vkCreateInstance>(glfwGetInstanceProcAddress(nullptr, "vkCreateInstance"));
        VkApplicationInfo app_info{ .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
        const VkInstanceCreateInfo create_info{ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app_info };
        VkInstance probe = VK_NULL_HANDLE;
        supported = create_instance != nullptr && create_instance(&create_info, nullptr, &probe) == VK_SUCCESS;
        if (supported) {
            const auto enumerate_devices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
              glfwGetInstanceProcAddress(probe, "vkEnumeratePhysicalDevices"));
            const auto destroy_instance =
              reinterpret_cast<PFN_vkDestroyInstance>(glfwGetInstanceProcAddress(probe, "vkDestroyInstance"));
            std::uint32_t device_count = 0;
            supported = enumerate_devices != nullptr && enumerate_devices(probe, &device_count, nullptr) == VK_SUCCESS
                        && device_count > 0;
            if (destroy_instance != nullptr) { destroy_instance(probe, nullptr); }
        }
    }
    glfwTerminate();
    return supported;
}

// Pins the scene the goldens use, whatever the build type's default, and restores the caller's choice.
class ScopedModelOverride
{
  public:
    explicit ScopedModelOverride(const char *model)
    {
        if (const char *previous_value = std::getenv(kName); previous_value != nullptr) { previous = previous_value; }
        set(model);
    }
    ScopedModelOverride(const ScopedModelOverride &) = delete;
    ScopedModelOverride &operator=(const ScopedModelOverride &) = delete;
    ~ScopedModelOverride() { set(previous.has_value() ? previous->c_str() : nullptr); }

  private:
    static constexpr const char *kName = "KATAGLYPHIS_MODEL_OVERRIDE";
    static void set(const char *value)
    {
#ifdef _WIN32
        _putenv_s(kName, value != nullptr ? value : "");
#else
        if (value != nullptr) {
            setenv(kName, value, 1);
        } else {
            unsetenv(kName);
        }
#endif
    }
    std::optional<std::string> previous;
};

// App::run's objects in its declaration order, so they tear down as it does.
struct RecordingRig
{
    ScopedModelOverride model{ "Models/Dinosaurs/dinosaurs.obj" };
    Kataglyphis::RenderSettings settings;
    Kataglyphis::RendererFeedback feedback;
    std::unique_ptr<Kataglyphis::Frontend::Window> window;
    std::unique_ptr<Kataglyphis::Scene> scene;
    std::unique_ptr<Kataglyphis::Frontend::GUI> gui;
    std::unique_ptr<Camera> camera;
    std::unique_ptr<Kataglyphis::VulkanRenderer> renderer;

    RecordingRig()
      : window(std::make_unique<Kataglyphis::Frontend::Window>(1200, 768)),
        scene(std::make_unique<Kataglyphis::Scene>()),
        gui(std::make_unique<Kataglyphis::Frontend::GUI>(window.get(), settings, feedback)),
        camera(std::make_unique<Camera>())
    {
        camera->set_camera_position(glm::vec3(0.0F, 6.0F, 26.0F));
        camera->set_orientation(-90.0F, -10.0F);
        camera->set_near_plane(0.1F);
        camera->set_far_plane(150.0F);
        camera->set_fov(45.0F);
        gui->setOverlayVisible(false);
        renderer =
          std::make_unique<Kataglyphis::VulkanRenderer>(window.get(), scene.get(), camera.get(), settings, feedback);
        gui->initializeVulkanContext(renderer->sharedDevice(),
          renderer->vulkanInstance(),
          renderer->swapChainFormat(),
          renderer->swapChainImageCount());
        gui->setUserSelectionForRRT(renderer->supportsHardwareRaytracing());
    }

    void renderFrame()
    {
        glfwPollEvents();
        gui->render();
        renderer->updateStateDueToUserInput(settings, feedback);
        Kataglyphis::applyRendererFeedback(settings, feedback);
        renderer->updateUniforms(scene.get(), camera.get(), settings);
        renderer->drawFrame(settings, feedback);
    }
};

// Google Benchmark calls a benchmark once per iteration-count probe; one rig per mode spares a reload each time.
std::unique_ptr<RecordingRig> recording_rig;
std::optional<RecordedMode> recording_rig_mode;

void releaseRecordingRig()
{
    recording_rig.reset();
    recording_rig_mode.reset();
}

// The rig for this mode, warmed up; nullptr once the state was skipped with a reason.
RecordingRig *recordingRigFor(benchmark::State &state, RecordedMode mode)
{
    if (recording_rig != nullptr && recording_rig_mode == mode) { return recording_rig.get(); }
    releaseRecordingRig();
    if (!vulkanWindowAvailable()) {
        state.SkipWithMessage("no display, Vulkan driver or device; record_commands needs one");
        return nullptr;
    }

    recording_rig = std::make_unique<RecordingRig>();
    RecordingRig &rig = *recording_rig;
    const bool traced = mode == RecordedMode::Raytracing || mode == RecordedMode::PathTracing;
    if (traced && !rig.renderer->supportsHardwareRaytracing()) {
        releaseRecordingRig();
        state.SkipWithMessage("the device has no hardware ray tracing");
        return nullptr;
    }

    auto &renderer_vars = rig.settings.renderer;
    renderer_vars.rasterizationMode =
      mode == RecordedMode::Deferred ? Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode::Deferred
                                     : Kataglyphis::VulkanRendererInternals::FrontendShared::RasterizationMode::Forward;
    renderer_vars.raytracing = mode == RecordedMode::Raytracing;
    renderer_vars.pathTracing = mode == RecordedMode::PathTracing;
    rig.settings.scene.shadows_enabled = true;

    // A Windows container has llvmpipe and a window but no image to acquire: its first frame already fails.
    rig.renderFrame();
    if (rig.renderer->hasFatalFrameError() && !rig.renderer->hasDeviceLost()) {
        releaseRecordingRig();
        state.SkipWithMessage("the swapchain acquires no image; record_commands needs a presentable display");
        return nullptr;
    }
    constexpr int kMaxLoadFrames = 20000;
    for (int frame = 0;
      frame < kMaxLoadFrames && rig.renderer->isModelLoadPending() && !rig.renderer->hasFatalFrameError();
      ++frame) {
        rig.renderFrame();
    }
    // A few more frames, so the mode switch and its descriptor rebinds have landed.
    for (int frame = 0; frame < 3; ++frame) { rig.renderFrame(); }
    if (rig.renderer->isModelLoadPending() || rig.renderer->hasDeviceLost() || rig.renderer->hasFatalFrameError()) {
        releaseRecordingRig();
        skipWithError(state, "the golden scene never loaded, or a frame failed fatally");
        return nullptr;
    }
    if (traced && !rig.feedback.tracingReady) {
        releaseRecordingRig();
        skipWithError(state, "the traced mode has no acceleration structure to draw with");
        return nullptr;
    }
    // The warm-up's frames must be done: the recordings below rewrite ImGui's vertex buffers they may still read.
    std::ignore = rig.renderer->sharedDevice()->getLogicalDevice().waitIdle();
    recording_rig_mode = mode;
    return recording_rig.get();
}

void recordCommandsIn(benchmark::State &state, RecordedMode mode)
{
    RecordingRig *const found = recordingRigFor(state, mode);
    if (found == nullptr) { return; }
    RecordingRig &rig = *found;
    const auto &renderer_vars = rig.settings.renderer;

    for (auto _ : state) {
        // ImGui's frame is begun outside the timing; post's ImGui::Render inside the recording ends it.
        state.PauseTiming();
        rig.gui->render();
        state.ResumeTiming();
        if (!rig.renderer->recordCommandsUnsubmittedForBenchmark()) {
            skipWithError(state, "record_commands refused the frame");
            break;
        }
    }
    // The meshes the recorded frame covers, so a row timed against an empty scene shows it.
    state.counters["scene_meshes"] = static_cast<double>(rig.scene->getObjectDescriptions().size());
    state.SetLabel(Kataglyphis::VulkanRendererInternals::FrontendShared::renderModeLogName(
      Kataglyphis::VulkanRendererInternals::FrontendShared::drawnRenderMode(
        renderer_vars.raytracing, renderer_vars.pathTracing, rig.feedback.tracingReady)));
}

void BM_RecordCommandsForward(benchmark::State &state) { recordCommandsIn(state, RecordedMode::Forward); }
BENCHMARK(BM_RecordCommandsForward)->Unit(benchmark::kMicrosecond);

void BM_RecordCommandsDeferred(benchmark::State &state) { recordCommandsIn(state, RecordedMode::Deferred); }
BENCHMARK(BM_RecordCommandsDeferred)->Unit(benchmark::kMicrosecond);

void BM_RecordCommandsRaytracing(benchmark::State &state) { recordCommandsIn(state, RecordedMode::Raytracing); }
BENCHMARK(BM_RecordCommandsRaytracing)->Unit(benchmark::kMicrosecond);

void BM_RecordCommandsPathTracing(benchmark::State &state) { recordCommandsIn(state, RecordedMode::PathTracing); }
BENCHMARK(BM_RecordCommandsPathTracing)->Unit(benchmark::kMicrosecond);

}// namespace

// BENCHMARK_MAIN's body, returning non-zero when any benchmark skipped with an error.
int main(int argc, char **argv)
{
    benchmark::MaybeReenterWithoutASLR(argc, argv);
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) { return 1; }
    benchmark::RunSpecifiedBenchmarks();
    // Before static destruction, which would tear the device down after GLFW and the loader are gone.
    releaseRecordingRig();
    benchmark::Shutdown();
    return skipped_with_error.load() == 0 ? 0 : 1;
}
