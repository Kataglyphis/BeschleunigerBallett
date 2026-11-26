// Build integrity: every imported Slang module is used, and none shadows or copies a shared one.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

// material_rules.slang stays binding-free so RT and PT entry points can import it without an ambiguous binding.
TEST(BuildIntegrity, MaterialRulesModuleStaysBindingFree)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path material_rules_path = repo_root / "Resources/ShadersSlang/common/material_rules.slang";
    const auto material_rules_text_opt = readFileText(material_rules_path);
    ASSERT_TRUE(material_rules_text_opt.has_value()) << "missing " << material_rules_path.string();
    const std::string &material_rules_text = *material_rules_text_opt;

    EXPECT_EQ(material_rules_text.find("[vk::binding"), std::string::npos)
      << "material_rules.slang declares a binding - it must stay binding-free so every shading path, including "
         "the ray-query kernels that declare their own objectDescription, can import it";
    EXPECT_NE(material_rules_text.find("float roughness_factor("), std::string::npos)
      << "material_rules.slang no longer defines roughness_factor()";
    EXPECT_NE(material_rules_text.find("float2 material_metallic_roughness("), std::string::npos)
      << "material_rules.slang no longer defines material_metallic_roughness()";
    EXPECT_NE(material_rules_text.find("bool material_masked_out("), std::string::npos)
      << "material_rules.slang no longer defines material_masked_out()";

    const fs::path material_fetch_path = repo_root / "Resources/ShadersSlang/common/material_fetch.slang";
    const auto material_fetch_text_opt = readFileText(material_fetch_path);
    ASSERT_TRUE(material_fetch_text_opt.has_value()) << "missing " << material_fetch_path.string();
    const std::string &material_fetch_text = *material_fetch_text_opt;

    EXPECT_EQ(material_fetch_text.find("float roughness_factor("), std::string::npos)
      << "material_fetch.slang re-defines roughness_factor() - it should live only in material_rules.slang";
    EXPECT_EQ(material_fetch_text.find("float2 material_metallic_roughness("), std::string::npos)
      << "material_fetch.slang re-defines material_metallic_roughness() - it should live only in "
         "material_rules.slang";
    EXPECT_EQ(material_fetch_text.find("bool material_masked_out("), std::string::npos)
      << "material_fetch.slang re-defines material_masked_out() - it should live only in material_rules.slang";
}

namespace {

// Struct definitions too, because a module can be imported for a struct alone.
struct SlangStructDef
{
    std::string name;
    std::string relative_file;// relative to Resources/ShadersSlang, forward slashes
};

// `struct` always precedes the type name, so no brace or paren bookkeeping is needed.
void collect_structs_from_file(const std::vector<std::string> &stripped_lines,
  const std::string &relative_file,
  std::vector<SlangStructDef> &out)
{
    std::string text;
    for (const auto &line : stripped_lines) {
        text += line;
        text += '\n';
    }
    const std::vector<SlangToken> tokens = tokenize_slang(text);
    for (std::size_t t = 0; t + 1 < tokens.size(); ++t) {
        if (tokens[t].is_identifier && tokens[t].text == "struct" && tokens[t + 1].is_identifier) {
            out.push_back({ tokens[t + 1].text, relative_file });
        }
    }
}

// Every struct definition under Resources/ShadersSlang, like collect_slang_functions.
std::vector<SlangStructDef> collect_slang_structs(const fs::path &slang_root)
{
    std::vector<SlangStructDef> structs;
    for (const std::string &relative_path : collect_all_slang_relative_paths(slang_root)) {
        const auto lines = readFileLines(slang_root / relative_path);
        if (!lines) { continue; }
        std::vector<std::string> stripped_lines;
        for (const auto &raw_line : *lines) {
            stripped_lines.push_back(strip_line_comment(strip_string_literals(raw_line)));
        }
        collect_structs_from_file(stripped_lines, relative_path, structs);
    }
    return structs;
}

// Resolves an import as slangc does: same directory, then common/, then the alphabetically first candidate.
std::optional<std::string> resolve_slang_module(const std::map<std::string, std::vector<std::string>> &files_by_stem,
  const std::string &module_name,
  const std::string &importer_relative_file)
{
    const auto found = files_by_stem.find(module_name);
    if (found == files_by_stem.end() || found->second.empty()) { return std::nullopt; }
    const std::vector<std::string> &candidates = found->second;
    if (candidates.size() == 1) { return candidates.front(); }

    const std::string importer_dir = fs::path(importer_relative_file).parent_path().generic_string();
    for (const auto &candidate : candidates) {
        if (fs::path(candidate).parent_path().generic_string() == importer_dir) { return candidate; }
    }
    for (const auto &candidate : candidates) {
        if (candidate.starts_with("common/")) { return candidate; }
    }
    return candidates.front();
}

}// namespace

