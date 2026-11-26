// Build integrity: light vectors, shadows, roughness and exposure are derived once, alike in every pass.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

// The shader re-clamps what the host clamps, as only its own clamp guards the matrix index and tap loop on the GPU.
TEST(BuildIntegrity, CascadedShadowClampsBothItsUboCounts)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path shadow_path = slangRoot() / "common" / "cascaded_shadow.slang";
    const auto contentOpt = readFileText(shadow_path);
    ASSERT_TRUE(contentOpt.has_value()) << "missing " << shadow_path.string();
    const std::string &content = *contentOpt;

    EXPECT_NE(content.find("clamp(int(sceneUBO.numCascades), 0, CASCADE_COUNT)"), std::string::npos)
      << "cascaded_shadow.slang must clamp numCascades to CASCADE_COUNT before indexing "
         "cascadeLightSpaceMatrices[]";
    EXPECT_NE(content.find("clamp(int(sceneUBO.pcfRadius), 0, MAX_PCF_RADIUS)"), std::string::npos)
      << "cascaded_shadow.slang must clamp pcfRadius to MAX_PCF_RADIUS before the tap loop";
}

// One init() and one seeding path, or a re-init leaves every image but the next with stale light matrices.
TEST(BuildIntegrity, ShadowLightMatricesAreProvisionedInOnePlace)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path source = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "VulkanRenderer.cpp";
    const auto text = readFileText(source);
    ASSERT_TRUE(text.has_value()) << "could not read " << source.string();
    const std::string &content = *text;

    std::size_t init_count = 0;
    for (std::size_t pos = content.find("dirShadowMap.init("); pos != std::string::npos;
      pos = content.find("dirShadowMap.init(", pos + 1)) {
        ++init_count;
    }
    EXPECT_EQ(init_count, 1U) << "dirShadowMap.init( must be called from exactly one place "
                                 "(reinitShadowMapForCurrentSettings) so every re-provisioning path stays in sync";

    const auto reinit_span =
      function_body_span(content, "Kataglyphis::VulkanRenderer::reinitShadowMapForCurrentSettings");
    ASSERT_TRUE(reinit_span.has_value()) << "reinitShadowMapForCurrentSettings(...) definition not found";
    const std::size_t init_pos = content.find("dirShadowMap.init(");
    ASSERT_NE(init_pos, std::string::npos);
    EXPECT_GE(init_pos, reinit_span->first) << "dirShadowMap.init( must live inside reinitShadowMapForCurrentSettings";
    EXPECT_LT(init_pos, reinit_span->second) << "dirShadowMap.init( must live inside reinitShadowMapForCurrentSettings";

    const auto update_span = function_body_span(content, "Kataglyphis::VulkanRenderer::update_uniform_buffers");
    ASSERT_TRUE(update_span.has_value()) << "update_uniform_buffers(...) definition not found";

    std::size_t upload_count = 0;
    for (std::size_t pos = content.find("uploadLightMatrices("); pos != std::string::npos;
      pos = content.find("uploadLightMatrices(", pos + 1)) {
        EXPECT_GE(pos, update_span->first)
          << "uploadLightMatrices( call at offset " << pos << " lies outside update_uniform_buffers";
        EXPECT_LT(pos, update_span->second)
          << "uploadLightMatrices( call at offset " << pos << " lies outside update_uniform_buffers";
        ++upload_count;
    }
    EXPECT_GT(upload_count, 0U) << "expected at least one uploadLightMatrices( call in VulkanRenderer.cpp";
}

