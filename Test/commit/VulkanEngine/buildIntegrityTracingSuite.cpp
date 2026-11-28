// Build integrity: ray- and path-traced shaders share their hit fetch, index objects right, match the dispatch.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "BuildIntegrityScan.hpp"
#include "renderer/PathTracingDispatch.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

namespace {

// Counts opcodes instead of recording presence, for gates that need "at least N"; same not-a-module contract.
std::optional<std::map<uint32_t, std::size_t>> parse_spirv_opcode_counts(const fs::path &spv_path)
{
    const auto text = readFileText(spv_path);
    if (!text) { return std::nullopt; }
    const std::vector<char> raw(text->begin(), text->end());
    if (raw.size() < kSpirvHeaderWordCount * sizeof(uint32_t) || raw.size() % sizeof(uint32_t) != 0) {
        return std::nullopt;
    }

    std::vector<uint32_t> words(raw.size() / sizeof(uint32_t));
    std::memcpy(words.data(), raw.data(), raw.size());
    if (words[0] != kSpirvMagicNumber) { return std::nullopt; }

    std::map<uint32_t, std::size_t> counts;
    std::size_t pos = kSpirvHeaderWordCount;
    while (pos < words.size()) {
        const uint32_t instruction_word = words[pos];
        const uint32_t word_count = instruction_word >> 16U;
        const uint32_t opcode = instruction_word & 0xFFFFU;
        if (word_count == 0 || pos + word_count > words.size()) { break; }// malformed stream - stop, do not read OOB

        ++counts[opcode];
        pos += word_count;
    }
    return counts;
}

constexpr uint32_t kDecorationBuiltIn = 11;

// BuiltIn values of OpDecorate, which opcode counts cannot tell apart; same not-a-module contract.
std::optional<std::set<uint32_t>> parse_spirv_builtin_decorations(const fs::path &spv_path)
{
    const auto text = readFileText(spv_path);
    if (!text) { return std::nullopt; }
    const std::vector<char> raw(text->begin(), text->end());
    if (raw.size() < kSpirvHeaderWordCount * sizeof(uint32_t) || raw.size() % sizeof(uint32_t) != 0) {
        return std::nullopt;
    }

    std::vector<uint32_t> words(raw.size() / sizeof(uint32_t));
    std::memcpy(words.data(), raw.data(), raw.size());
    if (words[0] != kSpirvMagicNumber) { return std::nullopt; }

    std::set<uint32_t> builtins;
    std::size_t pos = kSpirvHeaderWordCount;
    while (pos < words.size()) {
        const uint32_t instruction_word = words[pos];
        const uint32_t word_count = instruction_word >> 16U;
        const uint32_t opcode = instruction_word & 0xFFFFU;
        if (word_count == 0 || pos + word_count > words.size()) { break; }// malformed stream - stop, do not read OOB

        if (opcode == kOpDecorate && word_count >= 4 && words[pos + 2] == kDecorationBuiltIn) {
            builtins.insert(words[pos + 3]);
        }

        pos += word_count;
    }
    return builtins;
}

}// namespace

// Normals take WorldToObject as a row multiply, mul(v, M); the column form agrees only for orthonormal models.
TEST(BuildIntegrity, RayTracedNormalsUseTheInverseTransposeTransform)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "the object->world normal transform must be the inverse-transpose of WorldToObject, applied as a row "
      "multiply (mul(normalHit, M)), not a column multiply (mul(M, normalHit)) - a column multiply only agrees "
      "with the correct transform when the model matrix has no rotation/scale, which the default scene never "
      "exercises";

    // Both traced modes take their normal from traced_hit.slang's one fetch.
    const fs::path traced_hit_path = repo_root / "Resources/ShadersSlang/common/traced_hit.slang";
    const auto traced_hit_text_opt = readFileText(traced_hit_path);
    ASSERT_TRUE(traced_hit_text_opt.has_value()) << "could not open " << traced_hit_path.string();
    const std::string &traced_hit_text = *traced_hit_text_opt;
    EXPECT_EQ(traced_hit_text.find("mul((float3x3)worldToObject, normalHit"), std::string::npos)
      << "traced_hit.slang column-multiplies worldToObject for the normal again. " << kFailureMessage;
    EXPECT_EQ(traced_hit_text.find("mul(worldToObject, float4(normalHit"), std::string::npos)
      << "traced_hit.slang column-multiplies worldToObject for the normal again. " << kFailureMessage;
    EXPECT_NE(traced_hit_text.find("mul(normalHit, (float3x3)worldToObject)"), std::string::npos)
      << "traced_hit.slang no longer row-multiplies worldToObject for the normal. " << kFailureMessage;
}

