// Build integrity: shader sources keep one home per constant, binding, mapping and encode, and cite live files.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

// Comments must not name the deleted GLSL tree, a GLSL-era stage extension, or a .slang basename that does not exist.
TEST(BuildIntegrity, SourceCommentsDoNotReferenceDeletedShaderFiles)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    std::set<std::string> real_slang_basenames;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        const fs::path &path = shader_path;
        if (!isReadableRegularFile(shader_path) || path.extension() != ".slang") { continue; }
        if (shaderRelativePath(path, slang_root).generic_string().starts_with("build/")) { continue; }
        real_slang_basenames.insert(path.filename().string());
    }
    ASSERT_FALSE(real_slang_basenames.empty()) << "found zero .slang files under " << slang_root.string();

    // Trailing slash: without it this also matches the live Resources/ShadersSlang tree.
    static const std::string kDeadPath = "Resources/Shaders/";
    // Only a trailing GLSL-era extension is dead; raytrace.rchit.slang carries one mid-name.
    static const std::regex kDeadExtension(R"(\.(glsl|frag|vert|geom|tesc|tese|comp|rgen|rchit|rmiss)(?!\.slang)\b)");
    static const std::regex kSlangMention(R"([A-Za-z0-9_./-]+\.slang)");

    std::vector<std::string> violations;

    auto record = [&](const fs::path &path, int line_number, const std::string &line, const std::string &reason) {
        violations.push_back(fs::relative(path, repo_root).generic_string() + ':' + std::to_string(line_number) + ": "
                             + reason + ": " + line);
    };

    auto scan_line = [&](const fs::path &path, int line_number, const std::string &line, const std::string &text) {
        if (text.find(kDeadPath) != std::string::npos) {
            record(path, line_number, line, "references the deleted " + kDeadPath + " tree");
        }
        if (std::regex_search(text, kDeadExtension)) {
            record(path, line_number, line, "references a GLSL-era shader-stage extension that no longer exists");
        }
        for (auto match = std::sregex_iterator(text.begin(), text.end(), kSlangMention),
                  match_end = std::sregex_iterator();
          match != match_end;
          ++match) {
            const std::string basename = fs::path(match->str()).filename().string();
            if (!real_slang_basenames.contains(basename)) {
                record(path,
                  line_number,
                  line,
                  "names '" + basename + "', which does not exist under Resources/ShadersSlang/");
            }
        }
    };

    // .slang files: every line, as shader source has no filename string literals.
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        const fs::path &path = shader_path;
        if (!isReadableRegularFile(shader_path) || path.extension() != ".slang") { continue; }
        if (shaderRelativePath(path, slang_root).generic_string().starts_with("build/")) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        int line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            scan_line(path, line_number, line, line);
        }
    }

    // C++ sources: comments only, so live .spv string literals are never scanned.
    for (const char *sub_dir : { "GraphicsEngineVulkan", "shared" }) {
        const fs::path root = repo_root / "Src" / sub_dir;
        if (!sourceExists(root)) { continue; }
        for (fs::recursive_directory_iterator it(root, error), end; it != end; it.increment(error)) {
            if (error) { break; }
            const fs::path &path = it->path();
            if (!it->is_regular_file(error)) { continue; }
            const std::string extension = path.extension().string();
            if (extension != ".cpp" && extension != ".hpp" && extension != ".ixx") { continue; }

            const auto lines = readFileLines(path);
            if (!lines) { continue; }
            int line_number = 0;
            for (const auto &line : *lines) {
                ++line_number;
                const std::size_t comment_at = line.find("//");
                if (comment_at == std::string::npos) { continue; }
                scan_line(path, line_number, line, line.substr(comment_at));
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " line(s) reference a deleted GLSL-era shader file or an unresolved .slang filename - update the "
         "comment to name the file that actually exists today:"
      << joinViolations(violations);
}

// A local cascade-count copy survives a CASCADE_COUNT bump; WGSL-only shaders pin theirs on the Rust side, so skip them.
TEST(BuildIntegrity, NoShaderRedeclaresTheCascadeCount)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    const auto shared_constants = parse_shared_constants();
    ASSERT_TRUE(shared_constants.contains("CASCADE_COUNT")) << "CASCADE_COUNT not found in limits.slang";
    const int max_cascades = shared_constants.at("CASCADE_COUNT");

    const auto &manifest = shader_manifest();
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";
    const auto &vulkan_consumed_sources = manifest->vulkan_spirv_sources;
    ASSERT_FALSE(vulkan_consumed_sources.empty())
      << "no spirv-targeted rows in shader-manifest.json - manifest format changed?";

    static const std::string kDeclKeyword = "static const int ";

    std::vector<std::string> violations;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        const fs::path &path = shader_path;
        if (!isReadableRegularFile(shader_path) || path.extension() != ".slang") { continue; }
        const std::string relative_path = shaderRelativePath(path, slang_root).generic_string();
        if (relative_path.starts_with("build/")) { continue; }
        if (relative_path == "common/limits.slang") { continue; }
        if (!vulkan_consumed_sources.contains(relative_path)) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        int line_number = 0;
        for (const auto &raw_line : *lines) {
            ++line_number;
            const std::string line = strip_line_comment(raw_line);
            const auto keyword_pos = line.find(kDeclKeyword);
            if (keyword_pos == std::string::npos) { continue; }

            std::size_t name_start = keyword_pos + kDeclKeyword.size();
            std::size_t name_end = name_start;
            while (name_end < line.size() && is_identifier_char(line[name_end])) { ++name_end; }
            if (name_end == name_start) { continue; }

            const std::string name = line.substr(name_start, name_end - name_start);
            const auto value = parse_int_after(line, name_end);
            if (!value) { continue; }

            std::string lower_name = name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            if (*value == max_cascades && lower_name.find("cascade") != std::string::npos) {
                violations.push_back(fs::relative(path, repo_root).generic_string() + ':' + std::to_string(line_number)
                                     + ": " + name + " = " + std::to_string(*value));
            }
        }
    }

    EXPECT_TRUE(violations.empty()) << violations.size()
                                    << " shader-local cascade-count constant(s) redeclare CASCADE_COUNT ("
                                    << max_cascades << ") outside limits.slang - import CASCADE_COUNT instead: "
                                    << joinViolations(violations);
}

