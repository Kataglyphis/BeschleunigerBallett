// GPU-free so it runs in adapter-less CI containers; only optimized (clangcl-profile) timings mean anything.

#include <benchmark/benchmark.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <array>
#include <atomic>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Parsers compiled in directly: the engine loaders would drag Vulkan-touching global ctors into this binary.
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

import kataglyphis.vulkan.camera;
import kataglyphis.vulkan.scene_config;
import kataglyphis.vulkan.cascaded_shadow_map;
import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.vertex;

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

// A miss probes up to 8 parent directories, far slower than a hit; mind that in per-frame loops.
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
{
    parse_and_walk_gltf(find_model("GltfTest/cube_textured.gltf"), state);
}
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

}// namespace

// BENCHMARK_MAIN's body, returning non-zero when any benchmark skipped with an error.
int main(int argc, char **argv)
{
    benchmark::MaybeReenterWithoutASLR(argc, argv);
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) { return 1; }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return skipped_with_error.load() == 0 ? 0 : 1;
}
