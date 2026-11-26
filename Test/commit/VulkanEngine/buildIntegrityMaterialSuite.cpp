// Build integrity: every shading path applies the material's alpha, colour, metal, emission, normals and sides.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

// All three raster shaders share material_masked_out(), where the alpha mode decides, never the cutoff's sign.
TEST(BuildIntegrity, RasterShadersShareOneAlphaCutoffRule)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::array<const char *, 3> kShaders = {
        "Resources/ShadersSlang/deferred/deferred.slang",
        "Resources/ShadersSlang/rasterizer/rasterizer.slang",
        "Resources/ShadersSlang/rasterizer/shadows/shadow_map.slang",
    };
    static const std::string kSharedPredicate = "material_masked_out(";
    static const std::array<const char *, 2> kBannedFallbacks = { "alphaCutoff >=", "alphaCutoff <" };
    static const std::string kTextureGuard = "textureID >= 0";

    std::vector<std::string> violations;
    for (const char *relative_path : kShaders) {
        const fs::path path = repo_root / relative_path;
        const auto textOpt = readFileText(path);
        ASSERT_TRUE(textOpt.has_value()) << "missing " << path.string();
        const std::string &text = *textOpt;

        if (text.find(kSharedPredicate) == std::string::npos) {
            violations.push_back(std::string(relative_path) + ": does not call " + kSharedPredicate);
        }
        for (const char *fallback : kBannedFallbacks) {
            if (text.find(fallback) != std::string::npos) {
                violations.push_back(std::string(relative_path) + ": tests the cutoff's sign ('" + fallback
                                     + "') instead of material.alphaMode");
            }
        }

        // An untextured MASK material still alpha-tests its factor, so both sides of the textureID branch call it.
        std::size_t occurrences = 0;
        for (std::size_t pos = text.find(kSharedPredicate); pos != std::string::npos;
          pos = text.find(kSharedPredicate, pos + kSharedPredicate.size())) {
            ++occurrences;
        }
        if (text.find(kTextureGuard) != std::string::npos && occurrences < 2) {
            violations.push_back(std::string(relative_path) + ": calls " + kSharedPredicate
                                 + " only once - the untextured side of its '" + kTextureGuard
                                 + "' branch must alpha-test the factor too");
        }
    }

    // MASK alpha is baseColorFactor.a times texture alpha; without dissolve every caller loses the factor half.
    {
        const fs::path path = repo_root / "Resources/ShadersSlang/common/material_rules.slang";
        const auto textOpt = readFileText(path);
        ASSERT_TRUE(textOpt.has_value()) << "missing " << path.string();
        const std::string &text = *textOpt;

        const std::size_t fn_start = text.find("bool material_masked_out(");
        ASSERT_NE(fn_start, std::string::npos) << "material_masked_out() definition not found in " << path.string();
        // A fixed window covers the short body without brace matching.
        const std::string body = text.substr(fn_start, 400);
        if (body.find("material.dissolve") == std::string::npos) {
            violations.push_back(
              "material_rules.slang: material_masked_out() no longer multiplies by material.dissolve");
        }
        if (body.find("alpha_masked_out(material.alphaMode") == std::string::npos) {
            violations.push_back(
              "material_rules.slang: material_masked_out() no longer hands gltf_material.slang's "
              "alpha_masked_out() the alpha mode");
        }
    }

    EXPECT_TRUE(violations.empty()) << violations.size()
                                    << " raster shader(s) do not share the single material_masked_out() MASK rule:"
                                    << joinViolations(violations);
}