// The fullscreen uv's y differs per target, so a hand-written copy of either direction mirrors one renderer.
TEST(BuildIntegrity, NoShaderRedeclaresTheFullscreenUvMapping)
{
    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    // Shadow-map lookups convert NDC by hand until the shared shadow Slang, so only fullscreen importers are held to it.
    static const std::vector<std::string> kCopies{ "(y + 1.0) * 0.5", "uv * 2.0 - 1.0", "uv.y * 2.0" };
    // WebGPU's NDC-to-uv, 0.5 - y * 0.5, is fullscreen_ndc_to_uv's default branch.
    static const std::regex kNdcToUvCopy(R"(\b0\.5\s*-\s*[^;,]*\*\s*0\.5\b)");
    // A triangle of its own once mirrored OxidANT's IBL faces and BRDF LUT on SPIR-V.
    static const std::regex kOwnTriangle(R"(\.svPosition\s*=\s*float4\()");

    std::vector<std::string> violations;
    int importers = 0;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        const fs::path &path = shader_path;
        if (!isReadableRegularFile(shader_path) || path.extension() != ".slang") { continue; }
        const std::string relative_path = shaderRelativePath(path, slang_root).generic_string();
        if (relative_path.starts_with("build/") || relative_path == "common/fullscreen.slang") { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        const bool imports_fullscreen = std::ranges::any_of(
          *lines, [](const std::string &line) { return strip_line_comment(line).starts_with("import fullscreen;"); });
        if (!imports_fullscreen) { continue; }
        ++importers;

        int line_number = 0;
        for (const auto &raw_line : *lines) {
            ++line_number;
            const std::string line = strip_line_comment(raw_line);
            for (const auto &copy : kCopies) {
                if (line.find(copy) != std::string::npos) {
                    violations.push_back(relative_path + ':' + std::to_string(line_number) + ": " + copy);
                }
            }
            if (std::regex_search(line, kNdcToUvCopy)) {
                violations.push_back(relative_path + ':' + std::to_string(line_number) + ": NDC to uv by hand");
            }
            if (std::regex_search(line, kOwnTriangle)) {
                violations.push_back(
                  relative_path + ':' + std::to_string(line_number) + ": a fullscreen triangle of its own");
            }
        }
    }

    EXPECT_GE(importers, 5) << "fewer fullscreen.slang importers than expected - did the import syntax change?";
    EXPECT_TRUE(violations.empty()) << violations.size()
                                    << " fullscreen uv mapping copies - call fullscreen_vs / fullscreen_uv_to_ndc / "
                                       "fullscreen_ndc_to_uv: "
                                    << joinViolations(violations);
}

