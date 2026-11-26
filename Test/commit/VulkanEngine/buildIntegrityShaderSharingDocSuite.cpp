// Build integrity: model-loading.md and shader-sharing.md match the loaders, manifests and shading paths.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

// Every ObjMaterial member needs a row in the doc's material table; this list fails alongside ObjMaterial_natural's.
TEST(BuildIntegrity, ModelLoadingDocDocumentsEveryObjMaterialMember)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "source" / "model-loading.md";
    const auto doc_content = readFileText(doc_path);
    ASSERT_TRUE(doc_content.has_value()) << "could not open " << doc_path.string();

    static constexpr std::array<const char *, 22> kObjMaterialMembers{ "diffuse",
        "emission",
        "shininess",
        "dissolve",
        "textureID",
        "alphaCutoff",
        "uv_transform_row0",
        "uv_transform_row1",
        "metallic",
        "roughness",
        "emissiveTextureID",
        "normalTextureID",
        "normalScale",
        "metallicRoughnessTextureID",
        "normal_uv_transform_row0",
        "normal_uv_transform_row1",
        "metallic_roughness_uv_transform_row0",
        "metallic_roughness_uv_transform_row1",
        "emissive_uv_transform_row0",
        "emissive_uv_transform_row1",
        "unlit",
        "alphaTextureID" };

    const auto table_start = doc_content->find("Material fields and where they come from");
    ASSERT_NE(table_start, std::string::npos)
      << doc_path.string() << " is missing its \"Material fields and where they come from\" section";
    const std::string table_text = doc_content->substr(table_start);

    // A row pair shares one table row, so "_row1" counts in full or as the shorthand on its row0 line.
    std::vector<std::string> missing;
    for (const char *member : kObjMaterialMembers) {
        const std::string full = std::string("`") + member + "`";
        if (table_text.find(full) != std::string::npos) { continue; }

        const std::string_view member_view{ member };
        constexpr std::string_view kRow1Suffix = "_row1";
        if (member_view.size() > kRow1Suffix.size()
            && member_view.substr(member_view.size() - kRow1Suffix.size()) == kRow1Suffix) {
            const std::string row0_name =
              std::string(member_view.substr(0, member_view.size() - kRow1Suffix.size())) + "_row0";
            const auto row0_pos = table_text.find(std::string("`") + row0_name + "`");
            if (row0_pos != std::string::npos) {
                const auto line_end = table_text.find('\n', row0_pos);
                const auto line =
                  table_text.substr(row0_pos, line_end == std::string::npos ? std::string::npos : line_end - row0_pos);
                if (line.find("`_row1`") != std::string::npos) { continue; }
            }
        }

        missing.emplace_back(member);
    }

    EXPECT_TRUE(missing.empty()) << doc_path.string()
                                 << "'s material table is missing a row for the following ObjMaterial member(s):"
                                 << joinViolations(missing);
}

