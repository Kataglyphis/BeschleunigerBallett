// Build integrity: compiled SPIR-V exists, is newer than its sources and imports, and agrees with the host.

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "BuildIntegrityScan.hpp"
#include "common/host_device_shared_vars.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

namespace {

// Artifacts are "<source-stem>.<entry-point>.spv" and only the stem may contain dots, so drop the last component.
fs::path source_for_spirv(const fs::path &spv_path, const fs::path &spirv_root, const fs::path &slang_root)
{
    const fs::path relative_dir = fs::relative(spv_path.parent_path(), spirv_root);
    const std::string stem_and_entry = spv_path.stem().string();// strips only ".spv"

    const auto last_dot = stem_and_entry.find_last_of('.');
    if (last_dot == std::string::npos) { return {}; }// no entry-point separator: not a manifest artifact

    const std::string source_stem = stem_and_entry.substr(0, last_dot);
    return slang_root / relative_dir / (source_stem + ".slang");
}

// Import-only files have no entry point and are never compiled alone, so they need no matching .spv.
bool has_entry_point(const fs::path &slang_source)
{
    const auto content = readFileText(slang_source);
    if (!content) { return false; }
    return content->find("[shader(") != std::string::npos;
}

// True if a .spv in the mirror of source's directory maps back to exactly this source.
bool has_compiled_binary_for_source(const fs::path &source, const fs::path &spirv_root, const fs::path &slang_root)
{
    const fs::path relative_source_dir = fs::relative(source.parent_path(), slang_root);
    const fs::path binary_dir = spirv_root / relative_source_dir;

    std::error_code error;
    if (!sourceExists(binary_dir, error)) { return false; }
    for (fs::directory_iterator it(binary_dir, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".spv") { continue; }
        if (source_for_spirv(it->path(), spirv_root, slang_root) == source) { return true; }
    }
    return false;
}

constexpr uint32_t kOpName = 5;
constexpr uint32_t kOpEntryPoint = 15;
constexpr uint32_t kOpCapability = 17;
constexpr uint32_t kDecorationNonUniform = 5300;
constexpr uint32_t kCapabilityShaderNonUniform = 5301;
constexpr uint32_t kCapabilitySampledImageArrayNonUniformIndexing = 5307;

// Implicit-LOD sampling needs derivatives, defined only in Fragment; OpImageQueryLod is left out, legal in GLCompute.
const std::set<uint32_t> kImplicitLodImageOpcodes = {
    87,//  OpImageSampleImplicitLod
    89,//  OpImageSampleDrefImplicitLod
    91,//  OpImageSampleProjImplicitLod
    93,//  OpImageSampleProjDrefImplicitLod
    305,// OpImageSparseSampleImplicitLod
    307,// OpImageSparseSampleDrefImplicitLod
    309,// OpImageSparseSampleProjImplicitLod
    311,// OpImageSparseSampleProjDrefImplicitLod
};

// Execution-model names for failure messages; only the models this repo uses.
std::string spirv_execution_model_name(uint32_t model)
{
    switch (model) {
    case 0:
        return "Vertex";
    case 4:
        return "Fragment";
    case 5:
        return "GLCompute";
    case 5313:
        return "RayGenerationKHR";
    case 5314:
        return "IntersectionKHR";
    case 5315:
        return "AnyHitKHR";
    case 5316:
        return "ClosestHitKHR";
    case 5317:
        return "MissKHR";
    case 5318:
        return "CallableKHR";
    default:
        return "Unknown(" + std::to_string(model) + ")";
    }
}

// A module's execution model (every .spv here has one entry point), the distinct opcodes and capabilities it uses.
struct SpirvEntryPointInfo
{
    uint32_t execution_model = 0;
    std::set<uint32_t> opcodes_present;
    std::set<uint32_t> capabilities;
    std::size_t non_uniform_decorations = 0;
    // material_textures.slang's array survived dead-code elimination, so the entry samples textures[].
    bool names_material_textures = false;
};

std::optional<SpirvEntryPointInfo> parse_spirv_entry_point_info(const fs::path &spv_path)
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

    SpirvEntryPointInfo info;
    bool found_entry_point = false;