// dirLight.direction is the travel direction, so toward-light vectors negate it inside normalize(); .w is the ambient.
TEST(BuildIntegrity, EveryShaderDerivesTheLightVectorByNegation)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    static const std::string kMarker = "dirLight.direction";
    static const std::string kNormalizeCall = "normalize(";

    int occurrences_checked = 0;
    std::set<std::string> ambient_readers;
    std::vector<std::string> violations;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        const fs::path &path = shader_path;
        if (!isReadableRegularFile(shader_path) || path.extension() != ".slang") { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        int line_number = 0;
        for (const auto &raw_line : *lines) {
            ++line_number;
            const std::string line = strip_line_comment(raw_line);
            std::size_t search_from = 0;
            while (true) {
                const auto marker_pos = line.find(kMarker, search_from);
                if (marker_pos == std::string::npos) { break; }
                search_from = marker_pos + kMarker.size();
                const auto swizzled = [&line, search_from](std::string_view swizzle) {
                    const std::size_t end = search_from + swizzle.size();
                    return line.compare(search_from, swizzle.size(), swizzle) == 0
                           && (end == line.size() || !is_identifier_char(line[end]));
                };
                if (swizzled(".w")) {
                    ambient_readers.insert(path.filename().string());
                    continue;
                }
                ++occurrences_checked;
                // The whole float4, .xyzw or [3] would hand the ambient in .w to a direction.
                if (!swizzled(".xyz")) {
                    violations.push_back(fs::relative(path, repo_root).generic_string() + ':'
                                         + std::to_string(line_number) + ": " + kMarker
                                         + " is read without .xyz, so its .w (the ambient) joins the direction");
                    continue;
                }

                // Walk back over the "scene." or "sceneUBO_lighting." prefix to where the read starts.
                std::size_t expr_start = marker_pos;
                while (expr_start > 0 && (is_identifier_char(line[expr_start - 1]) || line[expr_start - 1] == '.')) {
                    --expr_start;
                }

                std::size_t before = expr_start;
                while (before > 0 && std::isspace(static_cast<unsigned char>(line[before - 1])) != 0) { --before; }

                const bool negated = before > 0 && line[before - 1] == '-';
                std::size_t normalize_end = negated ? before - 1 : before;
                while (normalize_end > 0 && std::isspace(static_cast<unsigned char>(line[normalize_end - 1])) != 0) {
                    --normalize_end;
                }
                const bool inside_normalize =
                  normalize_end >= kNormalizeCall.size()
                  && line.compare(normalize_end - kNormalizeCall.size(), kNormalizeCall.size(), kNormalizeCall) == 0;

                if (!negated || !inside_normalize) {
                    violations.push_back(fs::relative(path, repo_root).generic_string() + ':'
                                         + std::to_string(line_number) + ": "
                                         + line.substr(expr_start, marker_pos + kMarker.size() - expr_start)
                                         + " is not negated inside normalize(...)");
                }
            }
        }
    }

    EXPECT_EQ(occurrences_checked, 5)
      << "expected exactly 5 shader sites reading dirLight.direction, found " << occurrences_checked
      << " - update this count if a consumer was intentionally added or removed, and confirm the new "
         "site also negates the field";
    EXPECT_EQ(ambient_readers,
      (std::set<std::string>{ "deferred.slang", "path_tracing.slang", "rasterizer.slang", "raytrace.rchit.slang" }))
      << "the shaders that read the ambient in dirLight.direction.w: forward, deferred and closest-hit add the sky, "
         "and "
         "the path tracer scales its bounce sky by it";

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " shader site(s) read dirLight.direction without negating it inside normalize(...) - "
         "dirLight.direction is a travel direction, negate it to get the vector toward the light: "
      << joinViolations(violations);
}