// The hand-summarised srgb row must name every map_... directive the *TextureID rows name.
TEST(BuildIntegrity, ModelLoadingDocSrgbRowCoversEveryObjTextureDirective)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "source" / "model-loading.md";
    const auto doc_content = readFileText(doc_path);
    ASSERT_TRUE(doc_content.has_value()) << "could not open " << doc_path.string();

    const auto table_start = doc_content->find("Material fields and where they come from");
    ASSERT_NE(table_start, std::string::npos)
      << doc_path.string() << " is missing its \"Material fields and where they come from\" section";
    const auto table_end = doc_content->find("\n## ", table_start);
    const std::string table_text =
      doc_content->substr(table_start, table_end == std::string::npos ? std::string::npos : table_end - table_start);

    // Table rows only: prose lines without a leading '|' would shift the row indices below.
    std::vector<std::vector<std::string>> rows;
    {
        std::istringstream stream(table_text);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty() || line.front() != '|') { continue; }
            std::vector<std::string> cells;
            std::size_t pos = 1;// skip the leading '|'
            while (pos < line.size()) {
                const std::size_t next = line.find('|', pos);
                const std::size_t end = next == std::string::npos ? line.size() : next;
                std::string cell = line.substr(pos, end - pos);
                const std::size_t first = cell.find_first_not_of(" \t");
                cell = first == std::string::npos ? "" : cell.substr(first, cell.find_last_not_of(" \t") - first + 1);
                cells.push_back(std::move(cell));
                pos = end + 1;
            }
            rows.push_back(std::move(cells));
        }
    }
    // Rows 0 and 1 are the header and separator; data starts at 2.
    ASSERT_GE(rows.size(), 2U) << doc_path.string() << "'s material table has no data rows";

    static const std::regex kBacktickToken(R"(`([^`]*)`)");

    std::string srgb_mtl_cell;
    std::vector<std::string> required_directives;
    for (std::size_t i = 2; i < rows.size(); ++i) {
        const auto &cells = rows[i];
        if (cells.size() < 3) { continue; }

        std::smatch member_match;
        if (!std::regex_search(cells[0], member_match, kBacktickToken)) { continue; }
        const std::string member = member_match[1].str();

        if (member == "srgb") { srgb_mtl_cell = cells[2]; }

        const bool is_texture_id_row =
          member == "textureID" || (member.size() > 9 && member.compare(member.size() - 9, 9, "TextureID") == 0);
        if (!is_texture_id_row) { continue; }

        for (auto it = std::sregex_iterator(cells[2].begin(), cells[2].end(), kBacktickToken);
          it != std::sregex_iterator();
          ++it) {
            const std::string token = (*it)[1].str();
            if (token.rfind("map_", 0) == 0) { required_directives.push_back(token); }
        }
    }

    ASSERT_FALSE(srgb_mtl_cell.empty()) << doc_path.string()
                                        << "'s material table has no `srgb` row (or its `.mtl` cell is empty)";

    std::string srgb_mtl_cell_lower = srgb_mtl_cell;
    std::transform(
      srgb_mtl_cell_lower.begin(), srgb_mtl_cell_lower.end(), srgb_mtl_cell_lower.begin(), [](unsigned char c) {
          return static_cast<char>(std::tolower(c));
      });

    std::vector<std::string> missing;
    for (const std::string &directive : required_directives) {
        std::string needle = directive;
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (srgb_mtl_cell_lower.find(needle) == std::string::npos) { missing.push_back(directive); }
    }
    std::sort(missing.begin(), missing.end());
    missing.erase(std::unique(missing.begin(), missing.end()), missing.end());

    EXPECT_TRUE(missing.empty()) << doc_path.string() << "'s `srgb` row's `.mtl` cell (\"" << srgb_mtl_cell
                                 << "\") is missing the following directive(s) named in a *TextureID row's `.mtl` cell:"
                                 << joinViolations(missing, "\n  add `", "` to the srgb row's .mtl column");
}

// Every known C++/Rust glTF loader divergence needs a doc row; presence only, not accuracy.
TEST(BuildIntegrity, ShaderSharingDocCoversEveryKnownLoaderDivergence)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "source" / "shader-sharing.md";
    const auto doc_content = readFileText(doc_path);
    ASSERT_TRUE(doc_content.has_value()) << "could not open " << doc_path.string();

    static constexpr std::array<const char *, 6> kDivergenceKeys{
        "TEXCOORD_0", "KHR_materials_unlit", "occlusionTexture", "KHR_texture_transform", "BLEND", "Pr"
    };

    const auto section_start = doc_content->find("Known glTF loader divergences");
    ASSERT_NE(section_start, std::string::npos)
      << doc_path.string() << " is missing its \"Known glTF loader divergences\" section";
    const auto section_end = doc_content->find("\n## ", section_start);
    const std::string section_text = doc_content->substr(
      section_start, section_end == std::string::npos ? std::string::npos : section_end - section_start);

    std::vector<std::string> missing;
    for (const char *key : kDivergenceKeys) {
        if (section_text.find(key) == std::string::npos) { missing.emplace_back(key); }
    }

    EXPECT_TRUE(missing.empty())
      << doc_path.string()
      << "'s \"Known glTF loader divergences\" section is missing a row covering the following key(s):"
      << joinViolations(missing);
}