// A traced object-index source and how many instance-plus-geometry index pairs it must contain.
struct TracedObjectIndexFile
{
    std::string relative_path;
    int expected_pair_count;
};

// Update these counts when a traced object-index site is added, removed or moved.
const std::vector<TracedObjectIndexFile> kTracedObjectIndexFiles = {
    { "path_tracing/path_tracing.slang", 1 },
    { "common/traced_hit.slang", 1 },
    { "raytracing/raytrace.rchit.slang", 1 },
    { "raytracing/raytrace.rahit.slang", 1 },
};

// The custom index names a model's first mesh, so every traced index must add the geometry index to reach the hit mesh.
TEST(BuildIntegrity, TracedObjectIndexAddsTheGeometryIndex)
{
    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    static const std::regex kInstanceIndexRegex(R"(((?:Committed|Candidate)?)InstanceID\(\))");

    std::vector<std::string> violations;
    for (const auto &file : kTracedObjectIndexFiles) {
        const fs::path path = slang_root / file.relative_path;
        const auto lines = readFileLines(path);
        ASSERT_TRUE(lines.has_value()) << "could not read " << path.string();

        int pair_count = 0;
        for (const auto &raw_line : *lines) {
            const std::string line = strip_line_comment(raw_line);

            for (auto it = std::sregex_iterator(line.begin(), line.end(), kInstanceIndexRegex);
              it != std::sregex_iterator();
              ++it) {
                const std::string prefix = (*it)[1].str();
                const std::string geometry_call = prefix + "GeometryIndex()";
                if (line.find(geometry_call) == std::string::npos) {
                    violations.push_back(file.relative_path + ": '" + it->str() + "' with no matching '" + geometry_call
                                         + "' on the same line");
                } else {
                    ++pair_count;
                }
            }
        }

        EXPECT_EQ(pair_count, file.expected_pair_count)
          << file.relative_path << ": expected " << file.expected_pair_count
          << " instance+geometry index pair(s), found " << pair_count
          << " - update kTracedObjectIndexFiles in TracedObjectIndexAddsTheGeometryIndex "
             "if this file's traced object-index sites changed";
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " traced object-index site(s) use an instance accessor with no matching geometry accessor - "
         "a multi-mesh model resolves every mesh past its first to mesh 0's vertex/index/material "
         "buffers unless the geometry term is added (see raytrace.rchit.slang's "
         "InstanceID() + GeometryIndex() shape):"
      << joinViolations(violations);

    constexpr uint32_t kOpRayQueryGetIntersectionGeometryIndexKHR = 6022;
    // traced_hit.slang's one candidate loop, which both queries run, and the committed-hit block.
    constexpr std::size_t kMinGeometryIndexOpcodeCount = 2;

    const fs::path spv_path = spirvRoot() / "path_tracing" / "path_tracing.path_tracing_main.spv";
    if (!sourceExists(spv_path)) {
        GTEST_SKIP() << "missing " << spv_path.string() << " - shaders have not been compiled";
    }

    const auto counts = parse_spirv_opcode_counts(spv_path);
    ASSERT_TRUE(counts.has_value()) << "could not parse " << spv_path.string() << " as SPIR-V";

    const auto count_it = counts->find(kOpRayQueryGetIntersectionGeometryIndexKHR);
    const std::size_t actual_count = (count_it == counts->end()) ? 0 : count_it->second;
    EXPECT_GE(actual_count, kMinGeometryIndexOpcodeCount)
      << spv_path.string() << ": expected OpRayQueryGetIntersectionGeometryIndexKHR at least "
      << kMinGeometryIndexOpcodeCount << " times (one per RayQuery candidate/committed-hit site), found "
      << actual_count
      << " - path_tracing.slang may have regressed to a bare instance index, or the compiled .spv is "
         "stale (recompile with Build-SlangShaders.ps1)";
}

// One hit fetch for the closest-hit shader and the path tracer, and one integrator for both renderers' path tracers.
TEST(BuildIntegrity, TracedModesShareOneHitFetchAndOneIntegrator)
{
    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();
    const auto read = [](const fs::path &path) {
        const auto text = readFileText(path);
        EXPECT_TRUE(text.has_value()) << "missing " << path.string();
        return text.value_or(std::string{});
    };
    const std::string traced_hit = read(slang_root / "common" / "traced_hit.slang");
    EXPECT_NE(traced_hit.find("TracedSurface traced_surface("), std::string::npos)
      << "traced_hit.slang no longer defines the one hit fetch, traced_surface()";
    EXPECT_NE(traced_hit.find("void commit_unmasked_candidates(inout RayQuery<0> query)"), std::string::npos)
      << "traced_hit.slang no longer owns the ray queries' one MASK candidate loop";

    for (const char *relative : { "raytracing/raytrace.rchit.slang", "path_tracing/path_tracing.slang" }) {
        const std::string text = read(slang_root / relative);
        EXPECT_NE(text.find("import traced_hit;"), std::string::npos) << relative << " no longer imports traced_hit";
        EXPECT_NE(text.find("traced_surface("), std::string::npos)
          << relative << " no longer reads its hit through traced_surface()";
        // A second copy of the fetch is what this module replaced: rchit and the path tracer drifted apart twice.
        EXPECT_EQ(text.find("vertices->v["), std::string::npos) << relative << " reads hit vertices itself again";
        EXPECT_EQ(text.find("Proceed()"), std::string::npos) << relative << " runs a candidate loop of its own again";
    }

    const std::string path_tracing = read(slang_root / "path_tracing" / "path_tracing.slang");
    EXPECT_NE(path_tracing.find("import path_integrator;"), std::string::npos)
      << "path_tracing.slang no longer imports OxidANT's path_integrator.slang";
    EXPECT_NE(path_tracing.find(": ITracedScene"), std::string::npos)
      << "path_tracing.slang no longer gives the integrator an ITracedScene";
    EXPECT_NE(path_tracing.find("trace_path(scene, segment,"), std::string::npos)
      << "path_tracing.slang no longer walks its paths with the shared trace_path()";
    // The Sun's Ambient slider scales a bounce's sky, as the raster's ambient term; a fixed 0.35 once ignored it.
    EXPECT_NE(path_tracing.find("return sky * sceneUBO.dirLight.direction.w;"), std::string::npos)
      << "path_tracing.slang's bounce sky no longer follows the Ambient slider";
    EXPECT_EQ(path_tracing.find("BOUNCE_SKY_SCALE"), std::string::npos)
      << "path_tracing.slang scales its bounce sky by a fixed constant again";

    const std::string integrator = read(sharedSlangRoot() / "common" / "path_integrator.slang");
    EXPECT_NE(integrator.find("float3 trace_path<S : ITracedScene>("), std::string::npos)
      << "path_integrator.slang no longer defines the shared trace_path";
}

// InstanceIndex() is the TLAS instance index, not the stamped custom one; presence only, as counts move with inlining.
TEST(BuildIntegrity, TracedObjectIndexReadsTheInstanceCustomIndex)
{
    constexpr uint32_t kBuiltInInstanceId = 6;
    constexpr uint32_t kBuiltInInstanceCustomIndexKHR = 5327;
    constexpr uint32_t kOpRayQueryGetIntersectionInstanceCustomIndexKHR = 6019;
    constexpr uint32_t kOpRayQueryGetIntersectionInstanceIdKHR = 6020;

    for (const char *relative_spv :
      { "raytracing/raytrace.rchit.rchit_main.spv", "raytracing/raytrace.rahit.rahit_main.spv" }) {
        const fs::path spv_path = spirvRoot() / relative_spv;
        if (!sourceExists(spv_path)) {
            GTEST_SKIP() << "missing " << spv_path.string() << " - shaders have not been compiled";
        }

        const auto builtins = parse_spirv_builtin_decorations(spv_path);
        ASSERT_TRUE(builtins.has_value()) << "could not parse " << spv_path.string() << " as SPIR-V";

        EXPECT_TRUE(builtins->count(kBuiltInInstanceCustomIndexKHR) > 0)
          << spv_path.string()
          << ": expected BuiltIn InstanceCustomIndexKHR (5327) - InstanceID() may have regressed to "
             "InstanceIndex(), or the compiled .spv is stale (recompile with Build-SlangShaders.ps1)";
        EXPECT_EQ(builtins->count(kBuiltInInstanceId), 0U)
          << spv_path.string()
          << ": expected no BuiltIn InstanceId (6) - it reads the TLAS instance index, not the "
             "host-written instanceCustomIndex";
    }

    const fs::path path_tracing_spv = spirvRoot() / "path_tracing" / "path_tracing.path_tracing_main.spv";
    if (!sourceExists(path_tracing_spv)) {
        GTEST_SKIP() << "missing " << path_tracing_spv.string() << " - shaders have not been compiled";
    }

    const auto counts = parse_spirv_opcode_counts(path_tracing_spv);
    ASSERT_TRUE(counts.has_value()) << "could not parse " << path_tracing_spv.string() << " as SPIR-V";

    const auto custom_index_it = counts->find(kOpRayQueryGetIntersectionInstanceCustomIndexKHR);
    EXPECT_TRUE(custom_index_it != counts->end() && custom_index_it->second > 0)
      << path_tracing_spv.string()
      << ": expected OpRayQueryGetIntersectionInstanceCustomIndexKHR to be present - Candidate/Committed "
         "InstanceID() may have regressed to InstanceIndex(), or the compiled .spv is stale (recompile "
         "with Build-SlangShaders.ps1)";

    const auto instance_id_it = counts->find(kOpRayQueryGetIntersectionInstanceIdKHR);
    const std::size_t instance_id_count = (instance_id_it == counts->end()) ? 0 : instance_id_it->second;
    EXPECT_EQ(instance_id_count, 0U)
      << path_tracing_spv.string()
      << ": expected no OpRayQueryGetIntersectionInstanceIdKHR - it reads the TLAS instance index, not "
         "the host-written instanceCustomIndex, found "
      << instance_id_count << " occurrence(s)";
}

// The dispatch constants have no compiler link to [numthreads]; a mismatch under-covers the image every frame.
TEST(BuildIntegrity, PathTracingDispatchMatchesTheShaderWorkgroupSize)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path path_tracing_path = slangRoot() / "path_tracing" / "path_tracing.slang";

    const auto path_tracing_threads = parse_numthreads(path_tracing_path);
    ASSERT_TRUE(path_tracing_threads.has_value())
      << "no [numthreads(...)] attribute found in " << path_tracing_path.string();
    EXPECT_EQ((*path_tracing_threads)[0], static_cast<int>(Kataglyphis::kPathTracingWorkgroupSizeX))
      << path_tracing_path.string() << "'s [numthreads(" << (*path_tracing_threads)[0] << ", "
      << (*path_tracing_threads)[1] << ", " << (*path_tracing_threads)[2]
      << ")] X does not match PathTracingDispatch.hpp's kPathTracingWorkgroupSizeX ("
      << Kataglyphis::kPathTracingWorkgroupSizeX << ')';
    EXPECT_EQ((*path_tracing_threads)[1], static_cast<int>(Kataglyphis::kPathTracingWorkgroupSizeY))
      << path_tracing_path.string() << "'s [numthreads(" << (*path_tracing_threads)[0] << ", "
      << (*path_tracing_threads)[1] << ", " << (*path_tracing_threads)[2]
      << ")] Y does not match PathTracingDispatch.hpp's kPathTracingWorkgroupSizeY ("
      << Kataglyphis::kPathTracingWorkgroupSizeY << ')';
    EXPECT_EQ((*path_tracing_threads)[2], 1)
      << path_tracing_path.string() << "'s [numthreads(" << (*path_tracing_threads)[0] << ", "
      << (*path_tracing_threads)[1] << ", " << (*path_tracing_threads)[2] << ")] Z is not 1";
}