// Ray queries have no any-hit stage, so under FORCE_OPAQUE a MASK cut-out hits and shadows as a solid quad.
TEST(BuildIntegrity, EveryShadingPathAlphaTestsMaskMaterials)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    std::vector<std::string> violations;

    {
        const fs::path path = repo_root / "Resources/ShadersSlang/path_tracing/path_tracing.slang";
        const auto textOpt = readFileText(path);
        ASSERT_TRUE(textOpt.has_value()) << "missing " << path.string();
        const std::string &text = *textOpt;

        if (text.find("RAY_FLAG_FORCE_OPAQUE") != std::string::npos) {
            violations.push_back(
              "path_tracing.slang: still contains RAY_FLAG_FORCE_OPAQUE - ray queries have no any-hit stage, "
              "so this forces MASK cut-outs to be solid again");
        }
        if (text.find("commit_unmasked_candidates(query)") == std::string::npos) {
            violations.push_back(
              "path_tracing.slang: its ray queries no longer run traced_hit.slang's commit_unmasked_candidates()");
        }
    }

    {
        const fs::path path = repo_root / "Resources/ShadersSlang/common/traced_hit.slang";
        const auto textOpt = readFileText(path);
        ASSERT_TRUE(textOpt.has_value()) << "missing " << path.string();
        const std::string &text = *textOpt;

        if (text.find("ray_hit_masked_out(") == std::string::npos) {
            violations.push_back("traced_hit.slang: does not call the shared ray_hit_masked_out() alpha test");
        }
        if (text.find("CommitNonOpaqueTriangleHit()") == std::string::npos) {
            violations.push_back(
              "traced_hit.slang: does not call CommitNonOpaqueTriangleHit() - without it, no candidate "
              "triangle is ever committed and every ray reports a miss");
        }
    }

    {
        const fs::path path = repo_root / "Resources/ShadersSlang/raytracing/raytrace.rahit.slang";
        const auto textOpt = readFileText(path);
        ASSERT_TRUE(textOpt.has_value()) << "missing " << path.string();
        const std::string &text = *textOpt;

        if (text.find("ray_hit_masked_out(") == std::string::npos) {
            violations.push_back("raytrace.rahit.slang: does not call the shared ray_hit_masked_out() alpha test");
        }
    }

    {
        const fs::path path = repo_root / "Resources/ShadersSlang/common/alpha_test.slang";
        const auto textOpt = readFileText(path);
        ASSERT_TRUE(textOpt.has_value()) << "missing " << path.string();
        const std::string &text = *textOpt;

        if (text.find("bool ray_hit_masked_out(") == std::string::npos) {
            violations.push_back("alpha_test.slang: does not define ray_hit_masked_out()");
        }
    }

    EXPECT_TRUE(violations.empty()) << violations.size()
                                    << " shading path(s) do not alpha-test MASK materials in their ray queries:"
                                    << joinViolations(violations);
}

// glTF base colour is factor times texture, so every textured branch must route its sample through base_color(.
TEST(BuildIntegrity, EveryBaseColourSampleIsScaledByTheMaterialFactor)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::array<const char *, 3> kShaders = {
        "Resources/ShadersSlang/rasterizer/rasterizer.slang",
        "Resources/ShadersSlang/deferred/deferred.slang",
        "Resources/ShadersSlang/common/traced_hit.slang",
    };
    static const std::string kGuard = "material.textureID >= 0)";
    static const std::string kSample = ".Sample";// covers both .Sample( and .SampleLevel(
    static const std::string kHelper = "base_color(";

    std::vector<std::string> violations;
    for (const char *relative_path : kShaders) {
        const fs::path path = repo_root / relative_path;
        const auto lines_opt = readFileLines(path);
        ASSERT_TRUE(lines_opt.has_value()) << "could not open " << path.string();
        const auto &lines = *lines_opt;

        std::size_t guard_start = lines.size();
        for (std::size_t index = 0; index < lines.size(); ++index) {
            if (lines[index].find(kGuard) != std::string::npos) {
                guard_start = index;
                break;
            }
        }
        ASSERT_LT(guard_start, lines.size())
          << relative_path << " has no '" << kGuard << "' branch to check - did the texture guard move?";

        bool sawSample = false;
        bool sawHelper = false;
        int brace_depth = 0;
        bool body_started = false;
        for (std::size_t index = guard_start; index < lines.size(); ++index) {
            brace_depth += static_cast<int>(std::count(lines[index].begin(), lines[index].end(), '{'));
            brace_depth -= static_cast<int>(std::count(lines[index].begin(), lines[index].end(), '}'));
            if (brace_depth > 0) { body_started = true; }
            if (lines[index].find(kSample) != std::string::npos) { sawSample = true; }
            if (lines[index].find(kHelper) != std::string::npos) { sawHelper = true; }
            if (body_started && brace_depth <= 0) { break; }
        }

        if (!sawSample) {
            violations.push_back(std::string(relative_path) + ": textured branch no longer samples a texture");
        }
        if (!sawHelper) {
            violations.push_back(
              std::string(relative_path) + ": samples the base-colour texture without routing it through base_color(");
        }
    }

    EXPECT_TRUE(violations.empty()) << violations.size()
                                    << " shader(s) do not scale their sampled base colour by the material factor:"
                                    << joinViolations(violations);
}