// An import whose functions and structs the importer never names is stale, and so is any comment justifying it.
TEST(BuildIntegrity, EveryImportedSlangModuleIsUsed)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    const std::vector<std::string> all_relative_paths = collect_all_slang_relative_paths(slang_root);
    std::map<std::string, std::vector<std::string>> files_by_stem;
    for (const auto &relative_path : all_relative_paths) {
        files_by_stem[fs::path(relative_path).stem().string()].push_back(relative_path);
    }
    for (auto &entry : files_by_stem) { std::sort(entry.second.begin(), entry.second.end()); }

    const std::vector<SlangFunctionDef> functions = collect_slang_functions(slang_root);
    const std::vector<SlangStructDef> structs = collect_slang_structs(slang_root);

    std::map<std::string, std::vector<std::string>> exported_names_by_file;
    for (const auto &fn : functions) {
        // A method is reached through its struct, which the struct list exports.
        if (fn.owner.empty()) { exported_names_by_file[fn.relative_file].push_back(fn.name); }
    }
    for (const auto &st : structs) { exported_names_by_file[st.relative_file].push_back(st.name); }
    // A module-scope constant is an export too: material_rules.slang imports constants for MIN_ROUGHNESS alone.
    static const std::regex kConstantRe(R"(^static\s+const\s+\w+\s+([A-Za-z_]\w*)\s*=)");
    // So is a bound resource: path_tracing.slang imports ambient_maps for iblPrefiltered as well.
    static const std::regex kBindingRe(R"(^\[vk::binding\([^)]*\)\]\s*[\w<>, ]*[\w>]\s+([A-Za-z_]\w*)\s*;)");
    for (const auto &relative_path : all_relative_paths) {
        const auto lines = readFileLines(slang_root / relative_path);
        if (!lines) { continue; }
        for (const auto &raw_line : *lines) {
            const std::string stripped = strip_line_comment(raw_line);
            std::smatch match;
            if (std::regex_search(stripped, match, kConstantRe) || std::regex_search(stripped, match, kBindingRe)) {
                exported_names_by_file[relative_path].push_back(match[1].str());
            }
        }
    }

    static const std::regex kImportRe(R"(^\s*import\s+([A-Za-z_]\w*)\s*;)");

    int imports_checked = 0;
    std::vector<std::string> violations;

    for (const auto &relative_path : all_relative_paths) {
        const auto lines = readFileLines(slang_root / relative_path);
        ASSERT_TRUE(lines.has_value()) << "could not open " << relative_path;

        std::vector<std::string> module_names;
        std::vector<std::string> stripped_lines;
        for (const auto &raw_line : *lines) {
            const std::string stripped = strip_line_comment(strip_string_literals(raw_line));
            stripped_lines.push_back(stripped);
            std::smatch match;
            if (std::regex_search(stripped, match, kImportRe)) { module_names.push_back(match[1].str()); }
        }
        if (module_names.empty()) { continue; }

        std::string text;
        for (const auto &line : stripped_lines) {
            text += line;
            text += '\n';
        }
        std::set<std::string> identifiers;
        for (const auto &tok : tokenize_slang(text)) {
            if (tok.is_identifier) { identifiers.insert(tok.text); }
        }

        for (const auto &module_name : module_names) {
            ++imports_checked;
            const auto resolved = resolve_slang_module(files_by_stem, module_name, relative_path);
            if (!resolved.has_value()) {
                violations.push_back(relative_path + ": import " + module_name
                                     + " does not resolve to any .slang file under " + slang_root.string()
                                     + " - the resolution scan itself is broken");
                continue;
            }

            const auto exported = exported_names_by_file.find(*resolved);
            const bool used = exported != exported_names_by_file.end()
                              && std::any_of(exported->second.begin(),
                                exported->second.end(),
                                [&identifiers](const std::string &name) { return identifiers.contains(name); });
            if (!used) {
                violations.push_back(relative_path + ": import " + module_name + " (" + *resolved
                                      + ") is unused - none of its exported function, struct or constant names "
                                        "appear in this file");
            }
        }
    }

    ASSERT_GE(imports_checked, 10) << "found only " << imports_checked << " Slang import statement(s) under "
                                   << slang_root.string() << " - the import scan itself is broken";

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " Slang import(s) pull in a module whose exported functions/structs are never referenced by the "
         "importing file - delete the dead import (and any comment justifying it):"
      << joinViolations(violations);
}