// TMin and TMax assume a unit bounce direction (see docs/source/path-tracing.md's Bounce bullet).
TEST(BuildIntegrity, PathTracingBounceDirectionIsNormalized)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "the bounce direction must be renormalized after the hemisphere sample - the unnormalized "
      "sum (normal + a unit vector) has length in [0, 2], which corrupts ray.TMin's "
      "self-intersection epsilon, ray.TMax's visibility distance, and the miss branch's sky "
      "gradient, all of which are measured in units of rayDirection's length";

    // The shared estimator draws the bounce for both path tracers.
    const fs::path estimator_path = sharedSlangRoot() / "common" / "path_estimator.slang";
    const auto estimator = readFileText(estimator_path);
    ASSERT_TRUE(estimator.has_value()) << "missing " << estimator_path.string();
    EXPECT_NE(estimator->find("o.direction = normalize(direction);"), std::string::npos)
      << estimator_path.string() << " no longer normalizes the bounce direction. " << kFailureMessage;

    // The shared integrator walks the path for both renderers.
    const fs::path integrator_path = sharedSlangRoot() / "common" / "path_integrator.slang";
    const auto contentsOpt = readFileText(integrator_path);
    ASSERT_TRUE(contentsOpt.has_value()) << "missing " << integrator_path.string();
    const std::string &contents = *contentsOpt;
    EXPECT_NE(contents.find("ray.direction = next.direction;"), std::string::npos)
      << integrator_path.string() << " no longer takes the shared sample_bounce()'s direction. " << kFailureMessage;
    EXPECT_EQ(contents.find("ray.direction = newDirection;"), std::string::npos)
      << integrator_path.string() << " assigns a bounce direction of its own without normalizing it. "
      << kFailureMessage;
}