// f0 = mix(0.04, albedo, metallic); a literal 0.0 there renders every metal as a dielectric.
TEST(BuildIntegrity, NoShadingPathPinsMetallicToZero)
{
    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    static const std::regex kPinnedToZero(R"(lerp\(float3\(0\.04\)\s*,[^,]+,\s*0\.0\s*\))");

    std::vector<std::string> violations;
    std::error_code error;
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
            if (std::regex_search(line, kPinnedToZero)) {
                violations.push_back(shaderRelativePath(path, slang_root).generic_string() + ':'
                                     + std::to_string(line_number) + ": " + line);
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " shading path(s) pin f0's metallic mix to a 0.0 literal instead of the material's metallic value:"
      << joinViolations(violations);
}

// Every texture sample line must also call transform_uv( or a per-slot accessor, or render modes disagree.
TEST(BuildIntegrity, EveryBaseColourSampleAppliesTheUvTransform)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::array<const char *, 5> kShaders = {
        "Resources/ShadersSlang/rasterizer/rasterizer.slang",
        "Resources/ShadersSlang/deferred/deferred.slang",
        "Resources/ShadersSlang/rasterizer/shadows/shadow_map.slang",
        "Resources/ShadersSlang/common/traced_hit.slang",
        "Resources/ShadersSlang/common/alpha_test.slang",
    };
    static const std::string kTextures = "textures[";
    static const std::string kSamplers = "textureSamplers[";
    static const std::array<const char *, 5> kTransformCalls = {
        "transform_uv(",
        "base_color_uv(",
        "normal_uv(",
        "metallic_roughness_uv(",
        "emissive_uv(",
    };

    std::vector<std::string> violations;
    for (const char *relative_path : kShaders) {
        const fs::path path = repo_root / relative_path;
        const auto lines = readFileLines(path);
        ASSERT_TRUE(lines.has_value()) << "could not open " << path.string();

        bool sawSampleSite = false;
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find(kTextures) == std::string::npos || line.find(kSamplers) == std::string::npos) { continue; }
            sawSampleSite = true;
            const bool hasTransformCall = std::any_of(kTransformCalls.begin(),
              kTransformCalls.end(),
              [&line](const char *call) { return line.find(call) != std::string::npos; });
            if (!hasTransformCall) {
                violations.push_back(std::string(relative_path) + ":" + std::to_string(line_number)
                                      + " samples the base-colour texture without transform_uv(, so this mode "
                                        "disagrees with the others on any KHR_texture_transform material");
            }
        }
        if (!sawSampleSite) {
            violations.push_back(std::string(relative_path) + ": no base-colour sample site found - did it move?");
        }
    }

    EXPECT_TRUE(violations.empty()) << violations.size()
                                    << " base-colour sample site(s) skip the KHR_texture_transform UV matrix:"
                                    << joinViolations(violations);
}