// One shared-set [vk::binding(...)] declaration and the scene_types.slang constant it must name.
struct SharedDescriptorBinding
{
    std::string relative_path;
    std::string variable_name;
};

// Shared-set declarations per file; pipeline-local bindings with no named constant are deliberately absent.
const std::vector<SharedDescriptorBinding> kSharedDescriptorSetBindings = {
    { "common/material_fetch.slang", "objectDescription" },
    { "common/material_textures.slang", "textures" },
    { "common/material_textures.slang", "textureSamplers" },
    { "common/cascaded_shadow.slang", "directionalShadowMaps" },
    { "rasterizer/rasterizer.slang", "globalUBO" },
    { "rasterizer/rasterizer.slang", "sceneUBO" },
    { "deferred/deferred.slang", "globalUBO" },
    { "deferred/deferred.slang", "sceneUBO_lighting" },
    { "deferred/deferred.slang", "inNormal" },
    { "deferred/deferred.slang", "inAlbedo" },
    { "deferred/deferred.slang", "inMaterial" },
    { "deferred/deferred.slang", "inDepth" },
    { "raytracing/raytrace.rchit.slang", "sceneUBO" },
    { "raytracing/raytrace.rchit.slang", "TLAS" },
    { "raytracing/raytrace.rgen.slang", "globalUBO" },
    { "raytracing/raytrace.rgen.slang", "TLAS" },
    { "raytracing/raytrace.rgen.slang", "image" },
    { "raytracing/raytrace.rgen.slang", "tracedDepth" },
    { "path_tracing/path_tracing.slang", "globalUBO" },
    { "path_tracing/path_tracing.slang", "sceneUBO" },
    { "path_tracing/path_tracing.slang", "TLAS" },
    { "path_tracing/path_tracing.slang", "image" },
    { "path_tracing/path_tracing.slang", "accumulationImage" },
    { "path_tracing/path_tracing.slang", "tracedDepth" },
    { "path_tracing/path_tracing.slang", "skyboxCubemap" },
};