    std::size_t pos = kSpirvHeaderWordCount;
    while (pos < words.size()) {
        const uint32_t instruction_word = words[pos];
        const uint32_t word_count = instruction_word >> 16U;
        const uint32_t opcode = instruction_word & 0xFFFFU;
        if (word_count == 0 || pos + word_count > words.size()) { break; }// malformed stream - stop, do not read OOB

        if (opcode == kOpEntryPoint && !found_entry_point && word_count >= 2) {
            info.execution_model = words[pos + 1];
            found_entry_point = true;
        }
        info.opcodes_present.insert(opcode);
        if (opcode == kOpCapability && word_count >= 2) { info.capabilities.insert(words[pos + 1]); }
        if (opcode == kOpDecorate && word_count >= 3 && words[pos + 2] == kDecorationNonUniform) {
            ++info.non_uniform_decorations;
        }
        if (opcode == kOpName && word_count >= 3) {
            // A literal string: NUL-terminated and padded to whole words after the target id.
            std::string name((word_count - 2U) * sizeof(uint32_t), '\0');
            std::memcpy(name.data(), &words[pos + 2], name.size());
            name.erase(std::min(name.find('\0'), name.size()));
            if (name == "textures") { info.names_material_textures = true; }
        }

        pos += word_count;
    }

    if (!found_entry_point) { return std::nullopt; }
    return info;
}

}// namespace

// A reused stale .spv silently ignores shader edits, so no .spv may be older than its .slang source.
TEST(BuildIntegrity, CompiledShadersAreNotOlderThanTheirSources)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    const fs::path spirv_root = spirvRoot();
    ASSERT_TRUE(sourceExists(spirv_root)) << "missing " << spirv_root.string();

    std::vector<std::string> stale;
    std::vector<std::string> unmapped;
    std::error_code error;
    for (fs::recursive_directory_iterator it(spirv_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &spv = it->path();
        if (!it->is_regular_file(error) || spv.extension() != ".spv") { continue; }

        const fs::path source = source_for_spirv(spv, spirv_root, slang_root);
        if (source.empty() || !sourceExists(source, error)) {
            unmapped.push_back(fs::relative(spv, repo_root).string());
            continue;
        }

        const auto source_time = fs::last_write_time(shaderSourcePath(source), error);
        if (error) { continue; }
        const auto spv_time = fs::last_write_time(spv, error);
        if (error) { continue; }

        if (spv_time < source_time) { stale.push_back(fs::relative(spv, repo_root).string()); }
    }

    EXPECT_TRUE(unmapped.empty())
      << unmapped.size()
      << " compiled .spv could not be mapped back to a .slang source under Resources/ShadersSlang "
         "(naming contract in Build-SlangShaders.ps1 broken, or a source was deleted after compiling): "
      << joinViolations(unmapped);

    EXPECT_TRUE(stale.empty()) << "SPIR-V older than its source - the GPU would run stale shaders. "
                               << "Stale binaries (" << stale.size() << "): " << joinViolations(stale);
}

// A .spv older than a module its source really imports was not recompiled; scoped to the closure, not all of common/.
TEST(BuildIntegrity, CompiledShadersAreNotOlderThanSharedIncludes)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty());

    const fs::path slang_root = slangRoot();
    const fs::path spirv_root = spirvRoot();
    ASSERT_TRUE(sourceExists(spirv_root));

    std::error_code error;
    std::vector<std::string> stale;
    int checked = 0;
    for (fs::recursive_directory_iterator it(spirv_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".spv") { continue; }

        const fs::path source = source_for_spirv(it->path(), spirv_root, slang_root);
        if (source.empty() || !sourceExists(source)) {
            continue;
        }// unmapped: CompiledShadersAreNotOlderThanTheirSources owns this

        fs::file_time_type newest_import{};
        fs::path newest_import_path;
        if (!newest_import_for(slang_root, source, newest_import, newest_import_path)) {
            continue;
        }// no imports: nothing shared to be stale against

        const auto spv_time = fs::last_write_time(it->path(), error);
        if (error) { continue; }

        ++checked;
        if (spv_time < newest_import) {
            stale.push_back(fs::relative(it->path(), repo_root).string() + " is older than its imported "
                            + fs::relative(newest_import_path, repo_root).string());
        }
    }

    if (checked == 0) { GTEST_SKIP() << "no compiled .spv imports a shared Slang module under common/"; }

    EXPECT_TRUE(stale.empty()) << stale.size()
                               << " SPIR-V binaries are older than a shared Slang import they actually depend "
                                  "on; editing a shared module must rebuild its dependents:"
                               << joinViolations(stale);
}