// Every shading path must consume material.emission, or emitters render black.
TEST(BuildIntegrity, EmissiveIsConsumedByEveryShadingPath)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "ObjMaterial::emission is uploaded per material; a shading path that ignores it renders every glTF emitter "
      "black";

    const fs::path scene_types_path = repo_root / "Resources/ShadersSlang/common/scene_types.slang";
    const auto scene_types_text_opt = readFileText(scene_types_path);
    ASSERT_TRUE(scene_types_text_opt.has_value()) << "could not open " << scene_types_path.string();
    const std::string &scene_types_text = *scene_types_text_opt;
    EXPECT_NE(scene_types_text.find("float3 emission"), std::string::npos)
      << "scene_types.slang no longer declares ObjMaterial::emission. " << kFailureMessage;

    // The paths call resolved_emission[_lod0](); the next test checks that helper still reaches material_emission().
    const fs::path rasterizer_path = repo_root / "Resources/ShadersSlang/rasterizer/rasterizer.slang";
    const auto rasterizer_text_opt = readFileText(rasterizer_path);
    ASSERT_TRUE(rasterizer_text_opt.has_value()) << "could not open " << rasterizer_path.string();
    const std::string &rasterizer_text = *rasterizer_text_opt;
    EXPECT_NE(rasterizer_text.find("resolved_emission(obj, material, In.texCoords)),"), std::string::npos)
      << "rasterizer.slang no longer uses resolved_emission(). " << kFailureMessage;

    const fs::path deferred_path = repo_root / "Resources/ShadersSlang/deferred/deferred.slang";
    const auto deferred_text_opt = readFileText(deferred_path);
    ASSERT_TRUE(deferred_text_opt.has_value()) << "could not open " << deferred_path.string();
    const std::string &deferred_text = *deferred_text_opt;
    EXPECT_NE(deferred_text.find("surface.emissive = hdr_safe(resolved_emission(obj, material, In.texCoords))"),
      std::string::npos)
      << "deferred.slang's geometry pass no longer packs resolved_emission() into the G-buffer. " << kFailureMessage;
    EXPECT_NE(deferred_text.find("ambient, s.emissive),"), std::string::npos)
      << "deferred.slang's lighting pass no longer adds the G-buffer's packed emissive term. " << kFailureMessage;

    // Both traced modes read emission through traced_hit.slang's one fetch; each adds the fetched term.
    const fs::path traced_hit_path = repo_root / "Resources/ShadersSlang/common/traced_hit.slang";
    const auto traced_hit_text_opt = readFileText(traced_hit_path);
    ASSERT_TRUE(traced_hit_text_opt.has_value()) << "could not open " << traced_hit_path.string();
    EXPECT_NE(
      traced_hit_text_opt->find("s.emissive = resolved_emission_lod0(obj, material, texCoords)"), std::string::npos)
      << "traced_hit.slang no longer reads resolved_emission_lod0() into the surface. " << kFailureMessage;

    const fs::path rchit_path = repo_root / "Resources/ShadersSlang/raytracing/raytrace.rchit.slang";
    const auto rchit_text_opt = readFileText(rchit_path);
    ASSERT_TRUE(rchit_text_opt.has_value()) << "could not open " << rchit_path.string();
    EXPECT_NE(rchit_text_opt->find("payload.hit_value += s.emissive"), std::string::npos)
      << "raytrace.rchit.slang no longer adds the hit's emission. " << kFailureMessage;

    const fs::path integrator_path = sharedSlangRoot() / "common" / "path_integrator.slang";
    const auto integrator_text_opt = readFileText(integrator_path);
    ASSERT_TRUE(integrator_text_opt.has_value()) << "could not open " << integrator_path.string();
    EXPECT_NE(integrator_text_opt->find("radiance += throughput * (s.emissive + "), std::string::npos)
      << "path_integrator.slang no longer adds throughput * the hit's emission. " << kFailureMessage;
}

// The emissive texture lights only through emission.slang's shared helper, which must multiply it into the factor.
TEST(BuildIntegrity, EmissionSamplingUsesTheSharedHelperInEveryShadingPath)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "material_emission() in common/emission.slang is the sole place the emissiveFactor * emissiveTexture "
      "multiply happens; a shading path that hand-rolls it instead risks silently diverging from the other three";

    const fs::path emission_path = repo_root / "Resources/ShadersSlang/common/emission.slang";
    const auto emission_text_opt = readFileText(emission_path);
    ASSERT_TRUE(emission_text_opt.has_value()) << "could not open " << emission_path.string();
    const std::string &emission_text = *emission_text_opt;
    EXPECT_NE(emission_text.find("material.emission * sampled"), std::string::npos)
      << "emission.slang's material_emission() no longer multiplies the sampled emissiveTexture into the factor. "
      << kFailureMessage;

    // material_textures.slang is the sole caller of material_emission(); the paths call resolved_emission[_lod0]().
    const fs::path material_textures_path = repo_root / "Resources/ShadersSlang/common/material_textures.slang";
    const auto material_textures_text_opt = readFileText(material_textures_path);
    ASSERT_TRUE(material_textures_text_opt.has_value()) << "could not open " << material_textures_path.string();
    const std::string &material_textures_text = *material_textures_text_opt;
    EXPECT_NE(material_textures_text.find("import emission;"), std::string::npos)
      << "material_textures.slang no longer imports common/emission.slang. " << kFailureMessage;
    EXPECT_NE(material_textures_text.find("material_emission("), std::string::npos)
      << "material_textures.slang no longer calls material_emission(). " << kFailureMessage;
    EXPECT_NE(material_textures_text.find("material.emissiveTextureID"), std::string::npos)
      << "material_textures.slang's resolved_emission helpers no longer branch on ObjMaterial::emissiveTextureID. "
      << kFailureMessage;

    const std::vector<fs::path> shading_paths = {
        repo_root / "Resources/ShadersSlang/rasterizer/rasterizer.slang",
        repo_root / "Resources/ShadersSlang/deferred/deferred.slang",
        repo_root / "Resources/ShadersSlang/common/traced_hit.slang",
    };
    for (const fs::path &path : shading_paths) {
        const auto text_opt = readFileText(path);
        ASSERT_TRUE(text_opt.has_value()) << "could not open " << path.string();
        const std::string &text = *text_opt;
        EXPECT_NE(text.find("import material_textures;"), std::string::npos)
          << path.string() << " no longer imports common/material_textures.slang. " << kFailureMessage;
        const bool calls_resolved_emission = text.find("resolved_emission(") != std::string::npos
                                             || text.find("resolved_emission_lod0(") != std::string::npos;
        EXPECT_TRUE(calls_resolved_emission)
          << path.string() << " no longer calls resolved_emission()/resolved_emission_lod0(). " << kFailureMessage;
    }
}