// The shader-targets marker table; std::nullopt without the markers, so an empty map cannot match vacuously.
std::optional<std::map<std::string, std::string>> parse_shader_targets_marker(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::regex kRowPattern(R"(\|\s*`([^`]+)`\s*\|\s*(spirv|wgsl|both)\s*\|)");

    std::map<std::string, std::string> rows;
    bool in_block = false;
    bool saw_block = false;
    for (const auto &line : *lines) {
        if (line.find("<!-- shader-targets:begin -->") != std::string::npos) {
            in_block = true;
            saw_block = true;
            continue;
        }
        if (line.find("<!-- shader-targets:end -->") != std::string::npos) {
            in_block = false;
            continue;
        }
        if (!in_block) { continue; }

        std::smatch match;
        if (std::regex_search(line, match, kRowPattern)) { rows[match[1].str()] = match[2].str(); }
    }

    if (!saw_block) { return std::nullopt; }
    return rows;
}

// The shared-module-targets marker table (spirv, wgsl, both or unused), with the same missing-marker contract.
std::optional<std::map<std::string, std::string>> parse_shared_module_targets_marker(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::regex kRowPattern(R"(\|\s*`(common/[^`]+)`\s*\|\s*(spirv|wgsl|both|\(unused\))\s*\|)");

    std::map<std::string, std::string> rows;
    bool in_block = false;
    bool saw_block = false;
    for (const auto &line : *lines) {
        if (line.find("<!-- shared-module-targets:begin -->") != std::string::npos) {
            in_block = true;
            saw_block = true;
            continue;
        }
        if (line.find("<!-- shared-module-targets:end -->") != std::string::npos) {
            in_block = false;
            continue;
        }
        if (!in_block) { continue; }

        std::smatch match;
        if (std::regex_search(line, match, kRowPattern)) { rows[match[1].str()] = match[2].str(); }
    }

    if (!saw_block) { return std::nullopt; }
    return rows;
}

// Per common/*.slang module, the union of targets over every enabled source (tests included) whose closure reaches it.
std::map<std::string, std::string> shared_module_target_truth(const fs::path &slang_root,
  const ShaderManifestData &manifest)
{
    std::map<std::string, std::set<std::string>> module_targets;

    std::error_code dir_error;
    for (const auto &path : shaderSourceFiles(slang_root, dir_error)) {
        const auto relative = shaderRelativePath(path, slang_root);
        if (relative.parent_path() == "common") { module_targets[relative.generic_string()]; }
    }

    for (const auto &[source, targets] : manifest.file_targets) {
        for (const auto &import_path : import_closure(slang_root, slang_root / source)) {
            std::error_code rel_error;
            const std::string relative = shaderRelativePath(import_path, slang_root).generic_string();
            if (rel_error) { continue; }

            const auto it = module_targets.find(relative);
            if (it == module_targets.end()) { continue; }// not a common/ module
            for (const auto &target : targets) { it->second.insert(target); }
        }
    }

    std::map<std::string, std::string> result;
    for (const auto &[module, targets] : module_targets) {
        if (targets.empty()) {
            result[module] = "(unused)";
        } else if (targets.size() > 1) {
            result[module] = "both";
        } else {
            result[module] = *targets.begin();
        }
    }
    return result;
}