// ssao's closure lacks material_fetch, rasterizer's reaches scene_types through it, and an #include counts.
TEST(BuildIntegrity, ImportClosureFollowsOnlyRealImports)
{
    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root));

    const auto relative_closure = [&slang_root](const fs::path &source) {
        std::set<std::string> relative;
        for (const auto &import_path : import_closure(slang_root, source)) {
            relative.insert(shaderRelativePath(import_path, slang_root).generic_string());
        }
        return relative;
    };

    const auto ssao_closure = relative_closure(slang_root / "ssao" / "ssao.slang");
    EXPECT_TRUE(ssao_closure.contains("common/fullscreen.slang"));
    EXPECT_FALSE(ssao_closure.contains("common/material_fetch.slang"));

    const auto rasterizer_closure = relative_closure(slang_root / "rasterizer" / "rasterizer.slang");
    EXPECT_TRUE(rasterizer_closure.contains("common/material_fetch.slang"));
    EXPECT_TRUE(rasterizer_closure.contains("common/scene_types.slang"));

    const auto scene_types_closure = relative_closure(slang_root / "common" / "scene_types.slang");
    EXPECT_TRUE(scene_types_closure.contains("common/shared_constants.slang"))
      << "an `__exported import` must count as an import";

    const auto twin_closure = relative_closure(slang_root / "rasterizer" / "rasterizer_uniform_index.slang");
    EXPECT_TRUE(twin_closure.contains("rasterizer/rasterizer.slang")) << "an `#include` must count as an import";
    EXPECT_TRUE(twin_closure.contains("common/material_textures.slang"))
      << "the included base's imports must be the twin's too";
}

// Only a *_uniform_index twin samples textures[] without NonUniform; rays, bounces and draws pick their own slot.
TEST(BuildIntegrity, UniformIndexTwinsDeclareNoNonUniformIndexing)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";
    const fs::path spirv_root = spirvRoot();
    ASSERT_TRUE(sourceExists(spirv_root)) << "missing " << spirv_root.string() << " - shaders have not been compiled";

    const auto &manifest = shader_manifest();
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    static const std::string kTwinSuffix = "_uniform_index";
    const auto spirv_of = [&spirv_root](const fs::path &file, const std::string &entry) {
        return spirv_root / file.parent_path() / (file.stem().string() + "." + entry + ".spv");
    };
    std::vector<std::string> violations;
    std::size_t twins = 0;
    std::size_t sampling = 0;
    for (const auto &[file, entry] : manifest->spirv_entries) {
        const fs::path source(file);
        const std::string label = file + " " + entry;
        const auto info = parse_spirv_entry_point_info(spirv_of(source, entry));
        if (source.stem().string().ends_with(kTwinSuffix)) {
            ++twins;
            if (!info) {
                violations.push_back(label + ": no compiled SPIR-V");
            } else if (info->capabilities.contains(kCapabilitySampledImageArrayNonUniformIndexing)
                       || info->capabilities.contains(kCapabilityShaderNonUniform)
                       || info->non_uniform_decorations > 0) {
                violations.push_back(label + ": still declares non-uniform indexing");
            }
            const std::string base_stem = source.stem().string();
            const fs::path base =
              source.parent_path() / (base_stem.substr(0, base_stem.size() - kTwinSuffix.size()) + ".slang");
            const auto base_info = parse_spirv_entry_point_info(spirv_of(base, entry));
            // Control: a twin of a base that never declared the capability is dead weight, or this parser finds none.
            if (!base_info || !base_info->capabilities.contains(kCapabilitySampledImageArrayNonUniformIndexing)) {
                violations.push_back(label + ": its base " + base.generic_string()
                                     + " does not declare SampledImageArrayNonUniformIndexing");
            }
            continue;
        }
        if (!info) { continue; }
        const bool declares = info->capabilities.contains(kCapabilitySampledImageArrayNonUniformIndexing);
        if (info->names_material_textures) {
            ++sampling;
            // VUID-RuntimeSpirv-NonUniform-06274: a slot that varies across invocations needs the decoration.
            if (!declares || info->non_uniform_decorations == 0) {
                violations.push_back(label + ": samples textures[] without NonUniformResourceIndex");
            }
        }
        if (!declares) { continue; }
        // Without the feature VulkanDevice.cpp turns hardware RT off, and with it ray tracing and path tracing.
        const bool ray_tracing_stage = info->execution_model >= 5313 && info->execution_model <= 5318;
        if (ray_tracing_stage || file.starts_with("path_tracing/")) { continue; }
        const std::string twin =
          (source.parent_path() / (source.stem().string() + kTwinSuffix + ".slang")).generic_string();
        if (!manifest->spirv_entries.contains({ twin, entry })) {
            violations.push_back(label + ": declares SampledImageArrayNonUniformIndexing and has no " + twin + " row");
        }
    }

    // Control: forward's two fragment entries, the G-buffer's and the shadow casters'.
    EXPECT_GE(twins, 4U) << "found only " << twins << " *_uniform_index rows in shader-manifest.json";
    // Control: the four twinned bases, closest-hit, any-hit and the path tracer; none means the OpNames were stripped.
    EXPECT_GE(sampling, 7U) << "found only " << sampling << " non-twin SPIR-V module(s) naming textures";
    EXPECT_TRUE(violations.empty()) << violations.size()
                                    << " shader(s) that sample textures[] with the wrong indexing for their device:"
                                    << joinViolations(violations);
}

