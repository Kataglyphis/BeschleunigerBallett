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

// The engine's tinyobj and cgltf builds (ObjLoader.cpp, cgltf_impl.cpp); a second copy here would clash.
#include "tiny_obj_loader.h"

#include <cgltf.h>

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.scene_config;
import kataglyphis.vulkan.cascaded_shadow_map;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.gltf_loader;
import kataglyphis.vulkan.gui;
import kataglyphis.vulkan.gui_renderer_shared_vars;
import kataglyphis.vulkan.mesh_range;
import kataglyphis.vulkan.obj_loader;
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

// OBJ load: mirrors ObjLoader::loadVertices' parse and guarded walk, which dominates model load time.

std::string find_model(const char *name)
{
    namespace fs = std::filesystem;
    for (const auto *prefix : { "", "../", "../../", "../../../" }) {
        fs::path candidate = fs::path(prefix) / "Resources" / "Models" / name;
        if (fs::exists(candidate)) { return candidate.string(); }
    }
    return {};
}

void parse_and_walk(const std::string &path, benchmark::State &state)
{
    if (path.empty()) {
        skipWithError(state, "model not found (run from the repo root)");
        return;
    }

    for (auto _ : state) {
        tinyobj::ObjReader reader;
        tinyobj::ObjReaderConfig config;
        config.triangulate = true;
        if (!reader.ParseFromFile(path, config)) {
            skipWithError(state, "failed to parse model");
            return;
        }

        const auto &attrib = reader.GetAttrib();
        double checksum = 0.0;
        for (const auto &shape : reader.GetShapes()) {
            size_t index_offset = 0;
            for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++) {
                const auto fv = static_cast<size_t>(shape.mesh.num_face_vertices[f]);
                for (size_t v = 0; v < fv; v++) {
                    if (index_offset + v >= shape.mesh.indices.size()) { break; }
                    const tinyobj::index_t idx = shape.mesh.indices[index_offset + v];
                    const auto vertex_index = static_cast<size_t>(idx.vertex_index);
                    if (idx.vertex_index < 0 || (3 * vertex_index) + 2 >= attrib.vertices.size()) { continue; }
                    checksum += attrib.vertices[3 * vertex_index];
                }
                index_offset += fv;
            }
        }
        benchmark::DoNotOptimize(checksum);
    }
}

void BM_ObjParse_Plane(benchmark::State &state) { parse_and_walk(find_model("plane.obj"), state); }
BENCHMARK(BM_ObjParse_Plane)->Unit(benchmark::kMicrosecond);

void BM_ObjParse_Suzanne(benchmark::State &state) { parse_and_walk(find_model("suzanne.obj"), state); }
BENCHMARK(BM_ObjParse_Suzanne)->Unit(benchmark::kMillisecond);

// glTF load: .glb and base64 .gltf take different buffer decode paths; both guard gross regressions, not scale.

void parse_and_walk_gltf(const std::string &path, benchmark::State &state)
{
    if (path.empty()) {
        skipWithError(state, "gltf model not found (run from the repo root)");
        return;
    }

    for (auto _ : state) {
        cgltf_options options{};
        cgltf_data *data = nullptr;
        if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
            skipWithError(state, "failed to parse gltf model");
            return;
        }
        if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
            cgltf_free(data);
            skipWithError(state, "failed to load gltf buffers");
            return;
        }

        double checksum = 0.0;
        for (cgltf_size m = 0; m < data->meshes_count; ++m) {
            const cgltf_mesh &mesh = data->meshes[m];
            for (cgltf_size p = 0; p < mesh.primitives_count; ++p) {
                const cgltf_primitive &primitive = mesh.primitives[p];
                for (cgltf_size a = 0; a < primitive.attributes_count; ++a) {
                    const cgltf_attribute &attribute = primitive.attributes[a];
                    if (attribute.type != cgltf_attribute_type_position) { continue; }
                    const cgltf_accessor *accessor = attribute.data;
                    for (cgltf_size i = 0; i < accessor->count; ++i) {
                        float pos[3] = { 0.0F, 0.0F, 0.0F };
                        cgltf_accessor_read_float(accessor, i, pos, 3);
                        checksum += static_cast<double>(pos[0]);
                    }
                }
            }
        }
        cgltf_free(data);
        benchmark::DoNotOptimize(checksum);
    }
}

void BM_GltfParse_CubeGlb(benchmark::State &state) { parse_and_walk_gltf(find_model("GltfTest/cube.glb"), state); }
BENCHMARK(BM_GltfParse_CubeGlb)->Unit(benchmark::kMicrosecond);

void BM_GltfParse_CubeTextured(benchmark::State &state)
{ parse_and_walk_gltf(find_model("GltfTest/cube_textured.gltf"), state); }
BENCHMARK(BM_GltfParse_CubeTextured)->Unit(benchmark::kMicrosecond);

// Tangent gen: one call per primitive over a growing array, where whole-array accumulators turn quadratic.