// Normal maps perturb shading only through gltf_material.slang's shared helpers, which every path in both renderers calls.
TEST(BuildIntegrity, NormalMappingIsAppliedByEveryShadingPath)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "tangent_space_normal() and apply_normal_map() in OxidANT's common/gltf_material.slang are the sole place the "
      "tangent-space normalTexture sample is turned into a world-space shading normal; a shading path that "
      "hand-rolls it instead risks silently diverging from the others, or never lighting normal maps at all";

    const fs::path gltf_material_path = sharedSlangRoot() / "common" / "gltf_material.slang";
    const auto gltf_material_text_opt = readFileText(gltf_material_path);
    ASSERT_TRUE(gltf_material_text_opt.has_value()) << "could not open " << gltf_material_path.string();
    const std::string &gltf_material_text = *gltf_material_text_opt;
    EXPECT_NE(gltf_material_text.find("texel * 2.0 - 1.0"), std::string::npos)
      << "gltf_material.slang's tangent_space_normal() no longer unpacks the sampled tangent-space normal from "
         "[0,1] to [-1,1]. "
      << kFailureMessage;
    EXPECT_NE(gltf_material_text.find("mul(tangent_normal, float3x3("), std::string::npos)
      << "gltf_material.slang's apply_normal_map() must right-multiply the row-built (t, b, n) basis by the "
         "tangent-space normal to apply the tangent-to-world transform; mul(float3x3(t, b, n), v) is the inverse "
         "(world-to-tangent) transform and produces an inverted shading normal.";
    EXPECT_EQ(gltf_material_text.find("mul(float3x3("), std::string::npos)
      << "gltf_material.slang's apply_normal_map() must not left-multiply the row-built (t, b, n) basis - that is "
         "the world-to-tangent transform, not tangent-to-world.";

    const fs::path forward_path = sharedSlangRoot() / "forward" / "forward.slang";
    const auto forward_text_opt = readFileText(forward_path);
    ASSERT_TRUE(forward_text_opt.has_value()) << "could not open " << forward_path.string();
    const std::string &forward_text = *forward_text_opt;
    EXPECT_NE(forward_text.find("apply_normal_map("), std::string::npos)
      << "forward.slang, the Rust renderer's raster pass, no longer calls apply_normal_map(). " << kFailureMessage;
    EXPECT_NE(forward_text.find("tangent_space_normal("), std::string::npos)
      << "forward.slang no longer calls tangent_space_normal(). " << kFailureMessage;

    // material_textures.slang is the sole caller of apply_normal_map() and folds in normalScale.
    const fs::path material_textures_path = repo_root / "Resources/ShadersSlang/common/material_textures.slang";
    const auto material_textures_text_opt = readFileText(material_textures_path);
    ASSERT_TRUE(material_textures_text_opt.has_value()) << "could not open " << material_textures_path.string();
    const std::string &material_textures_text = *material_textures_text_opt;
    EXPECT_NE(material_textures_text.find("import gltf_material;"), std::string::npos)
      << "material_textures.slang no longer imports OxidANT's common/gltf_material.slang. " << kFailureMessage;
    EXPECT_NE(material_textures_text.find("tangent_space_normal("), std::string::npos)
      << "material_textures.slang no longer calls tangent_space_normal(). " << kFailureMessage;
    EXPECT_NE(material_textures_text.find("apply_normal_map("), std::string::npos)
      << "material_textures.slang no longer calls apply_normal_map(). " << kFailureMessage;
    EXPECT_NE(material_textures_text.find("material.normalTextureID"), std::string::npos)
      << "material_textures.slang's resolved_normal helpers no longer branch on ObjMaterial::normalTextureID. "
      << kFailureMessage;
    EXPECT_NE(material_textures_text.find("material.normalScale"), std::string::npos)
      << "material_textures.slang no longer passes ObjMaterial::normalScale into tangent_space_normal(). "
      << kFailureMessage;

    const std::vector<fs::path> shading_paths = {
        repo_root / "Resources/ShadersSlang/rasterizer/rasterizer.slang",
        repo_root / "Resources/ShadersSlang/deferred/deferred.slang",
        repo_root / "Resources/ShadersSlang/common/traced_hit.slang",
    };
    for (const fs::path &path : shading_paths) {
        const auto text_opt = readFileText(path);
        ASSERT_TRUE(text_opt.has_value()) << "could not open " << path.string();
        const std::string &text = *text_opt;
        EXPECT_NE(text.find("import material_textures;"), std::string::npos)
          << path.string() << " no longer imports common/material_textures.slang. " << kFailureMessage;
        const bool calls_resolved_normal =
          text.find("resolved_normal(") != std::string::npos || text.find("resolved_normal_lod0(") != std::string::npos;
        EXPECT_TRUE(calls_resolved_normal)
          << path.string() << " no longer calls resolved_normal()/resolved_normal_lod0(). " << kFailureMessage;
        EXPECT_NE(text.find("worldTangent"), std::string::npos)
          << path.string() << " no longer derives a world-space tangent to build the TBN basis. " << kFailureMessage;
    }
}