// Roughness comes from roughness_factor() and gltf_material.slang's material_roughness() in every path, never a literal.
TEST(BuildIntegrity, EveryShadingPathDerivesRoughnessFromTheMaterial)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    static const std::regex kLiteralRoughnessAssignment(R"(\broughness\s*=\s*[0-9])");

    std::vector<std::string> violations;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        const fs::path &path = shader_path;
        if (!isReadableRegularFile(shader_path) || path.extension() != ".slang") { continue; }

        const auto content = readFileText(path);
        if (!content.has_value()) { continue; }

        std::istringstream stream(*content);
        std::string raw_line;
        int line_number = 0;
        while (std::getline(stream, raw_line)) {
            ++line_number;
            const std::string line = strip_line_comment(raw_line);
            if (std::regex_search(line, kLiteralRoughnessAssignment)) {
                violations.push_back(fs::relative(path, repo_root).generic_string() + ':'
                                      + std::to_string(line_number) + ": " + line
                                      + " assigns a numeric literal to roughness - derive it from the material via "
                                        "material_metallic_roughness() instead");
            }
        }
    }

    EXPECT_TRUE(violations.empty()) << violations.size()
                                    << " shader site(s) hard-code roughness instead of deriving it from the material:"
                                    << joinViolations(violations);

    // Three paths reach the roughness rule through material_textures.slang's resolved_metallic_roughness helpers.
    const fs::path material_textures_path = slang_root / "common/material_textures.slang";
    const auto material_textures_content = readFileText(material_textures_path);
    ASSERT_TRUE(material_textures_content.has_value()) << "missing " << material_textures_path.string();
    EXPECT_NE(material_textures_content->find("material_metallic_roughness(material, float4(1.0))"), std::string::npos)
      << "material_textures.slang's resolved_metallic_roughness helpers must derive the untextured roughness via "
         "material_metallic_roughness(), not their own copy of the mapping";

    static const std::array<const char *, 3> kShadingShaders = {
        "rasterizer/rasterizer.slang",
        "deferred/deferred.slang",
        "common/traced_hit.slang",
    };

    for (const char *relative : kShadingShaders) {
        const fs::path path = slang_root / relative;
        const auto content = readFileText(path);
        ASSERT_TRUE(content.has_value()) << "missing " << path.string();
        const bool calls_resolved = content->find("resolved_metallic_roughness(") != std::string::npos
                                    || content->find("resolved_metallic_roughness_lod0(") != std::string::npos;
        EXPECT_TRUE(calls_resolved) << relative
                                    << " must derive roughness via resolved_metallic_roughness()/"
                                       "resolved_metallic_roughness_lod0(), not its own copy of the mapping";
    }
}