// Each source's documented target (spirv, wgsl or both) must match the manifests; tests/ are out.
TEST(BuildIntegrity, ShaderSharingDocMatchesTheManifestTargets)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "source" / "shader-sharing.md";
    if (!sourceExists(doc_path)) {
        GTEST_SKIP() << "could not open " << doc_path.string() << " - not running from the repo root?";
    }

    const auto doc_targets = parse_shader_targets_marker(doc_path);
    ASSERT_TRUE(doc_targets.has_value())
      << doc_path.string()
      << " is missing its '<!-- shader-targets:begin -->' / '<!-- shader-targets:end -->' marker block";

    const auto &manifest = shader_manifest();
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    // An entry point both renderers compile, such as OxidANT's sky, reads "both".
    std::map<std::string, std::string> truth;
    for (const auto &[source, targets] : manifest->file_targets) {
        if (source.starts_with("tests/")) { continue; }
        truth.emplace(source, targets.size() > 1 ? "both" : *targets.begin());
    }

    std::vector<std::string> doc_only;
    std::vector<std::string> mismatched;
    for (const auto &[source, target] : *doc_targets) {
        const auto it = truth.find(source);
        if (it == truth.end()) {
            doc_only.push_back(source);
        } else if (it->second != target) {
            mismatched.push_back(source + ": doc says " + target + ", manifest says " + it->second);
        }
    }
    std::vector<std::string> manifest_only;
    for (const auto &[source, target] : truth) {
        if (!doc_targets->contains(source)) { manifest_only.push_back(source); }
    }

    EXPECT_TRUE(doc_only.empty()) << doc_path.string()
                                  << " lists file(s) shader-manifest.json does not have:" << joinViolations(doc_only);
    EXPECT_TRUE(manifest_only.empty()) << doc_path.string() << " is missing file(s) shader-manifest.json has:"
                                       << joinViolations(manifest_only);
    EXPECT_TRUE(mismatched.empty()) << doc_path.string() << " disagrees with shader-manifest.json on target(s):"
                                    << joinViolations(mismatched);
}

// The shared-module table must equal the targets recomputed from the import graph, in both directions.
TEST(BuildIntegrity, SharedModuleTargetsTableMatchesTheImportGraph)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";
    const fs::path slang_root = slangRoot();

    const fs::path doc_path = repo_root / "docs" / "source" / "shader-sharing.md";
    if (!sourceExists(doc_path)) {
        GTEST_SKIP() << "could not open " << doc_path.string() << " - not running from the repo root?";
    }

    const auto doc_targets = parse_shared_module_targets_marker(doc_path);
    ASSERT_TRUE(doc_targets.has_value())
      << doc_path.string()
      << " is missing its '<!-- shared-module-targets:begin -->' / '<!-- shared-module-targets:end -->' marker "
         "block";

    const auto &manifest = shader_manifest();
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    const auto truth = shared_module_target_truth(slang_root, *manifest);

    std::vector<std::string> doc_only;
    std::vector<std::string> mismatched;
    for (const auto &[module, target] : *doc_targets) {
        const auto it = truth.find(module);
        if (it == truth.end()) {
            doc_only.push_back(module);
        } else if (it->second != target) {
            mismatched.push_back(module + ": doc says " + target + ", import graph says " + it->second);
        }
    }
    std::vector<std::string> truth_only;
    for (const auto &[module, target] : truth) {
        if (!doc_targets->contains(module)) { truth_only.push_back(module + " (" + target + ")"); }
    }

    EXPECT_TRUE(doc_only.empty()) << doc_path.string()
                                  << "'s shared-module-targets table lists module(s) not found under "
                                  << (slang_root / "common").string() << ":" << joinViolations(doc_only);
    EXPECT_TRUE(truth_only.empty()) << doc_path.string() << "'s shared-module-targets table is missing module(s):"
                                    << joinViolations(truth_only);
    EXPECT_TRUE(mismatched.empty()) << doc_path.string()
                                    << "'s shared-module-targets table disagrees with the import graph on:"
                                    << joinViolations(mismatched);
}