// Unlit materials "MUST NOT be lit", so every shading path branches on ObjMaterial::unlit.
TEST(BuildIntegrity, UnlitIsHonouredByEveryShadingPath)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "KHR_materials_unlit (ObjMaterial::unlit) must skip lighting, shadowing and emissive alike in every shading "
      "path, per the extension's \"MUST NOT be lit\" requirement";

    const std::vector<fs::path> shading_paths = {
        repo_root / "Resources/ShadersSlang/rasterizer/rasterizer.slang",
        repo_root / "Resources/ShadersSlang/deferred/deferred.slang",
        repo_root / "Resources/ShadersSlang/common/traced_hit.slang",
    };
    for (const fs::path &path : shading_paths) {
        const auto text_opt = readFileText(path);
        ASSERT_TRUE(text_opt.has_value()) << "could not open " << path.string();
        const std::string &text = *text_opt;
        EXPECT_NE(text.find("material.unlit"), std::string::npos)
          << path.string() << " no longer branches on ObjMaterial::unlit. " << kFailureMessage;
    }

    const fs::path deferred_path = repo_root / "Resources/ShadersSlang/deferred/deferred.slang";
    const auto deferred_text_opt = readFileText(deferred_path);
    ASSERT_TRUE(deferred_text_opt.has_value()) << "could not open " << deferred_path.string();
    EXPECT_NE(deferred_text_opt->find("outAlbedo"), std::string::npos)
      << "deferred.slang's geometry pass no longer writes outAlbedo - the lighting pass has no other channel to "
         "read the unlit flag from. "
      << kFailureMessage;
}