// GltfLoader pins shininess to a fallback, safe only while roughness_factor() is its sole shader reader.
TEST(BuildIntegrity, MaterialShininessIsReadOnlyThroughMaterialRoughness)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    static const fs::path kAllowedReader = fs::path("common") / "material_rules.slang";
    static const std::regex kShininessRead(R"(\bmaterial\.shininess\b)");

    std::vector<std::string> violations;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        const fs::path &path = shader_path;
        if (!isReadableRegularFile(shader_path) || path.extension() != ".slang") { continue; }

        const fs::path relative = shaderRelativePath(path, slang_root);
        if (relative == kAllowedReader) { continue; }

        const auto content = readFileText(path);
        if (!content.has_value()) { continue; }

        std::istringstream stream(*content);
        std::string raw_line;
        int line_number = 0;
        while (std::getline(stream, raw_line)) {
            ++line_number;
            const std::string line = strip_line_comment(raw_line);
            if (std::regex_search(line, kShininessRead)) {
                violations.push_back(
                  fs::relative(path, repo_root).generic_string() + ':' + std::to_string(line_number) + ": " + line);
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " shader site(s) read material.shininess outside common/material_rules.slang - that field is pinned "
         "to a fixed fallback value by GltfLoaderRust.cpp and is only meaningful through roughness_factor()'s "
         "sentinel branch:"
      << joinViolations(violations);
}

// Bloom pre-exposes in its bright pass, so tonemap exposes only the raw HDR term, never bloom a second time.
TEST(BuildIntegrity, BloomAndTonemapAgreeOnWhereExposureIsApplied)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path bloom_path = slangRoot() / "bloom" / "bloom.slang";
    const auto bloom_source_opt = readFileText(bloom_path);
    ASSERT_TRUE(bloom_source_opt.has_value()) << "missing " << bloom_path.string();
    const std::string &bloom_source = *bloom_source_opt;

    EXPECT_NE(bloom_source.find("exposureState"), std::string::npos)
      << "bloom.slang must read exposureState so fs_brightpass thresholds the exposed HDR value, not the raw "
         "one: "
      << bloom_path.string();

    const fs::path tonemap_path = slangRoot() / "tonemap" / "tonemap.slang";
    const auto tonemap_source_opt = readFileText(tonemap_path);
    ASSERT_TRUE(tonemap_source_opt.has_value()) << "missing " << tonemap_path.string();
    const std::string &tonemap_source = *tonemap_source_opt;

    static const std::string kCompositeCall = "aces_tonemap(";
    const std::size_t call_pos = tonemap_source.find(kCompositeCall);
    ASSERT_NE(call_pos, std::string::npos)
      << "tonemap.slang's aces_tonemap(...) composite call moved or was renamed: " << tonemap_path.string();

    const std::size_t args_start = call_pos + kCompositeCall.size();
    std::size_t depth = 1;
    std::size_t pos = args_start;
    for (; pos < tonemap_source.size() && depth > 0; ++pos) {
        if (tonemap_source[pos] == '(') {
            ++depth;
        } else if (tonemap_source[pos] == ')') {
            --depth;
        }
    }
    ASSERT_EQ(depth, 0u) << "unbalanced parentheses after aces_tonemap( in " << tonemap_path.string();
    std::string composite_expr = tonemap_source.substr(args_start, pos - 1 - args_start);

    // The composite may be named first (`float3 scene = min(<composite>, cap)`); follow it to its first argument.
    static const std::regex kIdentifier(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*$)");
    std::smatch identifier_match;
    if (std::regex_match(composite_expr, identifier_match, kIdentifier)) {
        const std::string definition = "float3 " + identifier_match[1].str() + " = min(";
        const std::size_t definition_pos = tonemap_source.find(definition);
        ASSERT_NE(definition_pos, std::string::npos) << "aces_tonemap(" << identifier_match[1].str() << ") but no `"
                                                     << definition << "...)` in " << tonemap_path.string();
        const std::size_t min_start = definition_pos + definition.size();
        std::size_t min_depth = 0;
        std::size_t min_pos = min_start;
        for (; min_pos < tonemap_source.size(); ++min_pos) {
            const char ch = tonemap_source[min_pos];
            if (ch == '(') {
                ++min_depth;
            } else if (ch == ')' && min_depth > 0) {
                --min_depth;
            } else if ((ch == ',' || ch == ')') && min_depth == 0) {
                break;
            }
        }
        composite_expr = tonemap_source.substr(min_start, min_pos - min_start);
    }

    // Split top-level '+' terms, tracking paren depth for any future nested '+'.
    std::vector<std::string> terms;
    std::size_t term_start = 0;
    std::size_t term_depth = 0;
    for (std::size_t i = 0; i < composite_expr.size(); ++i) {
        const char ch = composite_expr[i];
        if (ch == '(') {
            ++term_depth;
        } else if (ch == ')') {
            --term_depth;
        } else if (ch == '+' && term_depth == 0) {
            terms.push_back(composite_expr.substr(term_start, i - term_start));
            term_start = i + 1;
        }
    }
    terms.push_back(composite_expr.substr(term_start));

    const auto bloom_term = std::find_if(
      terms.begin(), terms.end(), [](const std::string &term) { return term.find("bloom") != std::string::npos; });
    ASSERT_NE(bloom_term, terms.end()) << "no term of aces_tonemap(...)'s composite references bloom: "
                                       << composite_expr;
    EXPECT_EQ(bloom_term->find("exposure"), std::string::npos)
      << "tonemap.slang must not multiply the bloom term by exposure - bloom.slang's fs_brightpass already "
         "pre-exposed it, so multiplying again here double-exposes bloom: "
      << *bloom_term;

    const auto hdr_term = std::find_if(
      terms.begin(), terms.end(), [](const std::string &term) { return term.find("hdr") != std::string::npos; });
    ASSERT_NE(hdr_term, terms.end()) << "no term of aces_tonemap(...)'s composite references hdr: " << composite_expr;
    EXPECT_NE(hdr_term->find("exposure"), std::string::npos)
      << "tonemap.slang must still apply exposure to the raw HDR term: " << *hdr_term;
}