// Implicit LOD needs derivatives, which only Fragment has; outside it the sample is illegal and can lose the device.
TEST(BuildIntegrity, NoImplicitLodImageInstructionsOutsideFragmentShaders)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    const fs::path spirv_root = spirvRoot();
    if (!sourceExists(spirv_root)) {
        GTEST_SKIP() << "missing " << spirv_root.string() << " - shaders have not been compiled";
    }

    constexpr uint32_t kFragmentExecutionModel = 4;

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(spirv_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &spv = it->path();
        if (!it->is_regular_file(error) || spv.extension() != ".spv") { continue; }

        const auto info = parse_spirv_entry_point_info(spv);
        if (!info.has_value() || info->execution_model == kFragmentExecutionModel) { continue; }

        for (const uint32_t opcode : kImplicitLodImageOpcodes) {
            if (!info->opcodes_present.contains(opcode)) { continue; }
            violations.push_back(fs::relative(spv, repo_root).string() + " (execution model "
                                 + spirv_execution_model_name(info->execution_model) + ", opcode "
                                 + std::to_string(opcode) + ")");
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " compiled .spv use an implicit-LOD image instruction outside a Fragment shader - implicit LOD needs an "
         "automatic derivative, which is only defined for Fragment shader invocations. Use the explicit-LOD form "
         "instead (see raytrace.rchit.slang's rchit_main base-colour sample, SampleLevel(..., 0.0), for the fix "
         "pattern). Offending module(s):"
      << joinViolations(violations);
}

// Leaves GLM_FORCE_DEPTH_ZERO_TO_ONE undefined on purpose: it checks VulkanEngineCore propagates it to every linker.
TEST(BuildIntegrity, GlmProducesVulkanDepthRange)
{
    constexpr float kNear = 1.0F;
    constexpr float kFar = 100.0F;
    const glm::mat4 projection = glm::perspective(glm::radians(60.0F), 1.0F, kNear, kFar);

    // View space looks down -Z, so the near/far planes are at -kNear/-kFar.
    const glm::vec4 near_clip = projection * glm::vec4(0.0F, 0.0F, -kNear, 1.0F);
    const glm::vec4 far_clip = projection * glm::vec4(0.0F, 0.0F, -kFar, 1.0F);

    const float near_ndc = near_clip.z / near_clip.w;
    const float far_ndc = far_clip.z / far_clip.w;

    EXPECT_NEAR(near_ndc, 0.0F, 1e-3F) << "near plane should map to NDC z=0 (Vulkan). Got " << near_ndc
                                       << " - GLM_FORCE_DEPTH_ZERO_TO_ONE is not reaching this translation unit.";
    EXPECT_NEAR(far_ndc, 1.0F, 1e-3F) << "far plane should map to NDC z=1 (Vulkan). Got " << far_ndc;

    // glm::ortho must agree - the shadow cascades depend on it.
    const glm::mat4 ortho = glm::ortho(-1.0F, 1.0F, -1.0F, 1.0F, kNear, kFar);
    const float ortho_near = (ortho * glm::vec4(0.0F, 0.0F, -kNear, 1.0F)).z;
    EXPECT_NEAR(ortho_near, 0.0F, 1e-3F) << "glm::ortho near plane should map to 0, got " << ortho_near;
}

// Every entry-point source the engine consumes must compile, or one missing from the manifest surfaces only at runtime.
TEST(BuildIntegrity, EveryShaderSourceHasCompiledBinary)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty());

    const fs::path slang_root = slangRoot();
    const fs::path spirv_root = spirvRoot();

    const auto &manifest = shader_manifest();
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    std::vector<std::string> missing;
    std::error_code error;
    for (const auto &subdir : manifest->engine_spirv_subdirs) {
        const fs::path source_dir = slang_root / subdir;
        if (!sourceExists(source_dir, error)) { continue; }

        for (fs::recursive_directory_iterator it(source_dir, error), end; it != end; it.increment(error)) {
            if (error) { break; }
            const fs::path &source = it->path();
            if (!it->is_regular_file(error) || source.extension() != ".slang") { continue; }
            if (!has_entry_point(source)) { continue; }// import-only module, e.g. raytracing/rt_types.slang

            if (!has_compiled_binary_for_source(source, spirv_root, slang_root)) {
                missing.push_back(fs::relative(source, repo_root).string());
            }
        }
    }

    EXPECT_TRUE(missing.empty()) << missing.size()
                                 << " shader source(s) have no SPIR-V, which means slangc was never run for "
                                    "them (missing from Build-SlangShaders.ps1's manifest?) or failed: "
                                 << joinViolations(missing);
}