// Every shading path must consume Vertex.color (glTF COLOR_0), not just the raster ones.
TEST(BuildIntegrity, VertexColourIsConsumedByEveryShadingPath)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "Vertex::color (glTF COLOR_0) is uploaded per vertex; a shading path that ignores it silently drops "
      "vertex-painted colour";

    const fs::path rasterizer_path = repo_root / "Resources/ShadersSlang/rasterizer/rasterizer.slang";
    const auto rasterizer_text_opt = readFileText(rasterizer_path);
    ASSERT_TRUE(rasterizer_text_opt.has_value()) << "could not open " << rasterizer_path.string();
    const std::string &rasterizer_text = *rasterizer_text_opt;
    EXPECT_NE(rasterizer_text.find("fragmentColor"), std::string::npos)
      << "rasterizer.slang no longer uses In.fragmentColor. " << kFailureMessage;

    const fs::path deferred_path = repo_root / "Resources/ShadersSlang/deferred/deferred.slang";
    const auto deferred_text_opt = readFileText(deferred_path);
    ASSERT_TRUE(deferred_text_opt.has_value()) << "could not open " << deferred_path.string();
    const std::string &deferred_text = *deferred_text_opt;
    EXPECT_NE(deferred_text.find("fragmentColor"), std::string::npos)
      << "deferred.slang no longer uses In.fragmentColor. " << kFailureMessage;

    // The closest-hit shader and the path tracer read their hit through traced_hit.slang's one fetch.
    const fs::path traced_hit_path = repo_root / "Resources/ShadersSlang/common/traced_hit.slang";
    const auto traced_hit_text_opt = readFileText(traced_hit_path);
    ASSERT_TRUE(traced_hit_text_opt.has_value()) << "could not open " << traced_hit_path.string();
    const std::string &traced_hit_text = *traced_hit_text_opt;
    EXPECT_NE(traced_hit_text.find("albedo *= vertexColor.rgb"), std::string::npos)
      << "traced_hit.slang no longer multiplies Vertex.color into the albedo. " << kFailureMessage;
}

// Only material_fetch.slang declares objectDescription and does the lookup; scene_types.slang just defines the types.
TEST(BuildIntegrity, EveryShadingPathFetchesObjectDescriptionsThroughMaterialFetch)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    const auto &manifest = shader_manifest();
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";
    const auto &vulkan_consumed_sources = manifest->vulkan_spirv_sources;
    ASSERT_FALSE(vulkan_consumed_sources.empty())
      << "no spirv-targeted rows in shader-manifest.json - manifest format changed?";

    static const std::string kObjectDescriptionDecl = "StructuredBuffer<ObjectDescription> objectDescription";
    static const std::string kMaterialIndexAddress = "material_index_address";
    static const std::string kMaterialFetchRelative = "common/material_fetch.slang";
    static const std::string kSceneTypesRelative = "common/scene_types.slang";

    std::vector<std::string> binding_violations;
    std::vector<std::string> lookup_violations;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        const fs::path &path = shader_path;
        if (!isReadableRegularFile(shader_path) || path.extension() != ".slang") { continue; }
        const std::string relative_path = shaderRelativePath(path, slang_root).generic_string();
        if (relative_path.starts_with("build/")) { continue; }
        if (relative_path == kSceneTypesRelative) { continue; }
        const bool is_spirv_shader = vulkan_consumed_sources.contains(relative_path);
        const bool is_common_module = relative_path.starts_with("common/");
        if (!is_spirv_shader && !is_common_module) { continue; }

        const auto content = readFileText(path);
        if (!content) { continue; }

        if (content->find(kObjectDescriptionDecl) != std::string::npos && relative_path != kMaterialFetchRelative) {
            binding_violations.push_back(relative_path);
        }
        if (content->find(kMaterialIndexAddress) != std::string::npos && relative_path != kMaterialFetchRelative) {
            lookup_violations.push_back(relative_path);
        }
    }

    static const std::string kFixSuggestion =
      "use fetch_object_description() / fetch_material() from common/material_fetch.slang instead";

    EXPECT_TRUE(binding_violations.empty())
      << binding_violations.size() << " file(s) redeclare the objectDescription binding outside "
      << kMaterialFetchRelative << ": " << joinViolations(binding_violations) << " - " << kFixSuggestion;

    EXPECT_TRUE(lookup_violations.empty())
      << lookup_violations.size() << " file(s) hand-roll the material_index_address lookup outside "
      << kMaterialFetchRelative << ": " << joinViolations(lookup_violations) << " - " << kFixSuggestion;
}