// Slang imports by basename, so a Vulkan file named like an OxidANT module silently replaces it in every import.
TEST(BuildIntegrity, NoVulkanShaderShadowsASharedModule)
{
    std::set<std::string> shared;
    std::error_code error;
    for (fs::recursive_directory_iterator it(sharedSlangRoot(), error), end; !error && it != end; it.increment(error)) {
        if (it->path().extension() == ".slang") { shared.insert(it->path().filename().string()); }
    }
    ASSERT_FALSE(error) << "could not walk " << sharedSlangRoot().string() << ": " << error.message();
    ASSERT_GT(shared.size(), 20U) << "found few .slang files under " << sharedSlangRoot().string();

    std::vector<std::string> shadows;
    for (fs::recursive_directory_iterator it(slangRoot(), error), end; !error && it != end; it.increment(error)) {
        if (it->is_directory(error) && it->path().filename() == "build") {
            it.disable_recursion_pending();
        } else if (it->path().extension() == ".slang" && shared.contains(it->path().filename().string())) {
            shadows.push_back(shaderRelativePath(it->path(), slangRoot()).generic_string());
        }
    }
    ASSERT_FALSE(error) << "could not walk " << slangRoot().string() << ": " << error.message();
    EXPECT_TRUE(shadows.empty()) << "Vulkan shaders named like an OxidANT module, which they replace in every import:"
                                 << joinViolations(shadows);
}