// (.spv path, source) per literal: full paths, or names appended to the last in-scope slang_spv_dir.
std::vector<std::pair<std::string, std::string>> collect_spirv_paths_referenced_by_sources(const fs::path &repo_root)
{
    static const std::string kSpirvPrefix = "Resources/ShadersSlang/build/spirv/";
    static const std::regex kSlangSpvDirRegex(R"re(slang_spv_dir\s*=\s*"([^"]*)")re");
    static const std::regex kSpvLiteralRegex(R"re("([^"]*\.spv)")re");

    std::vector<std::pair<std::string, std::string>> result;
    const fs::path src_root = repo_root / "Src";
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".cpp") { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }

        const std::string relative_source = fs::relative(path, repo_root).generic_string();
        std::string in_scope_slang_spv_dir;
        for (const auto &raw_line : *lines) {
            const std::string line = strip_line_comment(raw_line);

            std::smatch dir_match;
            if (std::regex_search(line, dir_match, kSlangSpvDirRegex)) {
                in_scope_slang_spv_dir = dir_match[1].str();
                continue;
            }

            for (auto match = std::sregex_iterator(line.begin(), line.end(), kSpvLiteralRegex);
              match != std::sregex_iterator();
              ++match) {
                const std::string literal = (*match)[1].str();
                const std::string full = literal.starts_with(kSpirvPrefix) ? literal : in_scope_slang_spv_dir + literal;
                if (!full.starts_with(kSpirvPrefix)) { continue; }// not a spirv_root path; not this scanner's concern
                result.emplace_back(full.substr(kSpirvPrefix.size()), relative_source);
            }
        }
    }
    return result;
}

// Every loaded shader needs a .spv, or it fails only at runtime; the list derives from the literals under Src/.
TEST(BuildIntegrity, ActivePipelineShadersHaveCompiledBinaries)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty());

    const fs::path slang_root = slangRoot();
    const fs::path spirv_root = spirvRoot();

    const auto referenced = collect_spirv_paths_referenced_by_sources(repo_root);
    // Catches a scanner that silently finds nothing; raise this floor when a pass is added, never lower it.
    ASSERT_GE(referenced.size(), 21U)
      << "collect_spirv_paths_referenced_by_sources found only " << referenced.size()
      << " .spv reference(s) under Src/ - expected at least 21 from Rasterizer.cpp, DeferredRasterizer.cpp, "
         "PostStage.cpp, FxaaStage.cpp, SkyBox.cpp, CascadedShadowMap.cpp, Clouds.cpp, Raytracing.cpp and "
         "PathTracing.cpp. "
         "Did a source stop using a literal `.spv` string or the `slang_spv_dir` naming convention?";

    std::vector<std::string> missing;
    for (const auto &[relative, referencing_source] : referenced) {
        const fs::path spv = spirv_root / relative;
        const fs::path source = source_for_spirv(spv, spirv_root, slang_root);
        if (!source.empty() && !sourceExists(source)) { continue; }// shader itself moved - not this test's job
        if (!sourceExists(spv)) { missing.push_back(relative + " (referenced by " + referencing_source + ")"); }
    }

    EXPECT_TRUE(missing.empty()) << missing.size()
                                 << " active pipeline shaders have no compiled SPIR-V; pipeline "
                                    "creation would fail at runtime: "
                                 << joinViolations(missing);
}