// Bindings must use the named constants, or a renumbering moves both headers while a shader literal stays behind.
TEST(BuildIntegrity, SharedDescriptorSetBindingsUseTheNamedConstants)
{
    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    std::set<std::string> distinct_paths;
    for (const auto &binding : kSharedDescriptorSetBindings) { distinct_paths.insert(binding.relative_path); }
    for (const auto &relative_path : distinct_paths) {
        ASSERT_TRUE(sourceExists(slang_root / relative_path))
          << "kSharedDescriptorSetBindings names a file that no longer exists: " << relative_path
          << " - update the gate in buildIntegrityShaderSourceSuite.cpp";
    }

    std::vector<std::string> violations;
    for (const auto &binding : kSharedDescriptorSetBindings) {
        const fs::path path = slang_root / binding.relative_path;
        const auto lines = readFileLines(path);
        ASSERT_TRUE(lines.has_value()) << "could not read " << path.string();

        bool found_declaration = false;
        for (const auto &raw_line : *lines) {
            const std::string line = strip_line_comment(raw_line);
            const std::size_t binding_pos = line.find("vk::binding(");
            if (binding_pos == std::string::npos) { continue; }

            // Whole word, trying every occurrence: the name can prefix a longer identifier earlier on the line.
            bool matched_this_line = false;
            std::size_t search_from = 0;
            while (true) {
                const std::size_t name_pos = line.find(binding.variable_name, search_from);
                if (name_pos == std::string::npos) { break; }
                search_from = name_pos + 1;

                const bool left_ok = name_pos == 0 || !is_identifier_char(line[name_pos - 1]);
                const std::size_t name_end = name_pos + binding.variable_name.size();
                const bool right_ok = name_end >= line.size() || !is_identifier_char(line[name_end]);
                if (!left_ok || !right_ok) { continue; }

                matched_this_line = true;
                break;
            }
            if (!matched_this_line) { continue; }

            found_declaration = true;

            const std::size_t arg_start = binding_pos + std::string("vk::binding(").size();
            const std::size_t comma_pos = line.find(',', arg_start);
            ASSERT_NE(comma_pos, std::string::npos)
              << path.string() << ": malformed vk::binding(...) on the " << binding.variable_name << " declaration";
            std::string first_arg = line.substr(arg_start, comma_pos - arg_start);
            const auto first_non_space = first_arg.find_first_not_of(" \t");
            const auto last_non_space = first_arg.find_last_not_of(" \t");
            first_arg = (first_non_space == std::string::npos)
                          ? std::string()
                          : first_arg.substr(first_non_space, last_non_space - first_non_space + 1);

            const bool is_integer_literal =
              !first_arg.empty()
              && std::all_of(first_arg.begin(), first_arg.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
            if (is_integer_literal) {
                violations.push_back(fs::path(binding.relative_path).generic_string() + ":" + binding.variable_name);
            }
            break;
        }
        ASSERT_TRUE(found_declaration) << "kSharedDescriptorSetBindings names a declaration that no longer exists: "
                                       << binding.variable_name << " in " << path.string()
                                       << " - update the gate in buildIntegrityShaderSourceSuite.cpp";
    }

    EXPECT_TRUE(violations.empty())
      << violations.size() << " shared render descriptor set binding(s) use a bare integer literal - "
      << "use shared_constants.slang's named binding constant, which host_device_shared_vars.hpp includes: "
      << joinViolations(violations);
}

// The swapchain is UNORM, so shaders writing it encode sRGB; an sRGB surface format would double-encode.
TEST(BuildIntegrity, EverySwapchainWritingShaderEncodesSrgb)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path swapchain_choices_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "vulkan_base" / "SwapchainChoices.hpp";
    const auto swapchain_choices_text = readFileText(swapchain_choices_path);
    ASSERT_TRUE(swapchain_choices_text.has_value()) << "missing " << swapchain_choices_path.string();

    // An sRGB vk::Format, not the expected eSrgbNonlinear presentation color space.
    static const std::regex kSrgbFormat(R"(vk::Format::e\w*Srgb)");
    EXPECT_FALSE(std::regex_search(*swapchain_choices_text, kSrgbFormat))
      << swapchain_choices_path.string()
      << " now prefers an sRGB surface format - the shader-side linear_to_srgb encode in post.slang "
         "becomes a double-encode and must be removed";

    const fs::path slang_root = slangRoot();
    const std::array<fs::path, 1> shaders = { slang_root / "post" / "post.slang" };

    for (const fs::path &shader_path : shaders) {
        const auto source = readFileText(shader_path);
        ASSERT_TRUE(source.has_value()) << "missing " << shader_path.string();

        static const std::string kFragmentMarker = "[shader(\"fragment\")]";
        const std::size_t marker_pos = source->find(kFragmentMarker);
        ASSERT_NE(marker_pos, std::string::npos)
          << "no [shader(\"fragment\")] entry point found in " << shader_path.string();

        const std::size_t body_start = source->find('{', marker_pos);
        ASSERT_NE(body_start, std::string::npos)
          << "could not find the fragment entry point's opening brace in " << shader_path.string();

        std::size_t depth = 1;
        std::size_t pos = body_start + 1;
        for (; pos < source->size() && depth > 0; ++pos) {
            if ((*source)[pos] == '{') {
                ++depth;
            } else if ((*source)[pos] == '}') {
                --depth;
            }
        }
        ASSERT_EQ(depth, 0u) << "unbalanced braces in the fragment entry point of " << shader_path.string();

        const std::string body = source->substr(body_start, pos - body_start);
        EXPECT_NE(body.find("linear_to_srgb"), std::string::npos)
          << shader_path.string() << "'s fragment entry point must call linear_to_srgb before writing the swapchain";
    }
}