// Per shading path, since a field dropped by one path still passes the any-shader check; reads may go through helpers.
TEST(BuildIntegrity, ShaderSharingDocCoversEveryObjMaterialFieldPerShadingPath)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path scene_types_path = repo_root / "Resources/ShadersSlang/common/scene_types.slang";
    ASSERT_TRUE(sourceExists(scene_types_path)) << "missing " << scene_types_path.string();
    const std::vector<std::string> members = parse_obj_material_member_names(scene_types_path);
    ASSERT_GT(members.size(), 3U) << "found only " << members.size() << " ObjMaterial member(s) in "
                                  << scene_types_path.string() << " - the parser itself is broken";

    const fs::path doc_path = repo_root / "docs" / "source" / "shader-sharing.md";
    const auto doc_content = readFileText(doc_path);
    ASSERT_TRUE(doc_content.has_value()) << "could not open " << doc_path.string();

    const auto section_start = doc_content->find("Which C++ shading path reads which");
    ASSERT_NE(section_start, std::string::npos)
      << doc_path.string() << " is missing its \"Which C++ shading path reads which ObjMaterial field\" section";
    const auto section_end = doc_content->find("\n## ", section_start);
    const std::string section_text = doc_content->substr(
      section_start, section_end == std::string::npos ? std::string::npos : section_end - section_start);

    // Rows of pipe-delimited cells: the member, then one column per shading path.
    std::vector<std::vector<std::string>> rows;
    {
        std::istringstream stream(section_text);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty() || line.front() != '|') { continue; }
            std::vector<std::string> cells;
            std::size_t pos = 1;// skip the leading '|'
            while (pos < line.size()) {
                const std::size_t next = line.find('|', pos);
                const std::size_t end = next == std::string::npos ? line.size() : next;
                std::string cell = line.substr(pos, end - pos);
                const std::size_t first = cell.find_first_not_of(" \t");
                cell = first == std::string::npos ? "" : cell.substr(first, cell.find_last_not_of(" \t") - first + 1);
                cells.push_back(std::move(cell));
                pos = end + 1;
            }
            rows.push_back(std::move(cells));
        }
    }
    // Rows 0 and 1 are the header and separator; data starts at 2.
    ASSERT_GE(rows.size(), 2U) << doc_path.string() << "'s per-shading-path table has no data rows";

    static const std::regex kBacktickToken(R"(`([^`]*)`)");

    std::map<std::string, std::vector<std::string>> rows_by_member;
    std::vector<std::string> extra_rows;
    std::vector<std::string> malformed_rows;
    for (std::size_t i = 2; i < rows.size(); ++i) {
        const auto &cells = rows[i];
        if (cells.size() < 6) { continue; }

        std::smatch member_match;
        if (!std::regex_search(cells[0], member_match, kBacktickToken)) { continue; }
        const std::string member = member_match[1].str();

        const std::vector<std::string> shading_cells(cells.begin() + 1, cells.begin() + 6);
        if (std::any_of(shading_cells.begin(), shading_cells.end(), [](const std::string &c) { return c.empty(); })) {
            malformed_rows.push_back(member);
            continue;
        }

        if (std::find(members.begin(), members.end(), member) == members.end()) {
            extra_rows.push_back(member);
            continue;
        }
        if (rows_by_member.contains(member)) {
            extra_rows.push_back(member + " (duplicate)");
            continue;
        }
        rows_by_member.emplace(member, shading_cells);
    }

    EXPECT_TRUE(malformed_rows.empty()) << doc_path.string()
                                        << "'s per-shading-path table has row(s) with an empty shading-path cell:"
                                        << joinViolations(malformed_rows);

    EXPECT_TRUE(extra_rows.empty())
      << doc_path.string()
      << "'s per-shading-path table has row(s) that are not a current ObjMaterial member (or are duplicates):"
      << joinViolations(extra_rows);

    std::vector<std::string> missing_rows;
    for (const auto &member : members) {
        if (!rows_by_member.contains(member)) { missing_rows.push_back(member); }
    }
    EXPECT_TRUE(missing_rows.empty())
      << doc_path.string() << "'s per-shading-path table is missing a row for the following ObjMaterial member(s):"
      << joinViolations(missing_rows);

    // One source blob per column, in order; shadow_map and alpha_test share one, both alpha-only consumers.
    static constexpr std::array<const char *, 5> kColumnNames{ "rasterizer.slang",
        "deferred.slang",
        "raytrace.rchit.slang",
        "path_tracing.slang",
        "shadow_map.slang / alpha_test.slang" };
    std::array<std::string, 5> column_text;
    {
        const std::array<std::vector<const char *>, 5> kColumnFiles{ {
          { "Resources/ShadersSlang/rasterizer/rasterizer.slang" },
          { "Resources/ShadersSlang/deferred/deferred.slang" },
          // Both traced columns read their hit through traced_hit.slang's one fetch.
          { "Resources/ShadersSlang/raytracing/raytrace.rchit.slang",
            "Resources/ShadersSlang/common/traced_hit.slang" },
          { "Resources/ShadersSlang/path_tracing/path_tracing.slang",
            "Resources/ShadersSlang/common/traced_hit.slang" },
          { "Resources/ShadersSlang/rasterizer/shadows/shadow_map.slang",
            "Resources/ShadersSlang/common/alpha_test.slang" },
        } };
        for (std::size_t c = 0; c < kColumnFiles.size(); ++c) {
            for (const char *relative : kColumnFiles[c]) {
                const fs::path path = repo_root / relative;
                const auto text = readFileText(path);
                ASSERT_TRUE(text.has_value()) << "missing " << path.string();
                column_text[c] += *text;
                column_text[c] += '\n';
            }
        }
    }

    // Helpers and the fields they read, so naming a helper counts; substring matching covers the _lod0 forms.
    const std::vector<std::pair<std::string, std::vector<std::string>>> kHelperFields{
        { "resolved_emission",
          { "emission", "emissiveTextureID", "emissive_uv_transform_row0", "emissive_uv_transform_row1" } },
        { "resolved_metallic_roughness",
          { "metallic",
            "roughness",
            "shininess",
            "metallicRoughnessTextureID",
            "metallic_roughness_uv_transform_row0",
            "metallic_roughness_uv_transform_row1" } },
        { "resolved_normal",
          { "normalTextureID", "normalScale", "normal_uv_transform_row0", "normal_uv_transform_row1" } },
        { "resolved_occlusion",
          { "occlusionTextureID",
            "occlusionStrength",
            "occlusion_uv_transform_row0",
            "occlusion_uv_transform_row1",
            "uv1SlotMask" } },
        { "material_masked_out", { "alphaMode", "alphaCutoff", "dissolve" } },
        { "roughness_factor", { "roughness", "shininess" } },
        { "base_color", { "diffuse" } },
        { "transform_uv", { "uv_transform_row0", "uv_transform_row1" } },
        { "base_color_uv", { "uv_transform_row0", "uv_transform_row1", "uv1SlotMask" } },
        { "slot_uv", { "uv1SlotMask" } },
        { "blend_coverage", { "alphaMode", "dissolve" } },
        { "material_metallic_roughness", { "metallic" } },
        { "sample_metallic_roughness_lod0",
          { "metallic_roughness_uv_transform_row0", "metallic_roughness_uv_transform_row1" } },
    };

    std::vector<std::string> unverified;
    for (const auto &member : members) {
        const auto &cells = rows_by_member.at(member);
        const std::regex material_dot_field(R"(material\.)" + member + R"(\b)");

        for (std::size_t c = 0; c < cells.size(); ++c) {
            const std::string &cell = cells[c];
            if (cell.rfind("not read", 0) == 0) { continue; }

            const std::string &text = column_text[c];
            bool found = std::regex_search(text, material_dot_field);
            if (!found) {
                for (const auto &[helper, fields] : kHelperFields) {
                    if (std::find(fields.begin(), fields.end(), member) == fields.end()) { continue; }
                    if (text.find(helper) != std::string::npos) {
                        found = true;
                        break;
                    }
                }
            }
            if (!found) { unverified.push_back(member + " / " + kColumnNames[c] + " (cell: \"" + cell + "\")"); }
        }
    }

    EXPECT_TRUE(unverified.empty())
      << doc_path.string()
      << "'s per-shading-path table claims the following (member / shading path) read(s), but neither "
         "material.<name> nor a known helper appears in that shading path's own source:"
      << joinViolations(unverified);
}