// A divergence corrupts every binding without a validation error, so the header includes the shaders' file, no copy.
TEST(BuildIntegrity, HostHeaderIncludesTheShaderConstants)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path header = repo_root / "Src" / "GraphicsEngineVulkan" / "common" / "host_device_shared_vars.hpp";
    const auto text = readFileText(header);
    ASSERT_TRUE(text.has_value()) << "missing " << header.string();
    for (const auto *include : { "#include \"../../../Resources/ShadersSlang/common/shared_constants.slang\"",
           "#include \"../../../third_party/OxidANT/shaders/common/limits.slang\"" }) {
        EXPECT_NE(text->find(include), std::string::npos) << header.string() << " lacks " << include;
    }
    EXPECT_TRUE(parse_int_constants(header).empty()) << header.string() << " defines a shared constant itself";

    const auto shader = parse_shared_constants();
    for (const auto &name : kSharedConstantNames) {
        EXPECT_TRUE(shader.contains(name)) << name << " not found in shared_constants.slang or limits.slang";
    }
}

// The files both C++ and the shaders compile; only constant lines, so the C++ include stays valid.
TEST(BuildIntegrity, SharedShaderConstantsHaveOneHome)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const std::vector<fs::path> homes = { slangRoot() / "common" / "shared_constants.slang",
        slangRoot() / "common" / "uv1_slots.slang",
        sharedSlangRoot() / "common" / "limits.slang",
        sharedSlangRoot() / "common" / "alpha_mode.slang" };
    static const std::regex kDeclaration(R"(^static const int ([A-Za-z_][A-Za-z0-9_]*) = -?[0-9]+;$)");
    std::set<std::string> names;
    std::vector<std::string> violations;
    for (const auto &home : homes) {
        const auto lines = readFileLines(home);
        ASSERT_TRUE(lines.has_value()) << "missing " << home.string();
        for (const auto &line : *lines) {
            std::smatch match;
            if (std::regex_match(line, match, kDeclaration)) {
                names.insert(match[1].str());
            } else if (!line.empty() && !line.starts_with("//")) {
                violations.push_back(home.filename().string() + " holds a line C++ cannot include: " + line);
            }
        }
    }
    ASSERT_GE(names.size(), 24U) << "the scan found too few shared constants - did a home file move?";

    const auto declares_with_literal = [&names](const std::string &line, const std::regex &shape) {
        std::smatch match;
        return std::regex_search(line, match, shape) && names.contains(match[1].str());
    };
    static const std::regex kCppDefine(R"(^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+-?[0-9])");
    static const std::regex kCppConstant(R"(\b(?:const|constexpr)\b[^=;(]*\b([A-Za-z_][A-Za-z0-9_]*)\s*=\s*-?[0-9])");
    static const std::regex kSlangConstant(R"(\bstatic\s+const\s+\w+\s+([A-Za-z_][A-Za-z0-9_]*)\s*=)");

    std::error_code error;
    for (const auto &root : { repo_root / "Src", repo_root / "Test" }) {
        for (fs::recursive_directory_iterator it(root, error), end; it != end; it.increment(error)) {
            if (error) { break; }
            const auto extension = it->path().extension();
            if (!it->is_regular_file(error) || (extension != ".cpp" && extension != ".hpp" && extension != ".ixx")) {
                continue;
            }
            const auto lines = readFileLines(it->path());
            if (!lines) { continue; }
            for (const auto &raw_line : *lines) {
                const std::string line = strip_line_comment(raw_line);
                if (declares_with_literal(line, kCppDefine) || declares_with_literal(line, kCppConstant)) {
                    violations.push_back(fs::relative(it->path(), repo_root).generic_string() + ": " + raw_line);
                }
            }
        }
    }
    for (const auto &path : shaderSourceFiles(slangRoot(), error)) {
        if (std::find(homes.begin(), homes.end(), shaderSourcePath(path)) != homes.end()) { continue; }
        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        for (const auto &raw_line : *lines) {
            if (declares_with_literal(strip_line_comment(raw_line), kSlangConstant)) {
                violations.push_back(path.generic_string() + ": " + raw_line);
            }
        }
    }

    EXPECT_TRUE(violations.empty()) << violations.size()
                                    << " second home(s) of a shared constant - include or import its file instead: "
                                    << joinViolations(violations);
}