// An RGBA16F target stores radiance past 65504 as +Inf and ACES makes NaN of it, so every writer clamps through one helper.
TEST(BuildIntegrity, EveryHdrTargetWriteGoesThroughHdrSafe)
{
    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    const auto hdr_safe_text = readFileText(slang_root / "common" / "hdr_safe.slang");
    ASSERT_TRUE(hdr_safe_text.has_value()) << "missing common/hdr_safe.slang";
    EXPECT_NE(hdr_safe_text->find("static const float HDR_MAX = 65000.0;"), std::string::npos)
      << "hdr_safe.slang's maximum is no longer OxidANT's 65000, under fp16's 65504";

    struct Writer
    {
        const char *file;
        const char *needle;
        std::size_t minimum;
    };
    // A fragment shader's returns, a ray-generation or compute shader's stores, and the path tracer's per-sample sum.
    static const std::array<Writer, 5> kWriters = { {
      { "rasterizer/rasterizer.slang", "return float4(", 2U },
      { "deferred/deferred.slang", "return float4(", 2U },
      { "raytracing/raytrace.rgen.slang", "image[", 1U },
      { "path_tracing/path_tracing.slang", "image[pixel] =", 1U },
      { "path_tracing/path_tracing.slang", "summedColor += hdr_safe(", 1U },
    } };

    std::vector<std::string> violations;
    for (const Writer &writer : kWriters) {
        const auto lines = readFileLines(slang_root / writer.file);
        ASSERT_TRUE(lines.has_value()) << "could not open " << writer.file;
        std::size_t writes = 0;
        for (std::size_t i = 0; i < lines->size(); ++i) {
            const std::string line = strip_line_comment((*lines)[i]);
            if (line.find(writer.needle) == std::string::npos) { continue; }
            ++writes;
            if (line.find("hdr_safe(") == std::string::npos && line.find("surface_lighting(") == std::string::npos) {
                violations.push_back(std::string(writer.file) + ":" + std::to_string(i + 1) + " writes unclamped HDR");
            }
        }
        if (writes < writer.minimum) {
            violations.push_back(std::string(writer.file) + " has " + std::to_string(writes) + " \"" + writer.needle
                                 + "\" site(s), expected at least " + std::to_string(writer.minimum)
                                 + " - did a write move?");
        }
    }
    // A write through surface_lighting() is clamped there.
    const auto surface_text = readFileText(sharedSlangRoot() / "common" / "surface_lighting.slang");
    ASSERT_TRUE(surface_text.has_value()) << "missing OxidANT's common/surface_lighting.slang";
    EXPECT_NE(surface_text->find("    return hdr_safe(sunDirect * lerp("), std::string::npos)
      << "surface_lighting.slang's surface_lighting() no longer returns through hdr_safe()";
    EXPECT_TRUE(violations.empty()) << "HDR writes that bypass hdr_safe():" << joinViolations(violations);
}

// Both raster passes share raster_geometry_vs(), since the forward/deferred parity oracle needs identical outputs.
TEST(BuildIntegrity, TheTwoRasterGeometryPassesShareOneVertexStage)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "the forward and deferred geometry passes must share common/raster_geometry.slang's raster_geometry_vs() - "
      "the forward/deferred parity golden compares their outputs";

    const std::pair<const char *, const char *> kShaders[] = {
        { "Resources/ShadersSlang/rasterizer/rasterizer.slang", "rasterizer.slang" },
        { "Resources/ShadersSlang/deferred/deferred.slang", "deferred.slang" },
    };

    for (const auto &[relative_path, display_name] : kShaders) {
        const fs::path path = repo_root / relative_path;
        const auto text_opt = readFileText(path);
        ASSERT_TRUE(text_opt.has_value()) << "could not open " << path.string();
        const std::string &text = *text_opt;

        EXPECT_NE(text.find("import raster_geometry;"), std::string::npos)
          << display_name << " no longer imports raster_geometry. " << kFailureMessage;
        EXPECT_NE(text.find("raster_geometry_vs("), std::string::npos)
          << display_name << " no longer calls raster_geometry_vs(). " << kFailureMessage;
        EXPECT_EQ(text.find("o.worldTangent ="), std::string::npos)
          << display_name << " re-declares its own worldTangent assignment instead of sharing raster_geometry_vs(). "
          << kFailureMessage;
    }
}