// Each shared leaf module's constants and functions live in one OxidANT file; a private copy drifts from it unseen.
TEST(BuildIntegrity, NoShaderCopiesASharedLeafModule)
{
    struct Rule
    {
        std::string owner;
        std::regex pattern;
        std::string what;
    };
    static const std::string kDefines = R"(^\s*(?!return\b)\w+\s+)";
    // A generic's `<T : I>` stands between its name and its parameters.
    static const std::string kGeneric = R"(\s*(<[^>(]*>)?\s*\()";
    static const std::vector<Rule> kRules = {
        { "common/constants.slang",
          std::regex(R"(\bstatic\s+const\s+\w+\s+(\w*PI|MIN_\w*ROUGHNESS)\s*=)"),
          "a PI or roughness-floor constant" },
        { "common/constants.slang", std::regex(R"(\b3\.14159|\b6\.28318|\b0\.31830|\b1\.57079)"), "a PI literal" },
        { "common/constants.slang", std::regex(R"((^|[^0-9.])0\.045($|[^0-9]))"), "the 0.045 roughness floor" },
        { "common/hdr_safe.slang", std::regex(R"(\bstatic\s+const\s+\w+\s+\w*HDR_MAX\s*=)"), "an HDR maximum" },
        { "common/hdr_safe.slang", std::regex(R"(\b65(000|504)\b)"), "the HDR maximum literal" },
        { "common/hdr_safe.slang",
          std::regex(
            R"(\bisnan\s*\(|0x7[fF]800000|(^|[^\w.])([A-Za-z_]\w*(\.\w+|\[\w+\])*)\s*[!=]=\s*\2($|[^\w.\[(]))"),
          "a NaN check (isnan, the exponent mask or a self-comparison)" },
        { "common/hdr_safe.slang", std::regex(kDefines + R"(hdr_safe\s*\()"), "a second hdr_safe" },
        { "common/aces.slang", std::regex(R"(\b2\.51\b|\b2\.43\b)"), "an ACES coefficient" },
        { "common/aces.slang",
          std::regex(kDefines + R"((aces_tonemap|aces_tonemap_inverse|untonemapped|linear_to_srgb)\s*\()"),
          "an ACES or sRGB curve" },
        { "common/trace_constants.slang",
          std::regex(R"(\bstatic\s+const\s+\w+\s+\w*(TMIN|TMAX|SURFACE_OFFSET|LIGHT_SPAN)\s*=)"),
          "a ray bound" },
        { "common/trace_constants.slang",
          std::regex(
            R"(\b[Tt][Mm](in|ax)\s*=\s*[0-9.]*[1-9]|\b10000\.0\b|\b0\.0001\s*\*|\*\s*0\.0001\b|\b1\.0\s*-\s*1e-4\b)"),
          "a ray-bound literal" },
        { "common/fullscreen.slang",
          std::regex(
            kDefines + R"((fullscreen_vs|fullscreen_uv_to_ndc|fullscreen_ndc_to_uv|fullscreen_ndc_uv_y_sign)\s*\()"),
          "a fullscreen mapping" },
        { "common/fullscreen.slang",
          std::regex(R"(\b0\.5\s*-\s*[\w.\[\]]+\.y\s*\*\s*0\.5\b|\.xy\s*\*\s*0\.5\s*\+\s*0\.5\b)"),
          "an NDC-to-uv conversion" },
        { "common/camera_ray.slang",
          std::regex(kDefines + R"((camera_ray|camera_ray_through_pixel)\s*\()"),
          "a camera ray" },
        { "common/camera_ray.slang",
          std::regex(
            R"(mul\(\s*[\w.]*(inv_view_proj|inv_projection|clip_to_world)\s*,\s*float4\([^;]*,\s*[01](\.0)?\s*,\s*1(\.0)?\s*\)\s*\))"
            R"(|float4\s+\w+\s*=\s*float4\(\s*(?![-0-9.]+\s*,)[^;]*,\s*[01](\.0)?\s*,\s*1(\.0)?\s*\)\s*;)"
            R"(|\binv_view\s*,\s*float4\(\s*0(\.0)?\s*,\s*0(\.0)?\s*,\s*0(\.0)?\s*,\s*1(\.0)?\s*\)|\binv_view\s*\[\s*3\s*\])"),
          "a near- or far-plane unprojection, inline or a statement later, or the eye as a ray origin" },
        { "common/ambient.slang",
          std::regex(kDefines + R"((ambient_split_sum|analytic_ambient)\s*\()"),
          "an ambient term" },
        { "common/alpha_mode.slang",
          std::regex(R"(\bstatic\s+const\s+\w+\s+(ALPHA_OPAQUE|ALPHA_MASK|ALPHA_BLEND)\s*=)"),
          "an alpha mode" },
        { "common/gltf_material.slang",
          std::regex(
            kDefines
            + R"((transform_uv|normalize_or|tangent_space_normal|apply_normal_map|mirror_sign|normal_transform)"
              R"(|material_metallic|material_roughness|alpha_masked_out)\s*\()"),
          "a glTF material rule" },
        { "common/gltf_material.slang", std::regex(R"(\bsqrt\(\s*saturate\(\s*1\.0\s*-\s*dot\()"), "a BC5 z rebuild" },
        { "common/gltf_material.slang",
          std::regex(R"(\bdot\(\s*float3\(\s*[\w.]+\s*,\s*1\.0\s*\))"
                     R"(|\.x\s*\*\s*\w+\.x\s*\+\s*\w+\.y\s*\*\s*\w+\.y\s*\+\s*\w+\.z\b)"),
          "a texture transform" },
        { "common/gltf_material.slang", std::regex(R"(\bfloat3x3\(\s*t\s*,\s*b\s*,)"), "a tangent frame" },
        { "common/cascade_visibility.slang",
          std::regex(kDefines + R"((select_cascade|cascade_hit|cascade_bias|cascade_depth_slope|cascade_visibility))"
                     + kGeneric),
          "a cascade lookup" },
        { "common/cascade_visibility.slang", std::regex(R"(\bmax\(\s*0\.002\s*\*)"), "the cascades' depth bias" },
        { "common/punctual_shadow.slang",
          std::regex(kDefines
                     + R"((unpack_punctual_shadow|fetch_punctual_shadow|punctual_shadow_depth|punctual_shadow_face)"
                       R"(|punctual_shadow_face_coord|punctual_shadow_visibility))"
                     + kGeneric),
          "a punctual shadow lookup" },
        { "common/punctual_shadow.slang", std::regex(R"(\bstruct\s+PunctualShadow\b)"), "the punctual shadow record" },
        { "common/surface_lighting.slang",
          std::regex(kDefines + R"((surface_f0|surface_lighting)\s*\()"),
          "a surface lighting rule" },
        { "common/surface_lighting.slang", std::regex(R"(\bstruct\s+SurfacePoint\b)"), "the surface point" },
        { "common/surface_lighting.slang", std::regex(R"(\blerp\(\s*float3\(\s*0\.04\s*\))"), "a dielectric f0" },
        { "common/gbuffer.slang",
          std::regex(kDefines + R"((gbuffer_pack|gbuffer_unlit|gbuffer_unpack)\s*\()"),
          "a G-buffer packing" },
        { "common/gbuffer.slang", std::regex(R"(\.w\s*<\s*-0\.5\b)"), "the G-buffer's unlit sign" },
        { "common/cloud_upsample.slang",
          std::regex(kDefines + R"((half_texel_depth|cloud_upsample))" + kGeneric),
          "a cloud upsample" },
    };

    std::vector<std::string> violations;
    std::vector<bool> owner_matches(kRules.size(), false);
    std::error_code error;
    for (const fs::path &path : shaderSourceFiles(slangRoot(), error)) {
        const std::string relative = shaderRelativePath(path, slangRoot()).generic_string();
        if (relative.starts_with("build/")) { continue; }
        const auto lines = readFileLines(path);
        ASSERT_TRUE(lines.has_value()) << "could not open " << path.string();
        for (std::size_t i = 0; i < lines->size(); ++i) {
            const std::string line = strip_line_comment((*lines)[i]);
            for (std::size_t r = 0; r < kRules.size(); ++r) {
                if (!std::regex_search(line, kRules[r].pattern)) { continue; }
                if (relative == kRules[r].owner) {
                    owner_matches[r] = true;
                } else {
                    violations.push_back(relative + ":" + std::to_string(i + 1) + ": " + kRules[r].what);
                }
            }
        }
    }
    ASSERT_FALSE(error) << error.message();
    for (std::size_t r = 0; r < kRules.size(); ++r) {
        EXPECT_TRUE(owner_matches[r]) << kRules[r].owner << " no longer holds " << kRules[r].what
                                      << ", so the rule that guards its copies matches nothing";
    }
    EXPECT_TRUE(violations.empty()) << "private copies of a shared leaf module; import it instead:"
                                    << joinViolations(violations);
}