TEST(BuildIntegrity, EveryPcfKernelBoundsChecksItsTaps)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    static const std::string kMarker = "SampleCmpLevelZero(";
    static const std::string kGeLower = ">= 0.0";
    static const std::string kLeUpper = "<= 1.0";

    // A cube has no border, nor do OxidANT's face layers whose uv the face selection picks: counted apart, unchecked.
    static const std::array<std::string, 2> kCubeMarkers = { "punctualShadowMaps.SampleCmpLevelZero(",
        "punctualShadowMap.SampleCmpLevelZero(" };
    int occurrences_checked = 0;
    int cube_occurrences = 0;
    std::vector<std::string> violations;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        const fs::path &path = shader_path;
        if (!isReadableRegularFile(shader_path) || path.extension() != ".slang") { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::string stripped_text;
        for (const auto &raw_line : *lines) {
            stripped_text += strip_line_comment(raw_line);
            stripped_text += '\n';
        }

        std::size_t search_from = 0;
        while (true) {
            const auto marker_pos = stripped_text.find(kMarker, search_from);
            if (marker_pos == std::string::npos) { break; }
            const bool cube_tap = std::ranges::any_of(kCubeMarkers, [&](const std::string &cube_marker) {
                const std::size_t cube_prefix = cube_marker.size() - kMarker.size();
                return marker_pos >= cube_prefix
                       && stripped_text.compare(marker_pos - cube_prefix, cube_marker.size(), cube_marker) == 0;
            });
            if (cube_tap) {
                ++cube_occurrences;
                search_from = marker_pos + kMarker.size();
                continue;
            }
            ++occurrences_checked;

            // The tap coordinate must be range-checked to [0,1] somewhere in its enclosing statement.
            std::size_t statement_start = marker_pos;
            while (statement_start > 0 && stripped_text[statement_start - 1] != ';'
                   && stripped_text[statement_start - 1] != '{') {
                --statement_start;
            }
            const std::string statement = stripped_text.substr(statement_start, marker_pos - statement_start);

            auto count_occurrences = [](const std::string &haystack, const std::string &needle) {
                int count = 0;
                std::size_t pos = 0;
                while ((pos = haystack.find(needle, pos)) != std::string::npos) {
                    ++count;
                    pos += needle.size();
                }
                return count;
            };

            const bool bounds_checked =
              count_occurrences(statement, kGeLower) >= 2 && count_occurrences(statement, kLeUpper) >= 2;

            search_from = marker_pos + kMarker.size();

            if (!bounds_checked) {
                std::size_t line_number =
                  1
                  + static_cast<std::size_t>(
                    std::count(stripped_text.begin(), stripped_text.begin() + static_cast<long>(marker_pos), '\n'));
                violations.push_back(fs::relative(path, repo_root).generic_string() + ':' + std::to_string(line_number)
                                      + ": SampleCmpLevelZero tap is not guarded by a [0,1] bounds check on both "
                                        "components of the tap coordinate in its enclosing statement");
            }
        }
    }

    // cascaded_shadow.slang's taps, OxidANT's forward_lighting.slang and its shadow_pcf_test.slang.
    EXPECT_EQ(occurrences_checked, 3)
      << "expected exactly 3 SampleCmpLevelZero call site(s), found " << occurrences_checked
      << " - update this count if a new PCF kernel was intentionally added, and confirm it also "
         "bounds-checks its tap coordinate";
    EXPECT_EQ(cube_occurrences, 2)
      << "punctual_lights.slang's CubeTaps and OxidANT's FaceTaps should sample through one call site each";

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " PCF tap(s) sample past the shadow map's border unguarded - the ClampToEdge comparison "
         "sampler replicates the border texel's depth for an out-of-map tap rather than treating it "
         "as unshadowed, so every tap must be range-checked to [0,1] before sampling: "
      << joinViolations(violations);
}

// The multiview depth attachment reuses the shadow array's sampled view instead of a byte-identical second one.
TEST(BuildIntegrity, ShadowMapArrayHasExactlyOneImageView)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path cpp_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "light" / "directional_light" / "CascadedShadowMap.cpp";
    const fs::path ixx_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "light" / "directional_light" / "CascadedShadowMap.ixx";
    const auto cpp_contents_opt = readFileText(cpp_path);
    ASSERT_TRUE(cpp_contents_opt.has_value()) << "missing " << cpp_path.string();
    const std::string &cpp_contents = *cpp_contents_opt;
    const auto ixx_contents_opt = readFileText(ixx_path);
    ASSERT_TRUE(ixx_contents_opt.has_value()) << "missing " << ixx_path.string();
    const std::string &ixx_contents = *ixx_contents_opt;

    const auto count_occurrences = [](const std::string &haystack, const std::string &needle) {
        std::size_t count = 0;
        std::size_t pos = 0;
        while ((pos = haystack.find(needle, pos)) != std::string::npos) {
            ++count;
            pos += needle.size();
        }
        return count;
    };

    EXPECT_EQ(count_occurrences(cpp_contents, "createImageView("), 1U)
      << "CascadedShadowMap.cpp should build exactly one image view for the shadow map array (the sampled "
         "view in init()) - the depth attachment in recordCommands() must reuse it via "
         "shadowMapArray->getImageView() rather than creating a second, byte-identical view";
    EXPECT_EQ(count_occurrences(cpp_contents, "buildImageViewCreateInfo("), 0U)
      << "CascadedShadowMap.cpp should not hand-roll an image-view create info for the depth "
         "attachment - reuse shadowMapArray->getImageView() instead";
    EXPECT_EQ(cpp_contents.find("shadowMapArrayView"), std::string::npos)
      << "CascadedShadowMap.cpp still references shadowMapArrayView - the depth attachment view "
         "should come from shadowMapArray->getImageView() instead of a second owned view";
    EXPECT_EQ(ixx_contents.find("shadowMapArrayView"), std::string::npos)
      << "CascadedShadowMap.ixx still declares the shadowMapArrayView member - the depth attachment "
         "reuses shadowMapArray's own image view";
}