// glTF 2.0 3.9.2: a doubleSided back face reverses its whole tangent frame; culling is off for it, SV_IsFrontFace tells.
TEST(BuildIntegrity, DoubleSidedBackFacesFlipTheShadingNormal)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "glTF 2.0 Section 3.9.2 requires reversing the normal of back-facing fragments of doubleSided materials, and "
      "with a normal map the whole tangent frame: both C++ renderers disable culling for doubleSided meshes, so a "
      "shading path that does not hand the face side to resolved_normal() renders those back faces black, and one "
      "that negates N before the call keeps the tangent and mirrors the map's relief along it";

    // The face side reaches the shared helper, which negates the mapped normal whole; no caller pre-flips N.
    for (const char *relative :
      { "Resources/ShadersSlang/rasterizer/rasterizer.slang", "Resources/ShadersSlang/deferred/deferred.slang" }) {
        const fs::path path = repo_root / relative;
        const auto text_opt = readFileText(path);
        ASSERT_TRUE(text_opt.has_value()) << "could not open " << path.string();
        const std::string &text = *text_opt;
        EXPECT_NE(text.find("SV_IsFrontFace"), std::string::npos)
          << relative << " no longer reads SV_IsFrontFace. " << kFailureMessage;
        EXPECT_NE(text.find("In.worldTangent, isFrontFace, obj, material"), std::string::npos)
          << relative << " no longer passes isFrontFace into resolved_normal(). " << kFailureMessage;
        EXPECT_EQ(text.find("N = -N"), std::string::npos)
          << relative << " negates N before resolving the normal map. " << kFailureMessage;
    }
    // Both traced modes resolve the normal in traced_hit.slang's one fetch, which takes the side from its caller.
    {
        const char *relative = "Resources/ShadersSlang/common/traced_hit.slang";
        const auto text_opt = readFileText(repo_root / relative);
        ASSERT_TRUE(text_opt.has_value()) << "could not open " << relative;
        EXPECT_NE(text_opt->find("frontFace, obj, material, texCoords)"), std::string::npos)
          << relative << " no longer passes its face-forward side into resolved_normal_lod0(). " << kFailureMessage;
    }
    // Like SV_IsFrontFace, the traced side comes from the winding; the interpolated normal can disagree near silhouettes.
    const std::array<std::pair<const char *, const char *>, 2> traced_sides = {
        std::pair{
          "Resources/ShadersSlang/raytracing/raytrace.rchit.slang", "HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE" },
        std::pair{ "Resources/ShadersSlang/path_tracing/path_tracing.slang", "query.CommittedTriangleFrontFace()" }
    };
    for (const auto &[relative, needle] : traced_sides) {
        const auto text_opt = readFileText(repo_root / relative);
        ASSERT_TRUE(text_opt.has_value()) << "could not open " << relative;
        EXPECT_NE(text_opt->find(needle), std::string::npos)
          << relative << " no longer takes the face side from the triangle's winding (" << needle << ")";
    }

    const fs::path gltf_material_path = sharedSlangRoot() / "common" / "gltf_material.slang";
    const auto gltf_material_text_opt = readFileText(gltf_material_path);
    ASSERT_TRUE(gltf_material_text_opt.has_value()) << "could not open " << gltf_material_path.string();
    EXPECT_NE(gltf_material_text_opt->find("return front_face ? mapped : -mapped;"), std::string::npos)
      << "gltf_material.slang's apply_normal_map() no longer negates the mapped normal as a whole on a back face. "
      << kFailureMessage;

    // The Rust renderer hands the same helper its SV_IsFrontFace.
    const fs::path forward_path = sharedSlangRoot() / "forward" / "forward.slang";
    const auto forward_text_opt = readFileText(forward_path);
    ASSERT_TRUE(forward_text_opt.has_value()) << "could not open " << forward_path.string();
    EXPECT_NE(forward_text_opt->find("SV_IsFrontFace"), std::string::npos)
      << "forward.slang's fs_main no longer reads SV_IsFrontFace. " << kFailureMessage;
    EXPECT_NE(forward_text_opt->find("apply_normal_map(nGeom, In.worldTangent, nTs, isFrontFace)"), std::string::npos)
      << "forward.slang's sample_material no longer hands apply_normal_map() the fragment's face. " << kFailureMessage;
    EXPECT_EQ(forward_text_opt->find("nGeom = -nGeom"), std::string::npos)
      << "forward.slang negates nGeom before the normal map again. " << kFailureMessage;
}