void BM_ComputeTangents(benchmark::State &state)
{
    const auto primitiveCount = static_cast<std::size_t>(state.range(0));
    const glm::vec3 normal(0.0F, 0.0F, 1.0F);

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::vector<std::size_t> firstIndices;
    // Slice to the counts at each call, as GltfLoader::processPrimitive sees them, or every call pays for all.
    std::vector<std::size_t> vertexCountSoFar;
    std::vector<std::size_t> indexCountSoFar;
    vertices.reserve(primitiveCount * 4);
    indices.reserve(primitiveCount * 6);
    firstIndices.reserve(primitiveCount);
    vertexCountSoFar.reserve(primitiveCount);
    indexCountSoFar.reserve(primitiveCount);

    for (std::size_t p = 0; p < primitiveCount; ++p) {
        const auto base = static_cast<unsigned int>(vertices.size());
        const float ox = static_cast<float>(p) * 2.0F;
        vertices.emplace_back(glm::vec3(ox + 0.0F, 0.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(0.0F, 0.0F));
        vertices.emplace_back(glm::vec3(ox + 1.0F, 0.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(1.0F, 0.0F));
        vertices.emplace_back(glm::vec3(ox + 1.0F, 1.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(1.0F, 1.0F));
        vertices.emplace_back(glm::vec3(ox + 0.0F, 1.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(0.0F, 1.0F));

        firstIndices.push_back(indices.size());
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);

        vertexCountSoFar.push_back(vertices.size());
        indexCountSoFar.push_back(indices.size());
    }

    for (auto _ : state) {
        for (std::size_t p = 0; p < primitiveCount; ++p) {
            vertex::computeTangents(std::span<Vertex>(vertices.data(), vertexCountSoFar[p]),
              std::span<const unsigned int>(indices.data(), indexCountSoFar[p]),
              firstIndices[p]);
        }
        benchmark::DoNotOptimize(vertices.data());
    }
}
BENCHMARK(BM_ComputeTangents)->Arg(1)->Arg(64)->Arg(512);

// Flat normals: BM_ComputeTangents' per-primitive shape and fixture, so all three compare directly.

void BM_ComputeFlatNormals(benchmark::State &state)
{
    const auto primitiveCount = static_cast<std::size_t>(state.range(0));
    const glm::vec3 normal(0.0F, 0.0F, 1.0F);

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::vector<std::size_t> firstIndices;
    std::vector<std::size_t> vertexCountSoFar;
    std::vector<std::size_t> indexCountSoFar;
    vertices.reserve(primitiveCount * 4);
    indices.reserve(primitiveCount * 6);
    firstIndices.reserve(primitiveCount);
    vertexCountSoFar.reserve(primitiveCount);
    indexCountSoFar.reserve(primitiveCount);

    for (std::size_t p = 0; p < primitiveCount; ++p) {
        const auto base = static_cast<unsigned int>(vertices.size());
        const float ox = static_cast<float>(p) * 2.0F;
        vertices.emplace_back(glm::vec3(ox + 0.0F, 0.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(0.0F, 0.0F));
        vertices.emplace_back(glm::vec3(ox + 1.0F, 0.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(1.0F, 0.0F));
        vertices.emplace_back(glm::vec3(ox + 1.0F, 1.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(1.0F, 1.0F));
        vertices.emplace_back(glm::vec3(ox + 0.0F, 1.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(0.0F, 1.0F));

        firstIndices.push_back(indices.size());
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);

        vertexCountSoFar.push_back(vertices.size());
        indexCountSoFar.push_back(indices.size());
    }

    for (auto _ : state) {
        for (std::size_t p = 0; p < primitiveCount; ++p) {
            vertex::computeFlatNormals(std::span<Vertex>(vertices.data(), vertexCountSoFar[p]),
              std::span<const unsigned int>(indices.data(), indexCountSoFar[p]),
              firstIndices[p]);
        }
        benchmark::DoNotOptimize(vertices.data());
    }
}
BENCHMARK(BM_ComputeFlatNormals)->Arg(1)->Arg(64)->Arg(512);

// Zeroed normals make it fill corners instead of measuring the early-out.
void BM_FillMissingFlatNormals(benchmark::State &state)
{
    const auto primitiveCount = static_cast<std::size_t>(state.range(0));
    const glm::vec3 normal(0.0F, 0.0F, 1.0F);

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::vector<std::size_t> firstIndices;
    std::vector<std::size_t> vertexCountSoFar;
    std::vector<std::size_t> indexCountSoFar;
    vertices.reserve(primitiveCount * 4);
    indices.reserve(primitiveCount * 6);
    firstIndices.reserve(primitiveCount);
    vertexCountSoFar.reserve(primitiveCount);
    indexCountSoFar.reserve(primitiveCount);

    for (std::size_t p = 0; p < primitiveCount; ++p) {
        const auto base = static_cast<unsigned int>(vertices.size());
        const float ox = static_cast<float>(p) * 2.0F;
        vertices.emplace_back(glm::vec3(ox + 0.0F, 0.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(0.0F, 0.0F));
        vertices.emplace_back(glm::vec3(ox + 1.0F, 0.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(1.0F, 0.0F));
        vertices.emplace_back(glm::vec3(ox + 1.0F, 1.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(1.0F, 1.0F));
        vertices.emplace_back(glm::vec3(ox + 0.0F, 1.0F, 0.0F), normal, glm::vec4(1.0F), glm::vec2(0.0F, 1.0F));

        firstIndices.push_back(indices.size());
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);

        vertexCountSoFar.push_back(vertices.size());
        indexCountSoFar.push_back(indices.size());
    }
    for (std::size_t i = 0; i < vertices.size(); i += 3) { vertices[i].normal = glm::vec3(0.0F); }

    for (auto _ : state) {
        for (std::size_t p = 0; p < primitiveCount; ++p) {
            vertex::fillMissingFlatNormals(std::span<Vertex>(vertices.data(), vertexCountSoFar[p]),
              std::span<const unsigned int>(indices.data(), indexCountSoFar[p]),
              firstIndices[p]);
        }
        benchmark::DoNotOptimize(vertices.data());
    }
}
BENCHMARK(BM_FillMissingFlatNormals)->Arg(1)->Arg(64)->Arg(512);

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

// Engine loaders: the raw parse above plus dedup, materials and ranges, i.e. what AsyncModelParse runs off-thread.

void BM_ObjLoaderParseCpu_Suzanne(benchmark::State &state)
{
    const std::string path = find_model("suzanne.obj");
    if (path.empty()) {
        skipWithError(state, "model not found (run from the repo root)");
        return;
    }
    for (auto _ : state) {
        Kataglyphis::ObjLoader loader;
        if (!loader.parseCpu(path)) {
            skipWithError(state, "ObjLoader::parseCpu failed");
            return;
        }
        benchmark::DoNotOptimize(loader.getVertices().data());
    }
}
BENCHMARK(BM_ObjLoaderParseCpu_Suzanne)->Unit(benchmark::kMillisecond);

void BM_GltfLoaderParseCpu_CubeTextured(benchmark::State &state)
{
    const std::string path = find_model("GltfTest/cube_textured.gltf");
    if (path.empty()) {
        skipWithError(state, "gltf model not found (run from the repo root)");
        return;
    }
    for (auto _ : state) {
        Kataglyphis::GltfLoader loader;
        if (!loader.parseCpu(path)) {
            skipWithError(state, "GltfLoader::parseCpu failed");
            return;
        }
        benchmark::DoNotOptimize(loader.getVertices().data());
    }
}
BENCHMARK(BM_GltfLoaderParseCpu_CubeTextured)->Unit(benchmark::kMicrosecond);

// Upload, CPU half: uploadParsed slices a range, then the staging copy memcpys vertices and indices into mapped memory.
void BM_UploadCpuHalf(benchmark::State &state)
{
    const auto quadCount = static_cast<std::size_t>(state.range(0));
    const glm::vec3 normal(0.0F, 0.0F, 1.0F);

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::vector<unsigned int> materialIndex(quadCount * 2, 0U);
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
    range.triCount = materialIndex.size();

    const std::size_t vertexBytes = sizeof(Vertex) * vertices.size();
    const std::size_t indexBytes = sizeof(unsigned int) * indices.size();
    std::vector<std::byte> staging(vertexBytes + indexBytes);

    for (auto _ : state) {
        const Kataglyphis::MeshSlice slice = Kataglyphis::sliceMeshRange(range, vertices, indices, materialIndex);
        std::memcpy(staging.data(), slice.vertices.data(), vertexBytes);
        std::memcpy(staging.data() + vertexBytes, slice.indices.data(), indexBytes);
        benchmark::DoNotOptimize(staging.data());
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(staging.size()));
}
BENCHMARK(BM_UploadCpuHalf)->Arg(64)->Arg(4096)->Arg(65536);

// record_commands per render mode, on the golden scene and camera; the GPU never runs what is recorded.

enum class RecordedMode : std::uint8_t { Forward, Deferred, Raytracing, PathTracing };

// The CI lanes have no display or Vulkan device: SkipWithMessage reports "skipped", never error_occurred.
bool vulkanWindowAvailable()
{
    if (glfwInit() == 0) { return false; }
    const bool supported = glfwVulkanSupported() != 0;
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
        state.SkipWithMessage("no display or Vulkan runtime; record_commands needs a device");
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

    constexpr int kMaxLoadFrames = 20000;
    for (int frame = 0; frame < kMaxLoadFrames && rig.renderer->isModelLoadPending(); ++frame) { rig.renderFrame(); }
    // A few more frames, so the mode switch and its descriptor rebinds have landed.
    for (int frame = 0; frame < 3; ++frame) { rig.renderFrame(); }
    if (rig.renderer->isModelLoadPending() || rig.renderer->hasDeviceLost()) {
        releaseRecordingRig();
        skipWithError(state, "the golden scene never loaded, or the device was lost");
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