// A single light-matrix UBO races in-flight passes invisibly to goldens and validation, so the source shape is pinned.
TEST(BuildIntegrity, ShadowLightMatricesAreDoubleBufferedPerSwapchainImage)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path shadow_dir = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "light" / "directional_light";

    const fs::path ixx_path = shadow_dir / "CascadedShadowMap.ixx";
    const auto ixx_contents_opt = readFileText(ixx_path);
    ASSERT_TRUE(ixx_contents_opt.has_value()) << "could not open " << ixx_path.string();
    const std::string &ixx_contents = *ixx_contents_opt;

    EXPECT_NE(ixx_contents.find("std::vector<VulkanBuffer> lightMatricesBuffers"), std::string::npos)
      << "CascadedShadowMap no longer declares lightMatricesBuffers as a std::vector<VulkanBuffer> in "
      << ixx_path.string()
      << " - a bare VulkanBuffer lightMatricesBuffer member means the CPU rewrites one buffer while an in-flight "
         "shadow pass for a different swapchain image may still be reading it.";

    EXPECT_EQ(ixx_contents.find("VulkanBuffer lightMatricesBuffer;"), std::string::npos)
      << "CascadedShadowMap still declares the old single-buffer lightMatricesBuffer member in " << ixx_path.string();

    const fs::path cpp_path = shadow_dir / "CascadedShadowMap.cpp";
    const auto cpp_contents_opt = readFileText(cpp_path);
    ASSERT_TRUE(cpp_contents_opt.has_value()) << "could not open " << cpp_path.string();
    const std::string &cpp_contents = *cpp_contents_opt;

    EXPECT_NE(cpp_contents.find("lightMatricesDescriptors.sets()[image_index]"), std::string::npos)
      << "CascadedShadowMap::recordCommands in " << cpp_path.string()
      << " no longer binds lightMatricesDescriptors.sets()[image_index] - it must select the descriptor set for "
         "the swapchain image being rendered, not always the same one.";

    EXPECT_EQ(cpp_contents.find("lightMatricesDescriptors.sets()[0]"), std::string::npos)
      << "CascadedShadowMap.cpp still binds lightMatricesDescriptors.sets()[0] unconditionally in " << cpp_path.string()
      << " - that is the exact bug this test pins: the CPU rewrites the light-matrix UBO for whichever image is "
         "current, while the shadow pass keeps sampling set 0 regardless of image_index.";
}

TEST(BuildIntegrity, CascadedShadowMapDoesNotStageThroughABufferManager)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path shadow_map_cpp =
      repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "light" / "directional_light" / "CascadedShadowMap.cpp";
    ASSERT_TRUE(sourceExists(shadow_map_cpp)) << "missing " << shadow_map_cpp.string();

    const auto contentsOpt = readFileText(shadow_map_cpp);
    ASSERT_TRUE(contentsOpt.has_value()) << "could not open " << shadow_map_cpp.string();
    const std::string &contents = *contentsOpt;

    EXPECT_EQ(contents.find("VulkanBufferManager"), std::string::npos)
      << "CascadedShadowMap.cpp should seed lightMatricesBuffers directly through getMappedData(), not a "
         "VulkanBufferManager staging round trip";
}