// The cloud depth is a whole extra primary ray per pixel, so the path tracer traces it only when the clouds read it.
TEST(BuildIntegrity, PathTracedCloudDepthIsTracedOnlyForTheClouds)
{
    const fs::path repo_root = repoRoot();
    const fs::path shader_path = repo_root / "Resources/ShadersSlang/path_tracing/path_tracing.slang";
    const auto shader = readFileText(shader_path);
    ASSERT_TRUE(shader.has_value()) << "could not open " << shader_path.string();
    // The depth probe rides the frame's first sample's closest-hit pass, wanted only when the clouds read it.
    EXPECT_NE(shader->find("bool depthWanted = pc_ray.trace_cloud_depth != 0u && pc_ray.first_sample == 0u;"),
      std::string::npos)
      << shader_path.string() << " probes the cloud depth whether or not clouds are on, or in every sample chunk";
    EXPECT_NE(shader->find("probe.wanted = depthWanted;"), std::string::npos)
      << shader_path.string() << " no longer gates its depth probe on depthWanted";
    const std::size_t gate = shader->find("if (depthWanted)");
    const std::size_t write = shader->find("tracedDepth[pixel] = probe.hit");
    ASSERT_NE(write, std::string::npos) << shader_path.string() << " no longer writes the probed depth";
    ASSERT_NE(gate, std::string::npos) << shader_path.string()
                                       << " writes the cloud depth whether or not clouds are on";
    EXPECT_TRUE(gate < write && write - gate < 240U)
      << shader_path.string() << " does not guard its depth write with trace_cloud_depth";

    const fs::path renderer_path = repo_root / "Src/GraphicsEngineVulkan/renderer/VulkanRenderer.cpp";
    const auto renderer = readFileText(renderer_path);
    ASSERT_TRUE(renderer.has_value()) << "could not open " << renderer_path.string();
    EXPECT_NE(
      renderer->find("recordRaytracingOrPathTracing(commandBuffer, image_index, sceneSettings.clouds_enabled);"),
      std::string::npos)
      << renderer_path.string() << " no longer hands the frame's clouds_enabled to the path tracer";
}