// Compares against the compiled header, since a text parse can agree with an edit the build never sees.
TEST(BuildIntegrity, SharedConstantsMatchTheCompiledHostValues)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const auto shader = parse_shared_constants();
    for (const auto &name : kSharedConstantNames) {
        ASSERT_TRUE(shader.contains(name)) << name << " not found in shared_constants.slang or limits.slang";
    }

    EXPECT_EQ(shader.at("MAX_TEXTURE_COUNT"), MAX_TEXTURE_COUNT);
    EXPECT_EQ(shader.at("CASCADE_COUNT"), CASCADE_COUNT);
    EXPECT_EQ(shader.at("MAX_PCF_RADIUS"), MAX_PCF_RADIUS);
    EXPECT_EQ(shader.at("globalUBO_BINDING"), globalUBO_BINDING);
    EXPECT_EQ(shader.at("sceneUBO_BINDING"), sceneUBO_BINDING);
    EXPECT_EQ(shader.at("OBJECT_DESCRIPTION_BINDING"), OBJECT_DESCRIPTION_BINDING);
    EXPECT_EQ(shader.at("TEXTURES_BINDING"), TEXTURES_BINDING);
    EXPECT_EQ(shader.at("SAMPLER_BINDING"), SAMPLER_BINDING);
    EXPECT_EQ(shader.at("SHADOW_MAP_BINDING"), SHADOW_MAP_BINDING);
    EXPECT_EQ(shader.at("PUNCTUAL_LIGHTS_BINDING"), PUNCTUAL_LIGHTS_BINDING);
    EXPECT_EQ(shader.at("PUNCTUAL_SHADOW_MAP_BINDING"), PUNCTUAL_SHADOW_MAP_BINDING);
    EXPECT_EQ(shader.at("TLAS_BINDING"), TLAS_BINDING);
    EXPECT_EQ(shader.at("OUT_IMAGE_BINDING"), OUT_IMAGE_BINDING);
    EXPECT_EQ(shader.at("ACCUMULATION_IMAGE_BINDING"), ACCUMULATION_IMAGE_BINDING);
    EXPECT_EQ(shader.at("TRACED_DEPTH_IMAGE_BINDING"), TRACED_DEPTH_IMAGE_BINDING);
    EXPECT_EQ(shader.at("SKYBOX_CUBEMAP_BINDING"), SKYBOX_CUBEMAP_BINDING);
    EXPECT_EQ(shader.at("GBUFFER_NORMAL_BINDING"), GBUFFER_NORMAL_BINDING);
    EXPECT_EQ(shader.at("GBUFFER_ALBEDO_BINDING"), GBUFFER_ALBEDO_BINDING);
    EXPECT_EQ(shader.at("GBUFFER_MATERIAL_BINDING"), GBUFFER_MATERIAL_BINDING);
    EXPECT_EQ(shader.at("GBUFFER_DEPTH_BINDING"), GBUFFER_DEPTH_BINDING);
}

// The inverse of the manifest-driven checks: every source with an entry point needs an enabled manifest row.
TEST(BuildIntegrity, EverySlangSourceWithAnEntryPointHasAnEnabledManifestRow)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    const auto &manifest = shader_manifest();
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    int entry_point_sources_checked = 0;
    std::vector<std::string> violations;
    for (const std::string &relative_path : collect_all_slang_relative_paths(slang_root)) {
        // common/ has no entry points; has_entry_point would match a "[shader(" in a usage-example comment.
        if (relative_path.starts_with("common/")) { continue; }
        if (!has_entry_point(slang_root / relative_path)) { continue; }

        ++entry_point_sources_checked;
        if (!manifest->all_enabled_manifest_files.contains(relative_path)) { violations.push_back(relative_path); }
    }

    ASSERT_GE(entry_point_sources_checked, 10)
      << "found only " << entry_point_sources_checked << " Slang source(s) with an entry point under "
      << slang_root.string() << " - the entry-point scan itself is broken";

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " Slang source(s) with an entry point have no enabled row in shader-manifest.json - a source that "
         "compiles to nothing:"
      << joinViolations(violations);
}
