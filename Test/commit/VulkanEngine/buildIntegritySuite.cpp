// Regression guards for build-system bugs that were expensive to diagnose.
//
// These are filesystem checks, not GPU tests: they run anywhere, including CI
// containers without an adapter.

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "ObjectDescription.hpp"
#include "RepoFiles.hpp"
#include "common/host_device_shared_vars.hpp"
#include "renderer/GlobalUBO.hpp"
#include "renderer/PathTracingDispatch.hpp"
#include "renderer/SceneUBO.hpp"
#include "renderer/pushConstants/PushConstantPathTracing.hpp"
#include "renderer/pushConstants/PushConstantPost.hpp"
#include "renderer/pushConstants/PushConstantRasterizer.hpp"
#include "renderer/pushConstants/PushConstantRayTracing.hpp"
#include "scene/atmospheric_effects/clouds/CloudDispatch.hpp"
#include "shared/scene/ObjMaterial.hpp"
#include "shared/scene/Vertex.hpp"

import kataglyphis.vulkan.cascaded_shadow_map;// Kataglyphis::ShadowPushConstants

namespace {

namespace fs = std::filesystem;

using Kataglyphis::TestSupport::joinViolations;
using Kataglyphis::TestSupport::readFileLines;
using Kataglyphis::TestSupport::readFileText;
using Kataglyphis::TestSupport::repoRoot;
using Kataglyphis::TestSupport::slangRoot;
using Kataglyphis::TestSupport::spirvRoot;

// Build-SlangShaders.ps1 names each compiled artifact
// "<source-stem>.<entry-point>.<ext>", where <source-stem> is the .slang
// filename with only the ".slang" extension removed - it may itself contain
// a dot, e.g. "raytrace.rchit" for raytracing/raytrace.rchit.slang - and
// <entry-point> is a manifest entry-point name, which never contains a dot.
// So the source stem is recovered by dropping the LAST dot-separated
// component of the .spv's own stem (the entry point), not the first.
fs::path source_for_spirv(const fs::path &spv_path, const fs::path &spirv_root, const fs::path &slang_root)
{
    const fs::path relative_dir = fs::relative(spv_path.parent_path(), spirv_root);
    const std::string stem_and_entry = spv_path.stem().string();// strips only ".spv"

    const auto last_dot = stem_and_entry.find_last_of('.');
    if (last_dot == std::string::npos) { return {}; }// no entry-point separator: not a manifest artifact

    const std::string source_stem = stem_and_entry.substr(0, last_dot);
    return slang_root / relative_dir / (source_stem + ".slang");
}

// Strips a trailing "// ..." comment so prose mentioning a constant's name
// cannot be mistaken for its definition.
std::string strip_line_comment(const std::string &line)
{
    const auto comment_pos = line.find("//");
    return comment_pos == std::string::npos ? line : line.substr(0, comment_pos);
}

// Recursion helper for import_closure: walks `source`'s `import <id>;`
// statements, resolving each identifier to a real .slang path (common/ takes
// precedence over the importing file's own directory, matching how the
// remaining name candidates - if that resolution ever needs a third
// fallback - could be added), and recurses into every resolved file exactly
// once via `visited`.
void collect_import_closure(const fs::path &slang_root, const fs::path &source, std::set<fs::path> &visited,
  std::set<fs::path> &closure)
{
    if (visited.contains(source)) { return; }// cycle guard
    visited.insert(source);

    const auto lines = readFileLines(source);
    if (!lines) { return; }

    static const std::regex import_pattern(R"(^\s*import\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)");
    for (const auto &raw_line : *lines) {
        const std::string line = strip_line_comment(raw_line);
        std::smatch match;
        if (!std::regex_search(line, match, import_pattern)) { continue; }

        const std::string identifier = match[1].str();
        std::error_code error;
        fs::path resolved = slang_root / "common" / (identifier + ".slang");
        if (!fs::exists(resolved, error)) { resolved = source.parent_path() / (identifier + ".slang"); }
        if (!fs::exists(resolved, error)) { continue; }// unresolvable import: the compiler's problem, not this gate's

        closure.insert(resolved);
        collect_import_closure(slang_root, resolved, visited, closure);
    }
}

// Every .slang file transitively reachable from `source` via `import <id>;`
// statements (Slang has no preprocessor #include; `import` plays the role
// .glsl includes used to). Does not include `source` itself. `slang_root`'s
// common/ is checked before the importing file's own directory, so a module
// name that exists in both resolves to the shared one.
std::set<fs::path> import_closure(const fs::path &slang_root, const fs::path &source)
{
    std::set<fs::path> visited;
    std::set<fs::path> closure;
    collect_import_closure(slang_root, source, visited, closure);
    closure.erase(source);
    return closure;
}

// The newest mtime across `source`'s import closure, plus which file it came
// from (so callers can name the actual stale import instead of a generic
// "somewhere under common/"). False when the closure is empty.
bool newest_import_for(const fs::path &slang_root, const fs::path &source, fs::file_time_type &out_time, fs::path &out_path)
{
    const auto closure = import_closure(slang_root, source);

    bool found = false;
    for (const auto &import_path : closure) {
        std::error_code error;
        const auto stamp = fs::last_write_time(import_path, error);
        if (error) { continue; }
        if (!found || stamp > out_time) {
            out_time = stamp;
            out_path = import_path;
            found = true;
        }
    }
    return found;
}

// Resources/ShadersSlang/shader-manifest.json is the SINGLE source of truth
// for the Slang shader build: both scripts/windows/Build-SlangShaders.ps1
// and scripts/linux/compile-slang-shaders.sh consume it. It replaced the two
// per-script hand-maintained copies of the manifest/WGSL-map/patch tables
// that this suite used to cross-check against each other - with one data
// file there is no second copy left to drift, so the tests below instead
// verify the FILESYSTEM (compiled SPIR-V, checked-in Rust-crate WGSL)
// against that one manifest, and ShaderManifestJsonIsPresentAndWellFormed
// fails loudly if the file goes missing or corrupt, because neither build
// script can run without it.

// One "wgslMap" row: a Slang source whose combined WGSL emit is copied into
// a Rust crate's shader directory. histogram.wgsl is absent by construction:
// it is hand-written, with no generating Slang source at all (Slang's
// InterlockedAdd on RWStructuredBuffer is not supported for the WGSL
// target).
struct WgslMapping
{
    std::string slang_source;// "src": relative to Resources/ShadersSlang/
    std::string dst_dir;     // "dst": relative to the repository root
    std::string wgsl_file;   // "out": destination file name
};

// Everything this suite needs out of shader-manifest.json, parsed once and
// shared (see shader_manifest below).
struct ShaderManifestData
{
    // Relative (to Resources/ShadersSlang/) .slang paths of every enabled
    // manifest row whose targets include 'spirv' - exactly the sources the
    // C++ Vulkan renderer consumes.
    std::set<std::string> vulkan_spirv_sources;
    // Distinct first path components of vulkan_spirv_sources - the
    // build/spirv/ subdirectories the manifest emits into. Every other
    // subdirectory of Resources/ShadersSlang/ (bloom, ssao, forward, sky,
    // ibl, gpu_cull, tonemap, tex_quad, ...) is a Rust/WebGPU shader that
    // only ever emits WGSL and must not be scanned for SPIR-V.
    std::set<std::string> engine_spirv_subdirs;
    // Relative (to Resources/ShadersSlang/) .slang paths of every enabled
    // manifest row, regardless of target - used to check that every Slang
    // source with an entry point is accounted for, not just the SPIR-V ones.
    std::set<std::string> all_enabled_manifest_files;
    // Union of "targets" (e.g. {"spirv"}, {"wgsl"}, or both) over every
    // enabled row for a given file - used by
    // ShaderSharingDocMatchesTheManifestTargets to classify each source as
    // spirv-only, wgsl-only, or (unexpectedly) both.
    std::map<std::string, std::set<std::string>> file_targets;
    std::vector<WgslMapping> wgsl_map;
    // Output filenames keyed by "depthTexturePatches" (documentation
    // "_comment" keys excluded).
    std::set<std::string> depth_patched_files;
    // "minSlangcVersionForWgsl": the toolchain floor both compile scripts use
    // to decide whether the combined WGSL emit may run at all. Empty when the
    // key is absent - ShaderManifestPinsAMinimumSlangcVersionForWgsl fails on
    // that, because an absent floor silently re-enables the broken emit.
    std::string min_slangc_version_for_wgsl;
};

// Strict parse of shader-manifest.json: any missing/mistyped field in a row
// returns std::nullopt rather than skipping the row, so a malformed manifest
// fails ShaderManifestJsonIsPresentAndWellFormed loudly instead of silently
// shrinking every JSON-derived check. Exceptions are disabled project-wide,
// so nlohmann's no-throw parse mode is used and every access is type-checked
// up front instead of relying on at()'s throws.
std::optional<ShaderManifestData> parse_shader_manifest(const fs::path &manifest_path)
{
    std::ifstream file(manifest_path);
    if (!file) { return std::nullopt; }

    const nlohmann::json doc = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) { return std::nullopt; }

    const auto manifest_it = doc.find("manifest");
    if (manifest_it == doc.end() || !manifest_it->is_array()) { return std::nullopt; }

    ShaderManifestData data;
    for (const auto &row : *manifest_it) {
        if (!row.is_object()) { return std::nullopt; }

        const auto disabled_it = row.find("disabled");
        if (disabled_it != row.end() && disabled_it->is_boolean() && disabled_it->get<bool>()) { continue; }

        const auto file_it = row.find("file");
        const auto targets_it = row.find("targets");
        if (file_it == row.end() || !file_it->is_string()) { return std::nullopt; }
        if (targets_it == row.end() || !targets_it->is_array() || targets_it->empty()) { return std::nullopt; }

        const std::string source = file_it->get<std::string>();
        data.all_enabled_manifest_files.insert(source);

        bool emits_spirv = false;
        for (const auto &target : *targets_it) {
            if (!target.is_string()) { return std::nullopt; }
            const std::string target_name = target.get<std::string>();
            data.file_targets[source].insert(target_name);
            if (target_name == "spirv") { emits_spirv = true; }
        }
        if (!emits_spirv) { continue; }

        data.vulkan_spirv_sources.insert(source);
        const std::size_t slash = source.find('/');
        if (slash != std::string::npos && slash > 0) { data.engine_spirv_subdirs.insert(source.substr(0, slash)); }
    }

    const auto wgsl_map_it = doc.find("wgslMap");
    if (wgsl_map_it == doc.end() || !wgsl_map_it->is_array()) { return std::nullopt; }
    for (const auto &row : *wgsl_map_it) {
        if (!row.is_object()) { return std::nullopt; }
        const auto src_it = row.find("src");
        const auto out_it = row.find("out");
        const auto dst_it = row.find("dst");
        if (src_it == row.end() || !src_it->is_string()) { return std::nullopt; }
        if (out_it == row.end() || !out_it->is_string()) { return std::nullopt; }
        if (dst_it == row.end() || !dst_it->is_string()) { return std::nullopt; }
        data.wgsl_map.push_back({ src_it->get<std::string>(), dst_it->get<std::string>(), out_it->get<std::string>() });
    }

    const auto patches_it = doc.find("depthTexturePatches");
    if (patches_it == doc.end() || !patches_it->is_object()) { return std::nullopt; }
    for (const auto &[key, value] : patches_it->items()) {
        if (key.starts_with("_")) { continue; }// documentation-only keys
        if (!value.is_array() || value.empty()) { return std::nullopt; }
        data.depth_patched_files.insert(key);
    }

    const auto min_version_it = doc.find("minSlangcVersionForWgsl");
    if (min_version_it != doc.end() && min_version_it->is_string()) {
        data.min_slangc_version_for_wgsl = min_version_it->get<std::string>();
    }

    return data;
}

// Parses shader-manifest.json exactly once per process and shares the result
// across every test in this suite. std::nullopt means the file is missing or
// malformed - callers must ASSERT on has_value(), never skip.
const std::optional<ShaderManifestData> &shader_manifest(const fs::path &repo_root)
{
    static const std::optional<ShaderManifestData> cached =
      parse_shader_manifest(slangRoot() / "shader-manifest.json");
    return cached;
}

// A .slang file is only ever compiled on its own if slangc can find an entry
// point in it. Files that exist purely to be `import`ed (e.g.
// raytracing/rt_types.slang) never appear in Build-SlangShaders.ps1's
// manifest and must not be expected to have a matching .spv.
bool has_entry_point(const fs::path &slang_source)
{
    const auto content = readFileText(slang_source);
    if (!content) { return false; }
    return content->find("[shader(") != std::string::npos;
}

// True if some .spv directly under spirv_root's mirror of source's directory
// maps back (via source_for_spirv) to exactly this source file.
bool has_compiled_binary_for_source(const fs::path &source, const fs::path &spirv_root, const fs::path &slang_root)
{
    const fs::path relative_source_dir = fs::relative(source.parent_path(), slang_root);
    const fs::path binary_dir = spirv_root / relative_source_dir;

    std::error_code error;
    if (!fs::exists(binary_dir, error)) { return false; }
    for (fs::directory_iterator it(binary_dir, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".spv") { continue; }
        if (source_for_spirv(it->path(), spirv_root, slang_root) == source) { return true; }
    }
    return false;
}

// Every binding constant shared between host_device_shared_vars.hpp (C++) and
// scene_types.slang (Slang). Both are hand-mirrored today - see
// HostAndShaderSharedConstantsAgree below for why that is dangerous.
const std::vector<std::string> kSharedConstantNames = {
    "MAX_TEXTURE_COUNT", "MAX_CASCADES", "MAX_PCF_RADIUS", "globalUBO_BINDING", "sceneUBO_BINDING", "OBJECT_DESCRIPTION_BINDING",
    "TEXTURES_BINDING", "SAMPLER_BINDING", "SHADOW_MAP_BINDING", "TLAS_BINDING", "OUT_IMAGE_BINDING",
    "ACCUMULATION_IMAGE_BINDING", "GBUFFER_NORMAL_BINDING", "GBUFFER_ALBEDO_BINDING", "GBUFFER_MATERIAL_BINDING",
    "GBUFFER_DEPTH_BINDING"
};

bool is_identifier_char(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_'; }

// Parses the integer that follows a constant name at `name_end`, accepting
// both "#define NAME 3" (no '=') and "[static] const int NAME = 3;" (with
// '=' before the digits).
std::optional<int> parse_int_after(const std::string &line, std::size_t name_end)
{
    std::size_t pos = name_end;
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])) != 0) { ++pos; }
    if (pos < line.size() && line[pos] == '=') {
        ++pos;
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])) != 0) { ++pos; }
    }

    const std::size_t start = pos;
    if (pos < line.size() && (line[pos] == '-' || line[pos] == '+')) { ++pos; }
    const std::size_t digits_start = pos;
    while (pos < line.size() && std::isdigit(static_cast<unsigned char>(line[pos])) != 0) { ++pos; }
    if (pos == digits_start) { return std::nullopt; }// no digits after an optional sign

    return std::stoi(line.substr(start, pos - start));
}

// Scans `path` line by line for every name in kSharedConstantNames, matching
// it as a whole word so e.g. TEXTURES_BINDING does not also match a longer
// identifier that merely contains it as a substring.
std::map<std::string, int> parse_int_constants(const fs::path &path)
{
    std::map<std::string, int> result;
    const auto lines = readFileLines(path);
    if (!lines) { return result; }

    for (const auto &raw_line : *lines) {
        const std::string line = strip_line_comment(raw_line);
        for (const auto &name : kSharedConstantNames) {
            if (result.contains(name)) { continue; }

            const std::size_t pos = line.find(name);
            if (pos == std::string::npos) { continue; }

            const bool left_ok = pos == 0 || !is_identifier_char(line[pos - 1]);
            const std::size_t name_end = pos + name.size();
            const bool right_ok = name_end >= line.size() || !is_identifier_char(line[name_end]);
            if (!left_ok || !right_ok) { continue; }

            if (const auto value = parse_int_after(line, name_end)) { result[name] = *value; }
        }
    }
    return result;
}

// Every distinct GTest suite name (a TEST(...)/TEST_F(...) macro's first
// argument) defined anywhere under Test/commit/VulkanEngine. Matches only
// lines whose first non-whitespace characters are the macro name, so a suite
// name appearing in a comment or a string literal is not picked up.
std::set<std::string> collect_defined_suites(const fs::path &tests_dir)
{
    std::set<std::string> suites;
    std::error_code error;
    for (fs::recursive_directory_iterator it(tests_dir, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".cpp") { continue; }

        const auto lines = readFileLines(it->path());
        if (!lines) { continue; }
        for (const auto &line : *lines) {
            const std::size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos) { continue; }

            for (const std::string &macro : { std::string("TEST_F("), std::string("TEST(") }) {
                if (line.compare(start, macro.size(), macro) != 0) { continue; }

                const std::size_t name_start = start + macro.size();
                const std::size_t comma = line.find(',', name_start);
                if (comma == std::string::npos) { break; }

                const std::size_t name_begin = line.find_first_not_of(" \t", name_start);
                const std::size_t name_end = line.find_last_not_of(" \t", comma - 1);
                if (name_begin == std::string::npos || name_begin > name_end) { break; }

                suites.insert(line.substr(name_begin, name_end - name_begin + 1));
                break;
            }
        }
    }
    return suites;
}

// Every TEST(<suite>, ...) test name defined anywhere under `tests_dir` whose
// suite is exactly `suite`. Same start-of-line anchoring as
// collect_defined_suites, so a name in a comment or a string literal is not
// picked up. Pure file I/O - never runs the tests themselves, which matters
// for GoldenRender/Integration: they require a GPU that the CI container
// does not have.
std::vector<std::string> collect_suite_test_names(const fs::path &tests_dir, const std::string &suite)
{
    std::vector<std::string> names;
    const std::string macro_prefix = "TEST(" + suite + ",";
    std::error_code error;
    for (fs::recursive_directory_iterator it(tests_dir, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".cpp") { continue; }

        const auto lines = readFileLines(it->path());
        if (!lines) { continue; }
        for (const auto &line : *lines) {
            const std::size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos) { continue; }
            if (line.compare(start, macro_prefix.size(), macro_prefix) != 0) { continue; }

            const std::size_t name_start = start + macro_prefix.size();
            const std::size_t close_paren = line.find(')', name_start);
            if (close_paren == std::string::npos) { continue; }

            const std::size_t name_begin = line.find_first_not_of(" \t", name_start);
            const std::size_t name_end = line.find_last_not_of(" \t", close_paren - 1);
            if (name_begin == std::string::npos || name_begin > name_end) { continue; }

            names.push_back(line.substr(name_begin, name_end - name_begin + 1));
        }
    }
    return names;
}

// The five named integers in docs/gpu-golden-testing.md's
// `<!-- golden-counts: defined=N runnable=N integration=N total=N excluded=N -->`
// marker line.
struct GoldenCountsMarker
{
    int defined = 0;
    int runnable = 0;
    int integration = 0;
    int total = 0;
    int excluded = 0;
};

std::optional<int> parse_marker_field(const std::string &line, const std::string &key)
{
    const std::size_t pos = line.find(key);
    if (pos == std::string::npos) { return std::nullopt; }

    const std::size_t digits_start = pos + key.size();
    std::size_t digits_end = digits_start;
    while (digits_end < line.size() && std::isdigit(static_cast<unsigned char>(line[digits_end]))) { ++digits_end; }
    if (digits_end == digits_start) { return std::nullopt; }

    return std::stoi(line.substr(digits_start, digits_end - digits_start));
}

// Parses docs/gpu-golden-testing.md's golden-counts marker line. Returns
// std::nullopt if the marker line, or any of its five fields, is missing -
// the caller distinguishes that from "file not found" so a deleted marker is
// a hard failure rather than a silent pass.
std::optional<GoldenCountsMarker> parse_golden_counts_marker(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    for (const auto &line : *lines) {
        if (line.find("<!-- golden-counts:") == std::string::npos) { continue; }

        const auto defined_val = parse_marker_field(line, "defined=");
        const auto runnable_val = parse_marker_field(line, "runnable=");
        const auto integration_val = parse_marker_field(line, "integration=");
        const auto total_val = parse_marker_field(line, "total=");
        const auto excluded_val = parse_marker_field(line, "excluded=");
        if (!defined_val || !runnable_val || !integration_val || !total_val || !excluded_val) {
            return std::nullopt;
        }

        GoldenCountsMarker marker;
        marker.defined = *defined_val;
        marker.runnable = *runnable_val;
        marker.integration = *integration_val;
        marker.total = *total_val;
        marker.excluded = *excluded_val;
        return marker;
    }
    return std::nullopt;
}

// Parses the `:-`-prefixed exclusion section of docs/gpu-golden-testing.md's
// `--gtest_filter='...'` line in the "Known issue" section (the known-device-
// lost tests excluded from the "runs clean" claim there), returning each
// excluded test as a (suite, name) pair. The doc has an earlier, unrelated
// `--gtest_filter='GoldenRender.*:Integration.*'` example with no exclusion
// section, so a line is only a match once it actually contains `:-`; lines
// without it are skipped rather than taken as "no exclusions". Returns
// std::nullopt only if the file cannot be opened; an empty vector means no
// line with a `:-` exclusion section was found at all - the caller must fail
// loudly on that, not skip.
std::optional<std::vector<std::pair<std::string, std::string>>> parse_golden_test_exclusion_filter(
  const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::string kFilterKey = "--gtest_filter='";
    for (const auto &line : *lines) {
        const std::size_t filter_start = line.find(kFilterKey);
        if (filter_start == std::string::npos) { continue; }

        const std::size_t value_start = filter_start + kFilterKey.size();
        const std::size_t value_end = line.find('\'', value_start);
        if (value_end == std::string::npos) { continue; }

        const std::string filter = line.substr(value_start, value_end - value_start);
        const std::size_t exclude_start = filter.find(":-");
        if (exclude_start == std::string::npos) { continue; }

        std::vector<std::pair<std::string, std::string>> excluded;
        std::size_t pos = exclude_start + 2;
        while (pos < filter.size()) {
            std::size_t next = filter.find(':', pos);
            if (next == std::string::npos) { next = filter.size(); }
            const std::string entry = filter.substr(pos, next - pos);
            const std::size_t dot = entry.find('.');
            if (dot != std::string::npos) { excluded.emplace_back(entry.substr(0, dot), entry.substr(dot + 1)); }
            pos = next + 1;
        }
        return excluded;
    }
    return std::vector<std::pair<std::string, std::string>>{};
}

// Parses docs/path-tracing.md's `<!-- pt-goldens: name1, name2, ... -->`
// marker line into the list of TEST(GoldenRender, ...) names it lists,
// trimming whitespace around each comma-separated entry. Returns
// std::nullopt if the marker line is missing, malformed (no closing "-->"),
// or lists zero names - a deleted marker must fail the calling test, not
// silently pass.
std::optional<std::vector<std::string>> parse_pt_goldens_marker(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::string kMarkerKey = "<!-- pt-goldens:";
    for (const auto &line : *lines) {
        const std::size_t marker_start = line.find(kMarkerKey);
        if (marker_start == std::string::npos) { continue; }

        const std::size_t value_start = marker_start + kMarkerKey.size();
        const std::size_t value_end = line.find("-->", value_start);
        if (value_end == std::string::npos) { return std::nullopt; }

        const std::string list = line.substr(value_start, value_end - value_start);
        std::vector<std::string> names;
        std::size_t pos = 0;
        while (pos <= list.size()) {
            std::size_t next = list.find(',', pos);
            if (next == std::string::npos) { next = list.size(); }
            const std::string entry = list.substr(pos, next - pos);
            const std::size_t begin = entry.find_first_not_of(" \t");
            const std::size_t end = entry.find_last_not_of(" \t");
            if (begin != std::string::npos && begin <= end) { names.push_back(entry.substr(begin, end - begin + 1)); }
            if (next == list.size()) { break; }
            pos = next + 1;
        }
        if (names.empty()) { return std::nullopt; }
        return names;
    }
    return std::nullopt;
}

// Parses the exact suite-name globs out of Windows.yml's hand-written
// `$gpuOnlySuites` PowerShell array (the "Run CPU-only tests inside the
// container" step). Anchored on the array opener and its `-join ':'` closer
// so an unrelated array elsewhere in the file cannot be picked up. Returns
// std::nullopt only if the file cannot be opened.
std::optional<std::vector<std::string>> parse_ci_gpu_excluded_suites(const fs::path &workflow_path)
{
    const auto lines = readFileLines(workflow_path);
    if (!lines) { return std::nullopt; }

    std::vector<std::string> suites;
    bool inside_array = false;
    for (const auto &line : *lines) {
        if (!inside_array) {
            if (line.find("$gpuOnlySuites = @(") != std::string::npos) { inside_array = true; }
            continue;
        }
        if (line.find("-join ':'") != std::string::npos) { break; }

        const std::size_t open_quote = line.find('\'');
        if (open_quote == std::string::npos) { continue; }
        const std::size_t close_quote = line.find('\'', open_quote + 1);
        if (close_quote == std::string::npos) { continue; }

        std::string entry = line.substr(open_quote + 1, close_quote - open_quote - 1);
        static const std::string kGlobSuffix = ".*";
        if (entry.size() > kGlobSuffix.size()
            && entry.compare(entry.size() - kGlobSuffix.size(), kGlobSuffix.size(), kGlobSuffix) == 0) {
            entry.erase(entry.size() - kGlobSuffix.size());
        }
        suites.push_back(entry);
    }
    return suites;
}

// Every fuzz-target name declared via kataglyphis_add_fuzz_test(<name> ...)
// in Test/fuzz/CMakeLists.txt. The function definition itself
// ("function(kataglyphis_add_fuzz_test fuzz_target source_file)") does not
// match: there is a space, not '(', right after the macro name there.
std::vector<std::string> parse_declared_fuzz_targets(const fs::path &cmake_path)
{
    std::vector<std::string> targets;
    const auto lines = readFileLines(cmake_path);
    if (!lines) { return targets; }

    static const std::string kMacro = "kataglyphis_add_fuzz_test(";
    for (const auto &line : *lines) {
        const std::size_t pos = line.find(kMacro);
        if (pos == std::string::npos) { continue; }

        const std::size_t name_start = pos + kMacro.size();
        std::size_t name_end = name_start;
        while (name_end < line.size() && is_identifier_char(line[name_end])) { ++name_end; }
        if (name_end == name_start) { continue; }
        targets.push_back(line.substr(name_start, name_end - name_start));
    }
    return targets;
}

// Every benchmark name Test/perf/perfSuite.cpp registers via BENCHMARK(<name>)
// - and, for names chained with one or more ->Arg(<n>), "<name>/<n>" for each
// argument, matching Google Benchmark's own run-name convention (e.g.
// "BM_ComputeCascadeData/1"). Modeled on parse_declared_fuzz_targets: a plain
// substring scan per line, with the same "parsed zero declarations" guard so
// a macro rename fails the perf-baseline gate test instead of silently
// passing it.
std::vector<std::string> parse_declared_perf_benchmarks(const fs::path &source_path)
{
    std::vector<std::string> names;
    const auto lines = readFileLines(source_path);
    if (!lines) { return names; }

    static const std::string kMacro = "BENCHMARK(";
    for (const auto &line : *lines) {
        const std::size_t pos = line.find(kMacro);
        if (pos == std::string::npos) { continue; }

        const std::size_t name_start = pos + kMacro.size();
        std::size_t name_end = name_start;
        while (name_end < line.size() && is_identifier_char(line[name_end])) { ++name_end; }
        if (name_end == name_start) { continue; }
        const std::string benchmark_name = line.substr(name_start, name_end - name_start);

        std::vector<std::string> args;
        static const std::string kArgMacro = "->Arg(";
        std::size_t arg_pos = name_end;
        while ((arg_pos = line.find(kArgMacro, arg_pos)) != std::string::npos) {
            const std::size_t digits_start = arg_pos + kArgMacro.size();
            std::size_t digits_end = digits_start;
            while (digits_end < line.size() && std::isdigit(static_cast<unsigned char>(line[digits_end])) != 0) {
                ++digits_end;
            }
            if (digits_end > digits_start) { args.push_back(line.substr(digits_start, digits_end - digits_start)); }
            arg_pos = digits_end;
        }

        if (args.empty()) {
            names.push_back(benchmark_name);
        } else {
            for (const auto &arg : args) { names.push_back(benchmark_name + "/" + arg); }
        }
    }
    return names;
}

// Reads Test/perf/baselines/win-9070xt-32core.json (Google Benchmark's own
// JSON output format) and returns benchmarks[].name for every row. Follows
// parse_shader_manifest's no-throw nlohmann convention above: exceptions are
// disabled project-wide, so a malformed file returns std::nullopt rather than
// throwing.
std::optional<std::vector<std::string>> parse_perf_baseline_names(const fs::path &baseline_path)
{
    std::ifstream file(baseline_path);
    if (!file) { return std::nullopt; }

    const nlohmann::json doc = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) { return std::nullopt; }

    const auto benchmarks_it = doc.find("benchmarks");
    if (benchmarks_it == doc.end() || !benchmarks_it->is_array()) { return std::nullopt; }

    std::vector<std::string> names;
    for (const auto &entry : *benchmarks_it) {
        if (!entry.is_object()) { continue; }
        const auto name_it = entry.find("name");
        if (name_it == entry.end() || !name_it->is_string()) { continue; }
        names.push_back(name_it->get<std::string>());
    }
    return names;
}

// Parses the fuzz-target names out of Windows.yml's "Run fuzz target seeds
// inside the container" step: a PowerShell `foreach (`$t in @('a','b',...))`
// loop. Anchored on "foreach (`$t in @(" (the backtick escapes $t inside the
// surrounding double-quoted PowerShell string) and the following "))", so an
// unrelated foreach loop elsewhere in the file cannot be picked up. Returns
// std::nullopt only if the file cannot be opened; an empty vector means the
// anchor text itself was not found, which the caller must fail loudly on
// rather than skip.
std::optional<std::vector<std::string>> parse_ci_fuzz_targets(const fs::path &workflow_path)
{
    const auto lines = readFileLines(workflow_path);
    if (!lines) { return std::nullopt; }

    // Two spellings, both legal: the step used to run in a host-side pwsh
    // block where `$t` needed backtick-escaping so the RUNNER did not expand
    // it; since the step moved to ANTfrastructure's run-in-windows-container
    // action the command is passed through env, so it is a plain `$t`. Accept
    // either rather than pinning the test to one CI plumbing style.
    static const std::array<std::string, 2> kAnchors = { "foreach (`$t in @(", "foreach ($t in @(" };
    static const std::string kCloser = "))";

    for (const auto &line : *lines) {
        std::size_t anchor_pos = std::string::npos;
        std::size_t anchor_size = 0;
        for (const auto &anchor : kAnchors) {
            const std::size_t pos = line.find(anchor);
            if (pos != std::string::npos) {
                anchor_pos = pos;
                anchor_size = anchor.size();
                break;
            }
        }
        if (anchor_pos == std::string::npos) { continue; }

        const std::size_t list_start = anchor_pos + anchor_size;
        const std::size_t closer_pos = line.find(kCloser, list_start);
        if (closer_pos == std::string::npos) { break; }

        const std::string list = line.substr(list_start, closer_pos - list_start);
        std::vector<std::string> targets;
        std::size_t pos = 0;
        while (pos < list.size()) {
            const std::size_t open_quote = list.find('\'', pos);
            if (open_quote == std::string::npos) { break; }
            const std::size_t close_quote = list.find('\'', open_quote + 1);
            if (close_quote == std::string::npos) { break; }
            targets.push_back(list.substr(open_quote + 1, close_quote - open_quote - 1));
            pos = close_quote + 1;
        }
        return targets;
    }
    return std::vector<std::string>{};
}

// Parses the fuzz-target names out of Linux.yml's "Run fuzzer tests" step: a
// bash `for t in a b c; do` loop. Anchored on "for t in " and the following
// "; do", so an unrelated for-loop elsewhere in the file cannot be picked up.
// Returns std::nullopt only if the file cannot be opened; an empty vector
// means the anchor text itself was not found, which the caller must fail
// loudly on rather than skip. Modeled on parse_ci_fuzz_targets above.
std::optional<std::vector<std::string>> parse_linux_ci_fuzz_targets(const fs::path &workflow_path)
{
    const auto lines = readFileLines(workflow_path);
    if (!lines) { return std::nullopt; }

    static const std::string kAnchor = "for t in ";
    static const std::string kCloser = "; do";

    for (const auto &line : *lines) {
        const std::size_t anchor_pos = line.find(kAnchor);
        if (anchor_pos == std::string::npos) { continue; }

        const std::size_t list_start = anchor_pos + kAnchor.size();
        const std::size_t closer_pos = line.find(kCloser, list_start);
        if (closer_pos == std::string::npos) { break; }

        const std::string list = line.substr(list_start, closer_pos - list_start);
        std::vector<std::string> targets;
        std::istringstream iss(list);
        std::string token;
        while (iss >> token) { targets.push_back(token); }
        return targets;
    }
    return std::vector<std::string>{};
}

// Parses the fuzz-executable names out of Invoke-ClangClDebug.ps1's local
// fuzz-run loop: a PowerShell `foreach ($fuzzExecutable in @('a.exe', ...))`
// loop. Anchored the same way as parse_ci_fuzz_targets, then strips the
// trailing ".exe" so the names compare directly against
// kataglyphis_add_fuzz_test(<name> ...) declarations. Returns std::nullopt
// only if the file cannot be opened; an empty vector means the anchor text
// itself was not found, which the caller must fail loudly on rather than
// skip.
std::optional<std::vector<std::string>> parse_local_runner_fuzz_targets(const fs::path &script_path)
{
    const auto lines = readFileLines(script_path);
    if (!lines) { return std::nullopt; }

    static const std::string kAnchor = "foreach ($fuzzExecutable in @(";
    static const std::string kCloser = "))";
    static const std::string kExeSuffix = ".exe";

    for (const auto &line : *lines) {
        const std::size_t anchor_pos = line.find(kAnchor);
        if (anchor_pos == std::string::npos) { continue; }

        const std::size_t list_start = anchor_pos + kAnchor.size();
        const std::size_t closer_pos = line.find(kCloser, list_start);
        if (closer_pos == std::string::npos) { break; }

        const std::string list = line.substr(list_start, closer_pos - list_start);
        std::vector<std::string> targets;
        std::size_t pos = 0;
        while (pos < list.size()) {
            const std::size_t open_quote = list.find('\'', pos);
            if (open_quote == std::string::npos) { break; }
            const std::size_t close_quote = list.find('\'', open_quote + 1);
            if (close_quote == std::string::npos) { break; }
            std::string entry = list.substr(open_quote + 1, close_quote - open_quote - 1);
            if (entry.size() > kExeSuffix.size()
                && entry.compare(entry.size() - kExeSuffix.size(), kExeSuffix.size(), kExeSuffix) == 0) {
                entry.erase(entry.size() - kExeSuffix.size());
            }
            targets.push_back(entry);
            pos = close_quote + 1;
        }
        return targets;
    }
    return std::vector<std::string>{};
}

using Kataglyphis::ShadowPushConstants;
using Kataglyphis::VulkanRendererInternals::DirectionalLightData;
using Kataglyphis::VulkanRendererInternals::GlobalUBO;
using Kataglyphis::VulkanRendererInternals::PushConstantPathTracing;
using Kataglyphis::VulkanRendererInternals::PushConstantPost;
using Kataglyphis::VulkanRendererInternals::PushConstantRasterizer;
using Kataglyphis::VulkanRendererInternals::PushConstantRaytracing;
using Kataglyphis::VulkanRendererInternals::SceneUBO;

// --- SPIR-V struct-offset parsing, backing SharedStructOffsetsMatchTheCompiledSpirv ---
//
// Mirrors Kataglyphis::validateSpirvBlob's magic-number check
// (vulkan_base/ShaderHelper.cpp) - that constant lives in an anonymous
// namespace inside a module implementation unit and cannot be included from
// here, so the literal is re-declared rather than reused.
constexpr uint32_t kSpirvMagicNumber = 0x07230203;
constexpr std::size_t kSpirvHeaderWordCount = 5;
constexpr uint32_t kOpName = 5;
constexpr uint32_t kOpMemberName = 6;
constexpr uint32_t kOpMemberDecorate = 72;
constexpr uint32_t kDecorationOffset = 35;

// Decodes a SPIR-V literal string operand: ASCII/UTF-8 bytes packed 4 per
// word (little end first), NUL-terminated and padded to a word boundary.
std::string spirv_literal_string(const std::vector<uint32_t> &words, std::size_t word_start, std::size_t word_count)
{
    std::string text;
    text.reserve(word_count * 4);
    for (std::size_t i = 0; i < word_count; ++i) {
        const uint32_t word = words[word_start + i];
        for (uint32_t shift = 0; shift < 32U; shift += 8U) {
            const auto ch = static_cast<char>((word >> shift) & 0xFFU);
            if (ch == '\0') { return text; }
            text.push_back(ch);
        }
    }
    return text;
}

// Parses a compiled .spv module for every named struct's per-member byte
// Offset decoration: { struct name as emitted (e.g. "SceneUBO_std140") ->
// { member name -> Offset } }. Returns std::nullopt if the file cannot be
// opened or does not start with the SPIR-V magic number - callers treat
// that as "not a SPIR-V module" rather than byte-swapping, the same
// contract as Kataglyphis::validateSpirvBlob.
std::optional<std::map<std::string, std::map<std::string, uint32_t>>> parse_spirv_member_offsets(
  const fs::path &spv_path)
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

    std::map<uint32_t, std::string> type_names;// OpName: id -> name
    std::map<std::pair<uint32_t, uint32_t>, std::string> member_names;// OpMemberName: (type id, member) -> name
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> member_offsets;// OpMemberDecorate Offset: (type id, member) -> offset

    std::size_t pos = kSpirvHeaderWordCount;
    while (pos < words.size()) {
        const uint32_t instruction_word = words[pos];
        const uint32_t word_count = instruction_word >> 16U;
        const uint32_t opcode = instruction_word & 0xFFFFU;
        if (word_count == 0 || pos + word_count > words.size()) { break; }// malformed stream - stop, do not read OOB

        if (opcode == kOpName && word_count >= 2) {
            type_names[words[pos + 1]] = spirv_literal_string(words, pos + 2, word_count - 2);
        } else if (opcode == kOpMemberName && word_count >= 3) {
            member_names[{ words[pos + 1], words[pos + 2] }] = spirv_literal_string(words, pos + 3, word_count - 3);
        } else if (opcode == kOpMemberDecorate && word_count >= 4) {
            if (words[pos + 3] == kDecorationOffset && word_count >= 5) {
                member_offsets[{ words[pos + 1], words[pos + 2] }] = words[pos + 4];
            }
        }

        pos += word_count;
    }

    std::map<std::string, std::map<std::string, uint32_t>> result;
    for (const auto &[ids, offset] : member_offsets) {
        const auto type_it = type_names.find(ids.first);
        const auto member_it = member_names.find(ids);
        if (type_it == type_names.end() || member_it == member_names.end()) { continue; }
        result[type_it->second][member_it->second] = offset;
    }
    return result;
}

constexpr uint32_t kOpEntryPoint = 15;

// SPIR-V's own opcodes for the implicit-LOD image sampling instructions -
// automatic derivatives are only defined for Fragment shader invocations, so
// any of these appearing in a module compiled for another execution model is
// a spec violation the validator layers reject at pipeline-creation time.
// Deliberately excludes OpImageQueryLod (100): that instruction is legal in
// GLCompute as well as Fragment.
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

// Human-readable name for a SPIR-V ExecutionModel operand, for test failure
// messages only - not exhaustive, just the models this repo's shaders use.
std::string spirv_execution_model_name(uint32_t model)
{
    switch (model) {
        case 0: return "Vertex";
        case 4: return "Fragment";
        case 5: return "GLCompute";
        case 5313: return "RayGenerationKHR";
        case 5314: return "IntersectionKHR";
        case 5315: return "AnyHitKHR";
        case 5316: return "ClosestHitKHR";
        case 5317: return "MissKHR";
        case 5318: return "CallableKHR";
        default: return "Unknown(" + std::to_string(model) + ")";
    }
}

// One compiled module's execution model (from its single OpEntryPoint - every
// .spv this repo emits has exactly one) and the set of distinct opcodes used
// anywhere in the module. Returns std::nullopt on the same "not a SPIR-V
// module" / "no entry point found" conditions as parse_spirv_member_offsets.
struct SpirvEntryPointInfo
{
    uint32_t execution_model = 0;
    std::set<uint32_t> opcodes_present;
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

        pos += word_count;
    }

    if (!found_entry_point) { return std::nullopt; }
    return info;
}

// Sibling of parse_spirv_entry_point_info that counts occurrences instead of
// just presence - some gates (e.g. TracedObjectIndexAddsTheGeometryIndex)
// need to know an instruction appears at least N times, which a std::set
// membership test cannot express. Same "not a SPIR-V module" contract.
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

constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kDecorationBuiltIn = 11;

// Sibling of parse_spirv_opcode_counts for OpDecorate ... BuiltIn <value>:
// counting bare opcodes cannot distinguish "BuiltIn InstanceId" from
// "BuiltIn InstanceCustomIndexKHR" since both are OpDecorate - the BuiltIn
// enum value is an operand, not the opcode. Backs
// TracedObjectIndexReadsTheInstanceCustomIndex. Same "not a SPIR-V module"
// contract as its siblings.
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

// One shared-layout struct's contract: the struct name Slang emits it as in
// compiled SPIR-V, paired with { emitted member name -> offsetof(HostType,
// member) }. ArrayStride/MatrixStride are deliberately not checked here -
// those live on OpDecorate of the pointer/array types, not OpMemberDecorate
// of the struct, and need type-id chasing this pass does not do; that is a
// scope decision, not an oversight.
struct SpirvStructContract
{
    std::string spirv_name;
    std::map<std::string, std::size_t> member_offsets;
};

// PushConstantSkyBox_std430 deliberately has no entry below: SkyBox's
// push-constant range setup (createGraphicsPipeline) and its push call
// (recordCommands) both push a bare sizeof(uint32_t) with no host struct to
// compare against.
std::vector<SpirvStructContract> build_shared_struct_offset_contracts()
{
    return {
        { "SceneUBO_std140",
          { { "dirLight", offsetof(SceneUBO, dirLight) },
            { "pcfRadius", offsetof(SceneUBO, pcfRadius) },
            { "cascadedShadowIntensity", offsetof(SceneUBO, cascadedShadowIntensity) },
            { "numCascades", offsetof(SceneUBO, numCascades) },
            { "cascadeSplits", offsetof(SceneUBO, cascadeSplits) },
            { "cascadeLightSpaceMatrices", offsetof(SceneUBO, cascadeLightSpaceMatrices) },
            { "view_dir", offsetof(SceneUBO, view_dir) },
            { "cam_pos", offsetof(SceneUBO, cam_pos) },
            { "cloudLightMarch", offsetof(SceneUBO, cloudLightMarch) },
            { "cloudMeshScale", offsetof(SceneUBO, cloudMeshScale) },
            { "cloudMeshOffset", offsetof(SceneUBO, cloudMeshOffset) },
            { "cloudParameters", offsetof(SceneUBO, cloudParameters) } } },
        { "GlobalUBO_std140",
          { { "projection", offsetof(GlobalUBO, projection) },
            { "view", offsetof(GlobalUBO, view) },
            { "inv_projection", offsetof(GlobalUBO, inv_projection) },
            { "inv_view", offsetof(GlobalUBO, inv_view) } } },
        // skybox.slang re-declares GlobalUBO's four members verbatim under its
        // own ConstantBuffer name - same host type (GlobalUBO), different
        // emitted struct name.
        { "CameraUBO_std140",
          { { "projection", offsetof(GlobalUBO, projection) },
            { "view", offsetof(GlobalUBO, view) },
            { "inv_projection", offsetof(GlobalUBO, inv_projection) },
            { "inv_view", offsetof(GlobalUBO, inv_view) } } },
        { "DirectionalLightData_std140",
          { { "direction", offsetof(DirectionalLightData, direction) },
            { "color", offsetof(DirectionalLightData, color) } } },
        { "ObjectDescription_std430",
          { { "vertex_address", offsetof(ObjectDescription, vertex_address) },
            { "index_address", offsetof(ObjectDescription, index_address) },
            { "material_index_address", offsetof(ObjectDescription, material_index_address) },
            { "material_address", offsetof(ObjectDescription, material_address) },
            { "texture_offset", offsetof(ObjectDescription, texture_offset) } } },
        { "ObjMaterial_natural",
          { { "diffuse", offsetof(ObjMaterial, diffuse) },
            { "emission", offsetof(ObjMaterial, emission) },
            { "shininess", offsetof(ObjMaterial, shininess) },
            { "dissolve", offsetof(ObjMaterial, dissolve) },
            { "textureID", offsetof(ObjMaterial, textureID) },
            { "alphaCutoff", offsetof(ObjMaterial, alphaCutoff) },
            { "uv_transform_row0", offsetof(ObjMaterial, uv_transform_row0) },
            { "uv_transform_row1", offsetof(ObjMaterial, uv_transform_row1) },
            { "metallic", offsetof(ObjMaterial, metallic) },
            { "roughness", offsetof(ObjMaterial, roughness) },
            { "emissiveTextureID", offsetof(ObjMaterial, emissiveTextureID) },
            { "normalTextureID", offsetof(ObjMaterial, normalTextureID) },
            { "normalScale", offsetof(ObjMaterial, normalScale) },
            { "metallicRoughnessTextureID", offsetof(ObjMaterial, metallicRoughnessTextureID) },
            { "normal_uv_transform_row0", offsetof(ObjMaterial, normal_uv_transform_row0) },
            { "normal_uv_transform_row1", offsetof(ObjMaterial, normal_uv_transform_row1) },
            { "metallic_roughness_uv_transform_row0", offsetof(ObjMaterial, metallic_roughness_uv_transform_row0) },
            { "metallic_roughness_uv_transform_row1", offsetof(ObjMaterial, metallic_roughness_uv_transform_row1) },
            { "emissive_uv_transform_row0", offsetof(ObjMaterial, emissive_uv_transform_row0) },
            { "emissive_uv_transform_row1", offsetof(ObjMaterial, emissive_uv_transform_row1) },
            { "unlit", offsetof(ObjMaterial, unlit) },
            { "alphaTextureID", offsetof(ObjMaterial, alphaTextureID) } } },
        { "Vertex_natural",
          { { "position", offsetof(Vertex, position) },
            { "normal", offsetof(Vertex, normal) },
            { "color", offsetof(Vertex, color) },
            { "texture_coords", offsetof(Vertex, texture_coords) },
            { "tangent", offsetof(Vertex, tangent) } } },
        { "PushConstantRasterizer_std430",
          { { "model", offsetof(PushConstantRasterizer, model) },
            { "invModelRows", offsetof(PushConstantRasterizer, invModelRows) },
            { "objectIndex", offsetof(PushConstantRasterizer, objectIndex) } } },
        { "PushConstantPathTracing_std430",
          { { "clearColor", offsetof(PushConstantPathTracing, clearColor) },
            { "width", offsetof(PushConstantPathTracing, width) },
            { "height", offsetof(PushConstantPathTracing, height) },
            { "frame_index", offsetof(PushConstantPathTracing, frame_index) },
            { "samples_per_pixel", offsetof(PushConstantPathTracing, samples_per_pixel) },
            { "max_bounces", offsetof(PushConstantPathTracing, max_bounces) } } },
        { "PushConstantPost_std430",
          { { "clouds_enabled", offsetof(PushConstantPost, clouds_enabled) } } },
        { "PushConstantRaytracing_std430",
          { { "clear_color", offsetof(PushConstantRaytracing, clear_color) } } },
        { "ShadowPushConstants_std430",
          // shadow_map.slang names its second field objectIndex, while the
          // host Kataglyphis::ShadowPushConstants (CascadedShadowMap.ixx)
          // calls the same field cascadeIndex - CascadedShadowMap::recordCommands
          // actually passes the flat object index through it
          // (makeShadowPush(modelMatrix, object_index)), so the field is
          // genuinely the shader's objectIndex under an older host name.
          // Keying on the emitted name here checks the two sides agree on
          // BYTE OFFSET, which is what the GPU actually reads.
          { { "model", offsetof(ShadowPushConstants, model) },
            { "objectIndex", offsetof(ShadowPushConstants, cascadeIndex) } } },
    };
}

// Finds the definition of `qualified_name(...)` in `text` (e.g.
// "Kataglyphis::VulkanBuffer::create") and returns the first non-comment,
// non-blank statement in its body, or std::nullopt if the function or its
// body cannot be located. Assumes the parameter list itself contains no
// parentheses - true for every overload this suite scans - so a simple
// depth counter finds the parameter list's matching close paren.
std::optional<std::string> first_statement_of_function(const std::string &text, const std::string &qualified_name)
{
    const std::string signature = qualified_name + "(";
    const std::size_t sig_pos = text.find(signature);
    if (sig_pos == std::string::npos) { return std::nullopt; }

    std::size_t pos = sig_pos + signature.size();
    int depth = 1;
    while (pos < text.size() && depth > 0) {
        if (text[pos] == '(') { ++depth; } else if (text[pos] == ')') { --depth; }
        ++pos;
    }
    if (depth != 0) { return std::nullopt; }

    const std::size_t brace_pos = text.find('{', pos);
    if (brace_pos == std::string::npos) { return std::nullopt; }

    std::size_t body_pos = brace_pos + 1;
    while (body_pos < text.size()) {
        while (body_pos < text.size() && std::isspace(static_cast<unsigned char>(text[body_pos])) != 0) {
            ++body_pos;
        }
        if (body_pos + 1 < text.size() && text[body_pos] == '/' && text[body_pos + 1] == '/') {
            const std::size_t newline = text.find('\n', body_pos);
            body_pos = newline == std::string::npos ? text.size() : newline + 1;
            continue;
        }
        break;
    }

    const std::size_t stmt_end = text.find(';', body_pos);
    if (stmt_end == std::string::npos) { return std::nullopt; }
    return text.substr(body_pos, stmt_end - body_pos + 1);
}

// Same signature-location logic as first_statement_of_function, but returns
// the [begin, end) offsets of the whole brace-matched body instead of just
// its first statement - so a caller can check that every occurrence of some
// other call text falls inside one specific function.
std::optional<std::pair<std::size_t, std::size_t>> function_body_span(
  const std::string &text, const std::string &qualified_name)
{
    const std::string signature = qualified_name + "(";
    const std::size_t sig_pos = text.find(signature);
    if (sig_pos == std::string::npos) { return std::nullopt; }

    std::size_t pos = sig_pos + signature.size();
    int paren_depth = 1;
    while (pos < text.size() && paren_depth > 0) {
        if (text[pos] == '(') { ++paren_depth; } else if (text[pos] == ')') { --paren_depth; }
        ++pos;
    }
    if (paren_depth != 0) { return std::nullopt; }

    const std::size_t brace_pos = text.find('{', pos);
    if (brace_pos == std::string::npos) { return std::nullopt; }

    std::size_t end_pos = brace_pos + 1;
    int brace_depth = 1;
    while (end_pos < text.size() && brace_depth > 0) {
        if (text[end_pos] == '{') { ++brace_depth; } else if (text[end_pos] == '}') { --brace_depth; }
        ++end_pos;
    }
    if (brace_depth != 0) { return std::nullopt; }
    return std::make_pair(brace_pos + 1, end_pos - 1);
}

}// namespace

// Both the build-time compiler (scripts/windows/Build-SlangShaders.ps1) and
// the runtime fallback used to reuse a .spv whenever it merely EXISTED, with
// no timestamp check. Every shader edit after the first build was then
// silently ignored and the GPU executed stale SPIR-V - a fragment shader
// edited at 14:00 was still being rendered from a .spv produced at 18:46 the
// previous day, which invalidated hours of debugging.
//
// This test fails if that regresses: after a build, no committed .spv may be
// older than the .slang source it was compiled from.
TEST(BuildIntegrity, CompiledShadersAreNotOlderThanTheirSources)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    const fs::path spirv_root = spirvRoot();
    ASSERT_TRUE(fs::exists(spirv_root)) << "missing " << spirv_root.string();

    std::vector<std::string> stale;
    std::vector<std::string> unmapped;
    std::error_code error;
    for (fs::recursive_directory_iterator it(spirv_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &spv = it->path();
        if (!it->is_regular_file(error) || spv.extension() != ".spv") { continue; }

        const fs::path source = source_for_spirv(spv, spirv_root, slang_root);
        if (source.empty() || !fs::exists(source, error)) {
            unmapped.push_back(fs::relative(spv, repo_root).string());
            continue;
        }

        const auto source_time = fs::last_write_time(source, error);
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
                              << "Stale binaries (" << stale.size() << "): "
                              << joinViolations(stale);
}

// A Slang module that a compiled .spv's source actually imports (transitively,
// via import_closure) being newer than that .spv means the shader was not
// recompiled after the module changed. Scoped per-entry-point rather than to
// "the newest edit anywhere under common/", so editing one material module no
// longer flags every .spv that never imports it.
TEST(BuildIntegrity, CompiledShadersAreNotOlderThanSharedIncludes)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty());

    const fs::path slang_root = slangRoot();
    const fs::path spirv_root = spirvRoot();
    ASSERT_TRUE(fs::exists(spirv_root));

    std::error_code error;
    std::vector<std::string> stale;
    int checked = 0;
    for (fs::recursive_directory_iterator it(spirv_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".spv") { continue; }

        const fs::path source = source_for_spirv(it->path(), spirv_root, slang_root);
        if (source.empty() || !fs::exists(source)) { continue; }// unmapped: CompiledShadersAreNotOlderThanTheirSources owns this

        fs::file_time_type newest_import{};
        fs::path newest_import_path;
        if (!newest_import_for(slang_root, source, newest_import, newest_import_path)) { continue; }// no imports: nothing shared to be stale against

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

// Proves import_closure resolves only real `import` statements and recurses
// through them: ssao.slang imports fullscreen and nothing material-related,
// so material_fetch must not appear in its closure. rasterizer.slang imports
// material_fetch directly, and material_fetch itself imports scene_types, so
// scene_types reaching rasterizer's closure only works if the recursion
// actually walks material_fetch's own imports rather than just rasterizer's.
TEST(BuildIntegrity, ImportClosureFollowsOnlyRealImports)
{
    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root));

    const auto relative_closure = [&slang_root](const fs::path &source) {
        std::set<std::string> relative;
        for (const auto &import_path : import_closure(slang_root, source)) {
            relative.insert(fs::relative(import_path, slang_root).generic_string());
        }
        return relative;
    };

    const auto ssao_closure = relative_closure(slang_root / "ssao" / "ssao.slang");
    EXPECT_TRUE(ssao_closure.contains("common/fullscreen.slang"));
    EXPECT_FALSE(ssao_closure.contains("common/material_fetch.slang"));

    const auto rasterizer_closure = relative_closure(slang_root / "rasterizer" / "rasterizer.slang");
    EXPECT_TRUE(rasterizer_closure.contains("common/material_fetch.slang"));
    EXPECT_TRUE(rasterizer_closure.contains("common/scene_types.slang"));
}

// Nothing in this repo validated the Slang compiler's output against the
// SPIR-V spec itself - an illegal implicit-LOD .Sample() call sat in
// path_tracing.slang's compute kernel (execution model GLCompute) until it
// device-lost the GPU, because implicit LOD needs an automatic derivative,
// which only exists for Fragment shader invocations. This walks every
// compiled .spv the same way CompiledShadersAreNotOlderThanTheirSources does
// and asserts none of them use an implicit-LOD image instruction outside a
// Fragment-stage module.
TEST(BuildIntegrity, NoImplicitLodImageInstructionsOutsideFragmentShaders)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    const fs::path spirv_root = spirvRoot();
    if (!fs::exists(spirv_root)) { GTEST_SKIP() << "missing " << spirv_root.string() << " - shaders have not been compiled"; }

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
            violations.push_back(fs::relative(spv, repo_root).string() + " (execution model " +
              spirv_execution_model_name(info->execution_model) + ", opcode " + std::to_string(opcode) + ")");
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

// VulkanBuffer and VulkanImage are documented in AGENTS.md as "move-only with
// destructor release". Both operator=(&&) overloads honour that by calling
// cleanUp() before overwriting their handle, but create() did not - calling
// create() a second time on an already-created instance overwrote `buffer`/
// `image` and `allocation` and leaked the previous VMA allocation. This test
// reads all four sources as text and asserts the first statement in each
// create()'s body is its matching release, so the obligation lives in
// create() itself rather than at every call site. A behavioural test would
// need a device.
TEST(BuildIntegrity, ResourceCreateReleasesThePreviousAllocation)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path vulkan_base_dir = repo_root / "Src" / "GraphicsEngineVulkan" / "vulkan_base";
    const fs::path scene_dir = repo_root / "Src" / "GraphicsEngineVulkan" / "scene";

    struct Target
    {
        fs::path source;
        std::string qualified_name;
        std::string expected_first_statement;
    };
    const std::array<Target, 4> targets = {
        Target{ vulkan_base_dir / "VulkanBuffer.cpp", "Kataglyphis::VulkanBuffer::create", "cleanUp();" },
        Target{ vulkan_base_dir / "VulkanImage.cpp", "Kataglyphis::VulkanImage::create", "cleanUp();" },
        Target{ vulkan_base_dir / "VulkanImageView.cpp", "Kataglyphis::VulkanImageView::create", "cleanUp();" },
        Target{ scene_dir / "Texture.cpp", "Kataglyphis::Texture::createTextureSampler", "releaseSampler();" },
    };

    for (const auto &target : targets) {
        const auto text = readFileText(target.source);
        ASSERT_TRUE(text.has_value()) << "could not read " << target.source.string();

        const auto first_statement = first_statement_of_function(*text, target.qualified_name);
        ASSERT_TRUE(first_statement.has_value())
          << target.qualified_name << "(...) definition not found in " << target.source.string();

        EXPECT_EQ(*first_statement, target.expected_first_statement)
          << target.qualified_name << "'s first statement must be " << target.expected_first_statement
          << ", so calling create() on an already-created instance "
             "releases the previous allocation instead of leaking it. Found: \""
          << *first_statement << "\" in " << target.source.string();
    }
}

// DescriptorSetGroup::create() did not release a previous layout/pool either
// - a second create() call overwrote `layout`/`pool` and leaked both. Same
// text-order reasoning as ResourceCreateReleasesThePreviousAllocation above:
// this asserts create()'s first statement is releaseGpuResources(), the
// GPU-resource half of cleanUp(), so the release lives in create() itself.
TEST(BuildIntegrity, DescriptorSetGroupCreateReleasesThePreviousAllocation)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path source = repo_root / "Src" / "GraphicsEngineVulkan" / "vulkan_base" / "DescriptorSetGroup.cpp";
    const auto text = readFileText(source);
    ASSERT_TRUE(text.has_value()) << "could not read " << source.string();

    const auto first_statement = first_statement_of_function(*text, "Kataglyphis::DescriptorSetGroup::create");
    ASSERT_TRUE(first_statement.has_value())
      << "Kataglyphis::DescriptorSetGroup::create(...) definition not found in " << source.string();

    EXPECT_EQ(*first_statement, "releaseGpuResources();")
      << "DescriptorSetGroup::create's first statement must be releaseGpuResources(), so calling create() on an "
         "already-created instance releases the previous layout/pool instead of leaking them. Found: \""
      << *first_statement << "\" in " << source.string();
}

// Texture::uploadRgba and SkyBox::uploadCubeMapFaces both re-create an image
// that an existing VulkanImageView still looks at. VUID-vkDestroyImage-image-
// 01000 requires the view to be destroyed before the image is, so the view
// release must appear, in source order, before the createImage() call that
// triggers VulkanImage::cleanUp() on the old image. This is a text-order
// check, not a behavioural one - see ResourceCreateReleasesThePreviousAllocation's
// comment for why (a behavioural test would need a device).
TEST(BuildIntegrity, ViewIsReleasedBeforeItsImageOnRecreate)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path scene_dir = repo_root / "Src" / "GraphicsEngineVulkan" / "scene";

    struct Target
    {
        fs::path source;
        std::string release_needle;
        std::string create_needle;
    };
    const std::array<Target, 2> targets = {
        Target{ scene_dir / "Texture.cpp", "vulkanImageView.cleanUp();", "createImage(device," },
        Target{ scene_dir / "sky_box" / "SkyBox.cpp", "cubeMapTexture->releaseImageView();",
          "cubeMapTexture->createImage(device," },
    };

    for (const auto &target : targets) {
        const auto text = readFileText(target.source);
        ASSERT_TRUE(text.has_value()) << "could not read " << target.source.string();

        const std::size_t release_pos = text->find(target.release_needle);
        const std::size_t create_pos = text->find(target.create_needle);
        ASSERT_NE(release_pos, std::string::npos)
          << "could not find \"" << target.release_needle << "\" in " << target.source.string();
        ASSERT_NE(create_pos, std::string::npos)
          << "could not find \"" << target.create_needle << "\" in " << target.source.string();

        EXPECT_LT(release_pos, create_pos)
          << target.source.string() << " must release the previous view (\"" << target.release_needle
          << "\") before it recreates the image (\"" << target.create_needle
          << "\"), or the view outlives the image it was created from.";
    }
}

// GLM_FORCE_DEPTH_ZERO_TO_ONE used to be defined ONLY in App.cpp - a
// translation unit that does no projection math - so camera and shadow
// projections were built with OpenGL's [-1,1] depth range while feeding a
// Vulkan [0,1] API. It is now set PUBLIC on VulkanEngineCore, which propagates
// to everything that links it (including this test).
//
// This test deliberately does NOT define the macro itself: it verifies the
// propagation. If someone removes the target_compile_definitions, projections
// silently revert to OpenGL conventions and every depth-dependent feature
// (shadow cascades above all) misbehaves in ways that are painful to trace.
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

    EXPECT_NEAR(near_ndc, 0.0F, 1e-3F)
      << "near plane should map to NDC z=0 (Vulkan). Got " << near_ndc
      << " - GLM_FORCE_DEPTH_ZERO_TO_ONE is not reaching this translation unit.";
    EXPECT_NEAR(far_ndc, 1.0F, 1e-3F)
      << "far plane should map to NDC z=1 (Vulkan). Got " << far_ndc;

    // glm::ortho must agree - the shadow cascades depend on it.
    const glm::mat4 ortho = glm::ortho(-1.0F, 1.0F, -1.0F, 1.0F, kNear, kFar);
    const float ortho_near = (ortho * glm::vec4(0.0F, 0.0F, -kNear, 1.0F)).z;
    EXPECT_NEAR(ortho_near, 0.0F, 1e-3F) << "glm::ortho near plane should map to 0, got " << ortho_near;
}

// EVERY Slang source with an entry point, in a subdirectory the C++ engine
// consumes, must produce SPIR-V - not just the ones a pipeline currently
// loads. Build-SlangShaders.ps1 fails the whole script on a slangc error
// (unlike the old glslc-based compile-shaders.ps1, which only warned), but a
// source that was never added to the manifest at all would otherwise go
// unnoticed until pipeline creation.
TEST(BuildIntegrity, EveryShaderSourceHasCompiledBinary)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty());

    const fs::path slang_root = slangRoot();
    const fs::path spirv_root = spirvRoot();

    const auto &manifest = shader_manifest(repo_root);
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    std::vector<std::string> missing;
    std::error_code error;
    for (const auto &subdir : manifest->engine_spirv_subdirs) {
        const fs::path source_dir = slang_root / subdir;
        if (!fs::exists(source_dir, error)) { continue; }

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

// Every literal .spv path referenced from Src/**/*.cpp, expressed relative to
// spirv_root the same way ActivePipelineShadersHaveCompiledBinaries checks
// it - either a full "Resources/ShadersSlang/build/spirv/..." literal
// (Clouds.cpp), or a bare filename concatenated onto the last in-scope
// `slang_spv_dir` constant declared earlier in the same source (the other
// seven callers). Positional (not per-file) tracking of slang_spv_dir keeps
// this honest if a file ever grows a second one for a different subdirectory.
// Returns {spv path relative to spirv_root, source file - for failure
// messages}; a source may appear more than once (one row per literal).
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
                 match != std::sregex_iterator(); ++match) {
                const std::string literal = (*match)[1].str();
                const std::string full = literal.starts_with(kSpirvPrefix) ? literal : in_scope_slang_spv_dir + literal;
                if (!full.starts_with(kSpirvPrefix)) { continue; }// not a spirv_root path; not this scanner's concern
                result.emplace_back(full.substr(kSpirvPrefix.size()), relative_source);
            }
        }
    }
    return result;
}

// Every shader the Vulkan pipelines actually load must have a compiled .spv.
// A missing binary only surfaces at pipeline creation, i.e. at runtime on a
// machine that may not be yours. The required list is derived (via
// collect_spirv_paths_referenced_by_sources) from the `.spv` literals under
// Src/ rather than hand-copied, so adding a compute/raster pass needs no edit
// here - it just needs to actually reference its .spv the way every existing
// caller does.
TEST(BuildIntegrity, ActivePipelineShadersHaveCompiledBinaries)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty());

    const fs::path slang_root = slangRoot();
    const fs::path spirv_root = spirvRoot();

    const auto referenced = collect_spirv_paths_referenced_by_sources(repo_root);
    // Guards against the scanner silently finding nothing - the failure mode
    // that would make this test *worse* than the hand-maintained list it
    // replaced. This floor (the size of the original list) must only be
    // raised, never lowered, when a pass is added.
    ASSERT_GE(referenced.size(), 19U)
      << "collect_spirv_paths_referenced_by_sources found only " << referenced.size()
      << " .spv reference(s) under Src/ - expected at least 19 from Rasterizer.cpp, DeferredRasterizer.cpp, "
         "PostStage.cpp, SkyBox.cpp, CascadedShadowMap.cpp, Clouds.cpp, Raytracing.cpp and PathTracing.cpp. "
         "Did a source stop using a literal `.spv` string or the `slang_spv_dir` naming convention?";

    std::vector<std::string> missing;
    for (const auto &[relative, referencing_source] : referenced) {
        const fs::path spv = spirv_root / relative;
        const fs::path source = source_for_spirv(spv, spirv_root, slang_root);
        if (!source.empty() && !fs::exists(source)) { continue; }// shader itself moved - not this test's job
        if (!fs::exists(spv)) { missing.push_back(relative + " (referenced by " + referencing_source + ")"); }
    }

    EXPECT_TRUE(missing.empty()) << missing.size()
                                 << " active pipeline shaders have no compiled SPIR-V; pipeline "
                                    "creation would fail at runtime: "
                                 << joinViolations(missing);
}

// host_device_shared_vars.hpp (C++) and scene_types.slang (Slang) hand-mirror
// the same ten binding constants with no shared source of truth. A silent
// divergence would corrupt every descriptor binding without a validation
// error, because each side is internally self-consistent. Asserting every
// name is present in BOTH files (not just equal where both happen to match)
// means a renamed constant fails loudly instead of the pair silently going
// unchecked.
TEST(BuildIntegrity, HostAndShaderSharedConstantsAgree)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const auto host = parse_int_constants(repo_root / "Src" / "GraphicsEngineVulkan" / "common"
                                           / "host_device_shared_vars.hpp");
    const auto shader = parse_int_constants(slangRoot() / "common" / "scene_types.slang");

    for (const auto &name : kSharedConstantNames) {
        ASSERT_TRUE(host.contains(name)) << name << " not found (or not parseable) in host_device_shared_vars.hpp";
        ASSERT_TRUE(shader.contains(name)) << name << " not found (or not parseable) in scene_types.slang";
        EXPECT_EQ(host.at(name), shader.at(name))
          << name << " differs between host_device_shared_vars.hpp (" << host.at(name) << ") and scene_types.slang ("
          << shader.at(name) << ')';
    }
}

// The test above parses host_device_shared_vars.hpp as text, so it would
// happily agree with a header edit that the actual build never sees (e.g. a
// stray duplicate definition later in the file, or a macro guarded out by an
// #ifdef). This test instead includes the real header and compares the
// Slang-side values against the constants the compiler actually produced.
TEST(BuildIntegrity, SharedConstantsMatchTheCompiledHostValues)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const auto shader = parse_int_constants(slangRoot() / "common" / "scene_types.slang");
    for (const auto &name : kSharedConstantNames) {
        ASSERT_TRUE(shader.contains(name)) << name << " not found (or not parseable) in scene_types.slang";
    }

    EXPECT_EQ(shader.at("MAX_TEXTURE_COUNT"), MAX_TEXTURE_COUNT);
    EXPECT_EQ(shader.at("MAX_CASCADES"), MAX_CASCADES);
    EXPECT_EQ(shader.at("MAX_PCF_RADIUS"), MAX_PCF_RADIUS);
    EXPECT_EQ(shader.at("globalUBO_BINDING"), globalUBO_BINDING);
    EXPECT_EQ(shader.at("sceneUBO_BINDING"), sceneUBO_BINDING);
    EXPECT_EQ(shader.at("OBJECT_DESCRIPTION_BINDING"), OBJECT_DESCRIPTION_BINDING);
    EXPECT_EQ(shader.at("TEXTURES_BINDING"), TEXTURES_BINDING);
    EXPECT_EQ(shader.at("SAMPLER_BINDING"), SAMPLER_BINDING);
    EXPECT_EQ(shader.at("SHADOW_MAP_BINDING"), SHADOW_MAP_BINDING);
    EXPECT_EQ(shader.at("TLAS_BINDING"), TLAS_BINDING);
    EXPECT_EQ(shader.at("OUT_IMAGE_BINDING"), OUT_IMAGE_BINDING);
    EXPECT_EQ(shader.at("ACCUMULATION_IMAGE_BINDING"), ACCUMULATION_IMAGE_BINDING);
    EXPECT_EQ(shader.at("GBUFFER_NORMAL_BINDING"), GBUFFER_NORMAL_BINDING);
    EXPECT_EQ(shader.at("GBUFFER_ALBEDO_BINDING"), GBUFFER_ALBEDO_BINDING);
    EXPECT_EQ(shader.at("GBUFFER_MATERIAL_BINDING"), GBUFFER_MATERIAL_BINDING);
    EXPECT_EQ(shader.at("GBUFFER_DEPTH_BINDING"), GBUFFER_DEPTH_BINDING);
}

// Windows.yml now runs every CPU suite by default and excludes GPU suites by
// name (`$gpuOnlySuites`), the same negative-filter shape Linux.yml already
// uses via `--ctest-exclude`. A suite added to Test/commit/VulkanEngine no
// longer needs to be added anywhere to run in CI - the only thing that can
// still drift silently is the GPU-exclusion list itself: an entry there that
// does not match the hardcoded set below, or that does not name a suite that
// actually exists.
TEST(BuildIntegrity, WindowsCiExcludesExactlyTheGpuSuites)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path workflow_path = repo_root / ".github" / "workflows" / "Windows.yml";
    const auto filter_suites_opt = parse_ci_gpu_excluded_suites(workflow_path);
    if (!filter_suites_opt.has_value()) {
        GTEST_SKIP() << "could not open " << workflow_path.string() << " - not running from the repo root?";
    }
    const std::vector<std::string> &filter_suites = *filter_suites_opt;
    ASSERT_FALSE(filter_suites.empty())
      << "parsed zero suites out of the $gpuOnlySuites array in " << workflow_path.string()
      << " - the anchor text ('$gpuOnlySuites = @(' / \"-join ':'\") may have changed";
    const std::set<std::string> filter_set(filter_suites.begin(), filter_suites.end());

    const std::set<std::string> defined_suites =
      collect_defined_suites(repo_root / "Test" / "commit" / "VulkanEngine");
    ASSERT_FALSE(defined_suites.empty()) << "found zero TEST()/TEST_F() suites under Test/commit/VulkanEngine - "
                                            "the scan itself is broken";

    // The container ships the Vulkan loader, so SKIP_WITHOUT_GPU's
    // glfwVulkanSupported() check can answer "yes" with no physical device
    // present, after which device creation aborts the process rather than
    // skipping. These stay excluded until a GPU-capable self-hosted runner
    // exists.
    const std::set<std::string> gpu_excluded_suites = { "GoldenRender", "Integration" };

    EXPECT_EQ(filter_set, gpu_excluded_suites)
      << "Windows.yml's $gpuOnlySuites exclusion list no longer matches the expected GPU-suite set - "
         "update either the workflow or this test's gpu_excluded_suites";

    for (const auto &suite : filter_suites) {
        EXPECT_TRUE(defined_suites.contains(suite))
          << "Windows.yml excludes '" << suite
          << "' from $gpuOnlySuites, but no such suite is defined under Test/commit/VulkanEngine "
             "(renamed or deleted?)";
    }
}

// The fuzz step's target list in Windows.yml is a hand-maintained array, and
// the step silently `continue`s past a missing executable rather than
// failing. Unlike the gtest suites above, this list has no negative-filter
// equivalent (fuzz targets are per-target executables, not gtest_filter
// globs), so it still needs an explicit check: a fuzz target declared in
// Test/fuzz/CMakeLists.txt but never added to Windows.yml's array does not
// run in CI, and nothing says so.
TEST(BuildIntegrity, EveryFuzzTargetIsInTheWindowsCiFuzzList)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const std::vector<std::string> declared_targets =
      parse_declared_fuzz_targets(repo_root / "Test" / "fuzz" / "CMakeLists.txt");
    ASSERT_FALSE(declared_targets.empty())
      << "parsed zero kataglyphis_add_fuzz_test(...) declarations out of Test/fuzz/CMakeLists.txt - the "
         "anchor text ('kataglyphis_add_fuzz_test(') may have changed";
    const std::set<std::string> declared_set(declared_targets.begin(), declared_targets.end());

    const fs::path workflow_path = repo_root / ".github" / "workflows" / "Windows.yml";
    const auto ci_targets_opt = parse_ci_fuzz_targets(workflow_path);
    if (!ci_targets_opt.has_value()) {
        GTEST_SKIP() << "could not open " << workflow_path.string() << " - not running from the repo root?";
    }
    const std::vector<std::string> &ci_targets = *ci_targets_opt;
    ASSERT_FALSE(ci_targets.empty())
      << "parsed zero fuzz targets out of the foreach array in " << workflow_path.string()
      << R"( - the anchor text ('foreach (`$t in @(' / '))') may have changed)";
    const std::set<std::string> ci_set(ci_targets.begin(), ci_targets.end());

    // FuzzTest smoke targets (dummy.cpp / example_fuzz_test.cpp) - they exist
    // to prove the fuzzing harness itself works, not to cover engine surface.
    // Windows.yml now runs them alongside every other declared target (see
    // BuildIntegrity.EveryRegisteredFuzzTargetRunsInCi below), so this set is
    // currently empty; it stays here as the extension point for a future
    // smoke-only target that should not gate CI.
    const std::set<std::string> excluded_from_ci;

    std::vector<std::string> missing_from_ci;
    for (const auto &target : declared_targets) {
        if (ci_set.contains(target) || excluded_from_ci.contains(target)) { continue; }
        missing_from_ci.push_back(target);
    }
    EXPECT_TRUE(missing_from_ci.empty())
      << missing_from_ci.size()
      << " fuzz target(s) declared in Test/fuzz/CMakeLists.txt are neither in Windows.yml's fuzz-seed foreach "
         "array nor in the smoke-target exclusion list, so they silently do not run in CI: "
      << joinViolations(missing_from_ci);

    std::vector<std::string> dead_ci_entries;
    for (const auto &target : ci_targets) {
        if (!declared_set.contains(target)) { dead_ci_entries.push_back(target); }
    }
    EXPECT_TRUE(dead_ci_entries.empty())
      << dead_ci_entries.size()
      << " entry/entries in Windows.yml's fuzz-seed foreach array do not correspond to any target declared "
         "in Test/fuzz/CMakeLists.txt (renamed or deleted?): "
      << joinViolations(dead_ci_entries);
}

// shader_file_reader_fuzz_test and texture_loading_fuzz_test used to run only
// on the opt-in Windows lane (see AGENTS.md "What CI runs": Linux.yml runs on
// every push, Windows.yml only on [build-win]), so a real engine-surface
// fuzzer could sit unexercised for weeks between opt-in runs, and nothing
// noticed when a new target was added to neither lane. This gates every
// target declared in Test/fuzz/CMakeLists.txt against both workflow files
// AND the local host runner (Invoke-ClangClDebug.ps1), which used to hard-code
// only the two FuzzTest smoke targets and silently skip the five with real
// engine-surface coverage.
TEST(BuildIntegrity, EveryRegisteredFuzzTargetRunsInCi)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const std::vector<std::string> declared_targets =
      parse_declared_fuzz_targets(repo_root / "Test" / "fuzz" / "CMakeLists.txt");
    ASSERT_FALSE(declared_targets.empty())
      << "parsed zero kataglyphis_add_fuzz_test(...) declarations out of Test/fuzz/CMakeLists.txt - the "
         "anchor text ('kataglyphis_add_fuzz_test(') may have changed";
    const std::set<std::string> declared_set(declared_targets.begin(), declared_targets.end());

    const fs::path linux_workflow_path = repo_root / ".github" / "workflows" / "Linux.yml";
    const fs::path windows_workflow_path = repo_root / ".github" / "workflows" / "Windows.yml";
    const fs::path local_runner_path = repo_root / "scripts" / "windows" / "Invoke-ClangClDebug.ps1";

    const auto linux_targets_opt = parse_linux_ci_fuzz_targets(linux_workflow_path);
    if (!linux_targets_opt.has_value()) {
        GTEST_SKIP() << "could not open " << linux_workflow_path.string() << " - not running from the repo root?";
    }
    const auto windows_targets_opt = parse_ci_fuzz_targets(windows_workflow_path);
    if (!windows_targets_opt.has_value()) {
        GTEST_SKIP() << "could not open " << windows_workflow_path.string() << " - not running from the repo root?";
    }
    const auto local_targets_opt = parse_local_runner_fuzz_targets(local_runner_path);
    if (!local_targets_opt.has_value()) {
        GTEST_SKIP() << "could not open " << local_runner_path.string() << " - not running from the repo root?";
    }
    ASSERT_FALSE(linux_targets_opt->empty())
      << "parsed zero fuzz targets out of the for-loop in " << linux_workflow_path.string()
      << " - the anchor text ('for t in ' / '; do') may have changed";
    ASSERT_FALSE(windows_targets_opt->empty())
      << "parsed zero fuzz targets out of the foreach array in " << windows_workflow_path.string()
      << R"( - the anchor text ('foreach (`$t in @(' / '))') may have changed)";
    ASSERT_FALSE(local_targets_opt->empty())
      << "parsed zero fuzz targets out of the foreach array in " << local_runner_path.string()
      << R"( - the anchor text ('foreach ($fuzzExecutable in @(' / '))') may have changed)";

    const std::set<std::string> linux_set(linux_targets_opt->begin(), linux_targets_opt->end());
    const std::set<std::string> windows_set(windows_targets_opt->begin(), windows_targets_opt->end());
    const std::set<std::string> local_set(local_targets_opt->begin(), local_targets_opt->end());

    // A target may be absent from a lane only if it is listed here, with a
    // reason - e.g. a linked-VulkanEngineCore target crashing at static init
    // on the Linux lane the way scene_config_fuzz_test's comment above
    // describes. Empty means every declared target runs in every lane.
    struct NotRunInCi
    {
        std::string target;
        std::string lane;// "Linux.yml", "Windows.yml", or "Invoke-ClangClDebug.ps1"
        std::string reason;
    };
    const std::vector<NotRunInCi> kNotRunInCi;

    std::vector<std::string> missing;
    auto check_lane = [&](const std::string &lane_name, const std::set<std::string> &lane_set) {
        for (const auto &target : declared_targets) {
            if (lane_set.contains(target)) { continue; }
            const bool excused = std::any_of(kNotRunInCi.begin(), kNotRunInCi.end(), [&](const NotRunInCi &entry) {
                return entry.target == target && entry.lane == lane_name;
            });
            if (excused) { continue; }
            missing.push_back(target + " missing from " + lane_name);
        }
    };
    check_lane("Linux.yml", linux_set);
    check_lane("Windows.yml", windows_set);
    check_lane("Invoke-ClangClDebug.ps1", local_set);

    EXPECT_TRUE(missing.empty())
      << missing.size()
      << " fuzz target(s) declared in Test/fuzz/CMakeLists.txt do not run in every CI lane and are not listed "
         "in kNotRunInCi with a reason: "
      << joinViolations(missing);

    // Unlike the two workflow files (whose foreach arrays are cross-checked
    // against declared_targets in BuildIntegrity.EveryFuzzTargetIsInTheWindowsCiFuzzList
    // and mirrored by construction for Linux.yml), the local runner has no
    // other test catching a stale/renamed entry, so check both directions here.
    std::vector<std::string> dead_local_entries;
    for (const auto &target : *local_targets_opt) {
        if (!declared_set.contains(target)) { dead_local_entries.push_back(target); }
    }
    EXPECT_TRUE(dead_local_entries.empty())
      << dead_local_entries.size()
      << " entry/entries in Invoke-ClangClDebug.ps1's local fuzz-run foreach array do not correspond to any "
         "target declared in Test/fuzz/CMakeLists.txt (renamed or deleted?): "
      << joinViolations(dead_local_entries);
}

// A source-shape gate, not a behavioural one: it does not exercise a runner
// end-to-end (that would need a stub executable and a fake build tree), only
// that each Invoke-ClangCl*.ps1 helper contains a top-level `exit` statement
// whose argument is a variable. Invoke-ClangClDebug.ps1 used to launch the app
// without ever propagating its exit code - the launch happened inside an
// Invoke-WithAsanOptions scriptblock, and a non-zero result was downgraded to
// a warning that never escaped - so a device-lost or fatal-submit run read as
// a clean quit. This stops that shape from silently coming back.
TEST(BuildIntegrity, EveryHostRunnerPropagatesTheApplicationExitCode)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path scripts_dir = repo_root / "scripts" / "windows";
    ASSERT_TRUE(fs::exists(scripts_dir)) << "could not locate " << scripts_dir.string();

    std::vector<fs::path> runner_scripts;
    for (const auto &entry : fs::directory_iterator(scripts_dir)) {
        if (!entry.is_regular_file()) { continue; }
        const fs::path &candidate = entry.path();
        if (candidate.extension() == ".ps1" && candidate.filename().string().rfind("Invoke-ClangCl", 0) == 0) {
            runner_scripts.push_back(candidate);
        }
    }
    ASSERT_FALSE(runner_scripts.empty())
      << "found zero Invoke-ClangCl*.ps1 helpers under " << scripts_dir.string()
      << " - the naming convention may have changed";

    static const std::regex kExitVariableLine(R"(^\s*exit\s+\$[A-Za-z_][A-Za-z0-9_:]*\s*$)");

    std::vector<std::string> missing_exit;
    for (const auto &script_path : runner_scripts) {
        const auto lines = readFileLines(script_path);
        if (!lines) {
            missing_exit.push_back(script_path.filename().string() + " (could not open)");
            continue;
        }

        bool found = false;
        for (const auto &line : *lines) {
            if (std::regex_match(line, kExitVariableLine)) {
                found = true;
                break;
            }
        }
        if (!found) { missing_exit.push_back(script_path.filename().string()); }
    }

    EXPECT_TRUE(missing_exit.empty())
      << missing_exit.size()
      << " Invoke-ClangCl*.ps1 helper(s) have no top-level 'exit $<variable>' line, so a failing launch inside "
         "them can silently report success to the caller: "
      << joinViolations(missing_exit);
}

// Test/perf/perfSuite.cpp and Test/perf/baselines/win-9070xt-32core.json are
// two independently hand-maintained lists of the same benchmark set, exactly
// like the fuzz-target check above. Compare-PerfBaseline.ps1 deliberately
// never fails on a one-sided entry (see its header - the suite grows over
// time and that alone should not fail a comparison), which means a benchmark
// added to perfSuite.cpp without a matching baseline row is silently never
// compared. This test closes that hole at the source instead of in the
// comparator.
TEST(BuildIntegrity, PerfBaselineCoversEveryRegisteredBenchmark)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const std::vector<std::string> declared_benchmarks =
      parse_declared_perf_benchmarks(repo_root / "Test" / "perf" / "perfSuite.cpp");
    ASSERT_FALSE(declared_benchmarks.empty())
      << "parsed zero BENCHMARK(...) declarations out of Test/perf/perfSuite.cpp - the anchor text "
         "('BENCHMARK(') may have changed";
    const std::set<std::string> declared_set(declared_benchmarks.begin(), declared_benchmarks.end());

    const fs::path baseline_path = repo_root / "Test" / "perf" / "baselines" / "win-9070xt-32core.json";
    const auto baseline_names_opt = parse_perf_baseline_names(baseline_path);
    ASSERT_TRUE(baseline_names_opt.has_value())
      << "could not parse " << baseline_path.string() << " as Google Benchmark JSON";
    const std::vector<std::string> &baseline_names = *baseline_names_opt;
    ASSERT_FALSE(baseline_names.empty())
      << "parsed zero benchmarks[] rows out of " << baseline_path.string() << " - is the file empty or malformed?";
    const std::set<std::string> baseline_set(baseline_names.begin(), baseline_names.end());

    static const char *const kRefreshHint =
      "see Compare-PerfBaseline.ps1's header for the refresh procedure (run the suite, eyeball the result, copy "
      "the JSON over by hand - there is deliberately no capture mode)";

    std::vector<std::string> missing_from_baseline;
    for (const auto &name : declared_benchmarks) {
        if (!baseline_set.contains(name)) { missing_from_baseline.push_back(name); }
    }
    EXPECT_TRUE(missing_from_baseline.empty())
      << missing_from_baseline.size()
      << " benchmark(s) registered in Test/perf/perfSuite.cpp have no row in " << baseline_path.string()
      << ", so Compare-PerfBaseline.ps1 silently never compares them (" << kRefreshHint << "): "
      << joinViolations(missing_from_baseline);

    std::vector<std::string> dead_baseline_rows;
    for (const auto &name : baseline_names) {
        if (!declared_set.contains(name)) { dead_baseline_rows.push_back(name); }
    }
    EXPECT_TRUE(dead_baseline_rows.empty())
      << dead_baseline_rows.size() << " row(s) in " << baseline_path.string()
      << " do not correspond to any BENCHMARK(...) currently registered in Test/perf/perfSuite.cpp (renamed or "
         "removed? "
      << kRefreshHint << "): "
      << joinViolations(dead_baseline_rows);
}

// SlangWgslPatchTablesAgree and SlangCompileManifestsAgree lived here until
// 2026-08-02. Both existed only to pin two hand-maintained copies of the same
// data against each other - the manifest and the depth-texture patch table,
// duplicated across Build-SlangShaders.ps1 and .sh. Both copies are gone:
// Resources/ShadersSlang/shader-manifest.json is now the single source both
// scripts read, so there is no second table left to disagree with. The
// remaining tests below verify the FILESYSTEM against that manifest, which is
// the check that still has teeth.

// CompiledShadersAreNotOlderThanTheirSources guards the SPIR-V artifacts; the
// checked-in Rust-crate WGSL artifacts (the manifest's wgslMap) have no
// equivalent guard, and they live two directories away
// (third_party/OxidANT/crates/.../shaders) from the
// Slang source that generates them. A regenerate that drops one of the
// depth-texture patches, or a .slang edit that never gets propagated, is
// silent today. This walks the wgslMap and asserts each checked-in .wgsl is
// not older than its source OR the newest import in that source's real
// import_closure (see import_closure/newest_import_for above) - the same
// pair of checks CompiledShadersAreNotOlderThanTheirSources /
// …ThanSharedIncludes apply to the SPIR-V side, now scoped to imports the
// source actually reaches rather than "newest file anywhere under common/".
// mtimes are meaningless after a fresh clone (git does not preserve them), so
// this is a local-iteration guard that catches "I edited a .slang and forgot
// to regenerate" before you commit - not the CI backstop; that is the
// content-based freshness gate (task 3 below). What this still does NOT
// catch: a shader that genuinely imports the edited module but whose emitted
// WGSL is byte-identical is never re-copied by the compile scripts, so its
// mtime stays behind and this gate keeps reporting it stale on every run.
// The real fix is a content stamp written by the compile scripts (which live
// upstream in ANTfrastructure); out of scope here.
TEST(BuildIntegrity, CheckedInWgslIsNotOlderThanItsSlangSource)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();

    const auto &manifest = shader_manifest(repo_root);
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    std::vector<std::string> stale;
    int checked = 0;
    for (const auto &mapping : manifest->wgsl_map) {
        const fs::path source = slang_root / mapping.slang_source;
        ASSERT_TRUE(fs::exists(source))
          << "Slang source mapped by the manifest's wgslMap is missing: " << source.string();

        const fs::path dest = repo_root / mapping.dst_dir / mapping.wgsl_file;
        if (!fs::exists(dest)) { continue; }// OxidANT submodule not checked out here

        std::error_code error;
        const auto source_time = fs::last_write_time(source, error);
        if (error) { continue; }
        const auto dest_time = fs::last_write_time(dest, error);
        if (error) { continue; }

        fs::file_time_type newest_import{};
        fs::path newest_import_path;
        const bool has_shared_import = newest_import_for(slang_root, source, newest_import, newest_import_path);
        const bool import_is_newer = has_shared_import && newest_import > source_time;
        const auto newest_time = import_is_newer ? newest_import : source_time;

        ++checked;
        if (dest_time < newest_time) {
            stale.push_back(fs::relative(dest, repo_root).string() + " (mtime ticks=" + std::to_string(dest_time.time_since_epoch().count())
                             + ") is older than "
                             + (import_is_newer ? fs::relative(newest_import_path, repo_root).string()
                                                 : fs::relative(source, repo_root).string())
                             + " (mtime ticks=" + std::to_string(newest_time.time_since_epoch().count()) + ')');
        }
    }

    if (checked == 0) {
        GTEST_SKIP() << "none of the checked-in Rust-crate WGSL destinations exist - the "
                        "OxidANT submodule is likely not checked out here";
    }

    EXPECT_TRUE(stale.empty()) << stale.size()
                               << " checked-in Rust-crate WGSL file(s) are older than the Slang source (or a "
                                  "shared common/ import) that generates them (regenerate via "
                                  "Build-SlangShaders.ps1/.sh): "
                               << joinViolations(stale);
}

// WGSL has no string literals, so a "//" can only ever appear as the start of
// a comment - the Slang WGSL backend itself emits none. A "//" in a checked-in
// destination from the manifest's wgslMap is therefore always a hand-edit made
// directly on the generated file, with a regenerate's expiry date on it: the
// next compile-slang-shaders run silently drops it. Catch it here instead.
TEST(BuildIntegrity, CheckedInWgslHasNoHandEdits)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const auto &manifest = shader_manifest(repo_root);
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    std::vector<std::string> hand_edits;
    int checked = 0;
    for (const auto &mapping : manifest->wgsl_map) {
        const fs::path dest = repo_root / mapping.dst_dir / mapping.wgsl_file;
        if (!fs::exists(dest)) { continue; }// OxidANT submodule not checked out here
        ++checked;

        const auto lines = readFileLines(dest);
        if (!lines) { continue; }

        int line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find("//") != std::string::npos) {
                hand_edits.push_back(fs::relative(dest, repo_root).string() + ':' + std::to_string(line_number)
                                      + ": " + line);
            }
        }
    }

    if (checked == 0) {
        GTEST_SKIP() << "none of the checked-in Rust-crate WGSL destinations exist - the "
                        "OxidANT submodule is likely not checked out here";
    }

    EXPECT_TRUE(hand_edits.empty())
      << hand_edits.size()
      << " line(s) with '//' found in checked-in generated WGSL - generated WGSL must not be hand-edited - put "
         "the change in the .slang source, or in the post-emit patch table in "
         "Build-SlangShaders.ps1/.sh: "
      << joinViolations(hand_edits);
}

// WGSL requires every non-builtin member of an inter-stage (varying) struct to
// carry @location(N); only @builtin members may omit it. slangc
// 2026.1-52-gc8ddf20bb (Vulkan SDK 1.4.341.1 - the ANTfrastructure Linux image)
// drops @location from varying structs in the COMBINED emit (compiled without
// -entry/-stage, which is exactly how the manifest's wgslMap files are
// produced) while emitting it correctly per entry point from the SAME binary;
// slangc 2026.8 is correct on both Windows and Linux and reproduces these
// files byte-for-byte. A regeneration on the older toolchain therefore turned
// `@location(0) uv_0 : vec2<f32>` into a bare `uv_0 : vec2<f32>` in eight of
// the ten checked-in files - WGSL naga rejects, committed silently because
// nothing looked at the emit. The compile scripts now skip the emit below the
// manifest's minSlangcVersionForWgsl and hard-fail on a violation above it;
// this is the backstop that runs in CI on every platform and cannot be
// bypassed by regenerating with a different tool.
//
// A struct with at least one @builtin/@location member is an IO struct, so
// every member of it must carry one of those attributes. Structs with no such
// member (uniform/storage layouts, which use @align instead) are not IO and
// are skipped.
TEST(BuildIntegrity, CheckedInWgslVaryingStructsCarryLocations)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const auto &manifest = shader_manifest(repo_root);
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    static const std::regex kStructHead(R"(^struct\s+([A-Za-z_]\w*))");
    static const std::regex kMember(R"(^\s*((?:@\w+\([^)]*\)\s*)*)([A-Za-z_]\w*)\s*:\s*\S.*?,?\s*$)");
    const auto is_io_attr = [](const std::string &attrs) {
        return attrs.find("@builtin(") != std::string::npos || attrs.find("@location(") != std::string::npos;
    };

    std::vector<std::string> violations;
    int checked = 0;
    for (const auto &mapping : manifest->wgsl_map) {
        const fs::path dest = repo_root / mapping.dst_dir / mapping.wgsl_file;
        if (!fs::exists(dest)) { continue; }// OxidANT submodule not checked out here
        ++checked;

        const auto lines_opt = readFileLines(dest);
        if (!lines_opt) { continue; }
        const auto &lines = *lines_opt;

        std::size_t index = 0;
        while (index < lines.size()) {
            std::smatch head;
            const std::string struct_line = lines[index];
            ++index;
            if (!std::regex_search(struct_line, head, kStructHead)) { continue; }
            const std::string struct_name = head[1].str();

            if (index < lines.size() && lines[index].find_first_not_of(" \t") != std::string::npos
                && lines[index].substr(lines[index].find_first_not_of(" \t")) == "{") {
                ++index;
            }

            // (1-based line number, attribute prefix, raw text) per member.
            std::vector<std::tuple<std::size_t, std::string, std::string>> members;
            while (index < lines.size()) {
                const std::size_t first = lines[index].find_first_not_of(" \t");
                if (first != std::string::npos && lines[index][first] == '}') { break; }
                std::smatch member;
                if (std::regex_match(lines[index], member, kMember)) {
                    members.emplace_back(index + 1, member[1].str(), lines[index]);
                }
                ++index;
            }

            const bool is_io_struct = std::any_of(members.begin(), members.end(), [&](const auto &member) {
                return is_io_attr(std::get<1>(member));
            });
            if (!is_io_struct) { continue; }

            for (const auto &[line_number, attrs, text] : members) {
                if (is_io_attr(attrs)) { continue; }
                violations.push_back(fs::relative(dest, repo_root).generic_string() + ':'
                                      + std::to_string(line_number) + ": struct " + struct_name + ": " + text);
            }
        }
    }

    if (checked == 0) {
        GTEST_SKIP() << "none of the checked-in Rust-crate WGSL destinations exist - the "
                        "OxidANT submodule is likely not checked out here";
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " member(s) of an inter-stage WGSL struct carry neither @builtin nor @location - naga rejects that. "
         "This is the signature of a regeneration with slangc older than the manifest's "
         "minSlangcVersionForWgsl; regenerate with a newer slangc rather than hand-editing: "
      << joinViolations(violations);
}

// The floor above is only enforced if it is actually in the manifest: both
// compile scripts treat a missing minSlangcVersionForWgsl as "no floor" (they
// must, so an older manifest still builds), which would silently re-enable the
// broken combined emit on the container's slangc. Pin its presence and shape
// here instead - the scripts compare the leading MAJOR.MINOR only.
TEST(BuildIntegrity, ShaderManifestPinsAMinimumSlangcVersionForWgsl)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const auto &manifest = shader_manifest(repo_root);
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    EXPECT_FALSE(manifest->min_slangc_version_for_wgsl.empty())
      << "shader-manifest.json has no \"minSlangcVersionForWgsl\" - without it both compile scripts stop "
         "skipping the combined WGSL emit on toolchains whose emit drops varying @location attributes";
    EXPECT_TRUE(std::regex_search(manifest->min_slangc_version_for_wgsl, std::regex(R"(^\d+\.\d+)")))
      << "\"minSlangcVersionForWgsl\" (" << manifest->min_slangc_version_for_wgsl
      << ") must start with MAJOR.MINOR - the compile scripts compare only that prefix and treat anything "
         "unparseable as new enough";
}

// `Resources/Shaders/` (the pre-Slang GLSL tree) was deleted once the Slang
// migration finished; every .slang file is now the sole source for its
// shader, and no .glsl/.vert/.frag/.geom/.tesc/.tese/.comp/.rgen/.rchit/.rmiss
// file exists anywhere in the tree any more. A handful of comments still
// said "Mirrors Resources/Shaders/..." or named a bare GLSL-era shader-stage
// file for months afterward, pointing a reader at something that no longer
// exists instead of at the file they were already reading - the same
// failure mode as trusting stale SPIR-V above: a comment claiming the
// authoritative version lives elsewhere. This walks every .slang file under
// Resources/ShadersSlang/ (excluding the build/ output directory) plus every
// comment in a .cpp/.hpp/.ixx file under Src/GraphicsEngineVulkan/ and
// Src/shared/, and fails naming any file plus line that either:
//   (a) mentions the deleted Resources/Shaders path,
//   (b) mentions a GLSL-era shader-stage extension - none of these can
//       exist in the tree any more, so any occurrence is stale, or
//   (c) names a *.slang file, by basename, for which no file with that
//       basename exists under Resources/ShadersSlang/.
TEST(BuildIntegrity, SourceCommentsDoNotReferenceDeletedShaderFiles)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    std::set<std::string> real_slang_basenames;
    std::error_code error;
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".slang") { continue; }
        if (fs::relative(path, slang_root).generic_string().starts_with("build/")) { continue; }
        real_slang_basenames.insert(path.filename().string());
    }
    ASSERT_FALSE(real_slang_basenames.empty()) << "found zero .slang files under " << slang_root.string();

    // Trailing slash matters: "Resources/Shaders" alone is a substring of
    // the current "Resources/ShadersSlang" tree, which every file under
    // Resources/ShadersSlang legitimately mentions in path comments.
    static const std::string kDeadPath = "Resources/Shaders/";
    // Not followed by ".slang": raytrace.rchit.slang legitimately contains
    // ".rchit" as a mid-name segment - only a *trailing* GLSL-era extension
    // (nothing left to exist as a real file) is dead.
    static const std::regex kDeadExtension(R"(\.(glsl|frag|vert|geom|tesc|tese|comp|rgen|rchit|rmiss)(?!\.slang)\b)");
    static const std::regex kSlangMention(R"([A-Za-z0-9_./-]+\.slang)");

    std::vector<std::string> violations;

    auto record = [&](const fs::path &path, int line_number, const std::string &line, const std::string &reason) {
        violations.push_back(fs::relative(path, repo_root).generic_string() + ':' + std::to_string(line_number)
                              + ": " + reason + ": " + line);
    };

    auto scan_line = [&](const fs::path &path, int line_number, const std::string &line, const std::string &text) {
        if (text.find(kDeadPath) != std::string::npos) {
            record(path, line_number, line, "references the deleted " + kDeadPath + " tree");
        }
        if (std::regex_search(text, kDeadExtension)) {
            record(path, line_number, line, "references a GLSL-era shader-stage extension that no longer exists");
        }
        for (auto match = std::sregex_iterator(text.begin(), text.end(), kSlangMention), match_end = std::sregex_iterator();
             match != match_end; ++match) {
            const std::string basename = fs::path(match->str()).filename().string();
            if (!real_slang_basenames.contains(basename)) {
                record(path, line_number, line,
                       "names '" + basename + "', which does not exist under Resources/ShadersSlang/");
            }
        }
    };

    // .slang files: scan every line - shader source has no string-literal
    // filenames that would collide with these patterns.
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".slang") { continue; }
        if (fs::relative(path, slang_root).generic_string().starts_with("build/")) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        int line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            scan_line(path, line_number, line, line);
        }
    }

    // C++ sources: comment-only, so a live string literal such as
    // Raytracing.cpp's "raytrace.rgen.rgen_main.spv" is never scanned.
    for (const char *sub_dir : { "GraphicsEngineVulkan", "shared" }) {
        const fs::path root = repo_root / "Src" / sub_dir;
        if (!fs::exists(root)) { continue; }
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

// The deferred geometry pass used to invent its own "0.1 alpha cutoff"
// fallback for OPAQUE materials instead of sharing material_rules.slang's
// alpha_masked_out() with the forward rasterizer and shadow map shaders -
// see ObjMaterial.hpp's alphaCutoff contract (negative means never discard).
// This pins all three raster shaders to the one shared predicate so the
// drift cannot silently come back.
TEST(BuildIntegrity, RasterShadersShareOneAlphaCutoffRule)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::array<const char *, 3> kShaders = {
        "Resources/ShadersSlang/deferred/deferred.slang",
        "Resources/ShadersSlang/rasterizer/rasterizer.slang",
        "Resources/ShadersSlang/rasterizer/shadows/shadow_map.slang",
    };
    static const std::string kSharedPredicate = "alpha_masked_out(";
    static const std::string kBannedFallback = "alphaCutoff >= 0.0) ?";
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
        if (text.find(kBannedFallback) != std::string::npos) {
            violations.push_back(std::string(relative_path) + ": still contains the hand-rolled '"
                                  + kBannedFallback + "' alpha-cutoff fallback");
        }

        // A MASK material with no base-colour texture must alpha-test its
        // factor alone (glTF's texture term defaults to 1) - so the shader
        // must call alpha_masked_out() on both sides of its "textureID >= 0"
        // branch, not just the textured one.
        std::size_t occurrences = 0;
        for (std::size_t pos = text.find(kSharedPredicate); pos != std::string::npos;
             pos = text.find(kSharedPredicate, pos + kSharedPredicate.size())) {
            ++occurrences;
        }
        if (text.find(kTextureGuard) != std::string::npos && occurrences < 2) {
            violations.push_back(std::string(relative_path)
                                  + ": calls " + kSharedPredicate + " only once - the untextured side of its '"
                                  + kTextureGuard + "' branch must alpha-test the factor too");
        }
    }

    // The MASK test's alpha is baseColorFactor.a * baseColorTexture.a - if the
    // shared predicate stops multiplying by material.dissolve, every caller
    // above silently loses the factor half of that product again.
    {
        const fs::path path = repo_root / "Resources/ShadersSlang/common/material_rules.slang";
        const auto textOpt = readFileText(path);
        ASSERT_TRUE(textOpt.has_value()) << "missing " << path.string();
        const std::string &text = *textOpt;

        const std::size_t fn_start = text.find("bool alpha_masked_out(");
        ASSERT_NE(fn_start, std::string::npos) << "alpha_masked_out() definition not found in " << path.string();
        // The function body is a handful of lines; a fixed window comfortably
        // covers it without needing to brace-match the closing '}'.
        const std::string body = text.substr(fn_start, 400);
        if (body.find("material.dissolve") == std::string::npos) {
            violations.push_back(
              "material_rules.slang: alpha_masked_out() no longer multiplies by material.dissolve");
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " raster shader(s) do not share the single alpha_masked_out() MASK rule:"
      << joinViolations(violations);
}

// Batch XVIII moved the guard-plus-default-plus-combine wrapper around the
// emissive/metallic-roughness/normal-map texture slots into
// common/material_textures.slang's resolved_emission[_lod0](),
// resolved_metallic_roughness[_lod0]() and resolved_normal[_lod0](), the
// same way batch X moved the raw sample_*() calls themselves. This pins the
// raw sample_emissive/sample_normal/sample_metallic_roughness (and their
// _lod0 twins) calls down to that one owning module, so a future shading
// path cannot silently reintroduce a hand-rolled wrapper around them.
TEST(BuildIntegrity, TextureSlotWrappersHaveOneOwner)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    static const std::array<const char *, 6> kRawSamplers = {
        "sample_emissive_lod0(", "sample_emissive(",
        "sample_normal_lod0(", "sample_normal(",
        "sample_metallic_roughness_lod0(", "sample_metallic_roughness("
    };
    static const std::string kOwningFile = "common/material_textures.slang";

    // path_tracing.slang's Lambertian-only kernel deliberately keeps its
    // metallic-roughness block inline (see resolved_metallic_roughness_lod0's
    // doc comment in material_textures.slang) rather than adopting
    // resolved_metallic_roughness_lod0(): that helper's untextured-material
    // fallback would silently start applying material.metallic to a kernel
    // whose glTF default is metallicFactor == 1.0.
    static const std::string kSanctionedExceptionFile = "path_tracing/path_tracing.slang";
    static const std::string kSanctionedExceptionCall = "sample_metallic_roughness_lod0(";

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".slang") { continue; }

        const std::string relative_path = fs::relative(path, slang_root).generic_string();
        if (relative_path.starts_with("build/")) { continue; }
        if (relative_path == kOwningFile) { continue; }

        const auto textOpt = readFileText(path);
        ASSERT_TRUE(textOpt.has_value()) << "could not open " << relative_path;
        const std::string &text = *textOpt;

        for (const char *raw_sampler : kRawSamplers) {
            if (text.find(raw_sampler) == std::string::npos) { continue; }
            if (relative_path == kSanctionedExceptionFile && std::string(raw_sampler) == kSanctionedExceptionCall) {
                continue;
            }
            violations.push_back(relative_path + " calls " + raw_sampler
                                  + " directly - only " + kOwningFile
                                  + " (and path_tracing.slang's sanctioned sample_metallic_roughness_lod0 "
                                    "exception) may call the raw sampler; every other shading path must go "
                                    "through resolved_emission/resolved_metallic_roughness/resolved_normal "
                                    "(and their _lod0 twins) instead");
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " raw texture-slot sampler call(s) found outside their owning module:"
      << joinViolations(violations);
}

// common/material_rules.slang exists so the ray tracing / path tracing entry
// points - which cannot import material_fetch.slang without an
// ambiguous-reference compile error over its objectDescription binding -
// can still call the three pure material rules. A `[vk::binding` sneaking
// back into this module, or either function reappearing in
// material_fetch.slang, would silently reintroduce that conflict for the
// next shading path that tries to import it.
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
    EXPECT_NE(material_rules_text.find("float material_roughness("), std::string::npos)
      << "material_rules.slang no longer defines material_roughness()";
    EXPECT_NE(material_rules_text.find("float2 material_metallic_roughness("), std::string::npos)
      << "material_rules.slang no longer defines material_metallic_roughness()";
    EXPECT_NE(material_rules_text.find("bool alpha_masked_out("), std::string::npos)
      << "material_rules.slang no longer defines alpha_masked_out()";

    const fs::path material_fetch_path = repo_root / "Resources/ShadersSlang/common/material_fetch.slang";
    const auto material_fetch_text_opt = readFileText(material_fetch_path);
    ASSERT_TRUE(material_fetch_text_opt.has_value()) << "missing " << material_fetch_path.string();
    const std::string &material_fetch_text = *material_fetch_text_opt;

    EXPECT_EQ(material_fetch_text.find("float material_roughness("), std::string::npos)
      << "material_fetch.slang re-defines material_roughness() - it should live only in material_rules.slang";
    EXPECT_EQ(material_fetch_text.find("float2 material_metallic_roughness("), std::string::npos)
      << "material_fetch.slang re-defines material_metallic_roughness() - it should live only in "
         "material_rules.slang";
    EXPECT_EQ(material_fetch_text.find("bool alpha_masked_out("), std::string::npos)
      << "material_fetch.slang re-defines alpha_masked_out() - it should live only in material_rules.slang";
}

// path_tracing.slang's ray queries used to trace with RAY_FLAG_FORCE_OPAQUE,
// and a ray query has no any-hit stage, so a glTF MASK cut-out committed as
// a solid quad and cast a solid shadow - the last of five shading paths with
// that bug. Both of path_tracing.slang's queries (the bounce ray and the NEE
// shadow ray) must instead alpha-test candidates via alpha_test.slang's
// shared ray_hit_masked_out(), which raytrace.rahit.slang's any-hit shader
// also calls. This pins that fix so RAY_FLAG_FORCE_OPAQUE cannot silently
// come back to either ray query.
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
        if (text.find("ray_hit_masked_out(") == std::string::npos) {
            violations.push_back("path_tracing.slang: does not call the shared ray_hit_masked_out() alpha test");
        }
        if (text.find("CommitNonOpaqueTriangleHit()") == std::string::npos) {
            violations.push_back(
              "path_tracing.slang: does not call CommitNonOpaqueTriangleHit() - without it, no candidate "
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

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " shading path(s) do not alpha-test MASK materials in their ray queries:"
      << joinViolations(violations);
}

// glTF base colour = baseColorFactor * sampled texture (see
// material_fetch.slang's base_color() helper and GltfLoader.cpp's
// fromGltfMaterial, which keeps the factor in ObjMaterial::diffuse even when
// a texture is also present). Each of these four shaders has a
// "textureID >= 0" branch that samples the base-colour texture; this scans
// that branch and fails if it samples a texture without routing the result
// through base_color(, which would silently drop the material's factor
// again the way forward.slang's reference path never did.
TEST(BuildIntegrity, EveryBaseColourSampleIsScaledByTheMaterialFactor)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::array<const char *, 4> kShaders = {
        "Resources/ShadersSlang/rasterizer/rasterizer.slang",
        "Resources/ShadersSlang/deferred/deferred.slang",
        "Resources/ShadersSlang/raytracing/raytrace.rchit.slang",
        "Resources/ShadersSlang/path_tracing/path_tracing.slang",
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
            violations.push_back(std::string(relative_path)
                                  + ": samples the base-colour texture without routing it through base_color(");
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size() << " shader(s) do not scale their sampled base colour by the material factor:"
      << joinViolations(violations);
}

// f0 = mix(0.04, albedo, metallic) is brdf.slang's contract for reflectance
// at normal incidence (see brdf_direct's doc comment). A shading path that
// hard-codes the third argument to a 0.0 literal instead of the material's
// metallic value renders every metal as a dielectric - this was true of
// rasterizer.slang, deferred.slang and raytrace.rchit.slang until
// ObjMaterial grew a metallic field. Scans every .slang file (excluding
// build/) for a `lerp(float3(0.04), <expr>, 0.0)` call and fails if the
// third argument is still the 0.0 literal rather than a variable.
TEST(BuildIntegrity, NoShadingPathPinsMetallicToZero)
{
    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    static const std::regex kPinnedToZero(R"(lerp\(float3\(0\.04\)\s*,[^,]+,\s*0\.0\s*\))");

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".slang") { continue; }
        if (fs::relative(path, slang_root).generic_string().starts_with("build/")) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        int line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (std::regex_search(line, kPinnedToZero)) {
                violations.push_back(fs::relative(path, slang_root).generic_string() + ':'
                                      + std::to_string(line_number) + ": " + line);
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " shading path(s) pin f0's metallic mix to a 0.0 literal instead of the material's metallic value:"
      << joinViolations(violations);
}

// KHR_texture_transform must apply to every base-colour sample, not just
// four of the five shading paths - the path-tracing ray-query kernel and the
// RT closest-hit shader used to sample raw UVs while the three raster paths
// (forward, deferred, shadow) already routed through transform_uv(), so a
// model using the extension rendered inconsistently depending on which mode
// was active. Scans each shading source as text and requires that every line
// sampling `textures[...]` via `textureSamplers[...]` also calls transform_uv(
// or one of its per-slot named accessors (normal_uv(/metallic_roughness_uv(/
// emissive_uv(, see common/base_color.slang) on the same line. The any-hit
// alpha test's sample site lives in common/alpha_test.slang
// (raytrace.rahit.slang and path_tracing.slang's ray-query candidate loop
// both call it), not in raytrace.rahit.slang itself.
TEST(BuildIntegrity, EveryBaseColourSampleAppliesTheUvTransform)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::array<const char *, 6> kShaders = {
        "Resources/ShadersSlang/rasterizer/rasterizer.slang",
        "Resources/ShadersSlang/deferred/deferred.slang",
        "Resources/ShadersSlang/rasterizer/shadows/shadow_map.slang",
        "Resources/ShadersSlang/raytracing/raytrace.rchit.slang",
        "Resources/ShadersSlang/common/alpha_test.slang",
        "Resources/ShadersSlang/path_tracing/path_tracing.slang",
    };
    static const std::string kTextures = "textures[";
    static const std::string kSamplers = "textureSamplers[";
    static const std::array<const char *, 4> kTransformCalls = {
        "transform_uv(",
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
                                                        [&line](const char *call) {
                                                            return line.find(call) != std::string::npos;
                                                        });
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

    EXPECT_TRUE(violations.empty())
      << violations.size() << " base-colour sample site(s) skip the KHR_texture_transform UV matrix:"
      << joinViolations(violations);
}

// ObjMaterial::emission is parsed by both loaders and uploaded to the GPU,
// but until this gate nothing in the shading paths actually read it - every
// glTF/OBJ emitter rendered black in both the forward and deferred paths
// while the Rust twin lit them. This scans the shader sources as text (the
// source-text gate pattern used throughout this file) and fails if any of
// the four shading paths (rasterizer, deferred, ray tracing, path tracing)
// stops consuming material.emission.
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

    // Batch XVIII moved the guard-plus-default-plus-combine wrapper around
    // material_emission() into common/material_textures.slang's
    // resolved_emission()/resolved_emission_lod0() - each of the four
    // shading paths below now calls one of those instead of material_emission()
    // directly (see EmissionSamplingUsesTheSharedHelperInEveryShadingPath for
    // the check that material_textures.slang itself still wires
    // emissiveTextureID through material_emission()).
    const fs::path rasterizer_path = repo_root / "Resources/ShadersSlang/rasterizer/rasterizer.slang";
    const auto rasterizer_text_opt = readFileText(rasterizer_path);
    ASSERT_TRUE(rasterizer_text_opt.has_value()) << "could not open " << rasterizer_path.string();
    const std::string &rasterizer_text = *rasterizer_text_opt;
    EXPECT_NE(rasterizer_text.find("color += resolved_emission(obj, material, In.texCoords)"), std::string::npos)
      << "rasterizer.slang no longer uses resolved_emission(). " << kFailureMessage;

    const fs::path deferred_path = repo_root / "Resources/ShadersSlang/deferred/deferred.slang";
    const auto deferred_text_opt = readFileText(deferred_path);
    ASSERT_TRUE(deferred_text_opt.has_value()) << "could not open " << deferred_path.string();
    const std::string &deferred_text = *deferred_text_opt;
    EXPECT_NE(
      deferred_text.find("outMaterial = float4(roughness, resolved_emission(obj, material, In.texCoords))"),
      std::string::npos)
      << "deferred.slang's geometry pass no longer packs resolved_emission() into outMaterial.gba. " << kFailureMessage;
    EXPECT_NE(deferred_text.find("color += material.gba"), std::string::npos)
      << "deferred.slang's lighting pass no longer adds the G-buffer's packed emissive term. " << kFailureMessage;

    const fs::path rchit_path = repo_root / "Resources/ShadersSlang/raytracing/raytrace.rchit.slang";
    const auto rchit_text_opt = readFileText(rchit_path);
    ASSERT_TRUE(rchit_text_opt.has_value()) << "could not open " << rchit_path.string();
    const std::string &rchit_text = *rchit_text_opt;
    EXPECT_NE(rchit_text.find("resolved_emission_lod0(obj, material, texCoords)"), std::string::npos)
      << "raytrace.rchit.slang no longer uses resolved_emission_lod0(). " << kFailureMessage;

    const fs::path path_tracing_path = repo_root / "Resources/ShadersSlang/path_tracing/path_tracing.slang";
    const auto path_tracing_text_opt = readFileText(path_tracing_path);
    ASSERT_TRUE(path_tracing_text_opt.has_value()) << "could not open " << path_tracing_path.string();
    const std::string &path_tracing_text = *path_tracing_text_opt;
    EXPECT_NE(path_tracing_text.find("radiance += throughput * resolved_emission_lod0(obj, material, texCoords)"),
              std::string::npos)
      << "path_tracing.slang no longer adds throughput * resolved_emission_lod0() at the hit. " << kFailureMessage;
}

// ObjMaterial::emissiveTextureID (glTF emissiveTexture, see EmissiveIsConsumedByEveryShadingPath above for the
// factor-only case) is dedup'd into the same texture-slot budget as textureID, but only actually LIT if every
// shading path samples it through the shared common/emission.slang helper rather than four hand-rolled multiplies -
// this scans the shader sources as text (the source-text gate pattern used throughout this file) and fails if any
// of the four shading paths stops calling material_emission(), or if emission.slang itself stops multiplying the
// sampled texture into the factor.
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

    // Batch XVIII moved the guard-plus-default-plus-combine wrapper around
    // material_emission() into material_textures.slang's resolved_emission()/
    // resolved_emission_lod0() - it is now the sole direct importer of
    // emission.slang and the sole direct caller of material_emission(); the
    // four shading paths below call resolved_emission[_lod0]() instead (see
    // EmissiveIsConsumedByEveryShadingPath for the exact call site per path).
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
        repo_root / "Resources/ShadersSlang/raytracing/raytrace.rchit.slang",
        repo_root / "Resources/ShadersSlang/path_tracing/path_tracing.slang",
    };
    for (const fs::path &path : shading_paths) {
        const auto text_opt = readFileText(path);
        ASSERT_TRUE(text_opt.has_value()) << "could not open " << path.string();
        const std::string &text = *text_opt;
        EXPECT_NE(text.find("import material_textures;"), std::string::npos)
          << path.string() << " no longer imports common/material_textures.slang. " << kFailureMessage;
        const bool calls_resolved_emission =
          text.find("resolved_emission(") != std::string::npos || text.find("resolved_emission_lod0(") != std::string::npos;
        EXPECT_TRUE(calls_resolved_emission)
          << path.string() << " no longer calls resolved_emission()/resolved_emission_lod0(). " << kFailureMessage;
    }
}

// ObjMaterial::normalTextureID (glTF normalTexture, follow-up to the tangent-plumbing task naming these four
// shading paths) is dedup'd into the same texture-slot budget as textureID/emissiveTextureID, but only actually
// perturbs shading if every shading path derives a TBN basis from the per-vertex tangent and runs the sampled
// texture through the shared common/normal_map.slang helper rather than four hand-rolled copies - this scans the
// shader sources as text (the source-text gate pattern used throughout this file) and fails if any of the four
// shading paths stops calling apply_normal_map(), or if normal_map.slang itself stops perturbing by the sampled
// tangent-space normal.
TEST(BuildIntegrity, NormalMappingIsAppliedByEveryShadingPath)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "apply_normal_map() in common/normal_map.slang is the sole place the tangent-space normalTexture sample is "
      "turned into a world-space shading normal; a shading path that hand-rolls it instead risks silently "
      "diverging from the other three, or never lighting normal maps at all";

    const fs::path normal_map_path = repo_root / "Resources/ShadersSlang/common/normal_map.slang";
    const auto normal_map_text_opt = readFileText(normal_map_path);
    ASSERT_TRUE(normal_map_text_opt.has_value()) << "could not open " << normal_map_path.string();
    const std::string &normal_map_text = *normal_map_text_opt;
    EXPECT_NE(normal_map_text.find("sampledNormal * 2.0 - 1.0"), std::string::npos)
      << "normal_map.slang's apply_normal_map() no longer unpacks the sampled tangent-space normal from [0,1] to "
         "[-1,1]. "
      << kFailureMessage;
    EXPECT_NE(normal_map_text.find("mul(nTs, float3x3("), std::string::npos)
      << "normal_map.slang's apply_normal_map() must right-multiply the row-built (t, b, n) basis by nTs "
         "(mul(nTs, float3x3(t, b, n))) to apply the tangent-to-world transform; mul(float3x3(t, b, n), nTs) is "
         "the inverse (world-to-tangent) transform and produces an inverted shading normal.";
    EXPECT_EQ(normal_map_text.find("mul(float3x3("), std::string::npos)
      << "normal_map.slang's apply_normal_map() must not left-multiply the row-built (t, b, n) basis - that is "
         "the world-to-tangent transform, not tangent-to-world.";

    const fs::path forward_path = repo_root / "Resources/ShadersSlang/forward/forward.slang";
    const auto forward_text_opt = readFileText(forward_path);
    ASSERT_TRUE(forward_text_opt.has_value()) << "could not open " << forward_path.string();
    const std::string &forward_text = *forward_text_opt;
    EXPECT_NE(forward_text.find("mul(nTs, float3x3("), std::string::npos)
      << "forward.slang's fs_main is normal_map.slang's wgsl-target inline twin and must right-multiply the "
         "row-built (t, b, nGeom) basis by nTs (mul(nTs, float3x3(t, b, nGeom))), same as apply_normal_map().";
    EXPECT_EQ(forward_text.find("mul(float3x3("), std::string::npos)
      << "forward.slang's fs_main must not left-multiply the row-built (t, b, nGeom) basis - that is the "
         "world-to-tangent transform, not tangent-to-world.";

    // Batch XVIII moved the guard-plus-default-plus-combine wrapper around
    // apply_normal_map() into material_textures.slang's resolved_normal()/
    // resolved_normal_lod0() - it is now the sole direct importer of
    // normal_map.slang and the sole direct caller of apply_normal_map(),
    // with material.normalScale folded in there instead of at each call
    // site. The four shading paths below call resolved_normal[_lod0]()
    // instead.
    const fs::path material_textures_path = repo_root / "Resources/ShadersSlang/common/material_textures.slang";
    const auto material_textures_text_opt = readFileText(material_textures_path);
    ASSERT_TRUE(material_textures_text_opt.has_value()) << "could not open " << material_textures_path.string();
    const std::string &material_textures_text = *material_textures_text_opt;
    EXPECT_NE(material_textures_text.find("import normal_map;"), std::string::npos)
      << "material_textures.slang no longer imports common/normal_map.slang. " << kFailureMessage;
    EXPECT_NE(material_textures_text.find("apply_normal_map("), std::string::npos)
      << "material_textures.slang no longer calls apply_normal_map(). " << kFailureMessage;
    EXPECT_NE(material_textures_text.find("material.normalTextureID"), std::string::npos)
      << "material_textures.slang's resolved_normal helpers no longer branch on ObjMaterial::normalTextureID. "
      << kFailureMessage;
    EXPECT_NE(material_textures_text.find("material.normalScale"), std::string::npos)
      << "material_textures.slang no longer passes ObjMaterial::normalScale into apply_normal_map(). "
      << kFailureMessage;

    const std::vector<fs::path> shading_paths = {
        repo_root / "Resources/ShadersSlang/rasterizer/rasterizer.slang",
        repo_root / "Resources/ShadersSlang/deferred/deferred.slang",
        repo_root / "Resources/ShadersSlang/raytracing/raytrace.rchit.slang",
        repo_root / "Resources/ShadersSlang/path_tracing/path_tracing.slang",
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

// ObjMaterial::alphaTextureID (OBJ map_d, follow-up to the map_d-becomes-a-MASK-cutoff task) is dedup'd into the
// same texture-slot budget as textureID/emissiveTextureID/normalTextureID, but only actually alpha-tests a map_d
// cut-out if every one of the four alpha-testing sites - the two raster paths, the shadow pass and the shared ray
// query alpha test - samples it and folds it into the alpha handed to alpha_masked_out(). This scans the shader
// sources as text (the source-text gate pattern used throughout this file) and fails if any of the four sites stops
// branching on ObjMaterial::alphaTextureID.
TEST(BuildIntegrity, AlphaTextureIsSampledByEveryAlphaTestingPath)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "material_rules.slang's alpha_masked_out() is the sole place the MASK cutoff test happens; a site that stops "
      "sampling ObjMaterial::alphaTextureID silently drops map_d cut-outs (the chain and hanging-plant geometry in "
      "the bundled crytek-sponza render as solid rectangles) instead of alpha-testing them";

    const std::vector<fs::path> alpha_testing_paths = {
        repo_root / "Resources/ShadersSlang/rasterizer/rasterizer.slang",
        repo_root / "Resources/ShadersSlang/deferred/deferred.slang",
        repo_root / "Resources/ShadersSlang/rasterizer/shadows/shadow_map.slang",
        repo_root / "Resources/ShadersSlang/common/alpha_test.slang",
    };
    for (const fs::path &path : alpha_testing_paths) {
        const auto text_opt = readFileText(path);
        ASSERT_TRUE(text_opt.has_value()) << "could not open " << path.string();
        const std::string &text = *text_opt;
        EXPECT_NE(text.find("alphaTextureID"), std::string::npos)
          << path.string() << " no longer branches on ObjMaterial::alphaTextureID. " << kFailureMessage;
        EXPECT_NE(text.find("alpha_masked_out("), std::string::npos)
          << path.string() << " no longer calls alpha_masked_out(). " << kFailureMessage;
    }
}

// KHR_texture_transform is declared per texture slot (glTF 2.0 spec), so the normal, metallic-roughness and
// emissive samples must transform their UV through their OWN named accessor (normal_uv()/metallic_roughness_uv()/
// emissive_uv() in common/base_color.slang), not the bare transform_uv(uv, material) overload - which resolves to
// the base-colour rows. Sampling a non-base slot with the base-colour rows silently tiles/offsets that slot's
// texture identically to base-colour instead of independently (or not at all, for a material that transforms only
// a non-base slot). common/material_textures.slang is the sole place these three slots are sampled today - every
// shading path calls its explicit-LOD or implicit-LOD helper instead of hand-rolling the sample
// (TextureSlotSamplingHasOneOwner below pins that no shading path still does) - so this scans that one file rather
// than every shading path, once each for its explicit-LOD and implicit-LOD helper.
TEST(BuildIntegrity, NonBaseTextureSlotsUseTheirOwnUvTransformRows)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "KHR_texture_transform is per texture slot; a non-base sample calling the bare transform_uv(uv, material) "
      "overload (the base-colour pair) instead of its own slot's named accessor stamps the base-colour transform "
      "onto a slot that may have a different transform, or none at all";

    const fs::path path = repo_root / "Resources/ShadersSlang/common/material_textures.slang";
    const auto text_opt = readFileText(path);
    ASSERT_TRUE(text_opt.has_value()) << "could not open " << path.string();
    const std::string &text = *text_opt;

    static const std::pair<const char *, std::size_t> kExpectedCounts[] = {
        { "normal_uv(", 2U },
        { "metallic_roughness_uv(", 2U },
        { "emissive_uv(", 2U },
    };
    for (const auto &[accessor, expected_count] : kExpectedCounts) {
        std::size_t count = 0;
        std::size_t pos = 0;
        while ((pos = text.find(accessor, pos)) != std::string::npos) {
            ++count;
            pos += std::strlen(accessor);
        }
        EXPECT_EQ(count, expected_count)
          << path.string() << " must call " << accessor
          << ") exactly once from its explicit-LOD helper and once from its implicit-LOD helper. " << kFailureMessage;
    }

    // The alpha (map_d) slot has no KHR_texture_transform slot of its own (an OBJ-only extension, not part of the
    // glTF spec's per-slot transform list) and deliberately reuses the base-colour rows via the bare
    // transform_uv(uv, material) overload, same as every shading path's own inline base-colour sample - that is the
    // legitimate case this gate must not flag. sample_alpha_lod0/sample_alpha are the only two bare-overload callers
    // left in this file.
    std::size_t bareTransformUvCalls = 0;
    std::size_t pos = 0;
    while ((pos = text.find("transform_uv(", pos)) != std::string::npos) {
        ++bareTransformUvCalls;
        pos += std::strlen("transform_uv(");
    }
    EXPECT_EQ(bareTransformUvCalls, 2U)
      << path.string() << " must call the bare transform_uv(uv, material) overload exactly twice (sample_alpha_lod0 "
         "and sample_alpha); every other slot must go through its own named accessor instead. "
      << kFailureMessage;
}

// Deferred from batch IX with a stated prerequisite (9ee460cb, symbolic binding numbers) that has since landed:
// rasterizer.slang, deferred.slang, shadow_map.slang, raytrace.rchit.slang and alpha_test.slang each declared their
// own [vk::binding(TEXTURES_BINDING, 0)] / [vk::binding(SAMPLER_BINDING, 0)] textures[]/textureSamplers[] pair, and
// rasterizer.slang, deferred.slang, raytrace.rchit.slang and path_tracing.slang each hand-rolled the same
// resolve_texture_slot(...) + Sample()/SampleLevel() pair for the alpha (map_d), normal, metallic-roughness and
// emissive slots. common/material_textures.slang is now the sole owner of both: this pins that no consumer
// redeclares the bindings, and that no consumer still spells the four-slot sample pattern inline instead of calling
// material_textures.slang's helpers. The base-colour (ObjMaterial::textureID) slot is deliberately not checked
// here - it stays inline at each call site next to the shading control flow that differs per shader.
TEST(BuildIntegrity, TextureSlotSamplingHasOneOwner)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";
    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    static const char *kTexturesBinding = "[vk::binding(TEXTURES_BINDING, 0)]";
    static const char *kSamplersBinding = "[vk::binding(SAMPLER_BINDING, 0)]";

    const fs::path owner_path = slang_root / "common/material_textures.slang";
    const auto owner_text_opt = readFileText(owner_path);
    ASSERT_TRUE(owner_text_opt.has_value()) << "could not open " << owner_path.string();
    EXPECT_NE(owner_text_opt->find(kTexturesBinding), std::string::npos)
      << owner_path.string() << " no longer declares the textures[] binding";
    EXPECT_NE(owner_text_opt->find(kSamplersBinding), std::string::npos)
      << owner_path.string() << " no longer declares the textureSamplers[] binding";

    // The five prior per-file binding owners, plus path_tracing.slang (never declared its own copy - it always
    // reached textures/textureSamplers through alpha_test.slang, and now also imports material_textures.slang
    // directly for its own explicit-LOD samples): the explicit consumer list, same shape as
    // kSharedDescriptorSetBindings above and for the same reason - a directory walk would also have to explain away
    // this test file's and material_textures.slang's own legitimate mentions of these identifiers.
    static const std::vector<fs::path> kConsumers = {
        slang_root / "rasterizer/rasterizer.slang",
        slang_root / "deferred/deferred.slang",
        slang_root / "rasterizer/shadows/shadow_map.slang",
        slang_root / "raytracing/raytrace.rchit.slang",
        slang_root / "common/alpha_test.slang",
        slang_root / "path_tracing/path_tracing.slang",
    };

    // The four texture-slot fields whose resolve_texture_slot(obj, material.<field>) + Sample()/SampleLevel() pair
    // must now live only in material_textures.slang. resolve_texture_slot(obj, material) (no field - the
    // base-colour overload) is deliberately excluded; it stays inline at every shading path's own call site.
    static const std::vector<std::string> kSlotFields = {
        "material.alphaTextureID",
        "material.normalTextureID",
        "material.metallicRoughnessTextureID",
        "material.emissiveTextureID",
    };

    std::vector<std::string> violations;
    for (const auto &path : kConsumers) {
        const auto text_opt = readFileText(path);
        ASSERT_TRUE(text_opt.has_value()) << "could not open " << path.string();
        const std::string &text = *text_opt;
        const std::string relative = fs::relative(path, repo_root).generic_string();

        if (text.find(kTexturesBinding) != std::string::npos) {
            violations.push_back(relative
                                  + " redeclares the textures[] binding - common/material_textures.slang is the "
                                    "sole owner");
        }
        if (text.find(kSamplersBinding) != std::string::npos) {
            violations.push_back(relative
                                  + " redeclares the textureSamplers[] binding - common/material_textures.slang is "
                                    "the sole owner");
        }
        for (const auto &field : kSlotFields) {
            if (text.find("resolve_texture_slot(obj, " + field + ")") != std::string::npos) {
                violations.push_back(relative + " still hand-rolls resolve_texture_slot(obj, " + field
                                      + ") inline - call common/material_textures.slang's helper instead");
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size() << " texture-slot sampling ownership violation(s):" << joinViolations(violations);
}

// normal_uv()/metallic_roughness_uv()/emissive_uv() in common/base_color.slang (C++ Vulkan renderer, ObjMaterial) and
// base_color_uv()/metallic_roughness_uv()/normal_uv()/emissive_uv()/occlusion_uv() in forward/forward.slang (Rust
// WebGPU renderer, PrimUniforms) are the sole named accessors for their slot's KHR_texture_transform row pair; the
// row members themselves - (normal|metallic_roughness|emissive)_uv_transform_row[01] on the C++ side,
// (base|mr|normal|emissive|occlusion)_uv_row[01] on the Rust side - must therefore appear only inside those
// accessor bodies and the two structs' declarations. A shading path naming a row member directly (rather than
// calling the accessor) can pair a slot's row0 with another slot's row1 - both are the same vector type on the same
// struct, so the mismatch is well-typed and silently samples the wrong UV. Unlike base_color.slang/scene_types.slang
// (which contain nothing but accessor/struct declarations, so a whole-file allowance is safe), forward.slang also
// holds the shading entry points (fs_main, vs_shadow_masked, fs_shadow_masked) that must call the accessors rather
// than name a row member themselves, so its allowance is scoped to just the accessor bodies and the PrimUniforms
// declaration, tracked by brace depth, rather than exempting the whole file.
TEST(BuildIntegrity, PerSlotUvTransformRowsAreSpelledInExactlyOnePlace)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = repo_root / "Resources/ShadersSlang";
    ASSERT_TRUE(fs::exists(slang_root)) << "could not find " << slang_root.string();

    static const std::regex kRowPattern(
      R"((normal|metallic_roughness|emissive)_uv_transform_row[01]|(base|mr|normal|emissive|occlusion)_uv_row[01])");
    static const std::set<fs::path> kAllowedFiles = {
        repo_root / "Resources/ShadersSlang/common/base_color.slang",
        repo_root / "Resources/ShadersSlang/common/scene_types.slang",
    };
    const fs::path forward_slang_path = repo_root / "Resources/ShadersSlang/forward/forward.slang";
    static const std::regex kForwardAllowedBlockStart(
      R"(^(struct PrimUniforms|float2 (base_color_uv_select|base_color_uv_rows|base_color_uv|metallic_roughness_uv|normal_uv|emissive_uv|occlusion_uv)\())");

    std::vector<std::string> violations;
    for (const auto &entry : fs::recursive_directory_iterator(slang_root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".slang") { continue; }
        if (kAllowedFiles.count(entry.path()) != 0) { continue; }

        const auto text_opt = readFileText(entry.path());
        ASSERT_TRUE(text_opt.has_value()) << "could not open " << entry.path().string();
        const std::string &text = *text_opt;
        const bool isForwardSlang = entry.path() == forward_slang_path;

        std::istringstream stream(text);
        std::string line;
        int lineNumber = 0;
        bool inAllowedBlock = false;
        int allowedBlockBraceDepth = 0;
        while (std::getline(stream, line)) {
            ++lineNumber;

            if (isForwardSlang) {
                if (!inAllowedBlock && std::regex_search(line, kForwardAllowedBlockStart)) {
                    inAllowedBlock = true;
                    allowedBlockBraceDepth = 0;
                }
                if (inAllowedBlock) {
                    for (char c : line) {
                        if (c == '{') { ++allowedBlockBraceDepth; }
                        else if (c == '}') {
                            --allowedBlockBraceDepth;
                            if (allowedBlockBraceDepth <= 0) { inAllowedBlock = false; }
                        }
                    }
                    continue; // inside an accessor body or the PrimUniforms declaration - allowed
                }
            }

            if (std::regex_search(line, kRowPattern)) {
                violations.push_back(fs::relative(entry.path(), repo_root).string() + ":" + std::to_string(lineNumber)
                                      + ": " + line);
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << "found a per-slot UV-transform row member named outside base_color.slang's/forward.slang's accessors and "
         "scene_types.slang's/forward.slang's struct declarations - call normal_uv()/metallic_roughness_uv()/"
         "emissive_uv() (or forward.slang's base_color_uv()/metallic_roughness_uv()/normal_uv()/emissive_uv()/"
         "occlusion_uv()) instead:\n"
      << joinViolations(violations, "", "\n");
}

// forward.slang's uv-set mask bit (prim.material_flags.y) must be read by exactly one accessor per texture slot -
// base_color_uv_select() (folded into base_color_uv()), metallic_roughness_uv(), normal_uv(), emissive_uv() and
// occlusion_uv() - never inlined a second time at a call site, or a future hand-rolled mask selection could
// silently diverge from the accessors' bit assignment. This is a narrower companion to
// PerSlotUvTransformRowsAreSpelledInExactlyOnePlace above (which catches a hand-paired row0/row1), covering the
// case where the mask bit alone is re-derived instead of the row pair: it counts every `prim.material_flags.y`
// read and fails if there are more than one per slot.
TEST(BuildIntegrity, ForwardShaderUvSetMaskHasOneOwnerPerSlot)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path forward_slang = repo_root / "Resources/ShadersSlang/forward/forward.slang";
    ASSERT_TRUE(fs::exists(forward_slang)) << "could not find " << forward_slang.string();

    const auto text_opt = readFileText(forward_slang);
    ASSERT_TRUE(text_opt.has_value()) << "could not open " << forward_slang.string();

    static const std::regex kMaskReadPattern(R"(prim\.material_flags\.y)");
    static constexpr int kSlotCount = 5; // base_color, metallic_roughness, normal, emissive, occlusion

    const auto matchCount =
      std::distance(std::sregex_iterator(text_opt->begin(), text_opt->end(), kMaskReadPattern), std::sregex_iterator());

    EXPECT_LE(matchCount, kSlotCount) << "forward.slang reads prim.material_flags.y in " << matchCount
                                       << " place(s), more than the " << kSlotCount
                                       << " per-slot UV accessors - a call site may be hand-rolling the uv-set mask "
                                          "instead of calling the accessor";
}

// ObjMaterial::metallicRoughnessTextureID (glTF pbrMetallicRoughness.metallicRoughnessTexture) is dedup'd into the
// same texture-slot budget as textureID/emissiveTextureID/normalTextureID, but only actually varies the metallic/
// roughness terms if every shading path samples it and runs the result through the shared
// common/material_rules.slang channel swizzle (glTF 2.0 SS3.9.2: G = roughness, B = metallic) rather than four
// hand-rolled copies - this scans the shader sources as text (the source-text gate pattern used throughout this
// file) and fails if material_rules.slang stops exposing the swizzle, or if rasterizer/deferred/raytrace.rchit/
// path_tracing stop calling it. path_tracing.slang is a Lambertian-only kernel with no roughness/BRDF term, so it
// is checked only for referencing the texture ID and calling material_metallic_roughness() (never a bare
// mrSample.b/mrSample.g read), same as the other three.
TEST(BuildIntegrity, MetallicRoughnessTextureIsSampledByEveryShadingPath)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "material_metallic_roughness() in common/material_rules.slang is the sole place the glTF G=roughness/B=metallic "
      "channel swizzle happens; a shading path that hand-rolls it instead risks silently diverging from the others";

    const fs::path material_rules_path = repo_root / "Resources/ShadersSlang/common/material_rules.slang";
    const auto material_rules_text_opt = readFileText(material_rules_path);
    ASSERT_TRUE(material_rules_text_opt.has_value()) << "could not open " << material_rules_path.string();
    const std::string &material_rules_text = *material_rules_text_opt;
    EXPECT_NE(material_rules_text.find("material.metallic * mrSample.b"), std::string::npos)
      << "material_rules.slang's material_metallic_roughness() no longer multiplies the sampled B channel into the "
         "metallic factor. "
      << kFailureMessage;
    EXPECT_NE(material_rules_text.find("material_roughness(material) * mrSample.g"), std::string::npos)
      << "material_rules.slang's material_metallic_roughness() no longer multiplies the sampled G channel into the "
         "roughness factor. "
      << kFailureMessage;

    // Batch XVIII moved the guard-plus-default-plus-combine wrapper around
    // material_metallic_roughness() into material_textures.slang's
    // resolved_metallic_roughness()/resolved_metallic_roughness_lod0() for
    // rasterizer/deferred/raytrace.rchit. path_tracing.slang deliberately
    // keeps its block inline instead of adopting
    // resolved_metallic_roughness_lod0() - see that helper's doc comment in
    // material_textures.slang: the shared helper's untextured-material
    // fallback (today's factors) would silently start applying
    // material.metallic to this Lambertian-only kernel, which never
    // consumed it before.
    const fs::path material_textures_path = repo_root / "Resources/ShadersSlang/common/material_textures.slang";
    const auto material_textures_text_opt = readFileText(material_textures_path);
    ASSERT_TRUE(material_textures_text_opt.has_value()) << "could not open " << material_textures_path.string();
    const std::string &material_textures_text = *material_textures_text_opt;
    EXPECT_NE(material_textures_text.find("material_metallic_roughness("), std::string::npos)
      << "material_textures.slang no longer calls material_metallic_roughness(). " << kFailureMessage;
    EXPECT_NE(material_textures_text.find("material.metallicRoughnessTextureID"), std::string::npos)
      << "material_textures.slang's resolved_metallic_roughness helpers no longer branch on "
         "ObjMaterial::metallicRoughnessTextureID. "
      << kFailureMessage;

    const std::vector<fs::path> brdf_shading_paths = {
        repo_root / "Resources/ShadersSlang/rasterizer/rasterizer.slang",
        repo_root / "Resources/ShadersSlang/deferred/deferred.slang",
        repo_root / "Resources/ShadersSlang/raytracing/raytrace.rchit.slang",
        repo_root / "Resources/ShadersSlang/path_tracing/path_tracing.slang",
    };
    for (const fs::path &path : brdf_shading_paths) {
        const auto text_opt = readFileText(path);
        ASSERT_TRUE(text_opt.has_value()) << "could not open " << path.string();
        const std::string &text = *text_opt;
        const bool is_path_tracing = path.filename() == "path_tracing.slang";
        if (is_path_tracing) {
            EXPECT_NE(text.find("metallicRoughnessTextureID"), std::string::npos)
              << path.string() << " no longer branches on ObjMaterial::metallicRoughnessTextureID. " << kFailureMessage;
            EXPECT_NE(text.find("material_metallic_roughness("), std::string::npos)
              << path.string() << " no longer calls material_metallic_roughness(). " << kFailureMessage;
        } else {
            EXPECT_NE(text.find("import material_textures;"), std::string::npos)
              << path.string() << " no longer imports common/material_textures.slang. " << kFailureMessage;
            const bool calls_resolved =
              text.find("resolved_metallic_roughness(") != std::string::npos
              || text.find("resolved_metallic_roughness_lod0(") != std::string::npos;
            EXPECT_TRUE(calls_resolved)
              << path.string() << " no longer calls resolved_metallic_roughness()/resolved_metallic_roughness_lod0(). "
              << kFailureMessage;
        }
        EXPECT_EQ(text.find("mrSample.g"), std::string::npos)
          << path.string() << " hand-rolls the roughness (G) channel read instead of going through "
                               "material_metallic_roughness(). "
          << kFailureMessage;
        EXPECT_EQ(text.find("mrSample.b"), std::string::npos)
          << path.string() << " hand-rolls the metallic (B) channel read instead of going through "
                               "material_metallic_roughness(). "
          << kFailureMessage;
    }
}

// KHR_materials_unlit must return the base colour unlit in all four shading
// paths (spec: unlit materials "MUST NOT be lit"). This scans the shader
// sources as text and fails if any of the four stops branching on
// ObjMaterial::unlit, reporting which file is missing it.
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
        repo_root / "Resources/ShadersSlang/raytracing/raytrace.rchit.slang",
        repo_root / "Resources/ShadersSlang/path_tracing/path_tracing.slang",
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

// Vertex.color (glTF COLOR_0) is fetched by every loader and uploaded to the
// GPU alongside position/normal, but the rasterizer and deferred paths are
// the only ones that ever multiplied it into shading - the ray tracing and
// path tracing paths silently ignored per-vertex colour. This scans the
// shader sources as text and fails if any of the four shading paths stops
// referencing Vertex.color.
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

    const fs::path rchit_path = repo_root / "Resources/ShadersSlang/raytracing/raytrace.rchit.slang";
    const auto rchit_text_opt = readFileText(rchit_path);
    ASSERT_TRUE(rchit_text_opt.has_value()) << "could not open " << rchit_path.string();
    const std::string &rchit_text = *rchit_text_opt;
    EXPECT_NE(rchit_text.find(".color"), std::string::npos)
      << "raytrace.rchit.slang no longer uses Vertex.color. " << kFailureMessage;

    const fs::path path_tracing_path = repo_root / "Resources/ShadersSlang/path_tracing/path_tracing.slang";
    const auto path_tracing_text_opt = readFileText(path_tracing_path);
    ASSERT_TRUE(path_tracing_text_opt.has_value()) << "could not open " << path_tracing_path.string();
    const std::string &path_tracing_text = *path_tracing_text_opt;
    EXPECT_NE(path_tracing_text.find(".color"), std::string::npos)
      << "path_tracing.slang no longer uses Vertex.color. " << kFailureMessage;
}

// The object->world normal transform is the inverse-transpose of the
// object->world matrix, i.e. WorldToObject applied as a *row* multiply
// (mul(v, M), the HLSL/Slang convention where mul(M, v) is a column
// multiply). Both raytrace.rchit.slang and path_tracing.slang once
// column-multiplied WorldToObject instead, which agrees with the correct
// row form only when the model's linear part is orthonormal with no
// non-uniform scale/shear (e.g. identity or pure translation) - exactly the
// default scene, which is why no golden test caught it. This scans the
// shader sources as text and fails if either reintroduces the column
// spelling, or drops the row spelling / the closest-hit face-forward.
TEST(BuildIntegrity, RayTracedNormalsUseTheInverseTransposeTransform)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "the object->world normal transform must be the inverse-transpose of WorldToObject, applied as a row "
      "multiply (mul(normalHit, M)), not a column multiply (mul(M, normalHit)) - a column multiply only agrees "
      "with the correct transform when the model matrix has no rotation/scale, which the default scene never "
      "exercises";

    const fs::path rchit_path = repo_root / "Resources/ShadersSlang/raytracing/raytrace.rchit.slang";
    const auto rchit_text_opt = readFileText(rchit_path);
    ASSERT_TRUE(rchit_text_opt.has_value()) << "could not open " << rchit_path.string();
    const std::string &rchit_text = *rchit_text_opt;
    EXPECT_EQ(rchit_text.find("mul((float3x3)WorldToObject()"), std::string::npos)
      << "raytrace.rchit.slang column-multiplies WorldToObject for the normal again. " << kFailureMessage;
    EXPECT_NE(rchit_text.find("mul(normalHit,"), std::string::npos)
      << "raytrace.rchit.slang no longer row-multiplies WorldToObject for the normal. " << kFailureMessage;
    EXPECT_NE(rchit_text.find("dot(worldNormal, WorldRayDirection())"), std::string::npos)
      << "raytrace.rchit.slang no longer face-forwards the closest-hit normal against WorldRayDirection(). "
      << kFailureMessage;

    const fs::path path_tracing_path = repo_root / "Resources/ShadersSlang/path_tracing/path_tracing.slang";
    const auto path_tracing_text_opt = readFileText(path_tracing_path);
    ASSERT_TRUE(path_tracing_text_opt.has_value()) << "could not open " << path_tracing_path.string();
    const std::string &path_tracing_text = *path_tracing_text_opt;
    EXPECT_EQ(path_tracing_text.find("mul(worldToObject, float4(normalHit"), std::string::npos)
      << "path_tracing.slang column-multiplies worldToObject for the normal again. " << kFailureMessage;
    EXPECT_NE(path_tracing_text.find("mul(normalHit,"), std::string::npos)
      << "path_tracing.slang no longer row-multiplies worldToObject for the normal. " << kFailureMessage;
}

// scene_types.slang's MAX_CASCADES is the gated source of truth for the
// cascade count - HostAndShaderSharedConstantsAgree above pins it against
// host_device_shared_vars.hpp. That gate is blind to a second, independent
// "static const int" redeclaring the same value under a cascade-ish name
// elsewhere: raising MAX_CASCADES on both currently-gated sides would pass
// every existing pin test while a shader still reading its own stale local
// copy kept clamping to the old count. shadow_map.slang carried exactly this
// (`NUM_CASCADES = 3`) until it was retired in favour of importing
// MAX_CASCADES directly. This scans every other .slang file for a
// "static const int" whose name contains "CASCADE" (case-insensitive) and
// whose value equals MAX_CASCADES, and fails if one exists.
//
// The scan is restricted to Vulkan-consumed shaders (per
// parse_vulkan_consumed_slang_sources, i.e. the compile-slang-shaders.sh
// manifest rows that target 'spirv') rather than every .slang file: WGSL-only
// shaders such as forward/forward.slang pin their own cascade count on the
// Rust side (see forward.slang's "Must match CASCADE_COUNT in forward.rs"
// comment) and cannot go stale against this C++-side MAX_CASCADES - the C++
// engine never loads them, so a redeclaration there is not the bug this gate
// exists to catch.
TEST(BuildIntegrity, NoShaderRedeclaresTheCascadeCount)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    const fs::path scene_types_relative = fs::path("common") / "scene_types.slang";
    const auto scene_types_constants = parse_int_constants(slang_root / scene_types_relative);
    ASSERT_TRUE(scene_types_constants.contains("MAX_CASCADES"))
      << "MAX_CASCADES not found (or not parseable) in " << (slang_root / scene_types_relative).string();
    const int max_cascades = scene_types_constants.at("MAX_CASCADES");

    const auto &manifest = shader_manifest(repo_root);
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";
    const auto &vulkan_consumed_sources = manifest->vulkan_spirv_sources;
    ASSERT_FALSE(vulkan_consumed_sources.empty())
      << "no spirv-targeted rows in shader-manifest.json - manifest format changed?";

    static const std::string kDeclKeyword = "static const int ";

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".slang") { continue; }
        const std::string relative_path = fs::relative(path, slang_root).generic_string();
        if (relative_path.starts_with("build/")) { continue; }
        if (relative_path == scene_types_relative.generic_string()) { continue; }
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
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (*value == max_cascades && lower_name.find("cascade") != std::string::npos) {
                violations.push_back(fs::relative(path, repo_root).generic_string() + ':'
                                      + std::to_string(line_number) + ": " + name + " = " + std::to_string(*value));
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " shader-local cascade-count constant(s) redeclare MAX_CASCADES (" << max_cascades
      << ") outside scene_types.slang - import MAX_CASCADES instead: "
      << joinViolations(violations);
}

// One declared variable's [vk::binding(...)] attribute in the shared render
// descriptor set, checked against scene_types.slang's named constant for it.
struct SharedDescriptorBinding
{
    std::string relative_path;
    std::string variable_name;
};

// The shared render descriptor set's nineteen set-0 declarations
// (globalUBO_BINDING / sceneUBO_BINDING / OBJECT_DESCRIPTION_BINDING /
// TEXTURES_BINDING / SAMPLER_BINDING / SHADOW_MAP_BINDING), the ray-tracing
// set-1 declarations that share the same shared descriptor set resources
// (TLAS_BINDING / OUT_IMAGE_BINDING / ACCUMULATION_IMAGE_BINDING), plus
// deferred.slang's four gbuffer input-attachment set-1 declarations
// (GBUFFER_NORMAL_BINDING / GBUFFER_ALBEDO_BINDING / GBUFFER_MATERIAL_BINDING
// / GBUFFER_DEPTH_BINDING) - twenty-three declarations in total, across the
// files that declare them. common/alpha_test.slang is not listed: it already
// used the named constants before this gate existed.
// textures/textureSamplers now declare once in common/material_textures.slang
// (TextureSlotSamplingHasOneOwner below pins that no consumer redeclares
// them) rather than once per consumer, so rasterizer.slang, deferred.slang,
// shadow_map.slang and raytrace.rchit.slang no longer have their own
// textures/textureSamplers rows here. Deliberately NOT the full set of every
// [vk::binding(...)] in these files - shadow_map.slang's own
// [vk::binding(1, 1)] lightSpaceMatrices is a pipeline-local set with no
// named constant in scene_types.slang, so listing it here would be a
// permanent, unfixable violation rather than a real regression.
const std::vector<SharedDescriptorBinding> kSharedDescriptorSetBindings = {
    {"common/material_fetch.slang", "objectDescription"},
    {"common/material_textures.slang", "textures"},
    {"common/material_textures.slang", "textureSamplers"},
    {"common/cascaded_shadow.slang", "directionalShadowMaps"},
    {"rasterizer/rasterizer.slang", "globalUBO"},
    {"rasterizer/rasterizer.slang", "sceneUBO"},
    {"deferred/deferred.slang", "globalUBO"},
    {"deferred/deferred.slang", "globalUBO_lighting"},
    {"deferred/deferred.slang", "sceneUBO_lighting"},
    {"deferred/deferred.slang", "inNormal"},
    {"deferred/deferred.slang", "inAlbedo"},
    {"deferred/deferred.slang", "inMaterial"},
    {"deferred/deferred.slang", "inDepth"},
    {"raytracing/raytrace.rchit.slang", "sceneUBO"},
    {"raytracing/raytrace.rchit.slang", "TLAS"},
    {"raytracing/raytrace.rgen.slang", "globalUBO"},
    {"raytracing/raytrace.rgen.slang", "TLAS"},
    {"raytracing/raytrace.rgen.slang", "image"},
    {"path_tracing/path_tracing.slang", "globalUBO"},
    {"path_tracing/path_tracing.slang", "sceneUBO"},
    {"path_tracing/path_tracing.slang", "TLAS"},
    {"path_tracing/path_tracing.slang", "image"},
    {"path_tracing/path_tracing.slang", "accumulationImage"},
};

// HostAndShaderSharedConstantsAgree pins scene_types.slang's nine binding
// constants against host_device_shared_vars.hpp, and the C++ side writes its
// descriptor layout with the names
// (VulkanRenderer::createSharedRenderDescriptorResources) - but nothing
// checked that a shader's own [vk::binding(...)] attribute is actually
// derived from the name rather than a bare integer that happens to agree
// today. Renaming or renumbering e.g. SHADOW_MAP_BINDING would update the
// host header and scene_types.slang in lockstep while a shader's literal
// stayed behind, with every existing pin test still green. Same failure
// mode NoShaderRedeclaresTheCascadeCount closes for MAX_CASCADES above; same
// gate shape here, but over an explicit (file, declared-variable) list
// rather than a directory walk - the exclusions noted on
// kSharedDescriptorSetBindings above are the reason: a walk would also have
// to explain away the pipeline-local bindings that have no named constant to
// begin with (shadow_map.slang's lightSpaceMatrices, deferred.slang's
// subpass inputs, and every standalone post-processing shader's own set).
TEST(BuildIntegrity, SharedDescriptorSetBindingsUseTheNamedConstants)
{
    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    std::set<std::string> distinct_paths;
    for (const auto &binding : kSharedDescriptorSetBindings) { distinct_paths.insert(binding.relative_path); }
    for (const auto &relative_path : distinct_paths) {
        ASSERT_TRUE(fs::exists(slang_root / relative_path))
          << "kSharedDescriptorSetBindings names a file that no longer exists: " << relative_path
          << " - update the gate in buildIntegritySuite.cpp";
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

            // Whole-word search: the variable name may also occur as a
            // prefix of a longer identifier earlier on the same line (e.g.
            // "globalUBO" inside "globalUBO_BINDING" or "GlobalUBO"'s type),
            // so every occurrence must be tried, not just the first.
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

            const bool is_integer_literal = !first_arg.empty()
              && std::all_of(first_arg.begin(), first_arg.end(),
                              [](unsigned char c) { return std::isdigit(c) != 0; });
            if (is_integer_literal) {
                violations.push_back(fs::path(binding.relative_path).generic_string() + ":" + binding.variable_name);
            }
            break;
        }
        ASSERT_TRUE(found_declaration)
          << "kSharedDescriptorSetBindings names a declaration that no longer exists: " << binding.variable_name
          << " in " << path.string() << " - update the gate in buildIntegritySuite.cpp";
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " shared render descriptor set binding(s) use a bare integer literal - "
      << "use scene_types.slang's named binding constant - host_device_shared_vars.hpp and "
      << "HostAndShaderSharedConstantsAgree already pin these: "
      << joinViolations(violations);
}

// One traced object-index source file and how many instance+geometry index
// pairs it is expected to contain, backing TracedObjectIndexAddsTheGeometryIndex.
struct TracedObjectIndexFile
{
    std::string relative_path;
    int expected_pair_count;
};

// path_tracing.slang's two RayQuery candidate loops (bounce + NEE shadow)
// plus its committed-hit block; raytrace.rchit.slang and raytrace.rahit.slang
// each resolve their one hit with a single expression. Update the expected
// count here if a traced object-index site is added, removed, or moved.
const std::vector<TracedObjectIndexFile> kTracedObjectIndexFiles = {
    {"path_tracing/path_tracing.slang", 3},
    {"raytracing/raytrace.rchit.slang", 1},
    {"raytracing/raytrace.rahit.slang", 1},
};

// Regression gate for the multi-mesh device-lost fix (BACKLOG.md):
// ASManager stamps instanceCustomIndex/InstanceID with the model's
// *first-mesh* flat index (ASManager.cpp's createTLAS), so every traced
// object-index expression must add the matching geometry accessor
// (Committed*/Candidate*/bare GeometryIndex()) to reach the mesh that was
// actually hit - an instance accessor alone always resolves to the model's
// first mesh. path_tracing_main's committed-hit block used to read only
// CommittedInstanceIndex() (now CommittedInstanceID()), which faulted on any
// multi-mesh model past its first mesh (out-of-bounds buffer-device-address
// read on the mismatched vertex/index buffers). Source half below asserts
// every instance accessor pairs with its geometry counterpart; SPIR-V half
// confirms the compiled path_tracing module actually emits the extra opcode
// this implies.
TEST(BuildIntegrity, TracedObjectIndexAddsTheGeometryIndex)
{
    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

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
                 it != std::sregex_iterator(); ++it) {
                const std::string prefix = (*it)[1].str();
                const std::string geometry_call = prefix + "GeometryIndex()";
                if (line.find(geometry_call) == std::string::npos) {
                    violations.push_back(
                      file.relative_path + ": '" + it->str() + "' with no matching '" + geometry_call
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
    constexpr std::size_t kMinGeometryIndexOpcodeCount = 3;// 2 candidate loops + 1 committed-hit block

    const fs::path spv_path = spirvRoot() / "path_tracing" / "path_tracing.path_tracing_main.spv";
    if (!fs::exists(spv_path)) {
        GTEST_SKIP() << "missing " << spv_path.string() << " - shaders have not been compiled";
    }

    const auto counts = parse_spirv_opcode_counts(spv_path);
    ASSERT_TRUE(counts.has_value()) << "could not parse " << spv_path.string() << " as SPIR-V";

    const auto count_it = counts->find(kOpRayQueryGetIntersectionGeometryIndexKHR);
    const std::size_t actual_count = (count_it == counts->end()) ? 0 : count_it->second;
    EXPECT_GE(actual_count, kMinGeometryIndexOpcodeCount)
      << spv_path.string() << ": expected OpRayQueryGetIntersectionGeometryIndexKHR at least "
      << kMinGeometryIndexOpcodeCount
      << " times (one per RayQuery candidate/committed-hit site), found " << actual_count
      << " - path_tracing.slang may have regressed to a bare instance index, or the compiled .spv is "
         "stale (recompile with Build-SlangShaders.ps1)";
}

// Regression gate for the second half of the multi-mesh device-lost fix
// (BACKLOG.md): Slang's HLSL-shaped InstanceIndex() reads the TLAS *instance*
// index (SPIR-V BuiltIn InstanceId, 6 / OpRayQueryGetIntersectionInstanceIdKHR,
// 6020), not the host-supplied instanceCustomIndex ASManager::createTLAS
// actually stamps with the model's first-mesh flat index - that value is
// only reachable via InstanceID() / Candidate*InstanceID() /
// Committed*InstanceID() (BuiltIn InstanceCustomIndexKHR, 5327 /
// OpRayQueryGetIntersectionInstanceCustomIndexKHR, 6019). The two spellings
// coincide only while every model holds exactly one mesh, so this was latent
// rather than firing on the single-model reproducer scene - see
// TracedObjectIndexAddsTheGeometryIndex above for the sibling geometry-index
// half of the same contract. Presence/absence only, not exact counts: counts
// move with inlining.
TEST(BuildIntegrity, TracedObjectIndexReadsTheInstanceCustomIndex)
{
    constexpr uint32_t kBuiltInInstanceId = 6;
    constexpr uint32_t kBuiltInInstanceCustomIndexKHR = 5327;
    constexpr uint32_t kOpRayQueryGetIntersectionInstanceCustomIndexKHR = 6019;
    constexpr uint32_t kOpRayQueryGetIntersectionInstanceIdKHR = 6020;

    for (const char *relative_spv :
      { "raytracing/raytrace.rchit.rchit_main.spv", "raytracing/raytrace.rahit.rahit_main.spv" }) {
        const fs::path spv_path = spirvRoot() / relative_spv;
        if (!fs::exists(spv_path)) {
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
    if (!fs::exists(path_tracing_spv)) {
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
         "the host-written instanceCustomIndex, found " << instance_id_count << " occurrence(s)";
}

// Regression gate for the RT/PT objectDescription centralization:
// common/material_fetch.slang owns the [vk::binding(OBJECT_DESCRIPTION_BINDING, 0)]
// objectDescription binding and the raw MaterialIDs*/Materials* lookup it wraps
// (fetch_object_description() / fetch_material()) - every other shading path
// must fetch through those two functions rather than re-declaring the
// binding or hand-rolling the lookup, the way path_tracing.slang and
// raytrace.rchit.slang used to. Scans every SPIR-V-targeted shader entry
// point (per shader_manifest's vulkan_spirv_sources, same source
// NoShaderRedeclaresTheCascadeCount above scans) plus the shared common/
// modules those shaders import - a stray binding or lookup could land in
// either. common/scene_types.slang is excluded: it is the struct definition
// site for both the objectDescription binding's element type and the
// material_index_address field itself, so it legitimately contains both
// substrings without being a "shading path".
TEST(BuildIntegrity, EveryShadingPathFetchesObjectDescriptionsThroughMaterialFetch)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    const auto &manifest = shader_manifest(repo_root);
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
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".slang") { continue; }
        const std::string relative_path = fs::relative(path, slang_root).generic_string();
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
      << kMaterialFetchRelative << ": "
      << joinViolations(binding_violations)
      << " - " << kFixSuggestion;

    EXPECT_TRUE(lookup_violations.empty())
      << lookup_violations.size() << " file(s) hand-roll the material_index_address lookup outside "
      << kMaterialFetchRelative << ": "
      << joinViolations(lookup_violations)
      << " - " << kFixSuggestion;
}

// cascaded_shadow.slang reads two SceneUBO fields the host already clamps
// (SceneUboMarshal.hpp's clampPcfRadius and fillSceneUboCascades) and must
// clamp them again itself: the host is the first lock, the shader is the
// second, and only the second is what actually protects the
// cascadeLightSpaceMatrices[] index / tap loop bound at GPU-execution time.
// This pins that both clamps still exist in the shader source, so a future
// edit cannot silently drop one half of the double lock.
TEST(BuildIntegrity, CascadedShadowClampsBothItsUboCounts)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path shadow_path = slangRoot() / "common" / "cascaded_shadow.slang";
    const auto contentOpt = readFileText(shadow_path);
    ASSERT_TRUE(contentOpt.has_value()) << "missing " << shadow_path.string();
    const std::string &content = *contentOpt;

    EXPECT_NE(content.find("clamp(int(sceneUBO.numCascades), 0, MAX_CASCADES)"), std::string::npos)
      << "cascaded_shadow.slang must clamp numCascades to MAX_CASCADES before indexing "
         "cascadeLightSpaceMatrices[]";
    EXPECT_NE(content.find("clamp(int(sceneUBO.pcfRadius), 0, MAX_PCF_RADIUS)"), std::string::npos)
      << "cascaded_shadow.slang must clamp pcfRadius to MAX_PCF_RADIUS before the tap loop";
}

// dirShadowMap.init(...) used to also run inline in VulkanRenderer's
// constructor (a second, hard-coded MAX_CASCADES/startup-resolution copy of
// what reinitShadowMapForCurrentSettings() does), and the per-image light
// matrices were re-seeded via an explicit loop right after construction
// instead of through the same flag-driven path every later re-init uses. A
// re-init (GUI shadow-resolution/cascade-count change, or a swapchain-image-
// count change) left every image but the next one drawFrame() acquired
// holding stale or default-constructed matrices, because dirShadowMap's own
// seed loop runs before updateUniforms() has recomputed cascadeData for the
// new settings. This is a source-level "one rule, one definition" gate - the
// only kind of coverage available for a path that needs a device.
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
        EXPECT_GE(pos, update_span->first) << "uploadLightMatrices( call at offset " << pos
                                            << " lies outside update_uniform_buffers";
        EXPECT_LT(pos, update_span->second) << "uploadLightMatrices( call at offset " << pos
                                             << " lies outside update_uniform_buffers";
        ++upload_count;
    }
    EXPECT_GT(upload_count, 0U) << "expected at least one uploadLightMatrices( call in VulkanRenderer.cpp";
}

// The flattened texture-slot clamp (int(obj.texture_offset) + textureID,
// clamped into [0, MAX_TEXTURE_COUNT - 1]) has exactly one definition:
// resolve_texture_slot() in scene_types.slang. Every consumer must call it
// rather than re-deriving the clamp locally - a second copy could drift from
// the "over-cap models sample a wrong slot" behaviour VulkanRenderer.cpp's
// warning documents.
TEST(BuildIntegrity, TextureSlotClampHasOneDefinition)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    static const std::string kClampLiteral = "MAX_TEXTURE_COUNT - 1)";
    const fs::path scene_types_relative = fs::path("common") / "scene_types.slang";

    std::vector<std::string> other_definitions;
    bool found_in_scene_types = false;
    std::error_code error;
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".slang") { continue; }
        const std::string relative_path = fs::relative(path, slang_root).generic_string();
        if (relative_path.starts_with("build/")) { continue; }

        const auto text = readFileText(path);
        if (!text.has_value() || text->find(kClampLiteral) == std::string::npos) { continue; }

        if (relative_path == scene_types_relative.generic_string()) {
            found_in_scene_types = true;
        } else {
            other_definitions.push_back(relative_path);
        }
    }

    EXPECT_TRUE(found_in_scene_types) << kClampLiteral << " not found in " << scene_types_relative.generic_string()
                                       << " - resolve_texture_slot() moved or was rewritten?";
    EXPECT_TRUE(other_definitions.empty())
      << other_definitions.size() << " .slang file(s) besides scene_types.slang re-derive the clamp instead of "
      << "calling resolve_texture_slot(): " << joinViolations(other_definitions);
}

// dirLight.direction stores the direction the light TRAVELS (a host-slider
// convention confirmed by CascadedShadowMapMath.cpp and the path-tracing
// history key). Every shader that wants the vector pointing TOWARD the light
// must negate it. clouds.slang once read the field un-negated, integrating
// self-shadowing and phase scattering away from the sun instead of toward it.
// This gate pins the one convention across every consumer: negation must
// happen right where the field is read, inside normalize(...).
TEST(BuildIntegrity, EveryShaderDerivesTheLightVectorByNegation)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    static const std::string kMarker = "dirLight.direction";
    static const std::string kNormalizeCall = "normalize(";

    int occurrences_checked = 0;
    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".slang") { continue; }

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
                ++occurrences_checked;

                // Walk back over the qualifying "scene." / "sceneUBO_lighting."
                // prefix to find where the read expression actually starts.
                std::size_t expr_start = marker_pos;
                while (expr_start > 0
                       && (is_identifier_char(line[expr_start - 1]) || line[expr_start - 1] == '.')) {
                    --expr_start;
                }

                std::size_t before = expr_start;
                while (before > 0 && std::isspace(static_cast<unsigned char>(line[before - 1])) != 0) { --before; }

                const bool negated = before > 0 && line[before - 1] == '-';
                std::size_t normalize_end = negated ? before - 1 : before;
                while (normalize_end > 0
                       && std::isspace(static_cast<unsigned char>(line[normalize_end - 1])) != 0) {
                    --normalize_end;
                }
                const bool inside_normalize = normalize_end >= kNormalizeCall.size()
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

    EXPECT_EQ(occurrences_checked, 6)
      << "expected exactly 6 shader sites reading dirLight.direction, found " << occurrences_checked
      << " - update this count if a consumer was intentionally added or removed, and confirm the new "
         "site also negates the field";

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " shader site(s) read dirLight.direction without negating it inside normalize(...) - "
         "dirLight.direction is a travel direction, negate it to get the vector toward the light: "
      << joinViolations(violations);
}

// Completed item 8 removed the hard-coded `roughness = 0.9` from forward
// shading; raytrace.rchit.slang kept its own copy until this test's commit
// moved the shininess -> roughness mapping into common/material_rules.slang's
// material_roughness(). Guards against a numeric literal being reintroduced
// by any shading path, and against a shading path silently dropping the
// shared helper call.
TEST(BuildIntegrity, EveryShadingPathDerivesRoughnessFromTheMaterial)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    static const std::regex kLiteralRoughnessAssignment(R"(\broughness\s*=\s*[0-9])");

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".slang") { continue; }

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
                                        "material_roughness() instead");
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size() << " shader site(s) hard-code roughness instead of deriving it from the material:"
      << joinViolations(violations);

    // Batch XVIII moved the metallic-roughness guard-plus-default-plus-combine
    // wrapper - including the material_roughness() fallback call - into
    // material_textures.slang's resolved_metallic_roughness()/
    // resolved_metallic_roughness_lod0(); rasterizer/deferred/raytrace.rchit
    // now derive roughness by calling one of those instead of
    // material_roughness() directly.
    const fs::path material_textures_path = slang_root / "common/material_textures.slang";
    const auto material_textures_content = readFileText(material_textures_path);
    ASSERT_TRUE(material_textures_content.has_value()) << "missing " << material_textures_path.string();
    EXPECT_NE(material_textures_content->find("material_roughness("), std::string::npos)
      << "material_textures.slang's resolved_metallic_roughness helpers must derive the fallback roughness via "
         "material_roughness(), not their own copy of the mapping";

    static const std::array<const char *, 3> kShadingShaders = {
        "rasterizer/rasterizer.slang",
        "deferred/deferred.slang",
        "raytracing/raytrace.rchit.slang",
    };

    for (const char *relative : kShadingShaders) {
        const fs::path path = slang_root / relative;
        const auto content = readFileText(path);
        ASSERT_TRUE(content.has_value()) << "missing " << path.string();
        const bool calls_resolved =
          content->find("resolved_metallic_roughness(") != std::string::npos
          || content->find("resolved_metallic_roughness_lod0(") != std::string::npos;
        EXPECT_TRUE(calls_resolved)
          << relative << " must derive roughness via resolved_metallic_roughness()/"
                          "resolved_metallic_roughness_lod0(), not its own copy of the mapping";
    }
}

// GltfLoader.cpp pins ObjMaterial::shininess to a fixed fallback value on the
// assumption that common/material_rules.slang's material_roughness() is the
// only shader reader (see the kFallbackShininess comment). This scans every
// checked-in .slang for `material.shininess` so a future shading path cannot
// start reading the pinned field elsewhere without this test noticing.
TEST(BuildIntegrity, MaterialShininessIsReadOnlyThroughMaterialRoughness)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    static const fs::path kAllowedReader = fs::path("common") / "material_rules.slang";
    static const std::regex kShininessRead(R"(\bmaterial\.shininess\b)");

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".slang") { continue; }

        const fs::path relative = fs::relative(path, slang_root);
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
                violations.push_back(fs::relative(path, repo_root).generic_string() + ':'
                                      + std::to_string(line_number) + ": " + line);
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " shader site(s) read material.shininess outside common/material_rules.slang - that field is pinned "
         "to a fixed fallback value by GltfLoader.cpp and is only meaningful through material_roughness()'s "
         "sentinel branch:"
      << joinViolations(violations);
}

// bloom.slang's fs_brightpass and tonemap.slang's fs_main must agree on
// where auto-exposure is applied: bloom pre-exposes in the bright pass (so
// its fixed THRESHOLD of 1.0 means something across the whole auto-exposure
// range), and tonemap must apply exposure to the raw HDR term only - never
// to the bloom term, which would double-expose it. See BACKLOG.md's
// "Threshold bloom in post-exposure units" entry for the reasoning.
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
        if (tonemap_source[pos] == '(') { ++depth; }
        else if (tonemap_source[pos] == ')') { --depth; }
    }
    ASSERT_EQ(depth, 0u) << "unbalanced parentheses after aces_tonemap( in " << tonemap_path.string();
    const std::string composite_expr = tonemap_source.substr(args_start, pos - 1 - args_start);

    // Split the composite's top-level '+' terms (none of them contain nested
    // '+' inside parens here, but track depth anyway so a future refactor
    // that adds one does not silently break this into the wrong terms).
    std::vector<std::string> terms;
    std::size_t term_start = 0;
    std::size_t term_depth = 0;
    for (std::size_t i = 0; i < composite_expr.size(); ++i) {
        const char ch = composite_expr[i];
        if (ch == '(') { ++term_depth; }
        else if (ch == ')') { --term_depth; }
        else if (ch == '+' && term_depth == 0) {
            terms.push_back(composite_expr.substr(term_start, i - term_start));
            term_start = i + 1;
        }
    }
    terms.push_back(composite_expr.substr(term_start));

    const auto bloom_term =
      std::find_if(terms.begin(), terms.end(), [](const std::string &term) { return term.find("bloom") != std::string::npos; });
    ASSERT_NE(bloom_term, terms.end())
      << "no term of aces_tonemap(...)'s composite references bloom: " << composite_expr;
    EXPECT_EQ(bloom_term->find("exposure"), std::string::npos)
      << "tonemap.slang must not multiply the bloom term by exposure - bloom.slang's fs_brightpass already "
         "pre-exposed it, so multiplying again here double-exposes bloom: "
      << *bloom_term;

    const auto hdr_term =
      std::find_if(terms.begin(), terms.end(), [](const std::string &term) { return term.find("hdr") != std::string::npos; });
    ASSERT_NE(hdr_term, terms.end()) << "no term of aces_tonemap(...)'s composite references hdr: " << composite_expr;
    EXPECT_NE(hdr_term->find("exposure"), std::string::npos)
      << "tonemap.slang must still apply exposure to the raw HDR term: " << *hdr_term;
}

TEST(BuildIntegrity, EveryPcfKernelBoundsChecksItsTaps)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    static const std::string kMarker = "SampleCmpLevelZero(";
    static const std::string kGeLower = ">= 0.0";
    static const std::string kLeUpper = "<= 1.0";

    int occurrences_checked = 0;
    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".slang") { continue; }

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
            ++occurrences_checked;

            // The enclosing statement runs from the previous statement
            // terminator (';' or '{') up to the call site - the tap coordinate
            // must be range-checked to [0,1] on both components somewhere in
            // that statement (e.g. the condition of a guarding ternary).
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
                std::size_t line_number = 1 + static_cast<std::size_t>(
                                                 std::count(stripped_text.begin(), stripped_text.begin() + static_cast<long>(marker_pos), '\n'));
                violations.push_back(fs::relative(path, repo_root).generic_string() + ':' + std::to_string(line_number)
                                      + ": SampleCmpLevelZero tap is not guarded by a [0,1] bounds check on both "
                                        "components of the tap coordinate in its enclosing statement");
            }
        }
    }

    EXPECT_EQ(occurrences_checked, 2)
      << "expected exactly 2 SampleCmpLevelZero call site(s), found " << occurrences_checked
      << " - update this count if a new PCF kernel was intentionally added, and confirm it also "
         "bounds-checks its tap coordinate";

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " PCF tap(s) sample past the shadow map's border unguarded - the ClampToEdge comparison "
         "sampler replicates the border texel's depth for an out-of-map tap rather than treating it "
         "as unshadowed, so every tap must be range-checked to [0,1] before sampling: "
      << joinViolations(violations);
}

namespace {

// A text-only call-graph reachability check for the whole Slang corpus (see
// EverySlangFunctionIsReachableFromAnEntryPoint below). No real Slang front
// end is available to this suite, so this is a tokenizer, not a parser: it
// extracts identifiers and a handful of punctuation marks and reasons about
// brace/paren nesting depth, nothing more.

// One identifier or one punctuation mark of interest ("(){}:;,") found while
// scanning a file. Whitespace, numeric literals, and every other character
// are not tokenized at all - see tokenize_slang below for why numeric
// literals need special handling.
struct SlangToken
{
    std::string text;
    std::size_t offset = 0;// into the comment/string-stripped text this token came from
    bool is_identifier = false;
};

// Tokenizes comment/string-stripped Slang source text into SlangTokens.
// Numeric literals (including a letter suffix, e.g. "8u", "1.0f") are
// consumed and discarded as a single opaque run rather than left for the
// generic scan - otherwise a suffix letter like the 'u' in "8u" would start
// its own spurious one-character identifier token.
std::vector<SlangToken> tokenize_slang(const std::string &text)
{
    std::vector<SlangToken> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (std::isspace(ch) != 0) {
            ++i;
            continue;
        }
        if (std::isdigit(ch) != 0) {
            while (i < text.size()
                   && (std::isalnum(static_cast<unsigned char>(text[i])) != 0 || text[i] == '.')) {
                ++i;
            }
            continue;
        }
        if (std::isalpha(ch) != 0 || ch == '_') {
            const std::size_t start = i;
            while (i < text.size() && is_identifier_char(text[i])) { ++i; }
            tokens.push_back({ text.substr(start, i - start), start, true });
            continue;
        }
        static const std::string kInteresting = "(){}:;,";
        if (kInteresting.find(static_cast<char>(ch)) != std::string::npos) {
            tokens.push_back({ std::string(1, static_cast<char>(ch)), i, false });
        }
        ++i;
    }
    return tokens;
}

// Blanks out double-quoted string contents (keeping the quotes and every
// other character in place, so offsets are unaffected) - only
// `[shader("...")]`-style attributes carry string literals in this corpus,
// but this is generic rather than special-cased to that attribute.
std::string strip_string_literals(const std::string &line)
{
    std::string result = line;
    bool in_string = false;
    for (char &ch : result) {
        if (ch == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) { ch = ' '; }
    }
    return result;
}

// True once `line`, trimmed, is empty.
bool is_blank_line(const std::string &line) { return line.find_first_not_of(" \t\r") == std::string::npos; }

// True if `line`, trimmed, starts with '[' - every Slang attribute
// ([shader(...)], [numthreads(...)], [vk::binding(...)], ...) in this corpus
// is written on its own line directly above the declaration it decorates.
bool is_attribute_line(const std::string &line)
{
    const auto first = line.find_first_not_of(" \t\r");
    return first != std::string::npos && line[first] == '[';
}

// Whether the contiguous run of blank/attribute lines immediately preceding
// stripped_lines[def_line_index] contains a `[shader("...")]` attribute -
// the marker that makes a function a call-graph root. Slang stacks multiple
// attributes directly on top of each other with no blank line between them
// (gpu_cull.slang's `cs_main` has `[shader("compute")]` then
// `[numthreads(64, 1, 1)]` immediately above it), so this walks the whole
// contiguous run rather than looking at only the single immediately
// preceding line, which would miss exactly that case.
bool preceded_by_shader_attribute(const std::vector<std::string> &stripped_lines, std::size_t def_line_index)
{
    std::size_t i = def_line_index;
    while (i > 0) {
        --i;
        const std::string &line = stripped_lines[i];
        if (is_blank_line(line)) { continue; }
        if (!is_attribute_line(line)) { break; }
        if (line.find("[shader(") != std::string::npos) { return true; }
    }
    return false;
}

// One function definition found somewhere under Resources/ShadersSlang.
struct SlangFunctionDef
{
    std::string name;
    std::string relative_file;// relative to Resources/ShadersSlang, forward slashes
    int line = 0;             // 1-based, the line the return type starts on
    bool is_root = false;     // preceded by a [shader("...")] attribute
    std::string body;         // comment/string-stripped body text, for call-graph edges
    std::string raw_def_line; // original (unstripped) text of `line`, for allowlist marker lookup
};

// Scans one already comment/string-stripped, tokenized file for function
// definitions: two consecutive identifier tokens (a return type and a name)
// immediately followed by '(' at brace depth 0. This is deliberately not a
// real parser - see EverySlangFunctionIsReachableFromAnEntryPoint's comment
// for why a text gate is enough here - but two lightweight checks keep it
// honest: the parameter list's matching ')' is located by paren-depth
// counting (so multi-line parameter lists such as rasterizer.slang's
// `vs_main` do not break it), and after skipping an optional
// " : SEMANTIC" clause the next token must be '{' - a bare declaration
// (next token ';') is not treated as a definition.
void collect_functions_from_file(const std::vector<std::string> &stripped_lines, const std::vector<std::string> &raw_lines,
                                  const std::string &relative_file, std::vector<SlangFunctionDef> &out)
{
    std::string text;
    std::vector<std::size_t> line_start_offsets;
    line_start_offsets.reserve(stripped_lines.size());
    for (const auto &line : stripped_lines) {
        line_start_offsets.push_back(text.size());
        text += line;
        text += '\n';
    }

    auto line_for_offset = [&](std::size_t offset) -> int {
        const auto it = std::upper_bound(line_start_offsets.begin(), line_start_offsets.end(), offset);
        return static_cast<int>(std::distance(line_start_offsets.begin(), it));// upper_bound - 1, then +1 for 1-based
    };

    const std::vector<SlangToken> tokens = tokenize_slang(text);

    int depth = 0;
    std::size_t t = 0;
    while (t < tokens.size()) {
        const SlangToken &tok = tokens[t];
        if (!tok.is_identifier) {
            if (tok.text == "{") { ++depth; }
            else if (tok.text == "}") { --depth; }
            ++t;
            continue;
        }

        if (depth == 0 && t + 2 < tokens.size() && tokens[t + 1].is_identifier && !tokens[t + 2].is_identifier
            && tokens[t + 2].text == "(") {
            // Find the parameter list's matching ')'.
            int paren_depth = 1;
            std::size_t j = t + 3;
            while (j < tokens.size() && paren_depth > 0) {
                if (!tokens[j].is_identifier && tokens[j].text == "(") { ++paren_depth; }
                else if (!tokens[j].is_identifier && tokens[j].text == ")") { --paren_depth; }
                ++j;
            }
            if (paren_depth == 0) {
                // Skip an optional " : SEMANTIC" clause.
                std::size_t k = j;
                if (k < tokens.size() && !tokens[k].is_identifier && tokens[k].text == ":") {
                    ++k;
                    if (k < tokens.size() && tokens[k].is_identifier) { ++k; }
                }
                if (k < tokens.size() && !tokens[k].is_identifier && tokens[k].text == "{") {
                    // Confirmed definition - locate the matching '}' for the body.
                    int body_depth = 1;
                    std::size_t m = k + 1;
                    while (m < tokens.size() && body_depth > 0) {
                        if (!tokens[m].is_identifier && tokens[m].text == "{") { ++body_depth; }
                        else if (!tokens[m].is_identifier && tokens[m].text == "}") { --body_depth; }
                        ++m;
                    }
                    const std::size_t body_begin_offset = tokens[k].offset;
                    const std::size_t body_end_offset =
                      (m > 0 && m <= tokens.size()) ? (tokens[m - 1].offset + 1) : text.size();

                    SlangFunctionDef fn;
                    fn.name = tokens[t + 1].text;
                    fn.relative_file = relative_file;
                    fn.line = line_for_offset(tok.offset);
                    fn.is_root = preceded_by_shader_attribute(stripped_lines, static_cast<std::size_t>(fn.line - 1));
                    fn.body = text.substr(body_begin_offset, body_end_offset - body_begin_offset);
                    fn.raw_def_line = (fn.line >= 1 && static_cast<std::size_t>(fn.line - 1) < raw_lines.size())
                                         ? raw_lines[static_cast<std::size_t>(fn.line - 1)]
                                         : std::string();
                    out.push_back(std::move(fn));

                    depth = 0;// back to top level once the function's own body has closed
                    t = m;
                    continue;
                }
            }
        }
        ++t;
    }
}

// Every function definition under Resources/ShadersSlang (excluding
// build/), analysed as one global corpus rather than per file - see
// EverySlangFunctionIsReachableFromAnEntryPoint for why.
std::vector<SlangFunctionDef> collect_slang_functions(const fs::path &slang_root)
{
    std::vector<SlangFunctionDef> functions;
    std::error_code error;
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".slang") { continue; }
        const std::string relative_path = fs::relative(path, slang_root).generic_string();
        if (relative_path.starts_with("build/")) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::vector<std::string> raw_lines;
        std::vector<std::string> stripped_lines;
        for (auto raw_line : *lines) {
            stripped_lines.push_back(strip_line_comment(strip_string_literals(raw_line)));
            raw_lines.push_back(std::move(raw_line));
        }

        collect_functions_from_file(stripped_lines, raw_lines, relative_path, functions);
    }
    return functions;
}

// A deliberate exception to EverySlangFunctionIsReachableFromAnEntryPoint,
// with the reason the function is not reachable from any [shader(...)]
// entry point. Every entry must be matched by a
// "// UNREACHABLE_SLANG_FUNCTION_OK: <marker>" trailing comment on the
// function's definition line (checked below), so an exemption cannot rot
// silently after the line it protects moves or is deleted.
struct UnreachableSlangAllowlistEntry
{
    std::string file;// relative to Resources/ShadersSlang/, forward slashes
    std::string marker;
};

const std::vector<UnreachableSlangAllowlistEntry> kUnreachableSlangAllowlist = {};

const std::string kUnreachableSlangMarkerPrefix = "UNREACHABLE_SLANG_FUNCTION_OK: ";

}// namespace

// Two shipped regressions in one week (the `ibl` batch XIII found, and the
// `forward` one immediately above in this file's history) shared the same
// signature: the .slang source still defined the helper, the emitted output
// no longer contained it, and nothing called it any more - a lost call site,
// not dead code that should have been deleted. This is the general form of
// that check: every function defined anywhere under Resources/ShadersSlang
// must be reachable, by name, from some `[shader("...")]` entry point.
//
// The corpus is analysed globally rather than per file because Slang has no
// preprocessor #include - modules are pulled in via `import` (e.g.
// ibl.slang's `import fullscreen;`), so a helper's caller routinely lives in
// a different file than its definition. Reachability is therefore also by
// NAME rather than by a fully resolved symbol: two unrelated functions that
// happen to share a name (ibl.slang's own `distribution_ggx` and
// common/brdf.slang's) are both treated as reachable if either name is
// called from a root, which can only produce false negatives (a genuinely
// dead function hiding behind a live same-named one), never false
// positives - an acceptable trade for a text gate with no real Slang front
// end behind it.
//
// A dead helper usually means a lost call site, not a helper that should be
// deleted: deleting `sky_radiance` (see forward.slang) would have "fixed"
// this gate and cemented that regression rather than catching it. Triage
// every finding - wire the call back up, or if it is a genuine one-off
// (a proven-out toolchain spike, a test fixture), justify it in
// kUnreachableSlangAllowlist above rather than deleting the function.
TEST(BuildIntegrity, EverySlangFunctionIsReachableFromAnEntryPoint)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    const std::vector<SlangFunctionDef> functions = collect_slang_functions(slang_root);

    // Self-verifying floors: EveryShaderSourceHasCompiledBinary has already
    // been burned once by a parser change that silently found nothing and
    // still read as a pass. These floors are well below the real counts (at
    // time of writing: ~90 functions, ~53 entry points) so ordinary shader
    // edits never bump into them, but a scan that finds (near-)zero of
    // either cannot pass silently.
    const int total_functions = static_cast<int>(functions.size());
    const int total_roots =
      static_cast<int>(std::count_if(functions.begin(), functions.end(), [](const auto &fn) { return fn.is_root; }));
    ASSERT_GT(total_functions, 40) << "found only " << total_functions
                                    << " Slang function definition(s) under " << slang_root.string()
                                    << " - the definition scan itself is broken";
    ASSERT_GT(total_roots, 20) << "found only " << total_roots
                                << " [shader(\"...\")] entry point(s) under " << slang_root.string()
                                << " - the root scan itself is broken";

    std::map<std::string, std::vector<std::size_t>> functions_by_name;
    for (std::size_t idx = 0; idx < functions.size(); ++idx) { functions_by_name[functions[idx].name].push_back(idx); }

    std::vector<bool> reachable(functions.size(), false);
    std::vector<std::size_t> worklist;
    for (std::size_t idx = 0; idx < functions.size(); ++idx) {
        if (functions[idx].is_root) {
            reachable[idx] = true;
            worklist.push_back(idx);
        }
    }

    while (!worklist.empty()) {
        const std::size_t idx = worklist.back();
        worklist.pop_back();

        std::set<std::string> called_names;
        for (const auto &tok : tokenize_slang(functions[idx].body)) {
            if (tok.is_identifier) { called_names.insert(tok.text); }
        }

        for (const auto &name : called_names) {
            const auto found = functions_by_name.find(name);
            if (found == functions_by_name.end()) { continue; }
            for (const std::size_t callee : found->second) {
                if (!reachable[callee]) {
                    reachable[callee] = true;
                    worklist.push_back(callee);
                }
            }
        }
    }

    std::vector<bool> allowlist_entry_matched(kUnreachableSlangAllowlist.size(), false);
    std::vector<std::string> violations;
    for (std::size_t idx = 0; idx < functions.size(); ++idx) {
        if (reachable[idx]) { continue; }
        const SlangFunctionDef &fn = functions[idx];

        int allowlist_index = -1;
        for (std::size_t a = 0; a < kUnreachableSlangAllowlist.size(); ++a) {
            const auto &entry = kUnreachableSlangAllowlist[a];
            if (entry.file != fn.relative_file) { continue; }
            if (fn.raw_def_line.find(kUnreachableSlangMarkerPrefix + entry.marker) == std::string::npos) { continue; }
            allowlist_index = static_cast<int>(a);
            break;
        }
        if (allowlist_index >= 0) {
            allowlist_entry_matched[static_cast<std::size_t>(allowlist_index)] = true;
            continue;
        }

        violations.push_back(fn.relative_file + ":" + std::to_string(fn.line) + ": " + fn.name);
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " Slang function(s) are not reachable, by name, from any [shader(\"...\")] entry point in "
      << slang_root.string()
      << " - this almost always means a lost call site (see this test's comment), not dead code to delete; wire "
         "the call back up, or if it is deliberate add a \"// UNREACHABLE_SLANG_FUNCTION_OK: <marker>\" comment "
         "on the definition line and a justified entry to kUnreachableSlangAllowlist above:"
      << joinViolations(violations);

    std::vector<std::string> dead_exemptions;
    for (std::size_t idx = 0; idx < kUnreachableSlangAllowlist.size(); ++idx) {
        if (!allowlist_entry_matched[idx]) {
            dead_exemptions.push_back(kUnreachableSlangAllowlist[idx].file + " (" + kUnreachableSlangAllowlist[idx].marker + ")");
        }
    }
    EXPECT_TRUE(dead_exemptions.empty())
      << dead_exemptions.size()
      << " kUnreachableSlangAllowlist entr(y/ies) matched no unreachable function - the exemption is dead and "
         "must be deleted:"
      << joinViolations(dead_exemptions);
}

namespace {

// Resolves the transitive closure of `import <module>;` lines starting from
// `entry_relative` (relative to slang_root, forward slashes). Every wgslMap
// source's imports resolve to Resources/ShadersSlang/common/<module>.slang
// today - see the grep in this task's history - so that is the only place
// looked up; a common/ module that itself imports another common/ module
// (e.g. cascaded_shadow.slang -> scene_types.slang) is still followed,
// because the worklist recurses into every file it adds.
std::set<std::string> resolve_slang_import_closure(const fs::path &slang_root, const std::string &entry_relative)
{
    std::set<std::string> file_set;
    std::vector<std::string> worklist{ entry_relative };
    static const std::regex kImportRe(R"(^\s*import\s+([A-Za-z_]\w*)\s*;)");

    while (!worklist.empty()) {
        const std::string relative = worklist.back();
        worklist.pop_back();
        if (!file_set.insert(relative).second) { continue; }// already visited

        const auto lines = readFileLines(slang_root / relative);
        if (!lines) { continue; }
        for (const auto &raw_line : *lines) {
            const std::string stripped = strip_line_comment(raw_line);
            std::smatch match;
            if (std::regex_search(stripped, match, kImportRe)) { worklist.push_back("common/" + match[1].str() + ".slang"); }
        }
    }
    return file_set;
}

// Every Slang function name reachable, by name, from a [shader("...")] entry
// point defined in `entry_relative` (the wgslMap source itself - a shared
// common/ module never carries an entry point), restricted to functions
// defined somewhere in `file_set`. This mirrors
// EverySlangFunctionIsReachableFromAnEntryPoint's worklist walk above, but
// scoped to one source's own import closure rather than the whole corpus:
// the combined WGSL emit for that source only ever contains ITS call graph,
// so a same-named function reachable only via some unrelated source (e.g.
// another wgslMap entry that happens to import the same common/ module) must
// not count here - that would produce a false negative, not a false
// positive, but it would defeat the point of restricting the scope at all.
std::set<std::string> slang_function_names_reachable_from_source(const std::vector<SlangFunctionDef> &all_functions,
                                                                   const std::set<std::string> &file_set,
                                                                   const std::string &entry_relative)
{
    std::vector<std::size_t> in_scope;
    for (std::size_t idx = 0; idx < all_functions.size(); ++idx) {
        if (file_set.contains(all_functions[idx].relative_file)) { in_scope.push_back(idx); }
    }

    std::map<std::string, std::vector<std::size_t>> functions_by_name;
    for (const std::size_t idx : in_scope) { functions_by_name[all_functions[idx].name].push_back(idx); }

    std::set<std::size_t> reachable;
    std::vector<std::size_t> worklist;
    for (const std::size_t idx : in_scope) {
        if (all_functions[idx].is_root && all_functions[idx].relative_file == entry_relative) {
            reachable.insert(idx);
            worklist.push_back(idx);
        }
    }

    while (!worklist.empty()) {
        const std::size_t idx = worklist.back();
        worklist.pop_back();

        std::set<std::string> called_names;
        for (const auto &tok : tokenize_slang(all_functions[idx].body)) {
            if (tok.is_identifier) { called_names.insert(tok.text); }
        }

        for (const auto &name : called_names) {
            const auto found = functions_by_name.find(name);
            if (found == functions_by_name.end()) { continue; }
            for (const std::size_t callee : found->second) {
                if (reachable.insert(callee).second) { worklist.push_back(callee); }
            }
        }
    }

    std::set<std::string> names;
    for (const std::size_t idx : reachable) { names.insert(all_functions[idx].name); }
    return names;
}

}// namespace

// mtimes (CheckedInWgslIsNotOlderThanItsSlangSource, above) cannot survive a
// `git clone`, so they are a local-iteration guard only, not a CI backstop -
// this is that backstop. For each wgslMap source, every Slang function
// reachable from its OWN [shader("...")] entry point(s) must appear, by
// name, in the checked-in destination WGSL: `fn <name>(` for an (unmangled)
// entry point itself, or `fn <name>_<digits>(` for a helper, since slangc's
// WGSL backend mangles every non-entry function with a numeric suffix. A
// lambert_diffuse-shaped regression - defined, called, but the emitted WGSL
// silently lost it on the next regenerate - fails loudly here instead of
// shipping. `fullscreen_vs` (imported by ibl.slang but never called, since
// ibl.slang defines its own vs_fullscreen with different winding) must NOT
// show up as a violation: the reachability restriction to `entry_relative`'s
// own import closure is what keeps it out of ibl.slang's reachable set in
// the first place.
TEST(BuildIntegrity, EveryReachableSlangFunctionSurvivesIntoItsCheckedInWgsl)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    const auto &manifest = shader_manifest(repo_root);
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    const std::vector<SlangFunctionDef> functions = collect_slang_functions(slang_root);
    ASSERT_GT(functions.size(), 40u) << "found only " << functions.size()
                                      << " Slang function definition(s) under " << slang_root.string()
                                      << " - the definition scan itself is broken";

    std::vector<std::string> violations;
    int checked_destinations = 0;
    int forward_reachable_count = 0;

    for (const auto &mapping : manifest->wgsl_map) {
        const fs::path dest = repo_root / mapping.dst_dir / mapping.wgsl_file;
        if (!fs::exists(dest)) { continue; }// OxidANT submodule not checked out here

        const auto dest_text_opt = readFileText(dest);
        ASSERT_TRUE(dest_text_opt.has_value()) << "could not open " << dest.string();
        const std::string &dest_text = *dest_text_opt;

        const std::set<std::string> file_set = resolve_slang_import_closure(slang_root, mapping.slang_source);
        const std::set<std::string> reachable_names =
          slang_function_names_reachable_from_source(functions, file_set, mapping.slang_source);

        ++checked_destinations;
        if (mapping.wgsl_file == "forward.wgsl") { forward_reachable_count = static_cast<int>(reachable_names.size()); }

        for (const auto &name : reachable_names) {
            const std::regex pattern("fn " + name + "(_[0-9]+)?\\(");
            if (!std::regex_search(dest_text, pattern)) {
                violations.push_back(mapping.slang_source + " -> " + fs::relative(dest, repo_root).string()
                                      + ": reachable function '" + name
                                      + "' is missing from the checked-in WGSL (regenerate with "
                                        "scripts/windows/Build-SlangShaders.ps1 or .sh)");
            }
        }
    }

    if (checked_destinations == 0) {
        GTEST_SKIP() << "none of the checked-in Rust-crate WGSL destinations exist - the OxidANT "
                        "submodule is likely not checked out here";
    }

    ASSERT_GE(checked_destinations, 8) << "only checked " << checked_destinations << " of "
                                        << manifest->wgsl_map.size()
                                        << " wgslMap destination(s) - most are missing, which is more than a "
                                           "submodule simply not being checked out";
    ASSERT_GT(forward_reachable_count, 8)
      << "found only " << forward_reachable_count
      << " function(s) reachable from forward.wgsl's own entry point(s) - the reachability scan itself is broken";

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " Slang function(s) reachable from a wgslMap source's own entry point(s) are missing from the "
         "checked-in WGSL that source generates: "
      << joinViolations(violations);
}

namespace {

// One `struct <Name> { ... }` definition found somewhere under
// Resources/ShadersSlang - the struct-name counterpart to SlangFunctionDef.
// Needed because a common module's public surface is not just its
// functions: ibl.slang imports fullscreen only for `FullscreenVsOut`, never
// calling `fullscreen_vs` at all (see
// EveryReachableSlangFunctionSurvivesIntoItsCheckedInWgsl's comment above) -
// a function-name-only version of the check below would flag that import as
// unused and be wrong.
struct SlangStructDef
{
    std::string name;
    std::string relative_file;// relative to Resources/ShadersSlang, forward slashes
};

// Scans one already comment/string-stripped file's tokens for `struct
// <Name>` pairs. Unlike collect_functions_from_file this needs no brace or
// paren bookkeeping: `struct` is a keyword that only ever precedes the
// type's name, so any "struct" identifier token immediately followed by
// another identifier token is a definition.
void collect_structs_from_file(const std::vector<std::string> &stripped_lines, const std::string &relative_file,
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

// Every relative path (forward slashes, relative to slang_root) of a
// non-generated .slang file under `slang_root`.
std::vector<std::string> collect_all_slang_relative_paths(const fs::path &slang_root)
{
    std::vector<std::string> paths;
    std::error_code error;
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".slang") { continue; }
        const std::string relative_path = fs::relative(path, slang_root).generic_string();
        if (relative_path.starts_with("build/")) { continue; }
        paths.push_back(relative_path);
    }
    return paths;
}

// Every `struct` definition under Resources/ShadersSlang (excluding
// build/), mirroring collect_slang_functions.
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

// Resolves `import <module_name>;` to the .slang file slangc would find:
// slangc searches -I paths in order (see Get-SlangIncludeArgument in
// WindowsSlang.Common.psm1) - the importing file's own directory first, then
// every directory under Resources/ShadersSlang. Two modules share a
// filename in this corpus today (common/noise.slang and compute/noise.slang
// - see tests/noise_test.slang, which imports the common/ one), so an exact
// same-directory match is tried first, then a common/ preference, matching
// how those two actually resolve in practice; anything left ambiguous falls
// back to the alphabetically-first candidate.
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

// forward.slang claimed to tonemap via `import aces;` while the actual
// tonemap pass lives entirely in tonemap/tonemap.slang - the import was dead
// weight left over from before that split, and the header comment above it
// was simply wrong (see the fix alongside this test). This is the general
// form of that check: for every `import <module>;` anywhere in the Slang
// corpus, the importing file must actually reference at least one of the
// module's exported function or struct names - otherwise the import (and
// any comment justifying it) is stale and should be deleted.
TEST(BuildIntegrity, EveryImportedSlangModuleIsUsed)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    const std::vector<std::string> all_relative_paths = collect_all_slang_relative_paths(slang_root);
    std::map<std::string, std::vector<std::string>> files_by_stem;
    for (const auto &relative_path : all_relative_paths) {
        files_by_stem[fs::path(relative_path).stem().string()].push_back(relative_path);
    }
    for (auto &entry : files_by_stem) { std::sort(entry.second.begin(), entry.second.end()); }

    const std::vector<SlangFunctionDef> functions = collect_slang_functions(slang_root);
    const std::vector<SlangStructDef> structs = collect_slang_structs(slang_root);

    std::map<std::string, std::vector<std::string>> exported_names_by_file;
    for (const auto &fn : functions) { exported_names_by_file[fn.relative_file].push_back(fn.name); }
    for (const auto &st : structs) { exported_names_by_file[st.relative_file].push_back(st.name); }

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
              && std::any_of(exported->second.begin(), exported->second.end(),
                              [&identifiers](const std::string &name) { return identifiers.contains(name); });
            if (!used) {
                violations.push_back(relative_path + ": import " + module_name + " (" + *resolved
                                      + ") is unused - none of its exported function or struct names appear in "
                                        "this file");
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

// histogram.slang used to compile to nothing (its manifest row was
// "disabled": true, kept only for documentation) while looking, to a casual
// reader, like a live shader - no gate could see that mismatch because every
// existing check starts from the manifest and walks outward, never from the
// filesystem inward. This is the inverse check: every .slang source with an
// entry point must be claimed by some enabled manifest row, so a source that
// compiles to nothing (or was never added at all) fails loudly instead of
// aging invisibly.
TEST(BuildIntegrity, EverySlangSourceWithAnEntryPointHasAnEnabledManifestRow)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    const auto &manifest = shader_manifest(repo_root);
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    int entry_point_sources_checked = 0;
    std::vector<std::string> violations;
    for (const std::string &relative_path : collect_all_slang_relative_paths(slang_root)) {
        // common/ is a module directory with no entry points of its own;
        // has_entry_point matches "[shader(" inside comments too, and
        // common/fullscreen.slang's fullscreen_vs usage-example comment has
        // one - an exclusion by convention, not a workaround for a real
        // entry point.
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

// The ten Slang sources feeding wgslMap used to say "Mirrors <x>.wgsl" in
// their header comments, describing a two-way relationship that stopped
// being true once the WGSL became a one-way build output of the Slang
// source - the same "source still says it does X" drift
// EveryImportedSlangModuleIsUsed and EverySlangFunctionIsReachableFromAnEntryPoint
// exist for, one level up: the file rather than the function. Cheap, exact,
// and it names the drift it prevents.
TEST(BuildIntegrity, NoGeneratedWgslSourceClaimsToMirrorItsOutput)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    const auto &manifest = shader_manifest(repo_root);
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";
    ASSERT_GE(manifest->wgsl_map.size(), 5U) << "found only " << manifest->wgsl_map.size()
                                              << " wgslMap entr(y/ies) - the manifest parse itself is broken";

    std::vector<std::string> violations;
    for (const auto &mapping : manifest->wgsl_map) {
        const auto content = readFileText(slang_root / mapping.slang_source);
        ASSERT_TRUE(content.has_value()) << "could not open " << mapping.slang_source;
        if (content->find("Mirrors ") != std::string::npos) { violations.push_back(mapping.slang_source); }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " wgslMap source(s) still claim to \"Mirror\" their generated output instead of saying they generate "
         "it:"
      << joinViolations(violations);
}

namespace {

// One `export module <name>;` declaration found in an .ixx file.
struct ModuleInterface
{
    std::string name;
    fs::path path;// absolute path of the declaring .ixx
};

// Extracts the module name from a line of the form "<prefix><name>;",
// trimming surrounding whitespace. Returns an empty string if `line` does
// not start with `prefix` or has no terminating ';'.
std::string extract_module_name(const std::string &line, const std::string &prefix)
{
    if (line.compare(0, prefix.size(), prefix) != 0) { return {}; }
    const std::string rest = line.substr(prefix.size());
    const auto semicolon = rest.find(';');
    if (semicolon == std::string::npos) { return {}; }

    const std::string name = rest.substr(0, semicolon);
    const auto first = name.find_first_not_of(" \t");
    if (first == std::string::npos) { return {}; }
    const auto last = name.find_last_not_of(" \t");
    return name.substr(first, last - first + 1);
}

// Every `export module <name>;` interface under `src_root`. Primary module
// interfaces only (the project has no module partitions today); a file is
// assumed to declare at most one module.
std::vector<ModuleInterface> collect_module_interfaces(const fs::path &src_root)
{
    std::vector<ModuleInterface> modules;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".ixx") { continue; }

        const auto lines = readFileLines(it->path());
        if (!lines) { continue; }
        for (const auto &line : *lines) {
            const std::string name = extract_module_name(line, "export module ");
            if (!name.empty()) {
                modules.push_back({ name, it->path() });
                break;
            }
        }
    }
    return modules;
}

// name -> generic-string paths of every .cpp/.ixx file under `roots`
// containing an `import <name>;` or `export import <name>;` line.
std::map<std::string, std::set<std::string>> collect_module_importers(const std::vector<fs::path> &roots)
{
    std::map<std::string, std::set<std::string>> importers;
    std::error_code error;
    for (const auto &root : roots) {
        for (fs::recursive_directory_iterator it(root, error), end; it != end; it.increment(error)) {
            if (error) { break; }
            const fs::path &path = it->path();
            if (!it->is_regular_file(error)) { continue; }
            const auto extension = path.extension();
            if (extension != ".cpp" && extension != ".ixx") { continue; }

            const auto lines = readFileLines(path);
            if (!lines) { continue; }
            for (const auto &line : *lines) {
                std::string name = extract_module_name(line, "export import ");
                if (name.empty()) { name = extract_module_name(line, "import "); }
                if (!name.empty()) { importers[name].insert(path.generic_string()); }
            }
        }
    }
    return importers;
}

}// namespace

// A module interface (.ixx) that nothing imports is still compiled into
// every build - Src/GraphicsEngineVulkan/CMakeLists.txt globs *.ixx into
// VulkanEngineCore's CXX_MODULES file set unconditionally - and is rescanned
// by clang-scan-deps on every configure. Two such modules
// (kataglyphis.shared.scene.vertex / kataglyphis.shared.scene.obj_material)
// existed for no reason but that nobody deleted their wrapper .ixx after the
// equivalent kataglyphis.vulkan.vertex / kataglyphis.vulkan.obj_material
// modules were introduced - a standing trap, since both pairs exported the
// same `::Vertex` / `::ObjMaterial` type. This test asserts the set of
// interfaces with zero importers stays empty, so a module that loses its
// last importer is caught instead of silently becoming build-time-only
// weight.
TEST(BuildIntegrity, EveryModuleInterfaceIsImported)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    const std::vector<ModuleInterface> modules = collect_module_interfaces(src_root);
    ASSERT_GT(modules.size(), 10U) << "found suspiciously few module interfaces under " << src_root.string()
                                   << " - the scan itself is likely broken";

    const std::vector<fs::path> search_roots = { src_root, repo_root / "Test" };
    const std::map<std::string, std::set<std::string>> importers = collect_module_importers(search_roots);

    // Modules with a justified reason to have no importer (e.g. a
    // consciously-rootless entry point module). Empty today - an entry here
    // requires a written reason, not just a failing test.
    const std::set<std::string> allowed_rootless_modules = {};

    std::vector<std::string> unimported;
    for (const auto &iface : modules) {
        if (allowed_rootless_modules.contains(iface.name)) { continue; }

        const auto it = importers.find(iface.name);
        const std::string declaring_path = iface.path.generic_string();
        const bool imported_elsewhere = it != importers.end()
          && std::any_of(it->second.begin(), it->second.end(), [&](const std::string &importer_path) {
                 return importer_path != declaring_path;
             });

        if (!imported_elsewhere) {
            unimported.push_back(iface.name + " (" + fs::relative(iface.path, repo_root).generic_string() + ")");
        }
    }

    EXPECT_TRUE(unimported.empty())
      << unimported.size()
      << " module interface(s) have no importer anywhere under Src/ or Test/ - either delete the dead module, "
         "or if it is deliberately rootless, add a justified entry to allowed_rootless_modules above: "
      << joinViolations(unimported);
}

namespace {

// file (relative to Src/GraphicsEngineVulkan/, forward slashes) : marker ->
// a deliberate exception to the rule below, with the reason it does not
// need ASSERT_VULKAN. This is not a way to silence a real gap - every entry
// must be justified, and every entry must be matched by a
// "// UNCHECKED_VULKAN_RESULT_OK: <marker>" trailing comment on the exempted
// line in the source (checked by VulkanCreationResultsAreChecked below), so
// an exemption cannot rot silently after the line it protects moves or is
// deleted.
struct AllowlistEntry
{
    std::string file;
    std::string marker;
};

// Both former entries here (ShaderHelper.cpp's already-fatal-abort-above and
// Clouds.cpp's noise-dispatch-skip-on-failure) became dead once the ".result"
// check in the ±8-line window was accepted as satisfying the gate on its own
// (see VulkanCreationResultsAreChecked below): each site's explicit
// `if (x.result != ...)` a few lines above its `.value` read already
// satisfies the gate without a special-case exemption.
const std::vector<AllowlistEntry> kCheckedResultAllowlist = {};

const std::string kUncheckedResultMarkerPrefix = "UNCHECKED_VULKAN_RESULT_OK: ";

// Returns the index of the kCheckedResultAllowlist entry whose marker is
// present as a trailing "// UNCHECKED_VULKAN_RESULT_OK: <marker>" comment on
// `line`, or -1 if none matches. Anchoring on the marker text rather than the
// line number means an unrelated edit above the exempted line cannot turn a
// live exemption into a false positive (or, worse, a silently-wrong one).
int allowlisted_result_check_index(const std::string &relative_file, const std::string &line)
{
    for (std::size_t idx = 0; idx < kCheckedResultAllowlist.size(); ++idx) {
        const AllowlistEntry &entry = kCheckedResultAllowlist[idx];
        if (entry.file != relative_file) { continue; }
        if (line.find(kUncheckedResultMarkerPrefix + entry.marker) != std::string::npos) {
            return static_cast<int>(idx);
        }
    }
    return -1;
}

// True if `line` calls something that looks like a Vulkan/VMA creation or
// allocation function: the keyword immediately followed by an uppercase
// letter (Vulkan's camelCase naming, e.g. "createDescriptorPool",
// "vmaCreateAllocator") and, after any further identifier characters, an
// opening paren. The uppercase requirement is what tells a real Vulkan call
// apart from an unrelated identifier that merely contains the keyword, such
// as std::filesystem::create_directories(...) or a "..._create_info"
// struct-field reference - both continue with '_', not a capital letter.
// The left-boundary check similarly rejects "recreateSwapChain(", where
// "create" is not a word start. Reuses is_identifier_char from the
// constant-parsing helpers above.
bool looks_like_creation_call(const std::string &line)
{
    static const std::vector<std::string> keywords = { "create", "Create", "allocate", "Allocate" };
    for (const auto &keyword : keywords) {
        std::size_t pos = 0;
        while ((pos = line.find(keyword, pos)) != std::string::npos) {
            const bool left_ok = pos == 0 || !is_identifier_char(line[pos - 1]);
            const std::size_t after_keyword = pos + keyword.size();
            const bool camel_case_continuation =
              after_keyword < line.size() && std::isupper(static_cast<unsigned char>(line[after_keyword])) != 0;
            if (left_ok && camel_case_continuation) {
                std::size_t scan = after_keyword;
                while (scan < line.size() && is_identifier_char(line[scan])) { ++scan; }
                while (scan < line.size() && std::isspace(static_cast<unsigned char>(line[scan])) != 0) { ++scan; }
                if (scan < line.size() && line[scan] == '(') { return true; }
            }
            pos += keyword.size();
        }
    }
    return false;
}

// Same shape as looks_like_creation_call, keyed on the query verbs instead
// of the creation/allocation ones. `enumeratePhysicalDevices`,
// `getSurfaceCapabilitiesKHR` and friends return a vk::ResultValue exactly
// like a create*/allocate* call does, but the naming-based gate above never
// saw them - every surface and enumeration query silently slipped past it.
bool looks_like_query_call(const std::string &line)
{
    static const std::vector<std::string> keywords = { "get", "Get", "enumerate", "Enumerate" };
    for (const auto &keyword : keywords) {
        std::size_t pos = 0;
        while ((pos = line.find(keyword, pos)) != std::string::npos) {
            const bool left_ok = pos == 0 || !is_identifier_char(line[pos - 1]);
            const std::size_t after_keyword = pos + keyword.size();
            const bool camel_case_continuation =
              after_keyword < line.size() && std::isupper(static_cast<unsigned char>(line[after_keyword])) != 0;
            if (left_ok && camel_case_continuation) {
                std::size_t scan = after_keyword;
                while (scan < line.size() && is_identifier_char(line[scan])) { ++scan; }
                while (scan < line.size() && std::isspace(static_cast<unsigned char>(line[scan])) != 0) { ++scan; }
                if (scan < line.size() && line[scan] == '(') { return true; }
            }
            pos += keyword.size();
        }
    }
    return false;
}

}// namespace

// The instance-extension check used to detect a missing extension, log it,
// and then build a vk::Instance from a createInstance() call whose
// ResultValue was never checked one line later - a failure would silently
// continue with a null-handle instance. Exceptions are disabled project-wide
// (VULKAN_HPP_NO_EXCEPTIONS), so ASSERT_VULKAN's log-critical-and-abort is the
// only fail-fast mechanism available; a missed check is a straight path to a
// null-handle dereference downstream.
//
// This test scans every .cpp under Src/GraphicsEngineVulkan/ for a
// vk::ResultValue::value read with no ASSERT_VULKAN nearby. It cannot
// understand control flow, so it looks in a window around the ".value" read
// on both sides - the check-then-assign idiom used throughout the codebase
// puts ASSERT_VULKAN just above the read, but Rasterizer.cpp and
// DeferredRasterizer.cpp instead assign inside an `if (result == eSuccess)`
// with ASSERT_VULKAN in the `else`, which is a few lines *below* the read.
TEST(BuildIntegrity, VulkanCreationResultsAreChecked)
{
    // Self-verifying floor: assert the widened matcher actually fires before
    // relying on it below, so a future edit that neuters it fails loudly
    // instead of quietly reporting zero violations forever.
    EXPECT_TRUE(looks_like_query_call("  auto x = d.getSurfaceFormatsKHR(*s).value;"));
    EXPECT_FALSE(looks_like_query_call("  auto x = getter(y).value;"));

    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path engine_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(engine_root)) << "missing " << engine_root.string();

    constexpr int kWindow = 8;
    std::vector<std::string> violations;
    std::vector<bool> allowlist_entry_matched(kCheckedResultAllowlist.size(), false);
    std::error_code error;
    for (fs::recursive_directory_iterator it(engine_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".cpp") { continue; }

        const auto lines_opt = readFileLines(path);
        if (!lines_opt) { continue; }
        const auto &lines = *lines_opt;

        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find(".value") == std::string::npos) { continue; }

            const std::size_t window_begin = (i >= static_cast<std::size_t>(kWindow)) ? i - kWindow : 0;
            const std::size_t window_end = std::min(lines.size() - 1, i + static_cast<std::size_t>(kWindow));

            bool triggered = false;
            bool asserted = false;
            for (std::size_t w = window_begin; w <= window_end; ++w) {
                if (w <= i && (looks_like_creation_call(lines[w]) || looks_like_query_call(lines[w]))) {
                    triggered = true;
                }
                if (lines[w].find("ASSERT_VULKAN") != std::string::npos) { asserted = true; }
                // A query (unlike a creation/allocation call) is legitimately
                // handled with an explicit `if (x.result != ...)` rather than
                // an abort. Exclude the .value line itself so an unrelated
                // ".result" mention there cannot retroactively satisfy a
                // check that was never actually performed.
                if (w != i && lines[w].find(".result") != std::string::npos) { asserted = true; }
            }
            if (!triggered || asserted) { continue; }

            const std::string relative_file = fs::relative(path, engine_root).generic_string();
            const int allowlist_index = allowlisted_result_check_index(relative_file, lines[i]);
            if (allowlist_index >= 0) {
                allowlist_entry_matched[static_cast<std::size_t>(allowlist_index)] = true;
                continue;
            }

            violations.push_back(relative_file + ":" + std::to_string(i + 1) + ": " + lines[i]);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " Vulkan creation/allocation/query result(s) read via .value with no ASSERT_VULKAN and no "
         "\".result\" check nearby (exceptions are disabled project-wide, so a failed call must either "
         "abort via ASSERT_VULKAN or be handled explicitly via an `if (x.result != ...)` check rather "
         "than continue with a silently default/empty value; or, for a deliberate exception, add a "
         "\"// UNCHECKED_VULKAN_RESULT_OK: <marker>\" comment on the line and a justified entry "
         "to kCheckedResultAllowlist above):"
      << joinViolations(violations);

    std::vector<std::string> dead_exemptions;
    for (std::size_t idx = 0; idx < kCheckedResultAllowlist.size(); ++idx) {
        if (!allowlist_entry_matched[idx]) {
            dead_exemptions.push_back(kCheckedResultAllowlist[idx].file + " (" + kCheckedResultAllowlist[idx].marker + ")");
        }
    }

    EXPECT_TRUE(dead_exemptions.empty())
      << dead_exemptions.size()
      << " kCheckedResultAllowlist entr(y/ies) matched no \"// UNCHECKED_VULKAN_RESULT_OK: <marker>\" "
         "comment in the source - the exemption is dead and must be deleted:"
      << joinViolations(dead_exemptions);
}

namespace {

// The one non-call occurrence of "beginCommandBuffer(" under Src/ is the
// function's own definition signature in CommandBufferManager.cpp.
bool is_begin_command_buffer_definition_line(const std::string &line)
{
    return line.find("beginCommandBuffer(vk::Device device") != std::string::npos;
}

// Looks backward from `call_line` (inclusive) up to `lookback` lines for a
// "vk::CommandBuffer <name>" declaration and returns <name>, or an empty
// string if none is found. The declaration is always on the call line itself
// (single-line "vk::CommandBuffer x = ...beginCommandBuffer(...)") or one to
// two lines above it, when the declaration and the call are split across a
// line break.
std::string declared_command_buffer_name(const std::vector<std::string> &lines, std::size_t call_line, int lookback)
{
    static const std::string marker = "vk::CommandBuffer ";
    const std::size_t begin =
      (call_line >= static_cast<std::size_t>(lookback)) ? call_line - static_cast<std::size_t>(lookback) : 0;
    for (std::size_t w = call_line + 1; w-- > begin;) {
        const std::string &line = lines[w];
        const std::size_t marker_pos = line.find(marker);
        if (marker_pos == std::string::npos) { continue; }
        const std::size_t name_begin = marker_pos + marker.size();
        std::size_t name_end = name_begin;
        while (name_end < line.size() && is_identifier_char(line[name_end])) { ++name_end; }
        if (name_end > name_begin) { return line.substr(name_begin, name_end - name_begin); }
    }
    return "";
}

}// namespace

// beginCommandBuffer (CommandBufferManager.cpp) documents a null vk::CommandBuffer
// return on either the pool-null guard or an allocate/begin failure. Exceptions
// are disabled project-wide, so that null handle is the only failure signal a
// caller has - recording commands into it, or handing it to
// endAndSubmitCommandBuffer's queue.submit, is undefined behaviour. This scans
// every .cpp under Src/ for a beginCommandBuffer( call and requires a
// "!<the assigned variable>" null-check within the following kWindow lines,
// mirroring VulkanCreationResultsAreChecked's window-scan shape above.
TEST(BuildIntegrity, EveryBeginCommandBufferResultIsChecked)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    constexpr int kLookback = 3;
    constexpr int kWindow = 6;
    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".cpp") { continue; }

        const auto lines_opt = readFileLines(path);
        if (!lines_opt) { continue; }
        const auto &lines = *lines_opt;

        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find("beginCommandBuffer(") == std::string::npos) { continue; }
            if (is_begin_command_buffer_definition_line(lines[i])) { continue; }

            const std::string relative_file = fs::relative(path, repo_root).generic_string();
            const std::string var_name = declared_command_buffer_name(lines, i, kLookback);
            if (var_name.empty()) {
                violations.push_back(relative_file + ":" + std::to_string(i + 1)
                  + ": beginCommandBuffer( call whose assigned vk::CommandBuffer variable could not be located - "
                    "cannot verify a null-check exists");
                continue;
            }

            const std::string negated_check = "!" + var_name;
            const std::size_t window_end = std::min(lines.size() - 1, i + static_cast<std::size_t>(kWindow));
            bool checked = false;
            for (std::size_t w = i; w <= window_end; ++w) {
                if (lines[w].find(negated_check) != std::string::npos) {
                    checked = true;
                    break;
                }
            }

            if (!checked) { violations.push_back(relative_file + ":" + std::to_string(i + 1) + ": " + lines[i]); }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " beginCommandBuffer() call(s) with no null-check on the returned command buffer within " << kWindow
      << " lines - beginCommandBuffer documents a null return on allocate/begin failure, and exceptions are "
         "disabled project-wide, so that null handle is the only failure signal available; recording into it "
         "or submitting it is undefined behaviour:"
      << joinViolations(violations);
}

// endAndSubmitCommandBuffer's submit half used to return void, so a failed
// queue.submit was invisible to every caller - compactBLAS in particular
// would destroy the (never-copied) original BLAS right after a submit that
// silently failed. It now returns bool; this scans every .cpp under Src/ for
// an endAndSubmitCommandBuffer( call and requires the result to be either
// assigned to a variable, used in a condition, or explicitly discarded via
// static_cast<void>(...) - mirroring EveryBeginCommandBufferResultIsChecked
// above, but the qualifying forms differ because most call sites have
// nothing to unwind on failure and legitimately discard the result.
TEST(BuildIntegrity, EveryEndAndSubmitCommandBufferResultIsChecked)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".cpp") { continue; }

        const auto lines_opt = readFileLines(path);
        if (!lines_opt) { continue; }
        const auto &lines = *lines_opt;

        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::string &line = lines[i];
            const std::size_t call_pos = line.find("endAndSubmitCommandBuffer(");
            if (call_pos == std::string::npos) { continue; }
            // The function's own definition signature, not a call.
            if (line.find("endAndSubmitCommandBuffer(vk::Device device") != std::string::npos) { continue; }

            const std::string relative_file = fs::relative(path, repo_root).generic_string();
            const std::string prefix = line.substr(0, call_pos);

            const bool discarded = prefix.find("static_cast<void>(") != std::string::npos;
            const bool assigned = prefix.find(" = ") != std::string::npos;
            const bool in_condition = prefix.find("if (") != std::string::npos
              || prefix.find("if(") != std::string::npos || prefix.find("while (") != std::string::npos;

            if (!discarded && !assigned && !in_condition) {
                violations.push_back(relative_file + ":" + std::to_string(i + 1) + ": " + line);
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " endAndSubmitCommandBuffer(...) call(s) whose bool result is neither assigned, used in a condition, "
         "nor explicitly static_cast<void>-discarded - a failed submit must not be silently ignored:"
      << joinViolations(violations);
}

// 0c4d2faa added the null-check gate above (EveryBeginCommandBufferResultIsChecked)
// for every beginCommandBuffer() call, but a checked return is not the same as a
// handled failure: ASManager::createTLAS used to index blas[model_index]
// unconditionally, and Clouds::createStorageTexture used to hand a null texture
// back into createTextures/createDescriptorSets/recreateFrameResources and four
// VulkanRenderer call sites. This pins the fix: createBLAS reports failure to its
// caller, createTLAS refuses to index a short BLAS vector, and the clouds storage
// texture path no longer has an escaping null.
TEST(BuildIntegrity, CommandBufferFailurePathsDoNotLeaveHalfBuiltResources)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path as_manager_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "accelerationStructures" / "ASManager.cpp";
    const auto as_manager_source_opt = readFileText(as_manager_path);
    ASSERT_TRUE(as_manager_source_opt.has_value()) << "missing " << as_manager_path.string();
    const std::string &as_manager_source = *as_manager_source_opt;

    EXPECT_NE(as_manager_source.find("bool Kataglyphis::VulkanRendererInternals::ASManager::createBLAS("),
      std::string::npos)
      << "ASManager::createBLAS must be declared returning bool so its caller can react to a failed build";

    {
        const std::string needle = "if (!createBLAS(";
        EXPECT_NE(as_manager_source.find(needle), std::string::npos)
          << "ASManager::createASForScene must guard its createBLAS(...) call with an if (!...) check";
    }

    {
        const std::size_t create_tlas_pos =
          as_manager_source.find("Kataglyphis::VulkanRendererInternals::ASManager::createTLAS(");
        ASSERT_NE(create_tlas_pos, std::string::npos) << "could not locate ASManager::createTLAS definition";
        const std::size_t body_start = as_manager_source.find('{', create_tlas_pos);
        ASSERT_NE(body_start, std::string::npos);
        const std::size_t first_index = as_manager_source.find("blas[", body_start);
        ASSERT_NE(first_index, std::string::npos) << "createTLAS no longer indexes blas[...]; update this test";
        const std::size_t size_guard = as_manager_source.find("blas.size()", body_start);
        EXPECT_NE(size_guard, std::string::npos) << "createTLAS must check blas.size() before indexing blas[...]";
        EXPECT_LT(size_guard, first_index)
          << "createTLAS's blas.size() guard must appear before the first blas[...] index";
    }

    const fs::path clouds_path = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "atmospheric_effects"
                                  / "clouds" / "Clouds.cpp";
    const auto clouds_source_opt = readFileText(clouds_path);
    ASSERT_TRUE(clouds_source_opt.has_value()) << "missing " << clouds_path.string();
    const std::string &clouds_source = *clouds_source_opt;

    EXPECT_EQ(clouds_source.find("return nullptr;"), std::string::npos)
      << "Clouds.cpp must not return a null texture from createStorageTexture - a half-initialized clouds "
         "subsystem has no defined rendering behaviour, so a failed command buffer must ASSERT_VULKAN instead";
}

// uploadRgba used to discard endAndSubmitCommandBuffer's result with
// static_cast<void>, so a failed submit still returned true and left an
// unwritten image bound into the descriptor array. This pins the fix: the
// call site in Texture.cpp must consume the result (not this call site
// specifically, discards are legitimate elsewhere - see
// EveryEndAndSubmitCommandBufferResultIsChecked above), and createDefaultTexture
// must forward uploadRgba's bool instead of swallowing it as void.
TEST(BuildIntegrity, TextureUploadConsumesTheSubmitResult)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path texture_cpp_path = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "Texture.cpp";
    const auto texture_cpp_source_opt = readFileText(texture_cpp_path);
    ASSERT_TRUE(texture_cpp_source_opt.has_value()) << "missing " << texture_cpp_path.string();
    const std::string &texture_cpp_source = *texture_cpp_source_opt;

    const std::size_t call_pos = texture_cpp_source.find("endAndSubmitCommandBuffer(");
    ASSERT_NE(call_pos, std::string::npos) << "could not locate endAndSubmitCommandBuffer( call in Texture.cpp";
    const std::size_t line_start = texture_cpp_source.rfind('\n', call_pos);
    const std::string prefix =
      texture_cpp_source.substr(line_start == std::string::npos ? 0 : line_start + 1, call_pos - (line_start + 1));
    EXPECT_EQ(prefix.find("static_cast<void>("), std::string::npos)
      << "Texture.cpp's endAndSubmitCommandBuffer(...) call must consume the submit result instead of "
         "discarding it with static_cast<void> - a failed upload must not be reported as a successful texture";

    const fs::path texture_ixx_path = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "Texture.ixx";
    const auto texture_ixx_source_opt = readFileText(texture_ixx_path);
    ASSERT_TRUE(texture_ixx_source_opt.has_value()) << "missing " << texture_ixx_path.string();
    const std::string &texture_ixx_source = *texture_ixx_source_opt;

    EXPECT_NE(texture_ixx_source.find("bool createDefaultTexture("), std::string::npos)
      << "Texture::createDefaultTexture must be declared returning bool, forwarding uploadRgba's failure to "
         "its callers instead of swallowing it as void";
}

// Third and last instalment of the family TextureUploadConsumesTheSubmitResult
// pins: the vertex/index/TLAS/object-description uploads and the skybox
// cubemap used to report success they never verified, because
// VulkanBufferManager::copyBuffer and createBufferAndUploadVectorOnDevice
// discarded endAndSubmitCommandBuffer's result with static_cast<void>. A
// failed transfer produces undefined buffer contents that the BLAS builder
// and every shader read as geometry - this must surface as a bool, not
// vanish silently.
TEST(BuildIntegrity, GeometryAndCubemapUploadsConsumeTheSubmitResult)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path buffer_manager_cpp_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "vulkan_base" / "VulkanBufferManager.cpp";
    const auto buffer_manager_cpp_source_opt = readFileText(buffer_manager_cpp_path);
    ASSERT_TRUE(buffer_manager_cpp_source_opt.has_value()) << "missing " << buffer_manager_cpp_path.string();
    const std::string &buffer_manager_cpp_source = *buffer_manager_cpp_source_opt;

    {
        const std::size_t call_pos = buffer_manager_cpp_source.find("endAndSubmitCommandBuffer(");
        ASSERT_NE(call_pos, std::string::npos)
          << "could not locate endAndSubmitCommandBuffer( call in VulkanBufferManager.cpp";
        const std::size_t line_start = buffer_manager_cpp_source.rfind('\n', call_pos);
        const std::string prefix = buffer_manager_cpp_source.substr(
          line_start == std::string::npos ? 0 : line_start + 1, call_pos - (line_start + 1));
        EXPECT_EQ(prefix.find("static_cast<void>("), std::string::npos)
          << "VulkanBufferManager.cpp's endAndSubmitCommandBuffer(...) call must consume the submit result "
             "instead of discarding it with static_cast<void> - a failed transfer must not be reported as a "
             "successful upload";
    }

    const fs::path sky_box_cpp_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "sky_box" / "SkyBox.cpp";
    const auto sky_box_cpp_source_opt = readFileText(sky_box_cpp_path);
    ASSERT_TRUE(sky_box_cpp_source_opt.has_value()) << "missing " << sky_box_cpp_path.string();
    const std::string &sky_box_cpp_source = *sky_box_cpp_source_opt;

    {
        const std::size_t call_pos = sky_box_cpp_source.find("endAndSubmitCommandBuffer(");
        ASSERT_NE(call_pos, std::string::npos) << "could not locate endAndSubmitCommandBuffer( call in SkyBox.cpp";
        const std::size_t line_start = sky_box_cpp_source.rfind('\n', call_pos);
        const std::string prefix = sky_box_cpp_source.substr(
          line_start == std::string::npos ? 0 : line_start + 1, call_pos - (line_start + 1));
        EXPECT_EQ(prefix.find("static_cast<void>("), std::string::npos)
          << "SkyBox.cpp's endAndSubmitCommandBuffer(...) call must consume the submit result instead of "
             "discarding it with static_cast<void> - a failed cubemap upload must not write a descriptor "
             "pointed at an unfilled image";
    }

    const fs::path buffer_manager_ixx_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "vulkan_base" / "VulkanBufferManager.ixx";
    const auto buffer_manager_ixx_source_opt = readFileText(buffer_manager_ixx_path);
    ASSERT_TRUE(buffer_manager_ixx_source_opt.has_value()) << "missing " << buffer_manager_ixx_path.string();
    const std::string &buffer_manager_ixx_source = *buffer_manager_ixx_source_opt;

    EXPECT_NE(buffer_manager_ixx_source.find("bool copyBuffer("), std::string::npos)
      << "VulkanBufferManager::copyBuffer must be declared returning bool";
    EXPECT_NE(buffer_manager_ixx_source.find("bool createBufferAndUploadVectorOnDevice("), std::string::npos)
      << "VulkanBufferManager::createBufferAndUploadVectorOnDevice must be declared returning bool";
    EXPECT_EQ(buffer_manager_ixx_source.find("copyImageBuffer(vk::Device device,"), std::string::npos)
      << "the dead seven-argument device overload of copyImageBuffer must be deleted - only the static "
         "command-buffer overload should remain";
}

// drawFrame used to have three post-acquire early returns that left
// frameSync's imageAvailableSemaphore() signaled with no pending wait, then
// handed that same semaphore straight back to the next frame's
// vkAcquireNextImageKHR (which requires an unsignaled semaphore) - a
// validation-layer hazard on every framebuffer-size-change, out-of-range
// image index or record_commands failure. abort_frame_after_acquire()
// (drawFrame's second lambda, next to abort_frame_with_fatal_error) fixes
// this by recreating the swap chain - which destroys and recreates every
// semaphore - before returning. This scans every bare `return;` between the
// acquireNextImageKHR( call and frameSync.advanceFrame() and requires it be
// preceded (within the previous three non-blank lines) by a call to
// abort_frame_after_acquire( or abort_frame_with_fatal_error(, so a future
// early return added to this span cannot reintroduce the leak silently.
TEST(BuildIntegrity, EveryPostAcquireEarlyReturnRetiresTheAcquireSemaphore)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path renderer_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "VulkanRenderer.cpp";
    const auto renderer_lines = readFileLines(renderer_path);
    ASSERT_TRUE(renderer_lines.has_value()) << "missing " << renderer_path.string();
    const auto &lines = *renderer_lines;

    std::size_t acquire_line = lines.size();
    std::size_t advance_frame_line = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (acquire_line == lines.size() && lines[i].find("acquireNextImageKHR(") != std::string::npos) {
            acquire_line = i;
        }
        if (lines[i].find("frameSync.advanceFrame();") != std::string::npos) {
            advance_frame_line = i;
            break;
        }
    }
    ASSERT_NE(acquire_line, lines.size()) << "could not locate the acquireNextImageKHR( call in drawFrame";
    ASSERT_NE(advance_frame_line, lines.size()) << "could not locate the frameSync.advanceFrame(); call in drawFrame";
    ASSERT_LT(acquire_line, advance_frame_line)
      << "acquireNextImageKHR( must appear before frameSync.advanceFrame(); - drawFrame was restructured; "
         "update this test";

    const auto trim = [](const std::string &s) -> std::string {
        const std::size_t begin = s.find_first_not_of(" \t");
        if (begin == std::string::npos) { return ""; }
        const std::size_t end = s.find_last_not_of(" \t");
        return s.substr(begin, end - begin + 1);
    };

    constexpr int kLookback = 3;
    std::vector<std::string> violations;
    for (std::size_t i = acquire_line; i <= advance_frame_line; ++i) {
        if (trim(lines[i]) != "return;") { continue; }

        bool retires_semaphore = false;
        int checked = 0;
        for (std::size_t w = i; w-- > 0 && checked < kLookback;) {
            if (trim(lines[w]).empty()) { continue; }
            ++checked;
            if (lines[w].find("abort_frame_after_acquire(") != std::string::npos
                || lines[w].find("abort_frame_with_fatal_error(") != std::string::npos) {
                retires_semaphore = true;
                break;
            }
        }

        if (!retires_semaphore) {
            violations.push_back(
              "VulkanRenderer.cpp:" + std::to_string(i + 1)
              + ": bare \"return;\" in drawFrame's post-acquire span with no abort_frame_after_acquire(/"
                "abort_frame_with_fatal_error( call in the previous " + std::to_string(kLookback) + " non-blank lines");
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " post-acquire early return(s) in drawFrame do not retire imageAvailableSemaphore() before returning "
         "(via abort_frame_after_acquire( or abort_frame_with_fatal_error():"
      << joinViolations(violations);
}

// docs/gpu-golden-testing.md's golden-suite counts have already had to be
// corrected twice by hand (commits 1cd6b8b5, e2767bb1), and a planner batch
// once found the doc claiming 21 tests when the suite held 28. Pins the
// doc's `<!-- golden-counts: ... -->` marker against a pure file-I/O count of
// TEST(GoldenRender, ...) / TEST(Integration, ...) definitions, following the
// same "parse two sources, compare, fail with both numbers" pattern this
// suite uses throughout. Must never run the golden tests themselves to count them
// - they need a GPU the CI container does not have.
// Scans docs/gpu-golden-testing.md for a bare integer written immediately
// next to "runnable" / "defined" / "`Integration` tests" outside the
// golden-counts marker line - a second hand-typed copy of a number the
// marker already tracks. This is exactly what let the doc's counts drift out
// of sync three times before the marker existed: the marker got fixed, the
// prose sentence next to it did not. Pure substr scanning, no regex, matching
// the rest of this file's parsing style.
std::vector<std::string> find_bare_golden_count_copies(const fs::path &doc_path)
{
    std::vector<std::string> violations;
    const auto lines = readFileLines(doc_path);
    if (!lines) { return violations; }

    static const std::array<std::string, 3> kKeywords = { "runnable", "defined", "`Integration` tests" };

    for (std::size_t line_no = 0; line_no < lines->size(); ++line_no) {
        const std::string &line = (*lines)[line_no];
        if (line.find("<!-- golden-counts:") != std::string::npos) { continue; }

        for (const auto &keyword : kKeywords) {
            const std::size_t pos = line.find(keyword);
            if (pos == std::string::npos) { continue; }

            std::size_t digit_end = pos;
            while (digit_end > 0 && line[digit_end - 1] == ' ') { --digit_end; }
            std::size_t digit_start = digit_end;
            while (digit_start > 0 && std::isdigit(static_cast<unsigned char>(line[digit_start - 1]))) {
                --digit_start;
            }

            if (digit_start != digit_end) {
                violations.push_back("line " + std::to_string(line_no + 1) + ": \"" + line + "\"");
            }
        }
    }
    return violations;
}

TEST(BuildIntegrity, GoldenTestCountsInDocsMatchTheSuite)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "gpu-golden-testing.md";
    if (!fs::exists(doc_path)) {
        GTEST_SKIP() << "could not open " << doc_path.string() << " - not running from the repo root?";
    }

    const auto marker = parse_golden_counts_marker(doc_path);
    ASSERT_TRUE(marker.has_value())
      << doc_path.string()
      << " is missing its '<!-- golden-counts: defined=N runnable=N integration=N total=N excluded=N -->' marker "
         "line, or one of its five fields - a deleted marker must fail this test, not silently pass";

    const fs::path tests_dir = repo_root / "Test" / "commit" / "VulkanEngine";
    const std::vector<std::string> golden_tests = collect_suite_test_names(tests_dir, "GoldenRender");
    const std::vector<std::string> integration_tests = collect_suite_test_names(tests_dir, "Integration");

    const int counted_defined = static_cast<int>(golden_tests.size());
    const int counted_runnable = static_cast<int>(std::count_if(golden_tests.begin(), golden_tests.end(),
      [](const std::string &name) { return !name.starts_with("DISABLED_"); }));
    const int counted_integration = static_cast<int>(integration_tests.size());

    EXPECT_EQ(marker->defined, counted_defined)
      << doc_path.string() << "'s golden-counts marker says defined=" << marker->defined << " but "
      << tests_dir.string() << " has " << counted_defined << " TEST(GoldenRender, ...) definitions";
    EXPECT_EQ(marker->runnable, counted_runnable)
      << doc_path.string() << "'s golden-counts marker says runnable=" << marker->runnable << " but "
      << counted_runnable << " of " << counted_defined
      << " TEST(GoldenRender, ...) definitions do not start with DISABLED_";
    EXPECT_EQ(marker->integration, counted_integration)
      << doc_path.string() << "'s golden-counts marker says integration=" << marker->integration << " but "
      << tests_dir.string() << " has " << counted_integration << " TEST(Integration, ...) definitions";
    EXPECT_EQ(marker->runnable + marker->integration, marker->total)
      << doc_path.string() << "'s golden-counts marker is internally inconsistent: runnable(" << marker->runnable
      << ") + integration(" << marker->integration << ") != total(" << marker->total << ")";

    const auto exclusion_filter = parse_golden_test_exclusion_filter(doc_path);
    ASSERT_TRUE(exclusion_filter.has_value())
      << doc_path.string() << " is missing its '--gtest_filter=' line in the \"Known issue\" section";

    const int counted_excluded = static_cast<int>(exclusion_filter->size());
    EXPECT_EQ(marker->excluded, counted_excluded)
      << doc_path.string() << "'s golden-counts marker says excluded=" << marker->excluded
      << " but the --gtest_filter= line's ':-'-prefixed section names " << counted_excluded << " test(s)";

    for (const auto &[suite, name] : *exclusion_filter) {
        const std::vector<std::string> &suite_tests = suite == "GoldenRender" ? golden_tests : integration_tests;
        EXPECT_TRUE(std::find(suite_tests.begin(), suite_tests.end(), name) != suite_tests.end())
          << doc_path.string() << "'s --gtest_filter= excludes " << suite << "." << name << " but no such TEST("
          << suite << ", " << name << ") exists in " << tests_dir.string();
    }

    const auto bare_copies = find_bare_golden_count_copies(doc_path);
    EXPECT_TRUE(bare_copies.empty())
      << doc_path.string()
      << " hand-types a count next to 'runnable'/'defined'/'`Integration` tests' outside the golden-counts marker "
         "- the marker is the single source of truth, a second copy just drifts out of sync with it:"
      << joinViolations(bare_copies);
}

// docs/path-tracing.md's "## Verification" section drifted out of sync with
// the golden suite: it counted four PT-facing goldens and its "Open work"
// section still asked for a furnace-mode golden after
// PathTracingPassesTheWhiteFurnaceTest had already shipped it, alongside
// RaytracedLargeMeshDoesNotLoseTheDevice - six PT-facing goldens exist, not
// four. Pins the doc's `<!-- pt-goldens: ... -->` marker against a pure
// file-I/O scan of TEST(GoldenRender, PathTracing...)/
// TEST(GoldenRender, Raytraced...) definitions, following
// GoldenTestCountsInDocsMatchTheSuite's "parse two sources, compare, fail
// with both sides named" pattern, and mirrors
// RendererImprovementLogDoesNotAskForShippedWork's shipped-work check for
// the furnace toggle specifically.
TEST(BuildIntegrity, PathTracingDocMatchesTheGoldenSuite)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "path-tracing.md";
    if (!fs::exists(doc_path)) {
        GTEST_SKIP() << "could not open " << doc_path.string() << " - not running from the repo root?";
    }

    const auto marker_names = parse_pt_goldens_marker(doc_path);
    ASSERT_TRUE(marker_names.has_value())
      << doc_path.string()
      << " is missing its '<!-- pt-goldens: name1, name2, ... -->' marker line, or it lists zero names - a "
         "deleted marker must fail this test, not silently pass";

    const fs::path tests_dir = repo_root / "Test" / "commit" / "VulkanEngine";
    const std::vector<std::string> golden_tests = collect_suite_test_names(tests_dir, "GoldenRender");

    std::vector<std::string> pt_tests;
    for (const auto &name : golden_tests) {
        if (name.starts_with("PathTracing") || name.starts_with("Raytraced")) { pt_tests.push_back(name); }
    }

    std::vector<std::string> missing_from_doc;
    for (const auto &name : pt_tests) {
        if (std::find(marker_names->begin(), marker_names->end(), name) == marker_names->end()) {
            missing_from_doc.push_back(name);
        }
    }
    std::vector<std::string> extra_in_doc;
    for (const auto &name : *marker_names) {
        if (std::find(pt_tests.begin(), pt_tests.end(), name) == pt_tests.end()) { extra_in_doc.push_back(name); }
    }

    const auto joined = [](const std::vector<std::string> &names) -> std::string {
        std::string result;
        for (const auto &name : names) { result += name + " "; }
        return result;
    };

    EXPECT_TRUE(missing_from_doc.empty() && extra_in_doc.empty())
      << doc_path.string() << "'s '<!-- pt-goldens: ... -->' marker is out of sync with " << tests_dir.string()
      << "'s TEST(GoldenRender, PathTracing...)/TEST(GoldenRender, Raytraced...) definitions - missing from doc: ["
      << joined(missing_from_doc) << "], extra in doc: [" << joined(extra_in_doc) << "]";

    const fs::path pathtracing_cpp_path = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "PathTracing.cpp";
    const auto cpp_content_opt = readFileText(pathtracing_cpp_path);
    ASSERT_TRUE(cpp_content_opt.has_value()) << "could not open " << pathtracing_cpp_path.string();
    const std::string &cpp_content = *cpp_content_opt;

    const auto doc_content_opt = readFileText(doc_path);
    ASSERT_TRUE(doc_content_opt.has_value()) << "could not open " << doc_path.string();
    const std::string &doc_content = *doc_content_opt;

    const bool shader_ships_furnace = cpp_content.find("KATAGLYPHIS_PT_FURNACE") != std::string::npos;
    const bool doc_still_wants_toggle = doc_content.find("wants a uniform-environment toggle") != std::string::npos;
    EXPECT_FALSE(shader_ships_furnace && doc_still_wants_toggle)
      << doc_path.string()
      << " still asks for a uniform-environment furnace toggle (\"wants a uniform-environment toggle\"), but "
      << pathtracing_cpp_path.string()
      << " already ships it (\"KATAGLYPHIS_PT_FURNACE\") - the doc is asking for shipped work.";
}

// Parses docs/model-loading.md's `<!-- max-texture-count: N -->` marker line.
// Returns std::nullopt if the marker line, or its value, is missing - a
// deleted or malformed marker must fail the calling test, not skip it.
std::optional<int> parse_max_texture_count_marker(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::regex kMarkerPattern(R"(<!--\s*max-texture-count:\s*(\d+)\s*-->)");

    for (const auto &line : *lines) {
        std::smatch match;
        if (std::regex_search(line, match, kMarkerPattern)) { return std::stoi(match[1].str()); }
    }
    return std::nullopt;
}

// Parses `const int MAX_TEXTURE_COUNT = <N>;` out of
// common/host_device_shared_vars.hpp by plain file I/O, not by including the
// header - the point is to catch the header changing out from under the doc.
std::optional<int> parse_max_texture_count_header(const fs::path &header_path)
{
    const auto lines = readFileLines(header_path);
    if (!lines) { return std::nullopt; }

    static const std::regex kMaxTextureCountPattern(R"(const\s+int\s+MAX_TEXTURE_COUNT\s*=\s*(\d+)\s*;)");

    for (const auto &line : *lines) {
        std::smatch match;
        if (std::regex_search(line, match, kMaxTextureCountPattern)) { return std::stoi(match[1].str()); }
    }
    return std::nullopt;
}

// docs/model-loading.md's "Textures, samplers and the 128-slot budget"
// section pins MAX_TEXTURE_COUNT in prose; pins the doc's
// `<!-- max-texture-count: N -->` marker against a pure file-I/O read of the
// header constant so the two cannot silently drift apart, following the same
// "parse two sources, compare, fail with both numbers" pattern as
// GoldenTestCountsInDocsMatchTheSuite above.
TEST(BuildIntegrity, MaxTextureCountInDocsMatchesTheHeader)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "model-loading.md";
    if (!fs::exists(doc_path)) {
        GTEST_SKIP() << "could not open " << doc_path.string() << " - not running from the repo root?";
    }

    const auto doc_value = parse_max_texture_count_marker(doc_path);
    ASSERT_TRUE(doc_value.has_value())
      << doc_path.string() << " is missing its '<!-- max-texture-count: N -->' marker line - a deleted marker must "
                              "fail this test, not silently pass";

    const fs::path header_path = repo_root / "Src" / "GraphicsEngineVulkan" / "common" / "host_device_shared_vars.hpp";
    const auto header_value = parse_max_texture_count_header(header_path);
    ASSERT_TRUE(header_value.has_value())
      << header_path.string() << " does not define 'const int MAX_TEXTURE_COUNT = <N>;'";

    EXPECT_EQ(*doc_value, *header_value)
      << doc_path.string() << "'s max-texture-count marker says " << *doc_value << " but " << header_path.string()
      << " defines MAX_TEXTURE_COUNT = " << *header_value;
}

// docs/model-loading.md used to be the one doc in the tree written to cite
// `file:line` locations, and this gate was scoped to it alone - but the same
// rotting shape had already spread into Src/ and the shaders unobserved:
// eighteen sites, at least eight already pointing at unrelated code by the
// time they were found (a function moves ten lines and the citation now
// points at unrelated code, silently). Widened to scan every comment under
// Src/ and Resources/ShadersSlang/ too, then widened again to Test/, the
// most heavily hand-commented tree in the repo and the last one left
// unscanned. docs/ as a whole is still NOT scanned: docs/cpp-renderer-
// improvements.md is the sole exemption, a chronological log where a
// citation pinned to a historical commit is legitimate, and BACKLOG.md is
// not scanned at all. Every other doc, and now every source/shader/test
// comment, cites symbol names instead.
//
// kFileLinePattern alone missed the same-file shorthand, because it
// requires a `filename.ext` before the colon. That shorthand rots exactly
// the same way (a function moves, the number now points at unrelated code)
// and had already rotted at multiple sites by the time it was found, so
// kBareLinePattern catches a bare colon followed by digits (optionally a
// dash and more digits) wherever it is not itself preceded by a digit
// (which would make it part of an unrelated ratio-style token or a second
// colon in a namespace or URL scheme).
TEST(BuildIntegrity, SourceAndDocsCiteSymbolsNotLineNumbers)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::regex kFileLinePattern(R"([A-Za-z_/.-]+\.(cpp|ixx|hpp|slang|wgsl|rs):[0-9]+)");
    static const std::regex kBareLinePattern(R"((^|[\s(`\[]):[0-9]+(-[0-9]+)?([^0-9]|$))");

    std::vector<std::string> violations;
    auto scan_file = [&](const fs::path &path) {
        const auto content = readFileText(path);
        if (!content.has_value()) { return; }
        const std::string relative = fs::relative(path, repo_root).generic_string();
        auto begin = std::sregex_iterator(content->begin(), content->end(), kFileLinePattern);
        for (auto it = begin; it != std::sregex_iterator(); ++it) {
            violations.push_back(relative + ": " + it->str());
        }
        auto bare_begin = std::sregex_iterator(content->begin(), content->end(), kBareLinePattern);
        for (auto it = bare_begin; it != std::sregex_iterator(); ++it) {
            violations.push_back(relative + ": " + it->str());
        }
    };

    scan_file(repo_root / "docs" / "model-loading.md");
    scan_file(repo_root / "docs" / "clouds.md");

    static constexpr std::array<const char *, 3> kSrcExtensions{ ".cpp", ".hpp", ".ixx" };
    std::error_code error;
    for (fs::recursive_directory_iterator it(repo_root / "Src", error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error)) { continue; }
        const std::string extension = it->path().extension().string();
        if (std::find(kSrcExtensions.begin(), kSrcExtensions.end(), extension) == kSrcExtensions.end()) { continue; }
        scan_file(it->path());
    }

    for (fs::recursive_directory_iterator it(repo_root / "Resources" / "ShadersSlang", error), end; it != end;
         it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".slang") { continue; }
        scan_file(it->path());
    }

    for (fs::recursive_directory_iterator it(repo_root / "Test", error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error)) { continue; }
        const std::string extension = it->path().extension().string();
        if (std::find(kSrcExtensions.begin(), kSrcExtensions.end(), extension) == kSrcExtensions.end()) { continue; }
        scan_file(it->path());
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " file:line location(s), including the same-file `:NNN` shorthand, across Src/, "
         "Resources/ShadersSlang/, Test/ and docs/model-loading.md, which rot within days - cite the function or "
         "member name instead:"
      << joinViolations(violations);
}

// docs/model-loading.md's "Material fields and where they come from" table
// claims to cover every ObjMaterial member; nothing enforced that claim, so a
// member has now been appended without a row twice. Held here as the same
// hand-maintained-list-plus-gate shape ObjMaterial_natural (above) already
// uses, so the two lists fail together when a member is appended without
// either being updated.
TEST(BuildIntegrity, ModelLoadingDocDocumentsEveryObjMaterialMember)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "model-loading.md";
    const auto doc_content = readFileText(doc_path);
    ASSERT_TRUE(doc_content.has_value()) << "could not open " << doc_path.string();

    static constexpr std::array<const char *, 22> kObjMaterialMembers{ "diffuse", "emission", "shininess",
        "dissolve", "textureID", "alphaCutoff", "uv_transform_row0", "uv_transform_row1", "metallic", "roughness",
        "emissiveTextureID", "normalTextureID", "normalScale", "metallicRoughnessTextureID",
        "normal_uv_transform_row0", "normal_uv_transform_row1", "metallic_roughness_uv_transform_row0",
        "metallic_roughness_uv_transform_row1", "emissive_uv_transform_row0", "emissive_uv_transform_row1",
        "unlit", "alphaTextureID" };

    const auto table_start = doc_content->find("Material fields and where they come from");
    ASSERT_NE(table_start, std::string::npos)
      << doc_path.string() << " is missing its \"Material fields and where they come from\" section";
    const std::string table_text = doc_content->substr(table_start);

    // A "_row0" / "_row1" pair is documented as one table row, and the table
    // is free to spell the second half out in full ("`foo_row1`") or as the
    // shorthand it actually uses for three of the four pairs ("`_row1`", right
    // next to "`foo_row0`" on the same line) - both are "documented", so a
    // "_row1" member is satisfied by either spelling on the row0 member's line.
    std::vector<std::string> missing;
    for (const char *member : kObjMaterialMembers) {
        const std::string full = std::string("`") + member + "`";
        if (table_text.find(full) != std::string::npos) { continue; }

        const std::string_view member_view{ member };
        constexpr std::string_view kRow1Suffix = "_row1";
        if (member_view.size() > kRow1Suffix.size()
            && member_view.substr(member_view.size() - kRow1Suffix.size()) == kRow1Suffix) {
            const std::string row0_name = std::string(member_view.substr(0, member_view.size() - kRow1Suffix.size()))
              + "_row0";
            const auto row0_pos = table_text.find(std::string("`") + row0_name + "`");
            if (row0_pos != std::string::npos) {
                const auto line_end = table_text.find('\n', row0_pos);
                const auto line = table_text.substr(
                  row0_pos, line_end == std::string::npos ? std::string::npos : line_end - row0_pos);
                if (line.find("`_row1`") != std::string::npos) { continue; }
            }
        }

        missing.emplace_back(member);
    }

    EXPECT_TRUE(missing.empty()) << doc_path.string()
                                  << "'s material table is missing a row for the following ObjMaterial member(s):"
                                  << joinViolations(missing);
}

// docs/model-loading.md's "srgb" row hand-summarises which `.mtl` directives
// are sRGB-uploaded vs. linear; nothing enforced that summary against the
// per-slot rows (textureID/emissiveTextureID/normalTextureID/
// metallicRoughnessTextureID) that actually name those directives, so
// `map_Ke` went missing from the srgb row three commits after
// ObjLoader.cpp's emissive slot started resolving it. Every backtick-quoted
// `map_...` directive named in a *TextureID row's `.mtl` cell must also
// appear (case-insensitively) in the `srgb` row's `.mtl` cell - a mechanical
// consequence of the table's own content, not a second hand-maintained list.
TEST(BuildIntegrity, ModelLoadingDocSrgbRowCoversEveryObjTextureDirective)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "model-loading.md";
    const auto doc_content = readFileText(doc_path);
    ASSERT_TRUE(doc_content.has_value()) << "could not open " << doc_path.string();

    const auto table_start = doc_content->find("Material fields and where they come from");
    ASSERT_NE(table_start, std::string::npos)
      << doc_path.string() << " is missing its \"Material fields and where they come from\" section";
    const auto table_end = doc_content->find("\n## ", table_start);
    const std::string table_text = doc_content->substr(
      table_start, table_end == std::string::npos ? std::string::npos : table_end - table_start);

    // Split the table into "| cell | cell | ... |" rows, each row into its
    // pipe-delimited cells (Member / glTF source / .mtl source / Read by).
    // Non-table prose lines (no leading '|') are skipped, so the heading's
    // own prose paragraph before the table does not confuse row 0/1 below.
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
                cell = first == std::string::npos ? ""
                                                   : cell.substr(first, cell.find_last_not_of(" \t") - first + 1);
                cells.push_back(std::move(cell));
                pos = end + 1;
            }
            rows.push_back(std::move(cells));
        }
    }
    // Row 0 is the header ("| Member | ... |"), row 1 the "| --- | ... |"
    // separator; data rows start at index 2.
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

        const bool is_texture_id_row = member == "textureID"
          || (member.size() > 9 && member.compare(member.size() - 9, 9, "TextureID") == 0);
        if (!is_texture_id_row) { continue; }

        for (auto it = std::sregex_iterator(cells[2].begin(), cells[2].end(), kBacktickToken);
             it != std::sregex_iterator(); ++it) {
            const std::string token = (*it)[1].str();
            if (token.rfind("map_", 0) == 0) { required_directives.push_back(token); }
        }
    }

    ASSERT_FALSE(srgb_mtl_cell.empty())
      << doc_path.string() << "'s material table has no `srgb` row (or its `.mtl` cell is empty)";

    std::string srgb_mtl_cell_lower = srgb_mtl_cell;
    std::transform(srgb_mtl_cell_lower.begin(), srgb_mtl_cell_lower.end(), srgb_mtl_cell_lower.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::vector<std::string> missing;
    for (const std::string &directive : required_directives) {
        std::string needle = directive;
        std::transform(
          needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (srgb_mtl_cell_lower.find(needle) == std::string::npos) { missing.push_back(directive); }
    }
    std::sort(missing.begin(), missing.end());
    missing.erase(std::unique(missing.begin(), missing.end()), missing.end());

    EXPECT_TRUE(missing.empty())
      << doc_path.string() << "'s `srgb` row's `.mtl` cell (\"" << srgb_mtl_cell
      << "\") is missing the following directive(s) named in a *TextureID row's `.mtl` cell:" << joinViolations(missing, "\n  add `", "` to the srgb row's .mtl column");
}

// docs/shader-sharing.md's "Known glTF loader divergences" section claims to
// be the place the C++ and Rust glTF loaders stay honest with each other, but
// nothing enforced that claim - it shipped with one bullet while at least
// four other real divergences (KHR_materials_unlit, occlusionTexture/
// occlusionStrength, per-slot KHR_texture_transform, alphaMode BLEND) went
// unrecorded. Same hand-maintained-list-plus-gate shape as
// ModelLoadingDocDocumentsEveryObjMaterialMember (above): this only checks
// that every key has a row, not that the row's content is accurate.
TEST(BuildIntegrity, ShaderSharingDocCoversEveryKnownLoaderDivergence)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "shader-sharing.md";
    const auto doc_content = readFileText(doc_path);
    ASSERT_TRUE(doc_content.has_value()) << "could not open " << doc_path.string();

    static constexpr std::array<const char *, 6> kDivergenceKeys{ "TEXCOORD_0", "KHR_materials_unlit",
        "occlusionTexture", "KHR_texture_transform", "BLEND", "Pr" };

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

// Parses docs/code-quality.md's `<!-- format-drift-denominator: N -->` marker
// line. Returns std::nullopt if the marker line, or its value, is missing -
// a deleted or malformed marker must fail the calling test, not skip it.
std::optional<int> parse_format_drift_denominator_marker(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::regex kMarkerPattern(R"(<!--\s*format-drift-denominator:\s*(\d+)\s*-->)");

    for (const auto &line : *lines) {
        std::smatch match;
        if (std::regex_search(line, match, kMarkerPattern)) { return std::stoi(match[1].str()); }
    }
    return std::nullopt;
}

// Counts files with the eight extensions Get-ProjectCppFiles (the container
// build's clang-format check) tracks, under one root, skipping any `build*`
// directory the same way that PowerShell helper's git-less fallback does.
std::size_t count_cpp_sources(const fs::path &root)
{
    static const std::set<std::string> kCppExtensions = { ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".ixx" };

    std::size_t count = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end; it != end;
         it.increment(error)) {
        if (error) { break; }
        if (it->is_directory(error) && it->path().filename().string().starts_with("build")) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(error) || !kCppExtensions.contains(it->path().extension().string())) { continue; }
        ++count;
    }
    return count;
}

// docs/code-quality.md's "Known state" section pins the total tracked C/C++
// source count (the denominator of its X-of-Y formatting-drift figure) in a
// `<!-- format-drift-denominator: N -->` marker so the figure can be checked
// mechanically even though the deviating count itself needs clang-format
// (unavailable in the Linux CI lane) to re-measure. This mirrors
// MaxTextureCountInDocsMatchesTheHeader's "parse two sources, compare, fail
// with both numbers" pattern, but pins the doc against a live directory walk
// instead of a header constant - the same drift that let 72/125 rot into
// 77/136 and then the actual 140/211 undetected for weeks.
//
// This only guards the denominator. The deviating numerator still needs a
// human to re-run clang-format and update the doc by hand; a green run here
// says nothing about whether that numerator is still accurate.
TEST(BuildIntegrity, FormatDriftDenominatorMatchesTheTrackedSourceCount)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "code-quality.md";
    if (!fs::exists(doc_path)) {
        GTEST_SKIP() << "could not open " << doc_path.string() << " - not running from the repo root?";
    }

    const auto doc_value = parse_format_drift_denominator_marker(doc_path);
    ASSERT_TRUE(doc_value.has_value())
      << doc_path.string()
      << " is missing its '<!-- format-drift-denominator: N -->' marker line - a deleted marker must fail this "
         "test, not silently pass";

    const std::size_t actual_count = count_cpp_sources(repo_root / "Src") + count_cpp_sources(repo_root / "Test");

    EXPECT_EQ(static_cast<std::size_t>(*doc_value), actual_count)
      << doc_path.string() << "'s format-drift-denominator marker says " << *doc_value << " but Src/ + Test/ "
      << "currently contain " << actual_count
      << " tracked C/C++ sources - re-run the clang-format drift measurement in docs/code-quality.md's \"Known "
         "state\" section and update both the marker and the X-of-Y prose (this test only pins the "
         "denominator, not the deviating count).";
}

// Parses docs/shader-sharing.md's `<!-- shader-targets:begin -->` /
// `:end` marker table - one `| \`<file>\` | spirv|wgsl |` row per Slang
// entry-point source. Returns std::nullopt if the marker pair is missing, so
// a deleted marker block fails the calling test instead of comparing against
// an empty (vacuously matching) map.
std::optional<std::map<std::string, std::string>> parse_shader_targets_marker(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::regex kRowPattern(R"(\|\s*`([^`]+)`\s*\|\s*(spirv|wgsl)\s*\|)");

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

// Parses docs/shader-sharing.md's `<!-- shared-module-targets:begin -->` /
// `:end` marker table - one `| \`common/<name>.slang\` | <target> |` row,
// where <target> is spirv, wgsl, both, or (unused). Mirrors
// parse_shader_targets_marker above: returns std::nullopt if the marker pair
// is missing, so a deleted marker block fails the calling test instead of
// comparing against an empty (vacuously matching) map.
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

// For every Resources/ShadersSlang/common/*.slang module, the union of
// targets ('spirv', 'wgsl') over every enabled manifest entry-point source -
// tests/*.slang included, they are real dual-emit consumers and exactly why
// brdf/noise are genuinely both-target - whose import_closure reaches it.
// A module with no consumer at all (present under common/ but not currently
// imported by anything compiled) maps to "(unused)"; two targets collapse to
// "both"; one target is reported as-is.
std::map<std::string, std::string> shared_module_target_truth(const fs::path &slang_root, const ShaderManifestData &manifest)
{
    std::map<std::string, std::set<std::string>> module_targets;

    std::error_code dir_error;
    for (fs::directory_iterator it(slang_root / "common", dir_error), end; it != end; it.increment(dir_error)) {
        if (dir_error) { break; }
        if (!it->is_regular_file(dir_error) || it->path().extension() != ".slang") { continue; }
        module_targets["common/" + it->path().filename().string()];
    }

    for (const auto &[source, targets] : manifest.file_targets) {
        for (const auto &import_path : import_closure(slang_root, slang_root / source)) {
            std::error_code rel_error;
            const std::string relative = fs::relative(import_path, slang_root, rel_error).generic_string();
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

// docs/shader-sharing.md's shader-targets table claims, per Slang
// entry-point source, which single target it compiles to (spirv for the C++
// Vulkan engine, wgsl for the Rust WebGPU renderer). shader-manifest.json is
// the actual source of truth Build-SlangShaders.ps1/.sh read from, so this
// pins the doc against it the same way MaxTextureCountInDocsMatchesTheHeader
// pins a doc constant against its header - a previous revision of this doc
// claimed ten WGSL-only shaders were "compiled to both targets", which this
// test would have caught immediately.
//
// tests/*.slang (brdf_test.slang, noise_test.slang) are excluded on both
// sides: they are CI dual-emit smoke tests documented separately in the
// "CI guards" paragraph, not production entry points, and they are the one
// case that genuinely does compile to both targets - which is exactly why
// they do not belong in a table whose two columns are spirv-only/wgsl-only.
// histogram.wgsl is excluded too: it has no Slang source (hand-written WGSL
// fallback), so it can never appear in shader-manifest.json's manifest[].
TEST(BuildIntegrity, ShaderSharingDocMatchesTheManifestTargets)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "shader-sharing.md";
    if (!fs::exists(doc_path)) {
        GTEST_SKIP() << "could not open " << doc_path.string() << " - not running from the repo root?";
    }

    const auto doc_targets = parse_shader_targets_marker(doc_path);
    ASSERT_TRUE(doc_targets.has_value())
      << doc_path.string()
      << " is missing its '<!-- shader-targets:begin -->' / '<!-- shader-targets:end -->' marker block";

    const auto &manifest = shader_manifest(repo_root);
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    std::map<std::string, std::string> truth;
    std::vector<std::string> ambiguous;
    for (const auto &[source, targets] : manifest->file_targets) {
        if (source.starts_with("tests/")) { continue; }
        if (targets.size() != 1) {
            ambiguous.push_back(source);
            continue;
        }
        truth.emplace(source, *targets.begin());
    }
    EXPECT_TRUE(ambiguous.empty())
      << "shader-manifest.json has " << ambiguous.size()
      << " non-test file(s) compiled to BOTH spirv and wgsl - " << doc_path.string()
      << "'s shader-targets table has only two columns (spirv-only / wgsl-only) and needs a third list for:"
      << joinViolations(ambiguous);

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

    EXPECT_TRUE(doc_only.empty()) << doc_path.string() << " lists file(s) shader-manifest.json does not have:"
                                  << joinViolations(doc_only);
    EXPECT_TRUE(manifest_only.empty())
      << doc_path.string() << " is missing file(s) shader-manifest.json has:" << joinViolations(manifest_only);
    EXPECT_TRUE(mismatched.empty())
      << doc_path.string() << " disagrees with shader-manifest.json on target(s):" << joinViolations(mismatched);

    EXPECT_FALSE(doc_targets->contains("histogram.wgsl"))
      << doc_path.string()
      << "'s shader-targets table must not list histogram.wgsl - it has no Slang source (hand-written WGSL "
         "fallback) and cannot appear in shader-manifest.json";
}

// docs/shader-sharing.md's shared-module-targets table claims, per
// Resources/ShadersSlang/common/ module, which target(s) actually reach it
// through the manifest's import graph. Recomputes shared_module_target_truth
// from import_closure + shader-manifest.json and asserts set equality in
// both directions, so a module missing from the table AND a table row for a
// module that no longer exists under common/ both fail loudly - the same
// shape as ShaderSharingDocMatchesTheManifestTargets above, just one level
// deeper (modules instead of entry points).
TEST(BuildIntegrity, SharedModuleTargetsTableMatchesTheImportGraph)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";
    const fs::path slang_root = slangRoot();

    const fs::path doc_path = repo_root / "docs" / "shader-sharing.md";
    if (!fs::exists(doc_path)) {
        GTEST_SKIP() << "could not open " << doc_path.string() << " - not running from the repo root?";
    }

    const auto doc_targets = parse_shared_module_targets_marker(doc_path);
    ASSERT_TRUE(doc_targets.has_value())
      << doc_path.string()
      << " is missing its '<!-- shared-module-targets:begin -->' / '<!-- shared-module-targets:end -->' marker "
         "block";

    const auto &manifest = shader_manifest(repo_root);
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

    EXPECT_TRUE(doc_only.empty())
      << doc_path.string() << "'s shared-module-targets table lists module(s) not found under "
      << (slang_root / "common").string() << ":" << joinViolations(doc_only);
    EXPECT_TRUE(truth_only.empty())
      << doc_path.string() << "'s shared-module-targets table is missing module(s):" << joinViolations(truth_only);
    EXPECT_TRUE(mismatched.empty())
      << doc_path.string() << "'s shared-module-targets table disagrees with the import graph on:" << joinViolations(mismatched);
}

// Parses the X, Y, Z triple out of the first "[numthreads(X, Y, Z)]" in
// `path`. Returns nullopt if the attribute is not found, so callers can tell
// "found and mismatched" apart from "a renamed/removed attribute silently
// matched zero times".
std::optional<std::array<int, 3>> parse_numthreads(const fs::path &path)
{
    const auto contentsOpt = readFileText(path);
    if (!contentsOpt) { return std::nullopt; }

    static const std::regex kNumThreadsPattern(R"(\[numthreads\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)\])");

    const std::string &contents = *contentsOpt;
    std::smatch match;
    if (!std::regex_search(contents, match, kNumThreadsPattern)) { return std::nullopt; }

    return std::array<int, 3>{ std::stoi(match[1].str()), std::stoi(match[2].str()), std::stoi(match[3].str()) };
}

// Clouds.cpp dispatches the noise and cloud compute passes using
// kNoiseWorkgroupSize/kCloudWorkgroupSize (CloudDispatch.hpp) to size the
// thread-group grid. Those constants have no compiler-enforced link to the
// [numthreads(...)] attribute the corresponding Slang kernel actually
// declares - halving noise.slang's workgroup to (4,4,4) without touching
// CloudDispatch.hpp would leave 7/8 of the noise volume undefined, and
// nothing short of a GPU golden test would notice. Mirrors
// HostAndShaderSharedConstantsAgree's "parse the shader text, compare
// against the compiled host value" shape.
TEST(BuildIntegrity, CloudDispatchGridsMatchTheShaderWorkgroupSizes)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path noise_path = slangRoot() / "compute" / "noise.slang";
    const fs::path clouds_path = slangRoot() / "compute" / "clouds.slang";

    const auto noise_threads = parse_numthreads(noise_path);
    ASSERT_TRUE(noise_threads.has_value()) << "no [numthreads(...)] attribute found in " << noise_path.string();
    EXPECT_EQ((*noise_threads)[0], static_cast<int>(Kataglyphis::kNoiseWorkgroupSize))
      << noise_path.string() << "'s [numthreads(" << (*noise_threads)[0] << ", " << (*noise_threads)[1] << ", "
      << (*noise_threads)[2] << ")] X does not match CloudDispatch.hpp's kNoiseWorkgroupSize ("
      << Kataglyphis::kNoiseWorkgroupSize << ')';
    EXPECT_EQ((*noise_threads)[1], static_cast<int>(Kataglyphis::kNoiseWorkgroupSize))
      << noise_path.string() << "'s [numthreads(" << (*noise_threads)[0] << ", " << (*noise_threads)[1] << ", "
      << (*noise_threads)[2] << ")] Y does not match CloudDispatch.hpp's kNoiseWorkgroupSize ("
      << Kataglyphis::kNoiseWorkgroupSize << ')';
    EXPECT_EQ((*noise_threads)[2], static_cast<int>(Kataglyphis::kNoiseWorkgroupSize))
      << noise_path.string() << "'s [numthreads(" << (*noise_threads)[0] << ", " << (*noise_threads)[1] << ", "
      << (*noise_threads)[2] << ")] Z does not match CloudDispatch.hpp's kNoiseWorkgroupSize ("
      << Kataglyphis::kNoiseWorkgroupSize << ')';

    const auto cloud_threads = parse_numthreads(clouds_path);
    ASSERT_TRUE(cloud_threads.has_value()) << "no [numthreads(...)] attribute found in " << clouds_path.string();
    EXPECT_EQ((*cloud_threads)[0], static_cast<int>(Kataglyphis::kCloudWorkgroupSize))
      << clouds_path.string() << "'s [numthreads(" << (*cloud_threads)[0] << ", " << (*cloud_threads)[1] << ", "
      << (*cloud_threads)[2] << ")] X does not match CloudDispatch.hpp's kCloudWorkgroupSize ("
      << Kataglyphis::kCloudWorkgroupSize << ')';
    EXPECT_EQ((*cloud_threads)[1], static_cast<int>(Kataglyphis::kCloudWorkgroupSize))
      << clouds_path.string() << "'s [numthreads(" << (*cloud_threads)[0] << ", " << (*cloud_threads)[1] << ", "
      << (*cloud_threads)[2] << ")] Y does not match CloudDispatch.hpp's kCloudWorkgroupSize ("
      << Kataglyphis::kCloudWorkgroupSize << ')';
    EXPECT_EQ((*cloud_threads)[2], 1) << clouds_path.string() << "'s [numthreads(" << (*cloud_threads)[0] << ", "
                                       << (*cloud_threads)[1] << ", " << (*cloud_threads)[2] << ")] Z is not 1";
}

// Parses the [min, max] bounds out of clouds.slang's
// `cloud.num_march_steps = int(clamp(scene.cloudParameters.w, MIN, MAX));`.
// Returns nullopt if the assignment is not found, matching parse_numthreads's
// convention so a rewritten expression fails loudly rather than matching
// zero times.
std::optional<std::pair<float, float>> parse_cloud_march_steps_range(const std::string &contents)
{
    static const std::regex kPattern(
      R"(num_march_steps\s*=\s*int\(\s*clamp\(\s*scene\.cloudParameters\.w\s*,\s*([0-9.]+)\s*,\s*([0-9.]+)\s*\)\s*\))");
    std::smatch match;
    if (!std::regex_search(contents, match, kPattern)) { return std::nullopt; }
    return std::make_pair(std::stof(match[1].str()), std::stof(match[2].str()));
}

// Parses the [min, max] bounds out of clouds.slang's
// `cloud.num_march_steps_to_light = int(clamp(scene.cloudLightMarch.x, MIN, MAX));`.
std::optional<std::pair<float, float>> parse_cloud_light_march_steps_range(const std::string &contents)
{
    static const std::regex kPattern(R"(num_march_steps_to_light\s*=\s*int\(\s*clamp\(\s*scene\.cloudLightMarch\.x\s*,)"
                                      R"(\s*([0-9.]+)\s*,\s*([0-9.]+)\s*\)\s*\))");
    std::smatch match;
    if (!std::regex_search(contents, match, kPattern)) { return std::nullopt; }
    return std::make_pair(std::stof(match[1].str()), std::stof(match[2].str()));
}

// clouds.slang clamps both march-step counts defensively even though the
// host (SceneUboMarshal.hpp's fillSceneUboClouds) already clamps them before
// packing - a shader must not trust a UBO. The two clamps have no
// compiler-enforced link to CloudDispatch.hpp's
// kMin/kMaxCloudMarchSteps/kMin/kMaxCloudLightMarchSteps, which the GUI
// slider and the host packer are also built from; this pins the shader
// literals against those constants so the three places agree. Mirrors
// CloudDispatchGridsMatchTheShaderWorkgroupSizes's "parse the shader text,
// compare against the compiled host value" shape.
TEST(BuildIntegrity, CloudMarchStepBoundsMatchTheShaderClamps)
{
    const fs::path clouds_path = slangRoot() / "compute" / "clouds.slang";
    const auto contents = readFileText(clouds_path);
    ASSERT_TRUE(contents.has_value()) << "missing " << clouds_path.string();

    const auto march_steps_range = parse_cloud_march_steps_range(*contents);
    ASSERT_TRUE(march_steps_range.has_value())
      << clouds_path.string() << ": no `num_march_steps = int(clamp(scene.cloudParameters.w, ...))` found";
    EXPECT_FLOAT_EQ(march_steps_range->first, static_cast<float>(Kataglyphis::kMinCloudMarchSteps))
      << clouds_path.string() << "'s num_march_steps lower bound does not match CloudDispatch.hpp's kMinCloudMarchSteps ("
      << Kataglyphis::kMinCloudMarchSteps << ')';
    EXPECT_FLOAT_EQ(march_steps_range->second, static_cast<float>(Kataglyphis::kMaxCloudMarchSteps))
      << clouds_path.string() << "'s num_march_steps upper bound does not match CloudDispatch.hpp's kMaxCloudMarchSteps ("
      << Kataglyphis::kMaxCloudMarchSteps << ')';

    const auto light_march_steps_range = parse_cloud_light_march_steps_range(*contents);
    ASSERT_TRUE(light_march_steps_range.has_value()) << clouds_path.string()
      << ": no `num_march_steps_to_light = int(clamp(scene.cloudLightMarch.x, ...))` found";
    EXPECT_FLOAT_EQ(light_march_steps_range->first, static_cast<float>(Kataglyphis::kMinCloudLightMarchSteps))
      << clouds_path.string()
      << "'s num_march_steps_to_light lower bound does not match CloudDispatch.hpp's kMinCloudLightMarchSteps ("
      << Kataglyphis::kMinCloudLightMarchSteps << ')';
    EXPECT_FLOAT_EQ(light_march_steps_range->second, static_cast<float>(Kataglyphis::kMaxCloudLightMarchSteps))
      << clouds_path.string()
      << "'s num_march_steps_to_light upper bound does not match CloudDispatch.hpp's kMaxCloudLightMarchSteps ("
      << Kataglyphis::kMaxCloudLightMarchSteps << ')';
}

// Parses the operand list of noise.slang's `noiseVolume[tid] = float4( ... );`
// assignment, splitting on top-level commas (respecting nested parens, so an
// operand like `worley(uvw, 4.0)` counts as one argument, not two). Returns
// nullopt if the assignment is not found, matching parse_numthreads's
// "std::nullopt when the pattern is absent" convention.
std::optional<std::vector<std::string>> parse_noise_volume_write_operands(const std::string &contents)
{
    static const std::regex kAssignStart(R"(noiseVolume\[tid\]\s*=\s*float4\()");
    std::smatch match;
    if (!std::regex_search(contents, match, kAssignStart)) { return std::nullopt; }

    size_t pos = static_cast<size_t>(match.position(0)) + static_cast<size_t>(match.length(0));
    int depth = 1;
    std::string operandsText;
    for (; pos < contents.size() && depth > 0; ++pos) {
        const char c = contents[pos];
        if (c == '(') {
            ++depth;
            operandsText += c;
        } else if (c == ')') {
            --depth;
            if (depth > 0) { operandsText += c; }
        } else {
            operandsText += c;
        }
    }
    if (depth != 0) { return std::nullopt; }

    std::vector<std::string> operands;
    std::string current;
    int nestDepth = 0;
    for (const char c : operandsText) {
        if (c == '(') {
            ++nestDepth;
            current += c;
        } else if (c == ')') {
            --nestDepth;
            current += c;
        } else if (c == ',' && nestDepth == 0) {
            operands.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    operands.push_back(current);

    for (auto &operand : operands) {
        const size_t start = operand.find_first_not_of(" \t\r\n");
        const size_t end = operand.find_last_not_of(" \t\r\n");
        operand = (start == std::string::npos) ? "" : operand.substr(start, end - start + 1);
    }
    return operands;
}

// Extracts the brace-delimited body of the first function found via
// `signature_needle` (a substring unique to that function's declaration).
std::string extract_function_body(const std::string &contents, const std::string &signature_needle)
{
    const size_t sig_pos = contents.find(signature_needle);
    if (sig_pos == std::string::npos) { return {}; }
    const size_t brace_pos = contents.find('{', sig_pos);
    if (brace_pos == std::string::npos) { return {}; }

    int depth = 1;
    size_t pos = brace_pos + 1;
    for (; pos < contents.size() && depth > 0; ++pos) {
        if (contents[pos] == '{') { ++depth; }
        else if (contents[pos] == '}') { --depth; }
    }
    return contents.substr(brace_pos, pos - brace_pos);
}

// noise.slang used to normalise its 128^3 thread id by a bare 256.0 (half
// the actual volume extent, so the noise only ever covered the volume's
// first octant) and hard-coded .b/.a to 0.0/1.0, even though clouds.slang's
// sample_density weights all four channels into baseDensity and the cirrus
// band. Neither gap was compiler-visible - both compiled and ran, just
// silently produced the wrong picture. Mirrors HostAndShaderSharedConstantsAgree's
// "parse the shader text, compare against the compiled host value" shape.
TEST(BuildIntegrity, CloudNoiseVolumeCoversItsFullDomainAndWritesEveryChannelTheMarchReads)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path noise_path = slangRoot() / "compute" / "noise.slang";
    const fs::path clouds_path = slangRoot() / "compute" / "clouds.slang";

    const auto noise_contents = readFileText(noise_path);
    ASSERT_TRUE(noise_contents.has_value()) << "could not read " << noise_path.string();

    // (a) NOISE_VOLUME_EXTENT must equal CloudDispatch.hpp's kNoiseVolumeExtent.
    static const std::regex kExtentPattern(R"(NOISE_VOLUME_EXTENT\s*=\s*([0-9]+(?:\.[0-9]*)?))");
    std::smatch extent_match;
    ASSERT_TRUE(std::regex_search(*noise_contents, extent_match, kExtentPattern))
      << "no NOISE_VOLUME_EXTENT initializer found in " << noise_path.string();
    const auto parsed_extent = static_cast<uint32_t>(std::stod(extent_match[1].str()));
    EXPECT_EQ(parsed_extent, Kataglyphis::kNoiseVolumeExtent)
      << noise_path.string() << "'s NOISE_VOLUME_EXTENT (" << parsed_extent
      << ") does not match CloudDispatch.hpp's kNoiseVolumeExtent (" << Kataglyphis::kNoiseVolumeExtent << ')';

    // (b) None of the four float4() operands may be a bare numeric literal -
    // that is exactly how .b/.a ended up hard-coded to 0.0/1.0 last time.
    const auto operands = parse_noise_volume_write_operands(*noise_contents);
    ASSERT_TRUE(operands.has_value()) << "no `noiseVolume[tid] = float4( ... );` assignment found in "
                                       << noise_path.string();
    ASSERT_EQ(operands->size(), 4u) << noise_path.string()
                                     << "'s noiseVolume[tid] = float4( ... ) assignment does not have 4 operands";

    static const std::regex kBareNumericLiteral(R"(^[+-]?[0-9]+(\.[0-9]*)?[fF]?$)");
    static const char *const kChannelNames[4] = { ".r", ".g", ".b", ".a" };
    for (size_t i = 0; i < operands->size(); ++i) {
        EXPECT_FALSE(std::regex_match((*operands)[i], kBareNumericLiteral))
          << noise_path.string() << "'s noiseVolume[tid] = float4(...) operand " << (i + 1) << " (channel "
          << kChannelNames[i] << ", value \"" << (*operands)[i]
          << "\") is a bare numeric literal, not a computed value - clouds.slang's sample_density weights every "
             "channel into baseDensity/the cirrus band, so a constant channel silently flattens part of the "
             "cloud shape";
    }

    // Cross-check: the swizzle components clouds.slang's sample_density
    // actually reads off noiseCoarse/noiseFine (the 256- and 64-world-unit
    // period samples) must be exactly {r, g, b, a}, so this gate fails
    // loudly if that consumer contract changes instead of silently checking
    // a stale set.
    const auto clouds_contents = readFileText(clouds_path);
    ASSERT_TRUE(clouds_contents.has_value()) << "could not read " << clouds_path.string();
    const std::string sample_density_body =
      extract_function_body(*clouds_contents, "float sample_density(float3 position, Clouds cloud)");
    ASSERT_FALSE(sample_density_body.empty())
      << "could not locate sample_density's body in " << clouds_path.string();

    static const std::regex kSwizzleRead(R"(noise(?:Coarse|Fine)\.([rgba]))");
    std::set<char> swizzle_components;
    for (auto it = std::sregex_iterator(sample_density_body.begin(), sample_density_body.end(), kSwizzleRead);
         it != std::sregex_iterator(); ++it) {
        swizzle_components.insert((*it)[1].str()[0]);
    }
    const std::set<char> expected_components = { 'r', 'g', 'b', 'a' };
    EXPECT_EQ(swizzle_components, expected_components)
      << clouds_path.string()
      << "'s sample_density reads a different swizzle-component set off noiseCoarse/noiseFine than {r, g, b, a}";
}

// The cloud noise volume used to be written on a dedicated compute queue
// while the eExclusive storage image it lives in is owned by the graphics
// family - undefined contents on the family that samples it, with no
// ownership transfer performed. Guards against the ad-hoc compute command
// pool / queue reappearing, and against VulkanDevice growing back a
// getComputeQueue() accessor that would tempt a caller into the same bug.
TEST(BuildIntegrity, CloudResourcesAreProducedAndConsumedOnOneQueue)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path clouds_path = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "atmospheric_effects"
                                  / "clouds" / "Clouds.cpp";
    const auto clouds_source_opt = readFileText(clouds_path);
    ASSERT_TRUE(clouds_source_opt.has_value()) << "missing " << clouds_path.string();
    const std::string &clouds_source = *clouds_source_opt;

    EXPECT_EQ(clouds_source.find("getComputeQueue"), std::string::npos)
      << "Clouds.cpp must dispatch noise generation on the graphics queue that owns the eExclusive noise image, "
         "not a separate compute queue";
    EXPECT_EQ(clouds_source.find("createCommandPool"), std::string::npos)
      << "Clouds.cpp must reuse the graphics command pool passed into init(), not create its own transient pool "
         "for a different queue family";

    const fs::path device_header_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "vulkan_base" / "VulkanDevice.ixx";
    const auto device_header_source_opt = readFileText(device_header_path);
    ASSERT_TRUE(device_header_source_opt.has_value()) << "missing " << device_header_path.string();
    const std::string &device_header_source = *device_header_source_opt;

    EXPECT_EQ(device_header_source.find("getComputeQueue"), std::string::npos)
      << "VulkanDevice must not expose getComputeQueue() - the only caller (Clouds.cpp) now dispatches on the "
         "graphics queue instead";
}

// sample_density used to wrap the sample position into the noise volume's
// [0, 1) domain with abs(fmod(x, N)) / N. fmod keeps the sign of its
// dividend, so for negative x that returns (-N, 0], and abs() then maps -x
// and +x onto the SAME texel - a mirror across the origin plane, not a wrap.
// With the shipped cloud defaults the mirror plane sits almost exactly in
// the middle of the visible box, so the cloud field rendered as a mirror
// image of itself. frac(x) = x - floor(x) returns [0, 1) for negative
// inputs too and wraps correctly. Guards against abs(fmod(...)) reappearing.
TEST(BuildIntegrity, CloudNoiseSamplingWrapsRatherThanMirrors)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path clouds_path = slangRoot() / "compute" / "clouds.slang";
    const auto contentsOpt = readFileText(clouds_path);
    ASSERT_TRUE(contentsOpt.has_value()) << "missing " << clouds_path.string();
    const std::string &contents = *contentsOpt;

    const std::string sample_density_body =
      extract_function_body(contents, "float sample_density(float3 position, Clouds cloud)");
    ASSERT_FALSE(sample_density_body.empty())
      << "could not locate sample_density's body in " << clouds_path.string();

    EXPECT_EQ(sample_density_body.find("abs(fmod("), std::string::npos)
      << clouds_path.string()
      << "'s sample_density wraps the sample position with abs(fmod(...)), which mirrors the "
         "noise field across the origin plane for negative coordinates instead of wrapping it - "
         "the default cloud box straddles that plane, so this renders as a mirror image of "
         "itself. Use frac(x), which returns [0, 1) for negative x too.";
    EXPECT_NE(sample_density_body.find("frac("), std::string::npos)
      << clouds_path.string() << "'s sample_density must wrap the sample position with frac(), "
                                 "not abs(fmod(...))";
}

// clouds.slang used to multiply density by the distance already travelled
// along the ray (`float(i) / float(num_march_steps)`) instead of a per-step
// LENGTH, making the volume ~63x too dense at the default quality and turning
// the quality slider into a de-facto density slider. Guards against that
// shape reappearing, both in the primary march and in light_march's mean
// (rather than integrated) density.
TEST(BuildIntegrity, CloudRayMarchesUseAConstantStepLength)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path clouds_path = slangRoot() / "compute" / "clouds.slang";
    const auto contentsOpt = readFileText(clouds_path);
    ASSERT_TRUE(contentsOpt.has_value()) << "missing " << clouds_path.string();
    const std::string &contents = *contentsOpt;

    static const std::regex kDistanceAsStepSize(R"(float\(i\)\s*/\s*float\()");
    EXPECT_FALSE(std::regex_search(contents, kDistanceAsStepSize))
      << clouds_path.string()
      << " contains the distance-as-step-size shape (float(i) / float(...)) - "
         "step length must be (segment length) / (step count), not a fraction "
         "of distance already travelled";

    static const std::regex kLightMarchOriginIsSamplePos(R"(box_intersect\(samplePos,)");
    EXPECT_TRUE(std::regex_search(contents, kLightMarchOriginIsSamplePos))
      << clouds_path.string() << "'s light_march must intersect the box from the point being "
                                 "shadowed (samplePos), not the camera";

    EXPECT_EQ(contents.find("totalDensity /= "), std::string::npos)
      << clouds_path.string()
      << " must not average the light march's density samples - exp(-totalDensity) needs an "
         "integrated optical depth (density * step length), not a mean density";
}

// phase_HG used to peak away from the sun (denominator added 2*g*cosTheta
// instead of subtracting it), and the powder effect used to raise
// transmittance directly instead of attenuating in-scattered light - both
// silent, since neither has a numerical oracle in this suite (see the task
// commit message for why one isn't added here either). This pins the text
// shape of the fix so a future edit can't reintroduce either bug unnoticed:
// a sign flip in the phase denominator, or a powder assignment onto
// transmittance, would both slip past every other test in this file.
TEST(BuildIntegrity, CloudScatteringKeepsItsPhaseSignAndItsMonotonicTransmittance)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path clouds_path = slangRoot() / "compute" / "clouds.slang";
    const auto contentsOpt = readFileText(clouds_path);
    ASSERT_TRUE(contentsOpt.has_value()) << "missing " << clouds_path.string();
    const std::string &contents = *contentsOpt;

    // cosTheta = dot(rayDirection, normalize(-dirLight.direction)) - a positive
    // g must peak at cosTheta = +1 (forward scattering, looking toward the
    // sun), which requires SUBTRACTING 2*g*cosTheta from the denominator.
    static const std::regex kPhaseSignFixed(R"(1\.0\s*\+\s*g\s*\*\s*g\s*-\s*2\.0\s*\*\s*g)");
    EXPECT_TRUE(std::regex_search(contents, kPhaseSignFixed))
      << clouds_path.string()
      << "'s phase_HG denominator must be 1.0 + g*g - 2.0*g*cosTheta so a positive g peaks "
         "toward the sun (cosTheta = +1), not away from it";

    static const std::regex kPhaseSignBroken(R"(1\.0\s*\+\s*g\s*\*\s*g\s*\+\s*2\.0\s*\*\s*g)");
    EXPECT_FALSE(std::regex_search(contents, kPhaseSignBroken))
      << clouds_path.string()
      << " contains the sign-flipped phase denominator (1.0 + g*g + 2.0*g*cosTheta), which "
         "peaks away from the sun instead of toward it";

    // No line may assign transmittance from an expression that mentions
    // powder - the powder term belongs on lightEnergy, not on transmittance.
    const auto lines = readFileLines(clouds_path);
    ASSERT_TRUE(lines.has_value()) << "missing " << clouds_path.string();
    static const std::regex kPlainTransmittanceAssign(R"(transmittance\s*=[^=*])");
    for (std::size_t i = 0; i < lines->size(); ++i) {
        const std::string &line = (*lines)[i];
        if (!std::regex_search(line, kPlainTransmittanceAssign)) { continue; }
        EXPECT_EQ(line.find("powder"), std::string::npos)
          << clouds_path.string() << ":" << (i + 1)
          << " assigns transmittance from an expression mentioning powder - the powder term "
             "must attenuate lightEnergy, not raise transmittance ("
          << line << ")";
    }

    // Inside the march loop, the ONLY assignment to transmittance must be the
    // Beer-Lambert `*= exp(-...)` - transmittance is monotonically
    // non-increasing along the ray by construction, and nothing else in the
    // loop may touch it.
    const std::size_t loop_start = contents.find("for (int i = 0; i < cloud.num_march_steps; i++)");
    ASSERT_NE(loop_start, std::string::npos)
      << clouds_path.string() << " is missing the primary march loop";
    // Tolerate CRLF: the file is checked in with Windows line endings.
    static const std::regex kLoopClosingBrace(R"(\r?\n        \}\r?\n)");
    std::size_t loop_body_end = std::string::npos;
    for (auto it = std::sregex_iterator(contents.begin(), contents.end(), kLoopClosingBrace),
              end = std::sregex_iterator();
         it != end; ++it) {
        if (static_cast<std::size_t>(it->position()) >= loop_start) {
            loop_body_end = static_cast<std::size_t>(it->position());
            break;
        }
    }
    ASSERT_NE(loop_body_end, std::string::npos)
      << clouds_path.string() << "'s primary march loop is missing its closing brace";
    const std::string loop_body = contents.substr(loop_start, loop_body_end - loop_start);

    static const std::regex kTransmittanceAssignAnyForm(R"(transmittance\s*(?:\*=|\+=|-=|=[^=])\s*[^;]*)");
    auto begin = std::sregex_iterator(loop_body.begin(), loop_body.end(), kTransmittanceAssignAnyForm);
    const auto end = std::sregex_iterator();
    ASSERT_NE(begin, end) << clouds_path.string() << "'s march loop never assigns transmittance";
    for (auto it = begin; it != end; ++it) {
        const std::string matched = it->str();
        EXPECT_TRUE(matched.find("*=") != std::string::npos && matched.find("exp(-") != std::string::npos)
          << clouds_path.string()
          << "'s march loop assigns transmittance with something other than `*= exp(-...)`: " << matched;
    }
}

// clouds.slang used to carry a write-only `model_to_world` (seven assignments
// feeding nothing - box_intersect only ever consumes inv_model_to_world)
// under three comments that all claimed the inverse was "precomputed on the
// CPU", which is false: Slang has no inverse() for SPIR-V, so it is formed
// inline in clouds_main from cloud.radius/cloud.offset. Guards against the
// dead forward matrix and the CPU-precompute claim both reappearing.
TEST(BuildIntegrity, CloudBoxInverseIsFormedInTheShaderNotOnTheHost)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path clouds_path = slangRoot() / "compute" / "clouds.slang";
    const auto clouds_contents_opt = readFileText(clouds_path);
    ASSERT_TRUE(clouds_contents_opt.has_value()) << "missing " << clouds_path.string();
    const std::string &clouds_contents = *clouds_contents_opt;

    // std::regex here is ECMAScript, which has no lookbehind, so "not
    // preceded by inv_" cannot be expressed directly. Every surviving
    // occurrence of "model_to_world" must be part of "inv_model_to_world" -
    // the two counts diverge the instant a forward matrix is reintroduced.
    auto countOccurrences = [](const std::string &text, const std::string &needle) {
        std::size_t count = 0;
        std::size_t pos = 0;
        while ((pos = text.find(needle, pos)) != std::string::npos) {
            ++count;
            pos += needle.size();
        }
        return count;
    };
    const std::size_t model_to_world_count = countOccurrences(clouds_contents, "model_to_world");
    const std::size_t inv_model_to_world_count = countOccurrences(clouds_contents, "inv_model_to_world");
    EXPECT_EQ(model_to_world_count, inv_model_to_world_count)
      << clouds_path.string() << " contains a \"model_to_world\" occurrence that is not part of "
                                 "\"inv_model_to_world\" - the write-only forward matrix must stay deleted";

    static const std::regex kCpuAttributionClaim(
      R"((inverse model matrix|inv_model_to_world)[^.]{0,80}(CPU|host))", std::regex::icase);
    std::smatch clouds_match;
    EXPECT_FALSE(std::regex_search(clouds_contents, clouds_match, kCpuAttributionClaim))
      << clouds_path.string()
      << " re-attributes the inverse model matrix to the CPU/host: \"" << (clouds_match.empty() ? "" : clouds_match.str())
      << "\" - it is formed in clouds_main, not precomputed on the host";

    const fs::path clouds_doc_path = repo_root / "docs" / "clouds.md";
    const auto clouds_doc_contents_opt = readFileText(clouds_doc_path);
    ASSERT_TRUE(clouds_doc_contents_opt.has_value()) << "missing " << clouds_doc_path.string();
    const std::string &clouds_doc_contents = *clouds_doc_contents_opt;

    std::smatch doc_match;
    EXPECT_FALSE(std::regex_search(clouds_doc_contents, doc_match, kCpuAttributionClaim))
      << clouds_doc_path.string()
      << " re-attributes the inverse model matrix to the CPU/host: \"" << (doc_match.empty() ? "" : doc_match.str())
      << "\" - it is formed in the shader, not precomputed on the host (this window is bounded to one "
         "sentence so it cannot collide with the doc's legitimate statement that inv_projection/inv_view "
         "ARE CPU-precomputed into GlobalUBO)";
}

// The nine cloud.<field> <- scene.<field>.<component> pairs SceneUboMarshal.hpp's
// fillSceneUboClouds (host) and clouds_main's unpack block (shader) must
// agree on. Shared by CloudUboPackingMatchesTheShaderUnpack (checks it
// against the shader) and CloudsDocTablesMatchTheirSources (checks
// docs/clouds.md's cloud-ubo table against this same list), so the two tests
// cannot silently diverge into two different "ground truths" for the same
// nine-row table.
struct CloudUboFieldPair
{
    const char *cloud_field;
    const char *scene_field;
    const char *component;
};
constexpr std::array<CloudUboFieldPair, 9> kCloudUboFieldPairs{ {
  { "num_march_steps", "cloudParameters", "w" },
  { "num_march_steps_to_light", "cloudLightMarch", "x" },
  { "scale", "cloudMeshScale", "w" },
  { "threshold", "cloudMeshOffset", "w" },
  { "pillowness", "cloudParameters", "x" },
  { "cirrus_effect", "cloudParameters", "y" },
  { "powder_effect", "cloudParameters", "z" },
  { "radius", "cloudMeshScale", "xyz" },
  { "offset", "cloudMeshOffset", "xyz" },
} };

// SceneUboMarshal.hpp's fillSceneUboClouds (the host packer) and this shader
// (the only unpack side) are two hand-written mirrors of the same seven-value
// layout, tied together by nothing but a comment. sceneUboLayoutSuite pins
// the four cloud vec4s' byte *offsets* but says nothing about what goes
// inside them, so a component swap on either side is invisible to every
// other test. This pins each host component to the exact shader field it
// must land in, matching on field-and-component pairs (not whole lines) so
// the shader's surrounding max/clamp/> 0.5 wrappers don't make it brittle,
// and names the pair that moved on failure.
TEST(BuildIntegrity, CloudUboPackingMatchesTheShaderUnpack)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path clouds_path = slangRoot() / "compute" / "clouds.slang";
    const auto contents = readFileText(clouds_path);
    ASSERT_TRUE(contents.has_value()) << "missing " << clouds_path.string();

    for (const auto &pair : kCloudUboFieldPairs) {
        const std::string pattern = std::string("cloud\\.") + pair.cloud_field + R"(\s*=[^;]*scene\.)"
          + pair.scene_field + "\\." + pair.component + "\\b";
        const std::regex field_regex(pattern);
        EXPECT_TRUE(std::regex_search(*contents, field_regex))
          << clouds_path.string() << " no longer assigns cloud." << pair.cloud_field << " from scene."
          << pair.scene_field << '.' << pair.component
          << " - the host packer (SceneUboMarshal.hpp's fillSceneUboClouds) and this shader's unpack "
             "must agree on which component every cloud slider lands in";
    }
}

// std::string-owning counterpart to CloudUboFieldPair (whose const char*
// fields point at literal storage from kCloudUboFieldPairs and must not be
// reused for parsed, temporary text).
struct ParsedCloudUboRow
{
    std::string cloud_field;
    std::string scene_field;
    std::string component;
};

// Parses docs/clouds.md's `<!-- cloud-ubo:begin -->` / `:end` marker table -
// one `| <gui control> | \`cloud.<field>\` | \`<sceneField>.<component>\` |`
// row per SceneUBO field/component pair. Returns std::nullopt if the marker
// pair is missing, matching parse_shader_targets_marker's convention.
std::optional<std::vector<ParsedCloudUboRow>> parse_cloud_ubo_doc_table(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::regex kRowPattern(
      R"(\|[^|]*\|\s*`cloud\.([A-Za-z_]+)`\s*\|\s*`([A-Za-z]+)\.([A-Za-z]+)`\s*\|)");

    std::vector<ParsedCloudUboRow> rows;
    bool in_block = false;
    bool saw_block = false;
    for (const auto &line : *lines) {
        if (line.find("<!-- cloud-ubo:begin -->") != std::string::npos) {
            in_block = true;
            saw_block = true;
            continue;
        }
        if (line.find("<!-- cloud-ubo:end -->") != std::string::npos) {
            in_block = false;
            continue;
        }
        if (!in_block) { continue; }

        std::smatch match;
        if (std::regex_search(line, match, kRowPattern)) {
            rows.push_back({ match[1].str(), match[2].str(), match[3].str() });
        }
    }

    if (!saw_block) { return std::nullopt; }
    return rows;
}

// Parses docs/clouds.md's `<!-- cloud-constants:begin -->` / `:end` marker
// table - one `| \`<name>\` | <value> | ... |` row per CloudDispatch.hpp
// constant. Returns std::nullopt if the marker pair is missing.
std::optional<std::map<std::string, long long>> parse_cloud_constants_doc_table(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::regex kRowPattern(R"(\|\s*`(k[A-Za-z]+)`\s*\|\s*([0-9]+)\s*\|)");

    std::map<std::string, long long> rows;
    bool in_block = false;
    bool saw_block = false;
    for (const auto &line : *lines) {
        if (line.find("<!-- cloud-constants:begin -->") != std::string::npos) {
            in_block = true;
            saw_block = true;
            continue;
        }
        if (line.find("<!-- cloud-constants:end -->") != std::string::npos) {
            in_block = false;
            continue;
        }
        if (!in_block) { continue; }

        std::smatch match;
        if (std::regex_search(line, match, kRowPattern)) { rows[match[1].str()] = std::stoll(match[2].str()); }
    }

    if (!saw_block) { return std::nullopt; }
    return rows;
}

// docs/clouds.md hand-maintains two tables that would otherwise silently rot:
// the cloud-ubo table (GUI control -> shader field -> SceneUBO
// field/component) and the cloud-constants table (every CloudDispatch.hpp
// constant + the shader token it pins). This checks both against their
// actual source of truth - kCloudUboFieldPairs (the same list
// CloudUboPackingMatchesTheShaderUnpack already derives, reused rather than
// hand-copied a second time) and the compiled CloudDispatch.hpp constants
// themselves - so a doc edit that drifts from either fails here instead of
// silently going stale, the same failure mode ShaderSharingDocMatchesTheManifestTargets
// guards against for the shader-targets table.
TEST(BuildIntegrity, CloudsDocTablesMatchTheirSources)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "clouds.md";
    if (!fs::exists(doc_path)) {
        GTEST_SKIP() << "could not open " << doc_path.string() << " - not running from the repo root?";
    }

    const auto ubo_rows = parse_cloud_ubo_doc_table(doc_path);
    ASSERT_TRUE(ubo_rows.has_value())
      << doc_path.string() << " is missing its '<!-- cloud-ubo:begin -->' / '<!-- cloud-ubo:end -->' marker block";

    ASSERT_EQ(ubo_rows->size(), kCloudUboFieldPairs.size())
      << doc_path.string() << "'s cloud-ubo table has " << ubo_rows->size() << " row(s), expected "
      << kCloudUboFieldPairs.size() << " to match kCloudUboFieldPairs";
    for (std::size_t i = 0; i < kCloudUboFieldPairs.size(); ++i) {
        const auto &expected = kCloudUboFieldPairs[i];
        const auto &actual = (*ubo_rows)[i];
        EXPECT_EQ(actual.cloud_field, expected.cloud_field)
          << doc_path.string() << "'s cloud-ubo table row " << i << " has shader field \"" << actual.cloud_field
          << "\", expected \"" << expected.cloud_field << '"';
        EXPECT_EQ(actual.scene_field, expected.scene_field)
          << doc_path.string() << "'s cloud-ubo table row " << i << " (cloud." << expected.cloud_field
          << ") has SceneUBO field \"" << actual.scene_field << "\", expected \"" << expected.scene_field << '"';
        EXPECT_EQ(actual.component, expected.component)
          << doc_path.string() << "'s cloud-ubo table row " << i << " (cloud." << expected.cloud_field
          << ") has SceneUBO component \"" << actual.component << "\", expected \"" << expected.component << '"';
    }

    const auto constants_rows = parse_cloud_constants_doc_table(doc_path);
    ASSERT_TRUE(constants_rows.has_value())
      << doc_path.string()
      << " is missing its '<!-- cloud-constants:begin -->' / '<!-- cloud-constants:end -->' marker block";

    const std::map<std::string, long long> truth{
        { "kNoiseVolumeExtent", static_cast<long long>(Kataglyphis::kNoiseVolumeExtent) },
        { "kNoiseWorkgroupSize", static_cast<long long>(Kataglyphis::kNoiseWorkgroupSize) },
        { "kCloudWorkgroupSize", static_cast<long long>(Kataglyphis::kCloudWorkgroupSize) },
        { "kMinCloudMarchSteps", static_cast<long long>(Kataglyphis::kMinCloudMarchSteps) },
        { "kMaxCloudMarchSteps", static_cast<long long>(Kataglyphis::kMaxCloudMarchSteps) },
        { "kMinCloudLightMarchSteps", static_cast<long long>(Kataglyphis::kMinCloudLightMarchSteps) },
        { "kMaxCloudLightMarchSteps", static_cast<long long>(Kataglyphis::kMaxCloudLightMarchSteps) },
    };

    for (const auto &[name, value] : truth) {
        const auto it = constants_rows->find(name);
        ASSERT_TRUE(it != constants_rows->end())
          << doc_path.string() << "'s cloud-constants table is missing a row for `" << name << '`';
        EXPECT_EQ(it->second, value) << doc_path.string() << "'s cloud-constants table says " << name << " = "
                                      << it->second << ", CloudDispatch.hpp says " << value;
    }
    for (const auto &[name, value] : *constants_rows) {
        if (truth.contains(name)) { continue; }
        ADD_FAILURE() << doc_path.string() << "'s cloud-constants table has an unexpected row `" << name << "` = "
                       << value << " with no matching CloudDispatch.hpp constant";
    }
}

// PathTracing.cpp dispatches the path tracing compute pass using
// kPathTracingWorkgroupSizeX/Y (PathTracingDispatch.hpp) to size the
// thread-group grid. Those constants have no compiler-enforced link to the
// [numthreads(...)] attribute path_tracing.slang actually declares - this
// already went wrong once (found 2026-07-31 at (16, 8) against a shader
// compiled at (8, 8), under-covering the image width by 2x every frame).
// Mirrors CloudDispatchGridsMatchTheShaderWorkgroupSizes's shape.
TEST(BuildIntegrity, PathTracingDispatchMatchesTheShaderWorkgroupSize)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path path_tracing_path =
      slangRoot() / "path_tracing" / "path_tracing.slang";

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

// Any image barrier that transitions into eShaderReadOnlyOptimal is handing
// the image to a shader that samples it - naming a stage other than
// eFragmentShader as the destination silently drops the actual hazard being
// guarded against (PathTracing.cpp named eVertexShader on both edges of its
// offscreen image barrier until this gate was added, even though nothing in
// the pass reads a vertex-stage sampler). Raytracing.cpp's
// raytracingToPostImageBarrier is the correct twin this gate is modelled on.
// Anchored on source text via regex, not line numbers, so it survives
// reformatting.
TEST(BuildIntegrity, OffscreenImageBarriersNameTheStageThatConsumesThem)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::array<const char *, 2> kFiles = {
        "Src/GraphicsEngineVulkan/renderer/PathTracing.cpp",
        "Src/GraphicsEngineVulkan/renderer/Raytracing.cpp",
    };

    // Barriers are built via Kataglyphis::buildImageMemoryBarrier
    // (common/ImageBarrierHelper.hpp) rather than field-by-field assignment,
    // so the gate looks for `name = buildImageMemoryBarrier(image, oldLayout,
    // newLayout, ...)` and reads newLayout back out of the third argument.
    static const std::regex kBarrierConstruction(
      R"((\w+)\s*=\s*Kataglyphis::buildImageMemoryBarrier\(([\s\S]*?)\);)");
    static const std::regex kPipelineBarrierCall(R"(commandBuffer\.pipelineBarrier\(([\s\S]*?)\);)");
    static const std::regex kIdentifier(R"([A-Za-z_]\w*)");

    std::size_t gated_barriers_found = 0;
    std::vector<std::string> violations;

    for (const char *relative_path : kFiles) {
        const fs::path path = repo_root / relative_path;
        const auto contentsOpt = readFileText(path);
        ASSERT_TRUE(contentsOpt.has_value()) << "missing " << path.string();
        const std::string &contents = *contentsOpt;

        std::set<std::string> transitions_to_shader_read_only;
        for (auto it = std::sregex_iterator(contents.begin(), contents.end(), kBarrierConstruction);
             it != std::sregex_iterator(); ++it) {
            const std::string barrier_name = (*it)[1].str();
            const std::string call_args = (*it)[2].str();

            std::vector<std::string> args;
            std::size_t arg_start = 0;
            while (true) {
                const std::size_t comma = call_args.find(',', arg_start);
                if (comma == std::string::npos) {
                    args.push_back(call_args.substr(arg_start));
                    break;
                }
                args.push_back(call_args.substr(arg_start, comma - arg_start));
                arg_start = comma + 1;
            }
            ASSERT_GE(args.size(), 3u)
              << relative_path
              << ": buildImageMemoryBarrier call does not have the expected (image, oldLayout, newLayout, ...) "
                 "shape - the scan needs updating:\n"
              << call_args;

            if (args[2].find("eShaderReadOnlyOptimal") != std::string::npos) {
                transitions_to_shader_read_only.insert(barrier_name);
            }
        }

        for (auto it = std::sregex_iterator(contents.begin(), contents.end(), kPipelineBarrierCall);
             it != std::sregex_iterator(); ++it) {
            const std::string call_args = (*it)[1].str();

            std::vector<std::string> parts;
            std::size_t start = 0;
            while (true) {
                const std::size_t comma = call_args.find(',', start);
                if (comma == std::string::npos) {
                    parts.push_back(call_args.substr(start));
                    break;
                }
                parts.push_back(call_args.substr(start, comma - start));
                start = comma + 1;
            }
            ASSERT_EQ(parts.size(), 6u) << relative_path
                                        << ": pipelineBarrier call does not have the expected 6 arguments - either "
                                           "this file grew a differently-shaped call the scan needs updating for, "
                                           "or the naive comma-split broke on one containing a literal comma:\n"
                                        << call_args;

            const std::string &dst_stage = parts[1];
            const std::string &image_barrier_arg = parts[5];

            std::smatch identifier_match;
            if (!std::regex_search(image_barrier_arg, identifier_match, kIdentifier)) { continue; }
            const std::string barrier_name = identifier_match.str();

            if (transitions_to_shader_read_only.find(barrier_name) == transitions_to_shader_read_only.end()) {
                continue;
            }

            ++gated_barriers_found;
            if (dst_stage.find("eFragmentShader") == std::string::npos) {
                violations.push_back(std::string(relative_path) + ": barrier '" + barrier_name +
                  "' transitions to eShaderReadOnlyOptimal but names dst stage '" + dst_stage +
                  "' instead of eFragmentShader");
            }
        }
    }

    ASSERT_GT(gated_barriers_found, 0u) << "found zero image barriers transitioning to eShaderReadOnlyOptimal in "
                                           "PathTracing.cpp/Raytracing.cpp - the scan itself is broken";

    EXPECT_TRUE(violations.empty()) << joinViolations(violations);
}

// A stray NUL or other C0 control byte inside a source file makes
// grep/ripgrep treat the whole file as binary ("binary file matches" instead
// of printing the line), which silently excludes every call site in it from
// this project's grep-based planner/executor workflow. SceneConfig.cpp's
// KATAGLYPHIS_MODEL_OVERRIDE guard carried a literal NUL byte instead of the
// two-character '\0' escape for exactly this reason until this test caught
// it. UTF-8 continuation bytes (>= 0x80) and BOMs are not control bytes and
// are intentionally not flagged.
TEST(BuildIntegrity, ProjectSourcesContainNoStrayControlBytes)
{
    const fs::path repo_root = repoRoot();
    if (repo_root.empty()) { GTEST_SKIP() << "could not locate the repository root"; }

    std::vector<std::string> offenders;

    auto scan_file = [&](const fs::path &path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) { return; }
        const std::string relative = fs::relative(path, repo_root).generic_string();
        char raw_byte;
        std::size_t offset = 0;
        while (file.get(raw_byte)) {
            const auto byte = static_cast<unsigned char>(raw_byte);
            const bool is_stray_control_byte = byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r';
            if (is_stray_control_byte) {
                offenders.push_back(relative + " (byte offset " + std::to_string(offset) + ")");
            }
            ++offset;
        }
    };

    const std::vector<fs::path> code_roots = { repo_root / "Src", repo_root / "Test" };
    for (const auto &root : code_roots) {
        std::error_code error;
        if (!fs::exists(root, error)) { continue; }

        for (fs::recursive_directory_iterator it(root, error), end; it != end; it.increment(error)) {
            if (error) { break; }
            if (!it->is_regular_file(error)) { continue; }
            const auto extension = it->path().extension();
            if (extension != ".cpp" && extension != ".hpp" && extension != ".ixx") { continue; }
            scan_file(it->path());
        }
    }

    const fs::path slang_root = slangRoot();
    const std::string slang_build_prefix = (slang_root / "build").generic_string() + "/";
    std::error_code error;
    if (fs::exists(slang_root, error)) {
        for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
            if (error) { break; }
            const fs::path &path = it->path();
            if (path.generic_string().rfind(slang_build_prefix, 0) == 0) { continue; }// compiled output tree
            if (!it->is_regular_file(error) || path.extension() != ".slang") { continue; }
            scan_file(path);
        }
    }

    EXPECT_TRUE(offenders.empty())
      << offenders.size()
      << " project source file(s) contain a stray control byte (NUL or a C0 control other than tab/LF/CR), "
         "which makes grep/ripgrep treat the file as binary and silently excludes it from every text search "
         "this project's tooling relies on: "
      << joinViolations(offenders);
}

// Every layout contract in this repo used to be a hand-copied number: the
// SceneUBO_std140.cascadeSplits drift (fixed by SceneUboLayoutUnit's pad
// member) survived because nothing compared the host offsetof()s against
// what slangc actually emitted. This test makes the compiled SPIR-V the
// source of truth: it parses every committed .spv's OpName/OpMemberName/
// OpMemberDecorate triples and checks each contracted struct's members land
// at the same byte offset the host struct computes.
TEST(BuildIntegrity, SharedStructOffsetsMatchTheCompiledSpirv)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path spirv_root = spirvRoot();
    ASSERT_TRUE(fs::exists(spirv_root)) << "missing " << spirv_root.string();

    std::map<std::string, std::map<std::string, uint32_t>> compiled;// union across every .spv
    std::error_code error;
    for (fs::recursive_directory_iterator it(spirv_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".spv") { continue; }

        const auto parsed = parse_spirv_member_offsets(it->path());
        if (!parsed.has_value()) { continue; }// unreadable/invalid SPIR-V - CompiledShadersAreNotOlderThan* catches that

        for (const auto &[struct_name, members] : *parsed) {
            for (const auto &[member_name, offset] : members) { compiled[struct_name][member_name] = offset; }
        }
    }

    const auto contracts = build_shared_struct_offset_contracts();
    std::vector<std::string> not_found_in_any_spv;
    std::vector<std::string> offset_mismatches;

    for (const auto &contract : contracts) {
        const auto struct_it = compiled.find(contract.spirv_name);
        if (struct_it == compiled.end()) {
            not_found_in_any_spv.push_back(contract.spirv_name);
            continue;
        }

        for (const auto &[member_name, host_offset] : contract.member_offsets) {
            const auto member_it = struct_it->second.find(member_name);
            if (member_it == struct_it->second.end()) {
                offset_mismatches.push_back(
                  contract.spirv_name + "." + member_name + ": not emitted as a member by any compiled .spv");
                continue;
            }
            if (member_it->second != host_offset) {
                offset_mismatches.push_back(contract.spirv_name + "." + member_name + ": compiled SPIR-V offset "
                  + std::to_string(member_it->second) + " != host offsetof " + std::to_string(host_offset));
            }
        }
    }

    EXPECT_TRUE(not_found_in_any_spv.empty())
      << not_found_in_any_spv.size()
      << " struct(s) expected in the compiled SPIR-V were not emitted by ANY .spv under " << spirv_root.string()
      << " - renamed or deleted shader struct: "
      << joinViolations(not_found_in_any_spv);

    EXPECT_TRUE(offset_mismatches.empty())
      << offset_mismatches.size() << " host/SPIR-V struct-offset mismatch(es):"
      << joinViolations(offset_mismatches);
}

// The host/device push-constant and UBO headers (PushConstant*.hpp,
// GlobalUBO.hpp, SceneUBO.hpp, and the shared/scene/ Vertex.hpp and
// ObjMaterial.hpp) used to be dual-compiled: an `#ifdef __cplusplus` guard
// switched between a C++ definition and a bare-GLSL one for a shader
// compiler that read these headers directly. That compiler was deleted with
// Resources/Shaders/ (see SlangSourcesDoNotReferenceTheDeletedGlslTree
// above), so every one of these headers is now compiled only by C++, and the
// GLSL half of the guard is dead text describing a build that no longer
// exists. `KTG_VEC2`/`KTG_VEC3` were the same shim under a different name.
// This walks every .hpp/.ixx under Src/ (skipping third_party/) and fails on
// any line still mentioning either, so the shim cannot silently come back.
TEST(BuildIntegrity, NoHostDeviceHeaderCarriesTheRetiredGlslDualCompileShim)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (path.generic_string().find("/third_party/") != std::string::npos) { continue; }
        if (!it->is_regular_file(error)) { continue; }
        const auto extension = path.extension();
        if (extension != ".hpp" && extension != ".ixx") { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        int line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find("__cplusplus") != std::string::npos || line.find("KTG_VEC") != std::string::npos) {
                violations.push_back(fs::relative(path, repo_root).generic_string() + ':'
                                      + std::to_string(line_number) + ": " + line);
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " line(s) under " << src_root.string()
      << " still carry the retired GLSL dual-compile shim (__cplusplus guard or KTG_VEC macro) - the GLSL tree "
         "these guarded against is gone and these headers are compiled only by C++: "
      << joinViolations(violations);
}

namespace {

// Parses the member names straight out of SceneUBO's C++ definition (not the
// compiled SPIR-V - a member the host writes but no shader reads compiles
// fine and never turns up in SharedStructOffsetsMatchTheCompiledSpirv, which
// only checks members shaders DO reference). Follows the same "scan by
// text, not by AST" approach as SharedStructOffsetsMatchTheCompiledSpirv's
// SPIR-V parser above. Members prefixed `_pad` are layout filler, not data a
// shader could plausibly read, and are skipped.
std::vector<std::string> parse_scene_ubo_member_names(const fs::path &header_path)
{
    const std::string text = readFileText(header_path).value_or(std::string{});

    const std::size_t struct_pos = text.find("struct SceneUBO");
    if (struct_pos == std::string::npos) { return {}; }
    const std::size_t open_brace = text.find('{', struct_pos);
    if (open_brace == std::string::npos) { return {}; }

    int depth = 1;
    std::size_t close_brace = std::string::npos;
    for (std::size_t pos = open_brace + 1; pos < text.size(); ++pos) {
        if (text[pos] == '{') {
            ++depth;
        } else if (text[pos] == '}') {
            --depth;
            if (depth == 0) {
                close_brace = pos;
                break;
            }
        }
    }
    if (close_brace == std::string::npos) { return {}; }

    const std::string body = text.substr(open_brace + 1, close_brace - open_brace - 1);
    const std::regex array_suffix(R"(\[[^\]]*\])");
    // A genuine member declaration's last token before ';' is a bare
    // identifier. Anything else (e.g. the closing `");` of a static_assert
    // message that wraps onto its own line) is not a declaration and is
    // skipped rather than mis-captured as a bogus member name.
    const std::regex identifier(R"(^[A-Za-z_]\w*$)");

    std::vector<std::string> names;
    std::istringstream body_stream(body);
    std::string line;
    while (std::getline(body_stream, line)) {
        const auto comment_pos = line.find("//");
        if (comment_pos != std::string::npos) { line = line.substr(0, comment_pos); }
        line = std::regex_replace(line, array_suffix, "");

        const auto semi_pos = line.find(';');
        if (semi_pos == std::string::npos) { continue; }

        std::istringstream decl_stream(line.substr(0, semi_pos));
        std::string token;
        std::string last_token;
        while (decl_stream >> token) { last_token = token; }

        if (!std::regex_match(last_token, identifier) || last_token.rfind("_pad", 0) == 0) { continue; }
        names.push_back(last_token);
    }
    return names;
}

// Parses the member names straight out of scene_types.slang's ObjMaterial
// mirror (the shader-visible layout, scalar layout - see
// ObjMaterial.hpp's comment). Same brace-matching approach as
// parse_scene_ubo_member_names above, but the member declarations are bare
// Slang type + identifier (`float3 diffuse;`), not C++ ones.
std::vector<std::string> parse_obj_material_member_names(const fs::path &scene_types_path)
{
    const std::string text = readFileText(scene_types_path).value_or(std::string{});

    const std::size_t struct_pos = text.find("struct ObjMaterial");
    if (struct_pos == std::string::npos) { return {}; }
    const std::size_t open_brace = text.find('{', struct_pos);
    if (open_brace == std::string::npos) { return {}; }

    int depth = 1;
    std::size_t close_brace = std::string::npos;
    for (std::size_t pos = open_brace + 1; pos < text.size(); ++pos) {
        if (text[pos] == '{') {
            ++depth;
        } else if (text[pos] == '}') {
            --depth;
            if (depth == 0) {
                close_brace = pos;
                break;
            }
        }
    }
    if (close_brace == std::string::npos) { return {}; }

    const std::string body = text.substr(open_brace + 1, close_brace - open_brace - 1);
    static const std::regex kMemberDecl(R"(\b(?:float3|float|int)\s+([A-Za-z_]\w*)\s*;)");

    std::vector<std::string> names;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), kMemberDecl); it != std::sregex_iterator(); ++it) {
        names.push_back((*it)[1].str());
    }
    return names;
}

}// namespace

// SceneUBO is 352 bytes uploaded to every swapchain image every frame, and
// nothing checked that a member the host fills is ever read by a shader -
// cloudMovementDirection shipped as a dead vec4 for this exact reason. This
// is the gate: parse every member name out of the C++ struct, then fail
// listing any that no Slang source under Resources/ShadersSlang mentions by
// name. Same shape as EveryShaderHotReloadImplementationIsCalledByTheRenderer
// and EverySlangFunctionIsReachableFromAnEntryPoint - reachability, checked
// in one direction, on data instead of code.
TEST(BuildIntegrity, EverySceneUboFieldIsReadByAShader)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path header_path = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "SceneUBO.hpp";
    ASSERT_TRUE(fs::exists(header_path)) << "missing " << header_path.string();

    const std::vector<std::string> members = parse_scene_ubo_member_names(header_path);
    ASSERT_GT(members.size(), 5U) << "found only " << members.size() << " SceneUBO member(s) in "
                                   << header_path.string() << " - the parser itself is broken";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    std::string all_slang_text;
    std::error_code error;
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".slang") { continue; }
        all_slang_text += readFileText(it->path()).value_or(std::string{});
        all_slang_text += '\n';
    }

    std::vector<std::string> unread;
    for (const auto &member : members) {
        const std::regex word_boundary(R"(\b)" + member + R"(\b)");
        if (!std::regex_search(all_slang_text, word_boundary)) { unread.push_back(member); }
    }

    EXPECT_TRUE(unread.empty())
      << unread.size()
      << " SceneUBO member(s) are written by the host every frame but read by no Slang shader source under "
      << slang_root.string()
      << " - either wire the field into a shader or delete it (see the cloudMovementDirection removal for the "
         "precedent):"
      << joinViolations(unread);
}

// ObjMaterial mirrors SceneUBO's blind spot: a member the host packs into
// every material record but that zero shaders read compiles fine and just
// costs bytes forever - ambient/specular/transmittance/ior/illum shipped
// this way until this gate. Parses the member names straight out of
// scene_types.slang's ObjMaterial mirror (the shader-visible layout) and
// fails listing any that no Slang source references as `material.<name>`.
TEST(BuildIntegrity, EveryObjMaterialFieldIsReadByAShader)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path scene_types_path = repo_root / "Resources/ShadersSlang/common/scene_types.slang";
    ASSERT_TRUE(fs::exists(scene_types_path)) << "missing " << scene_types_path.string();

    const std::vector<std::string> members = parse_obj_material_member_names(scene_types_path);
    ASSERT_GT(members.size(), 3U) << "found only " << members.size() << " ObjMaterial member(s) in "
                                   << scene_types_path.string() << " - the parser itself is broken";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    std::string all_slang_text;
    std::error_code error;
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".slang") { continue; }
        all_slang_text += readFileText(it->path()).value_or(std::string{});
        all_slang_text += '\n';
    }

    std::vector<std::string> unread;
    for (const auto &member : members) {
        const std::regex material_dot_field(R"(material\.)" + member + R"(\b)");
        if (!std::regex_search(all_slang_text, material_dot_field)) { unread.push_back(member); }
    }

    EXPECT_TRUE(unread.empty())
      << unread.size()
      << " ObjMaterial member(s) are uploaded per material but read by no Slang shader source (as "
         "material.<name>) under "
      << slang_root.string()
      << " - either wire the field into a shader or delete it (see the ambient/specular/transmittance/ior/illum "
         "removal for the precedent):"
      << joinViolations(unread);
}

// EverySceneUboFieldIsReadByAShader above checks whole member names, so a
// member whose .xyz is read but whose .w quietly starts carrying real data
// (cam_pos.w used to ship fov this way - nothing ever read it back) passes
// that gate anyway. This test reads SceneUboMarshal.hpp's fillSceneUboCamera
// / fillSceneUboDirectionalLight source directly: for each
// `ubo.<field> = glm::vec4(<xyz>, <wArg>)` assignment, if wArg is not a
// float literal (i.e. it packs real per-frame data rather than a filler
// constant), asserts some .slang source dereferences `<field>.w`.
TEST(BuildIntegrity, SceneUboWComponentsCarryingDataAreReadByAShader)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path marshal_path = repo_root / "Src" / "GraphicsEngineVulkan" / "common" / "SceneUboMarshal.hpp";
    const auto marshal_source = readFileText(marshal_path);
    ASSERT_TRUE(marshal_source.has_value()) << "missing " << marshal_path.string();

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();
    std::string all_slang_text;
    std::error_code error;
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".slang") { continue; }
        all_slang_text += readFileText(it->path()).value_or(std::string{});
        all_slang_text += '\n';
    }

    static const std::regex kFloatLiteral(R"(^-?[0-9]+\.[0-9]+[fF]?$)");
    static const std::array<const char *, 4> kWBearingFields{
        "view_dir", R"(dirLight\.direction)", R"(dirLight\.color)", "cam_pos"
    };

    bool matchedRadianceField = false;
    for (const char *field : kWBearingFields) {
        const std::regex assignRegex(std::string("ubo\\.") + field
          + R"(\s*=\s*glm::vec4\([^;]*?,\s*([A-Za-z_][A-Za-z0-9_]*|-?[0-9]+\.[0-9]+[fF]?)\s*\)\s*;)");
        std::smatch match;
        ASSERT_TRUE(std::regex_search(*marshal_source, match, assignRegex))
          << marshal_path.string() << " no longer assigns ubo." << field
          << " as glm::vec4(xyz, w) - update this parser to match the new packing shape";

        const std::string wArg = match[1].str();
        if (std::regex_match(wArg, kFloatLiteral)) { continue; }// filler constant, nothing to check

        if (std::string(field) == R"(dirLight\.color)") { matchedRadianceField = true; }

        const std::regex wRead(field + std::string(R"(\.w\b)"));
        EXPECT_TRUE(std::regex_search(all_slang_text, wRead))
          << "SceneUboMarshal.hpp packs a non-constant value (" << wArg << ") into ubo." << field
          << ".w, but no .slang source under " << slang_root.string() << " reads " << field
          << ".w - either wire it into a shader or write a constant filler instead";
    }

    EXPECT_TRUE(matchedRadianceField)
      << "expected dirLight.color's .w assignment (radiance) to be the one non-constant SceneUBO "
         ".w slot this test verifies - if that packing moved, this sanity check needs updating too";
}

// FileReader.ixx spells out the rule this test enforces: the error_code
// overload of std::filesystem's query functions is REQUIRED, not stylistic,
// because exceptions are disabled project-wide (-fno-exceptions/EHs-) and the
// throwing overloads therefore std::terminate the whole process on any OS
// error the query reports - most commonly permission denied. scanAvailableModels
// used to walk a user-populated directory (Resources/Models, recursively)
// with the throwing range-for form; a permission-denied subdirectory or a
// junction loop was enough to take the engine down instead of skipping the
// entry. This scans every .cpp/.ixx/.hpp under Src/ for a call to one of the
// throwing-capable functions and fails any call site with no error_code
// argument in sight. A deliberate exception can carry a
// "// NO_EC_OK: <reason>" trailing comment.
TEST(BuildIntegrity, EngineSourcesUseNonThrowingFilesystemOverloads)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const std::vector<std::string> kWatchedCalls = {
        "filesystem::exists(",
        "filesystem::current_path(",
        "filesystem::relative(",
        "filesystem::create_directories(",
        "directory_iterator(",// also matches recursive_directory_iterator(
    };
    static const std::string kNoEcMarker = "NO_EC_OK:";

    // Not a full parser - a plain substring search for the naming idioms this
    // codebase actually uses for an std::error_code out-parameter (ec,
    // *_ec, *_error, error_code, or the FileReader.ixx "ignored" idiom for a
    // deliberately-discarded one).
    auto carries_error_code_token = [](const std::string &text) {
        return text.find("ec") != std::string::npos || text.find("error") != std::string::npos
          || text.find("ignored") != std::string::npos;
    };

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        const auto extension = path.extension();
        if (extension != ".cpp" && extension != ".ixx" && extension != ".hpp") { continue; }

        const auto lines_opt = readFileLines(path);
        if (!lines_opt) { continue; }
        const auto &lines = *lines_opt;

        constexpr int kLookaheadLines = 2;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            for (const auto &watched : kWatchedCalls) {
                if (lines[i].find(watched) == std::string::npos) { continue; }
                if (lines[i].find(kNoEcMarker) != std::string::npos) { continue; }

                std::string window = lines[i];
                for (int extra = 1; extra <= kLookaheadLines && i + static_cast<std::size_t>(extra) < lines.size();
                     ++extra) {
                    window += lines[i + static_cast<std::size_t>(extra)];
                }

                if (!carries_error_code_token(window)) {
                    violations.push_back(
                      fs::relative(path, repo_root).generic_string() + ":" + std::to_string(i + 1) + ": " + lines[i]);
                }
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " std::filesystem call(s) under Src/ use a throwing overload (no error_code argument found nearby) - "
         "exceptions are disabled project-wide (-fno-exceptions/EHs-), so this aborts the process instead of "
         "returning an error; pass the error_code overload (see FileReader.ixx), or for a deliberate exception "
         "add a \"// NO_EC_OK: <reason>\" trailing comment on the line:"
      << joinViolations(violations);
}

TEST(BuildIntegrity, EngineSourcesDoNotLogRawVulkanHandles)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        const auto extension = path.extension();
        if (extension != ".cpp" && extension != ".ixx") { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find("spdlog::") == std::string::npos) { continue; }
            const bool has_vk_cast_to_uint64 = line.find("(uint64_t)(Vk") != std::string::npos;
            const bool has_hex_format_with_vk_cast = line.find("0x{:x}") != std::string::npos && line.find("Vk") != std::string::npos;
            if (has_vk_cast_to_uint64 || has_hex_format_with_vk_cast) {
                violations.push_back(
                  fs::relative(path, repo_root).generic_string() + ":" + std::to_string(line_number) + ": " + line);
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " spdlog call(s) under Src/ log a raw Vulkan handle - these are noise (the validation layers and Vulkan "
         "debug labels already give a better diagnostic) and read as intentional instrumentation; delete them "
         "instead of demoting to spdlog::debug:"
      << joinViolations(violations);
}

// Descriptor set layout/pool/set triads must be declared through
// DescriptorSetGroup (vulkan_base/DescriptorSetGroup.ixx) rather than
// hand-rolled per subsystem - see that class's own comment on the triads it
// was extracted to own. The allowlist below is anchored to relative file
// paths (stable across line-number churn), not line numbers, matching the
// other allowlists in this file.
TEST(BuildIntegrity, DescriptorSetsAreCreatedThroughDescriptorSetGroup)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const std::array<const char *, 3> kRawDescriptorTypeNames = {
        "vk::DescriptorSetLayoutCreateInfo", "vk::DescriptorPoolCreateInfo", "vk::DescriptorSetAllocateInfo"
    };

    // DescriptorSetGroup.cpp is the abstraction's own implementation - it
    // stays a whole-file exemption (its internal count is an implementation
    // detail, and pinning it to a fixed number would fail on every
    // legitimate edit). GUI.cpp is different: it creates exactly one
    // descriptor pool for ImGui, which allocates and manages its own
    // descriptor sets internally via ImGui_ImplVulkan, so it gets a
    // per-file budget instead of a blanket skip - a second, hand-rolled
    // triad added to that file would still be caught.
    static const std::array<const char *, 1> kExemptFiles = {
        "Src/GraphicsEngineVulkan/vulkan_base/DescriptorSetGroup.cpp"
    };
    static const std::map<std::string, std::size_t> kDescriptorBudgets = {
        { "Src/GraphicsEngineVulkan/gui/GUI.cpp", 1 },
    };

    std::map<std::string, std::size_t> counts;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (std::find(kExemptFiles.begin(), kExemptFiles.end(), relative_file) != kExemptFiles.end()) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        for (const auto &line : *lines) {
            for (const char *type_name : kRawDescriptorTypeNames) {
                if (line.find(type_name) == std::string::npos) { continue; }
                ++counts[relative_file];
            }
        }
    }

    std::vector<std::string> violations;
    for (const auto &[file, count] : counts) {
        const auto budget_it = kDescriptorBudgets.find(file);
        const std::size_t budget = budget_it == kDescriptorBudgets.end() ? 0 : budget_it->second;
        if (count == budget) { continue; }
        if (count > budget) {
            violations.push_back(file + ": found " + std::to_string(count)
                                  + " hand-rolled descriptor set layout/pool/set triad(s), budget is "
                                  + std::to_string(budget)
                                  + " - declare bindings through DescriptorSetGroup "
                                    "(vulkan_base/DescriptorSetGroup.ixx) via addBinding()/create() instead");
        } else {
            violations.push_back(file + ": found " + std::to_string(count)
                                  + " hand-rolled descriptor set layout/pool/set triad(s), budget is "
                                  + std::to_string(budget) + " - lower the budget in this test");
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " budget mismatch(es) for hand-rolled descriptor set layout/pool/set triad(s) found under Src/:"
      << joinViolations(violations);
}

TEST(BuildIntegrity, DescriptorBudgetsNameOnlyFilesThatStillHaveTriads)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::array<const char *, 3> kRawDescriptorTypeNames = {
        "vk::DescriptorSetLayoutCreateInfo", "vk::DescriptorPoolCreateInfo", "vk::DescriptorSetAllocateInfo"
    };
    static const std::map<std::string, std::size_t> kDescriptorBudgets = {
        { "Src/GraphicsEngineVulkan/gui/GUI.cpp", 1 },
    };

    for (const auto &[file, budget] : kDescriptorBudgets) {
        EXPECT_GT(budget, 0u) << file << ": a budgeted file must have a non-zero budget, or it is a dead entry";

        const fs::path path = repo_root / file;
        ASSERT_TRUE(fs::exists(path)) << "budgeted file no longer exists: " << file;

        const auto lines = readFileLines(path);
        ASSERT_TRUE(lines.has_value()) << "could not read " << file;
        std::size_t count = 0;
        for (const auto &line : *lines) {
            for (const char *type_name : kRawDescriptorTypeNames) {
                if (line.find(type_name) != std::string::npos) { ++count; }
            }
        }
        EXPECT_EQ(count, budget) << file << ": budget says " << budget << " but the file currently has " << count;
    }
}

// asManager.createASForScene()/createTLAS() rebuild the acceleration
// structure that the raytracing/post/GBuffer descriptors are bound to; a
// rebuild that isn't immediately followed by updateAllDescriptorSets()
// leaves those descriptors bound to the TLAS handle the rebuild just
// destroyed - reproduced on the RX 9070 XT as a
// VUID-vkCmdDispatch-None-08114 validation error (see 08f01ce5). The only
// place that pairs the rebuild with the descriptor refresh is
// VulkanRenderer::refreshAfterSceneChange, so every call site must go
// through it rather than calling the ASManager methods directly.
TEST(BuildIntegrity, AccelerationStructureRebuildsGoThroughTheSceneChangeHelper)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const std::array<const char *, 2> kGuardedCalls = { "asManager.createASForScene(",
        "asManager.createTLAS(" };
    static const char *const kAllowedFile = "Src/GraphicsEngineVulkan/renderer/VulkanRenderer.cpp";
    static const char *const kAllowedFunction = "VulkanRenderer::refreshAfterSceneChange";
    static const std::regex kFunctionSignature(R"(([A-Za-z_][A-Za-z0-9_:]*::[A-Za-z_][A-Za-z0-9_]*)\s*\()");

    std::vector<std::string> violations;
    std::size_t matches_found = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        const auto extension = path.extension();
        if (extension != ".cpp" && extension != ".ixx") { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        const std::string relative_file = fs::relative(path, repo_root).generic_string();

        std::size_t line_number = 0;
        int brace_depth = 0;
        std::string current_function;
        bool awaiting_open_brace = false;
        std::string pending_function_name;

        for (const auto &line : *lines) {
            ++line_number;

            if (awaiting_open_brace) {
                if (line.find('{') != std::string::npos) {
                    current_function = pending_function_name;
                    awaiting_open_brace = false;
                } else if (line.find(';') != std::string::npos) {
                    // A declaration/prototype after all, not a definition.
                    awaiting_open_brace = false;
                    pending_function_name.clear();
                }
            } else if (brace_depth == 0 && line.find("::") != std::string::npos) {
                std::smatch match;
                if (std::regex_search(line, match, kFunctionSignature)) {
                    if (line.find('{') != std::string::npos) {
                        // Signature and opening brace share this line (e.g. a
                        // one-line forwarding function) - the brace-counting
                        // pass below will clear this once its body closes.
                        current_function = match[1].str();
                    } else {
                        pending_function_name = match[1].str();
                        awaiting_open_brace = true;
                    }
                }
            }

            for (const char letter : line) {
                if (letter == '{') {
                    ++brace_depth;
                } else if (letter == '}') {
                    if (brace_depth > 0) { --brace_depth; }
                    if (brace_depth == 0) { current_function.clear(); }
                }
            }

            for (const char *guarded_call : kGuardedCalls) {
                if (line.find(guarded_call) == std::string::npos) { continue; }
                ++matches_found;
                const bool is_allowed =
                  relative_file == kAllowedFile && current_function.find(kAllowedFunction) != std::string::npos;
                if (!is_allowed) { violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line); }
            }
        }
    }

    ASSERT_GT(matches_found, 0u) << "found zero asManager.createASForScene(...)/asManager.createTLAS(...) call "
                                    "sites under Src/ - the scan itself is broken, or every call site was removed "
                                    "and this gate is now vacuous";

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " acceleration-structure rebuild(s) bypass VulkanRenderer::refreshAfterSceneChange(), the only place "
         "that follows a rebuild with updateAllDescriptorSets() - route through it instead of calling "
         "asManager.createASForScene()/createTLAS() directly:"
      << joinViolations(violations);

    const auto header_contents_opt =
      readFileText(repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "VulkanRenderer.ixx");
    ASSERT_TRUE(header_contents_opt.has_value()) << "missing VulkanRenderer.ixx";
    const std::string &header_contents = *header_contents_opt;
    ASSERT_NE(header_contents.find("void refreshAfterSceneChange(bool rebuildBottomLevel);"), std::string::npos)
      << "VulkanRenderer::refreshAfterSceneChange is no longer declared in VulkanRenderer.ixx - this gate's "
         "allow-list needs updating if it was renamed or its signature changed";
}

// Every stage class that defines its own shaderHotReload(...) must actually
// be called from VulkanRenderer::shaderHotReload, or hot reload silently
// does nothing for that stage (see DeferredRasterizer::shaderHotReload,
// which was fully implemented but uncalled). This is a count-based gate
// rather than a name-to-member allowlist deliberately - a hand-maintained
// mapping is the failure mode 30154355 just removed from Windows CI's suite
// allowlist.
TEST(BuildIntegrity, EveryShaderHotReloadImplementationIsCalledByTheRenderer)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const std::regex kDefinition(R"(([A-Za-z_][A-Za-z0-9_]*)::shaderHotReload\s*\()");
    static const char *const kExcludedClass = "VulkanRenderer";

    std::vector<std::string> implementing_classes;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp") { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        for (const auto &line : *lines) {
            std::smatch match;
            if (std::regex_search(line, match, kDefinition)) {
                std::string class_name = match[1].str();
                if (class_name != kExcludedClass) { implementing_classes.push_back(class_name); }
            }
        }
    }

    ASSERT_GT(implementing_classes.size(), 0u)
      << "found zero <Stage>::shaderHotReload(...) implementations under " << src_root.string()
      << " - the scan itself is broken, or every implementation was removed and this gate is now vacuous";

    const fs::path renderer_path = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "VulkanRenderer.cpp";
    const auto renderer_contents_opt = readFileText(renderer_path);
    ASSERT_TRUE(renderer_contents_opt.has_value()) << "missing " << renderer_path.string();
    const std::string &renderer_contents = *renderer_contents_opt;

    const std::size_t signature_pos = renderer_contents.find("VulkanRenderer::shaderHotReload(");
    ASSERT_NE(signature_pos, std::string::npos) << "VulkanRenderer::shaderHotReload is no longer defined in "
                                                 << renderer_path.string();

    const std::size_t body_open = renderer_contents.find('{', signature_pos);
    ASSERT_NE(body_open, std::string::npos) << "could not locate the opening brace of VulkanRenderer::shaderHotReload";

    int brace_depth = 0;
    std::size_t body_close = std::string::npos;
    for (std::size_t i = body_open; i < renderer_contents.size(); ++i) {
        if (renderer_contents[i] == '{') { ++brace_depth; }
        else if (renderer_contents[i] == '}') {
            --brace_depth;
            if (brace_depth == 0) {
                body_close = i;
                break;
            }
        }
    }
    ASSERT_NE(body_close, std::string::npos) << "could not brace-match the closing '}' of VulkanRenderer::shaderHotReload";

    const std::string body = renderer_contents.substr(body_open, body_close - body_open + 1);
    std::size_t call_sites = 0;
    std::size_t pos = 0;
    while ((pos = body.find(".shaderHotReload(", pos)) != std::string::npos) {
        ++call_sites;
        pos += std::string(".shaderHotReload(").size();
    }

    EXPECT_GE(call_sites, implementing_classes.size())
      << "VulkanRenderer::shaderHotReload only calls " << call_sites << " stage(s)' shaderHotReload, but "
      << implementing_classes.size() << " stage(s) implement it - some implementation is silently unreachable. "
         "Stage classes implementing shaderHotReload: "
      << joinViolations(implementing_classes);
}

// Every subsystem that loads SPIR-V must implement shaderHotReload, or the
// GUI's reload button silently does nothing for it. SkyBox, CascadedShadowMap
// and Clouds were missing theirs until this gate was added - source-scanned
// by the literal SPIR-V directory rather than a hand-maintained file list, so
// the NEXT pipeline-owning subsystem cannot be added without one either.
TEST(BuildIntegrity, EverySpirvLoadingSubsystemImplementsShaderHotReload)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const char *const kSpirvMarker = "Resources/ShadersSlang/build/spirv/";

    std::vector<fs::path> spirv_loading_files;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".cpp") { continue; }

        const auto contents = readFileText(path);
        if (!contents) { continue; }
        if (contents->find(kSpirvMarker) != std::string::npos) { spirv_loading_files.push_back(path); }
    }

    ASSERT_GT(spirv_loading_files.size(), 0u)
      << "found zero .cpp files referencing " << kSpirvMarker << " under " << src_root.string()
      << " - the scan itself is broken";

    std::vector<std::string> missing_implementation;
    for (const fs::path &path : spirv_loading_files) {
        const auto contents = readFileText(path);
        ASSERT_TRUE(contents.has_value()) << "could not re-open " << path.string();

        bool declares_reload = contents->find("shaderHotReload") != std::string::npos;
        if (!declares_reload) {
            const fs::path paired_ixx = fs::path(path).replace_extension(".ixx");
            if (const auto ixx_contents = readFileText(paired_ixx)) {
                declares_reload = ixx_contents->find("shaderHotReload") != std::string::npos;
            }
        }

        if (!declares_reload) { missing_implementation.push_back(fs::relative(path, repo_root).string()); }
    }

    EXPECT_TRUE(missing_implementation.empty())
      << missing_implementation.size()
      << " subsystem(s) load SPIR-V but declare no shaderHotReload (neither the .cpp nor its paired .ixx): "
      << joinViolations(missing_implementation);

    const fs::path renderer_path = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "VulkanRenderer.cpp";
    const auto renderer_contents_opt = readFileText(renderer_path);
    ASSERT_TRUE(renderer_contents_opt.has_value()) << "missing " << renderer_path.string();
    const std::string &renderer_contents = *renderer_contents_opt;

    const std::size_t signature_pos = renderer_contents.find("VulkanRenderer::shaderHotReload(");
    ASSERT_NE(signature_pos, std::string::npos) << "VulkanRenderer::shaderHotReload is no longer defined in "
                                                 << renderer_path.string();

    const std::size_t body_open = renderer_contents.find('{', signature_pos);
    ASSERT_NE(body_open, std::string::npos) << "could not locate the opening brace of VulkanRenderer::shaderHotReload";

    int brace_depth = 0;
    std::size_t body_close = std::string::npos;
    for (std::size_t i = body_open; i < renderer_contents.size(); ++i) {
        if (renderer_contents[i] == '{') { ++brace_depth; }
        else if (renderer_contents[i] == '}') {
            --brace_depth;
            if (brace_depth == 0) {
                body_close = i;
                break;
            }
        }
    }
    ASSERT_NE(body_close, std::string::npos) << "could not brace-match the closing '}' of VulkanRenderer::shaderHotReload";

    const std::string body = renderer_contents.substr(body_open, body_close - body_open + 1);
    std::size_t call_sites = 0;
    std::size_t pos = 0;
    while ((pos = body.find(".shaderHotReload(", pos)) != std::string::npos) {
        ++call_sites;
        pos += std::string(".shaderHotReload(").size();
    }

    EXPECT_GE(call_sites, spirv_loading_files.size())
      << "VulkanRenderer::shaderHotReload only calls " << call_sites << " stage(s)' shaderHotReload, but "
      << spirv_loading_files.size() << " subsystem(s) load SPIR-V - some reload is silently unreachable.";
}

// Every stage's shaderHotReload(...) recreates its pipeline (and thus a fresh
// vk::PipelineLayout) without first destroying the old layout - only
// DeferredRasterizer got this right; PostStage, Rasterizer, Raytracing and
// PathTracing all overwrote a live handle, leaking one vk::PipelineLayout per
// hot reload. All five stages create a layout in their create function, so
// the rule below needs no per-class exceptions.
TEST(BuildIntegrity, EveryShaderHotReloadDestroysThePipelineLayoutItRecreates)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const std::regex kDefinition(R"(([A-Za-z_][A-Za-z0-9_]*)::shaderHotReload\s*\()");
    static const char *const kExcludedClass = "VulkanRenderer";// delegates to the per-stage implementations

    std::vector<std::string> offenders;
    std::size_t checked = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".cpp") { continue; }

        const auto contentsOpt = readFileText(path);
        if (!contentsOpt) { continue; }
        const std::string &contents = *contentsOpt;

        std::smatch match;
        std::string remaining = contents;
        std::size_t offset = 0;
        while (std::regex_search(remaining, match, kDefinition)) {
            const std::size_t signature_pos = offset + static_cast<std::size_t>(match.position(0));
            const std::string class_name = match[1].str();

            const std::size_t body_open = contents.find('{', signature_pos);
            if (body_open == std::string::npos) { break; }

            int brace_depth = 0;
            std::size_t body_close = std::string::npos;
            for (std::size_t i = body_open; i < contents.size(); ++i) {
                if (contents[i] == '{') { ++brace_depth; }
                else if (contents[i] == '}') {
                    --brace_depth;
                    if (brace_depth == 0) {
                        body_close = i;
                        break;
                    }
                }
            }
            ASSERT_NE(body_close, std::string::npos)
              << "could not brace-match the closing '}' of " << class_name << "::shaderHotReload in " << path.string();

            if (class_name != kExcludedClass) {
                ++checked;
                const std::string body = contents.substr(body_open, body_close - body_open + 1);
                if (body.find("destroyPipelineLayout(") == std::string::npos
                    && body.find("destroyPipelineAndLayout(") == std::string::npos) {
                    offenders.push_back(class_name + " (" + fs::relative(path, repo_root).string() + ")");
                }
            }

            const std::size_t advance = body_close + 1;
            offset = advance;
            remaining = contents.substr(advance);
        }
    }

    ASSERT_GT(checked, 0u) << "found zero non-delegating <Stage>::shaderHotReload(...) implementations under "
                           << src_root.string() << " - the scan itself is broken";

    EXPECT_TRUE(offenders.empty())
      << offenders.size()
      << " shaderHotReload(...) implementation(s) recreate a pipeline layout without destroying the previous "
         "one first, leaking a vk::PipelineLayout on every hot reload: "
      << joinViolations(offenders);
}

// Raytracing::shaderHotReload destroys and recreates graphicsPipeline, but
// the shader binding table buffers hold shader-group handles read out of the
// *previous* pipeline (see Raytracing::createSBT). Shader group handles are
// only valid for the pipeline that produced them, so every hot reload must
// rebuild the SBT alongside the pipeline or traceRaysKHR reads handles from a
// destroyed pipeline - a spec violation. This gate cannot be deleted as
// arbitrary busywork for that reason.
TEST(BuildIntegrity, RaytracingShaderHotReloadRebuildsTheShaderBindingTable)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path raytracing_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "Raytracing.cpp";
    const auto contentsOpt = readFileText(raytracing_path);
    ASSERT_TRUE(contentsOpt.has_value()) << "missing " << raytracing_path.string();
    const std::string &contents = *contentsOpt;

    const std::size_t signature_pos = contents.find("Raytracing::shaderHotReload(");
    ASSERT_NE(signature_pos, std::string::npos)
      << "Raytracing::shaderHotReload is no longer defined in " << raytracing_path.string();

    const std::size_t body_open = contents.find('{', signature_pos);
    ASSERT_NE(body_open, std::string::npos) << "could not locate the opening brace of Raytracing::shaderHotReload";

    int brace_depth = 0;
    std::size_t body_close = std::string::npos;
    for (std::size_t i = body_open; i < contents.size(); ++i) {
        if (contents[i] == '{') { ++brace_depth; }
        else if (contents[i] == '}') {
            --brace_depth;
            if (brace_depth == 0) {
                body_close = i;
                break;
            }
        }
    }
    ASSERT_NE(body_close, std::string::npos)
      << "could not brace-match the closing '}' of Raytracing::shaderHotReload";

    const std::string body = contents.substr(body_open, body_close - body_open + 1);
    EXPECT_TRUE(body.find("recreateSBT") != std::string::npos || body.find("createSBT") != std::string::npos)
      << "Raytracing::shaderHotReload no longer rebuilds the shader binding table after recreating the "
         "pipeline - shader group handles are only valid for the pipeline that produced them, so every "
         "hot reload must call recreateSBT() (or createSBT()) after createGraphicsPipeline(), or "
         "traceRaysKHR reads shader-group handles from a destroyed pipeline";
}

// Clouds and PathTracing used to each hand-roll their own
// vk::ComputePipelineCreateInfo. createComputePipeline
// (vulkan_base/ShaderHelper.ixx/.cpp) is now the one place that builds one -
// this pins that down so a future compute pass cannot silently reintroduce
// the duplication.
TEST(BuildIntegrity, ComputePipelinesAreCreatedThroughTheSharedHelper)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const char *const kRawComputePipelineTypeName = "vk::ComputePipelineCreateInfo";

    // ComputePipelineHelper.hpp is the pure builder; ShaderHelper.cpp is the
    // device-side helper's own implementation.
    static const std::array<const char *, 2> kExemptFiles = {
        "Src/GraphicsEngineVulkan/common/ComputePipelineHelper.hpp",
        "Src/GraphicsEngineVulkan/vulkan_base/ShaderHelper.cpp"
    };

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (std::find(kExemptFiles.begin(), kExemptFiles.end(), relative_file) != kExemptFiles.end()) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find(kRawComputePipelineTypeName) == std::string::npos) { continue; }
            violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " hand-rolled vk::ComputePipelineCreateInfo found under Src/ - create compute pipelines through "
         "createComputePipeline (vulkan_base/ShaderHelper.ixx) instead:"
      << joinViolations(violations);
}

// Raytracing.cpp used to hand-roll six vk::PipelineShaderStageCreateInfo
// (four stages) and four vk::RayTracingShaderGroupCreateInfoKHR field-by-field,
// with twelve VK_SHADER_UNUSED_KHR sentinels spelled out where
// common/ShaderStageHelper.hpp's buildShaderStageCreateInfo/
// buildGeneralShaderGroup/buildTrianglesHitGroup builders now do the job in
// four lines. This pins that down so a future pipeline cannot silently
// reintroduce the duplication - a wrong pName or a wrong UNUSED_KHR sentinel
// is a value a compiler cannot check, and produces a silently broken
// pipeline rather than a build error.
TEST(BuildIntegrity, EveryPipelineShaderStageGoesThroughTheSharedBuilder)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    // ShaderStageHelper.hpp is the builders' own definition.
    static const std::array<const char *, 1> kExemptFiles = {
        "Src/GraphicsEngineVulkan/common/ShaderStageHelper.hpp"
    };

    static const std::regex kPNameAssignment(R"(\.pName\s*=[^=])");
    static const std::regex kStageAssignment(R"(\.stage\s*=[^=])");
    // A local vk::PipelineShaderStageCreateInfo declaration - PipelineBuilder's
    // std::span<...>/std::vector<...> *parameters* of that type construct
    // nothing, so they must not trip this. Requires the type name not be
    // immediately preceded by a template angle bracket on the same match.
    static const std::regex kLocalDeclaration(
      R"((?:^|[^<,]\s)vk::PipelineShaderStageCreateInfo\s+\w+\s*[{;])");

    std::size_t checked = 0;
    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx" && path.extension() != ".hpp") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (std::find(kExemptFiles.begin(), kExemptFiles.end(), relative_file) != kExemptFiles.end()) { continue; }

        const auto contentsOpt = readFileText(path);
        if (!contentsOpt.has_value()) { continue; }
        ++checked;

        bool has_pname_assignment = false;
        bool has_local_declaration = false;
        bool has_stage_assignment = false;
        std::istringstream stream(*contentsOpt);
        std::string line;
        while (std::getline(stream, line)) {
            if (std::regex_search(line, kPNameAssignment)) { has_pname_assignment = true; }
            if (std::regex_search(line, kLocalDeclaration)) { has_local_declaration = true; }
            if (std::regex_search(line, kStageAssignment)) { has_stage_assignment = true; }
        }

        if (has_pname_assignment) {
            violations.push_back(relative_file + ": hand-assigns .pName instead of using buildShaderStageCreateInfo");
        }
        if (has_local_declaration && has_stage_assignment) {
            violations.push_back(relative_file
              + ": declares a local vk::PipelineShaderStageCreateInfo and hand-assigns .stage instead of using "
                "buildShaderStageCreateInfo");
        }
    }

    ASSERT_GT(checked, 0u) << "found zero files under " << src_root.string() << " - the scan itself is broken";

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " hand-rolled vk::PipelineShaderStageCreateInfo field assignment(s) found under "
         "Src/GraphicsEngineVulkan/ - build shader stages through common/ShaderStageHelper.hpp's "
         "buildShaderStageCreateInfo instead:"
      << joinViolations(violations);
}

// Every hand-rolled destroyPipelineLayout(...)/destroyPipeline(...) teardown
// pair used to be spelled out at each of the 20 call sites across 8 files.
// Kataglyphis::destroyPipelineAndLayout (common/PipelineLayoutHelper.hpp) is
// now the one place that destroys a pipeline and its layout together - this
// pins that down so a future stage cannot silently reintroduce the
// duplication, the same way ComputePipelinesAreCreatedThroughTheSharedHelper
// pins down pipeline creation.
TEST(BuildIntegrity, PipelineTeardownGoesThroughTheSharedHelper)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const char *const kRawTeardownCall = "destroyPipelineLayout(";

    // PipelineLayoutHelper.hpp is the helper's own definition.
    static const std::array<const char *, 1> kExemptFiles = {
        "Src/GraphicsEngineVulkan/common/PipelineLayoutHelper.hpp"
    };

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (std::find(kExemptFiles.begin(), kExemptFiles.end(), relative_file) != kExemptFiles.end()) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find(kRawTeardownCall) == std::string::npos) { continue; }
            violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " hand-rolled destroyPipelineLayout(...) call(s) found under Src/GraphicsEngineVulkan/ - destroy "
         "pipelines and their layouts through Kataglyphis::destroyPipelineAndLayout "
         "(common/PipelineLayoutHelper.hpp) instead:"
      << joinViolations(violations);
}

// Every raster stage used to spell out its own depth attachment creation
// chain by hand: chooseDepthFormat -> createImage(...,
// eDepthStencilAttachment, eDeviceLocal) -> createImageView(...).
// Kataglyphis::VulkanRendererInternals::createDepthAttachment
// (renderer/DepthAttachment.ixx) is now the one place that chain is spelled
// out - this pins that down the same way PipelineTeardownGoesThroughTheSharedHelper
// does for pipeline teardown, so a fourth raster stage cannot pick a
// different tiling or memory property by accident. CascadedShadowMap's
// shadow map array is a deliberate non-goal (a sampled 2D array behind a
// comparison sampler, not a plain attachment) and is allowlisted via a
// "// DEPTH_ATTACHMENT_CHAIN_OK: <marker>" trailing comment on the exempted
// line rather than a bare file exemption, so an unrelated edit cannot
// silently widen the exemption to a second call site added later in the
// same file (e8b1db52 is the precedent for anchoring an allowlist to a
// source marker instead of a line number).
TEST(BuildIntegrity, NoStageHandRollsTheDepthAttachmentChain)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    // Matches only the image-usage-flag spelling, not the unrelated
    // vk::AccessFlagBits::eDepthStencilAttachment* / vk::ImageLayout::
    // eDepthStencilAttachmentOptimal / vk::FormatFeatureFlagBits::
    // eDepthStencilAttachment spellings, which appear in several other files.
    static const char *const kUsageBit = "ImageUsageFlagBits::eDepthStencilAttachment";
    static const char *const kMarkerPrefix = "DEPTH_ATTACHMENT_CHAIN_OK: ";

    // The helper's own definition is exempt from its own rule.
    static const char *const kHelperFile = "Src/GraphicsEngineVulkan/renderer/DepthAttachment.ixx";

    std::vector<std::string> violations;
    bool marker_found = false;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (relative_file == kHelperFile) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find(kUsageBit) == std::string::npos) { continue; }
            if (line.find(kMarkerPrefix) != std::string::npos) {
                marker_found = true;
                continue;
            }
            violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " hand-rolled vk::ImageUsageFlagBits::eDepthStencilAttachment use(s) found under "
         "Src/GraphicsEngineVulkan/ - route depth attachment creation through "
         "Kataglyphis::VulkanRendererInternals::createDepthAttachment (renderer/DepthAttachment.ixx) instead, "
         "or add a \"// DEPTH_ATTACHMENT_CHAIN_OK: <marker>\" trailing comment on the line if the site is a "
         "deliberate non-goal:"
      << joinViolations(violations);

    EXPECT_TRUE(marker_found) << "expected to find the CascadedShadowMap.cpp shadow-map-array exemption marker "
                                  "(\"// DEPTH_ATTACHMENT_CHAIN_OK: ...\") in source - if it was removed, delete "
                                  "this check too";
}

// Rasterizer, DeferredRasterizer and PostStage used to each spell out their
// own external subpass dependency by hand; they now share
// Kataglyphis::buildExternalColorDepthDependency (common/RenderPassHelper.hpp)
// for their single shared depth image. SkyBox and CascadedShadowMap keep
// their own inline dependency - a genuinely different edge, documented on the
// helper - so this pins down that no other raster stage picks the hand-rolled
// shape back up by accident.
TEST(BuildIntegrity, NoRasterStageHandRollsItsExternalSubpassDependency)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const char *const kExternalAssign = "srcSubpass = VK_SUBPASS_EXTERNAL";
    static const char *const kStageAssign = "srcStageMask =";
    // The helper's own definition, and the two passes with a genuinely
    // different dependency shape, are exempt from this rule.
    static const std::set<std::string> kAllowedFiles = {
        "Src/GraphicsEngineVulkan/common/RenderPassHelper.hpp",
        "Src/GraphicsEngineVulkan/scene/sky_box/SkyBox.cpp",
        "Src/GraphicsEngineVulkan/scene/light/directional_light/CascadedShadowMap.cpp"
    };
    // How many lines after the srcSubpass assignment to look for a hand-rolled
    // srcStageMask assignment - every existing hand-rolled site sets it within
    // the next couple of lines (an interleaved comment at most).
    static constexpr std::size_t kLookaheadLines = 4;

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx" && path.extension() != ".hpp") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (kAllowedFiles.contains(relative_file)) { continue; }

        const auto lines_opt = readFileLines(path);
        if (!lines_opt) { continue; }
        const auto &lines = *lines_opt;

        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find(kExternalAssign) == std::string::npos) { continue; }
            const std::size_t window_end = std::min(lines.size(), i + 1 + kLookaheadLines);
            for (std::size_t j = i; j < window_end; ++j) {
                if (lines[j].find(kStageAssign) == std::string::npos) { continue; }
                violations.push_back(relative_file + ":" + std::to_string(i + 1) + ": " + lines[i]);
                break;
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " hand-rolled external subpass dependency/dependencies found under Src/GraphicsEngineVulkan/ - route "
         "single-depth-image raster stages through Kataglyphis::buildExternalColorDepthDependency "
         "(common/RenderPassHelper.hpp) instead:"
      << joinViolations(violations);
}

// Colour twin of NoStageHandRollsTheDepthAttachmentChain: every raster stage
// used to spell out its own plain colour attachment creation chain by hand -
// createImage(..., eColorAttachment, eDeviceLocal) -> createImageView(...,
// eColor). Kataglyphis::VulkanRendererInternals::createColorAttachment
// (renderer/ColorAttachment.ixx) is now the one place that chain is spelled
// out. Non-plain colour views (storage/array/cube textures, and the swapchain
// view over an image it does not own) are deliberate non-goals, allowlisted
// via a "// COLOR_ATTACHMENT_CHAIN_OK: <marker>" trailing comment on the
// exempted line rather than a bare file exemption, for the same reason the
// depth gate anchors to a source marker instead of a line number.
TEST(BuildIntegrity, NoStageHandRollsTheColorAttachmentChain)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const char *const kViewCall = "createImageView(";
    static const char *const kColorAspect = "ImageAspectFlagBits::eColor";
    static const char *const kMarkerPrefix = "COLOR_ATTACHMENT_CHAIN_OK: ";

    // The helper's own definition is exempt from its own rule.
    static const char *const kHelperFile = "Src/GraphicsEngineVulkan/renderer/ColorAttachment.ixx";

    std::vector<std::string> violations;
    bool marker_found = false;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (relative_file == kHelperFile) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find(kViewCall) == std::string::npos) { continue; }
            if (line.find(kColorAspect) == std::string::npos) { continue; }
            if (line.find(kMarkerPrefix) != std::string::npos) {
                marker_found = true;
                continue;
            }
            violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " hand-rolled createImageView(..., vk::ImageAspectFlagBits::eColor, ...) call(s) found under "
         "Src/GraphicsEngineVulkan/ - route plain colour attachment creation through "
         "Kataglyphis::VulkanRendererInternals::createColorAttachment (renderer/ColorAttachment.ixx) instead, "
         "or add a \"// COLOR_ATTACHMENT_CHAIN_OK: <marker>\" trailing comment on the line if the site is a "
         "deliberate non-goal:"
      << joinViolations(violations);

    EXPECT_TRUE(marker_found) << "expected to find at least one of the non-goal exemption markers "
                                  "(\"// COLOR_ATTACHMENT_CHAIN_OK: ...\") in source - if all of them were "
                                  "removed, delete this check too";
}

// CascadedShadowMap used to create TWO byte-identical image views over the
// same shadow-map-array image: shadowMapArray's own sampled view (init(),
// passed (format, eDepth, 1, e2DArray, numCascades)) and a second view built
// by hand in createFramebuffers() from the exact same five values, purely to
// hand to the framebuffer as its attachment. createFramebuffers() now reuses
// shadowMapArray->getImageView() instead, and the shadowMapArrayView member
// is gone.
TEST(BuildIntegrity, ShadowMapArrayHasExactlyOneImageView)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path cpp_path = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "light" / "directional_light"
                               / "CascadedShadowMap.cpp";
    const fs::path ixx_path = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "light" / "directional_light"
                               / "CascadedShadowMap.ixx";
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
         "view in init()) - the framebuffer attachment in createFramebuffers() must reuse it via "
         "shadowMapArray->getImageView() rather than creating a second, byte-identical view";
    EXPECT_EQ(count_occurrences(cpp_contents, "buildImageViewCreateInfo("), 0U)
      << "CascadedShadowMap.cpp should no longer hand-roll an image-view create info for the framebuffer "
         "attachment - reuse shadowMapArray->getImageView() instead";
    EXPECT_EQ(cpp_contents.find("shadowMapArrayView"), std::string::npos)
      << "CascadedShadowMap.cpp still references shadowMapArrayView - the framebuffer attachment view "
         "should come from shadowMapArray->getImageView() instead of a second owned view";
    EXPECT_EQ(ixx_contents.find("shadowMapArrayView"), std::string::npos)
      << "CascadedShadowMap.ixx still declares the shadowMapArrayView member - it should be removed now "
         "that createFramebuffers() reuses shadowMapArray's own image view";
}

// Every hand-rolled framebuffer teardown used to be spelled out at each of
// nine call sites across five stages, four of which duplicated it a second
// time inside their own cleanUp(). Kataglyphis::destroyFramebuffers /
// destroyFramebuffer (common/FramebufferHelper.hpp) are now the one place
// that destroy framebuffers - this pins that down the same way
// PipelineTeardownGoesThroughTheSharedHelper does for pipelines.
TEST(BuildIntegrity, FramebufferTeardownGoesThroughTheSharedHelper)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const char *const kRawTeardownCall = ".destroyFramebuffer(";

    // FramebufferHelper.hpp is the helper's own definition.
    static const std::array<const char *, 1> kExemptFiles = {
        "Src/GraphicsEngineVulkan/common/FramebufferHelper.hpp"
    };

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (std::find(kExemptFiles.begin(), kExemptFiles.end(), relative_file) != kExemptFiles.end()) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find(kRawTeardownCall) == std::string::npos) { continue; }
            violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " hand-rolled .destroyFramebuffer(...) call(s) found under Src/GraphicsEngineVulkan/ - destroy "
         "framebuffers through Kataglyphis::destroyFramebuffers (common/FramebufferHelper.hpp) instead:"
      << joinViolations(violations);
}

// Every hand-rolled render-pass teardown used to be spelled out at each of
// five stages' cleanUp(). Kataglyphis::destroyRenderPass
// (common/RenderPassHelper.hpp) is now the one place that destroys render
// passes - this pins that down the same way
// FramebufferTeardownGoesThroughTheSharedHelper does for framebuffers.
TEST(BuildIntegrity, RenderPassTeardownGoesThroughTheSharedHelper)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const char *const kRawTeardownCall = ".destroyRenderPass(";

    // RenderPassHelper.hpp is the helper's own definition.
    static const std::array<const char *, 1> kExemptFiles = {
        "Src/GraphicsEngineVulkan/common/RenderPassHelper.hpp"
    };

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (std::find(kExemptFiles.begin(), kExemptFiles.end(), relative_file) != kExemptFiles.end()) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find(kRawTeardownCall) == std::string::npos) { continue; }
            violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " hand-rolled .destroyRenderPass(...) call(s) found under Src/GraphicsEngineVulkan/ - destroy render "
         "passes through Kataglyphis::destroyRenderPass (common/RenderPassHelper.hpp) instead:"
      << joinViolations(violations);
}

// Four render stages' framebuffers reference the outgoing swapchain's image
// views, so VulkanRenderer::recreateSwapChain() must destroy them before
// calling vulkanSwapChain.recreate() - recreateFrameResources() necessarily
// runs after that call (it needs the new image views), so the teardown
// cannot move into the stages themselves. Dropping one of those destroy
// calls leaks N framebuffers per window resize and only surfaces as a
// live-object error at vkDestroyDevice, which no CI lane runs - this pins
// the ordering down so that stays true.
TEST(BuildIntegrity, EveryStageFramebufferIsDestroyedBeforeTheSwapchainIsRecreated)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path renderer_cpp =
      repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "VulkanRenderer.cpp";
    const auto contentsOpt = readFileText(renderer_cpp);
    ASSERT_TRUE(contentsOpt.has_value()) << "missing " << renderer_cpp.string();
    const std::string &contents = *contentsOpt;

    static const std::string kFunctionSignature = "Kataglyphis::VulkanRenderer::recreateSwapChain(";
    const std::size_t signature_pos = contents.find(kFunctionSignature);
    ASSERT_NE(signature_pos, std::string::npos)
      << "VulkanRenderer::recreateSwapChain is no longer defined in " << renderer_cpp.string();

    const std::size_t body_open = contents.find('{', signature_pos);
    ASSERT_NE(body_open, std::string::npos) << "could not locate the opening brace of recreateSwapChain";

    int brace_depth = 0;
    std::size_t body_close = std::string::npos;
    for (std::size_t i = body_open; i < contents.size(); ++i) {
        if (contents[i] == '{') { ++brace_depth; }
        else if (contents[i] == '}') {
            --brace_depth;
            if (brace_depth == 0) {
                body_close = i;
                break;
            }
        }
    }
    ASSERT_NE(body_close, std::string::npos)
      << "could not brace-match the closing '}' of recreateSwapChain";

    const std::string body = contents.substr(body_open, body_close - body_open + 1);

    // Blank out '//'-comment lines (offsets preserved) so a call that was
    // commented out rather than removed cannot satisfy the checks below.
    std::string code_only = body;
    for (std::size_t line_start = 0; line_start < code_only.size();) {
        std::size_t line_end = code_only.find('\n', line_start);
        if (line_end == std::string::npos) { line_end = code_only.size(); }
        const std::size_t first_non_space = code_only.find_first_not_of(" \t", line_start);
        if (first_non_space != std::string::npos && first_non_space < line_end
            && code_only.compare(first_non_space, 2, "//") == 0) {
            std::fill(code_only.begin() + static_cast<std::ptrdiff_t>(line_start),
                code_only.begin() + static_cast<std::ptrdiff_t>(line_end), ' ');
        }
        line_start = line_end + 1;
    }

    const std::size_t recreate_pos = code_only.find("vulkanSwapChain.recreate(");
    ASSERT_NE(recreate_pos, std::string::npos)
      << "recreateSwapChain no longer calls vulkanSwapChain.recreate(...) - update this gate";

    // Receiver -> the file that would define its ::destroyFramebuffers(), if
    // it owns any framebuffers at all (clouds does not). A receiver missing
    // from this map fails the test so a sixth stage cannot be added silently.
    static const std::map<std::string, const char *> kReceiverFiles = {
        { "postStage", "Src/GraphicsEngineVulkan/renderer/PostStage.cpp" },
        { "rasterizer", "Src/GraphicsEngineVulkan/renderer/Rasterizer.cpp" },
        { "deferredRasterizer", "Src/GraphicsEngineVulkan/renderer/DeferredRasterizer.cpp" },
        { "skyBox", "Src/GraphicsEngineVulkan/scene/sky_box/SkyBox.cpp" },
        { "clouds", "Src/GraphicsEngineVulkan/scene/atmospheric_effects/clouds/Clouds.cpp" },
    };

    static const std::regex kReceiverPattern(R"((\w+)\.recreateFrameResources\()");
    std::set<std::string> receivers;
    for (auto it = std::sregex_iterator(code_only.begin(), code_only.end(), kReceiverPattern),
              end = std::sregex_iterator();
         it != end; ++it) {
        receivers.insert((*it)[1].str());
    }
    ASSERT_FALSE(receivers.empty()) << "found no X.recreateFrameResources(...) calls in recreateSwapChain";

    for (const std::string &receiver : receivers) {
        const auto map_it = kReceiverFiles.find(receiver);
        ASSERT_NE(map_it, kReceiverFiles.end())
          << "recreateSwapChain calls " << receiver
          << ".recreateFrameResources(...) but this gate has no entry for it - add " << receiver
          << " to kReceiverFiles above (pointing at the file that defines its ::destroyFramebuffers(), "
             "or that owns none, like clouds) so a sixth stage cannot silently skip this check";

        const fs::path stage_path = repo_root / map_it->second;
        const auto stage_contents_opt = readFileText(stage_path);
        ASSERT_TRUE(stage_contents_opt.has_value()) << "missing " << stage_path.string();
        const std::string &stage_contents = *stage_contents_opt;
        const bool owns_framebuffers = stage_contents.find("::destroyFramebuffers()") != std::string::npos;

        if (!owns_framebuffers) { continue; }// e.g. clouds - nothing to require here

        const std::string destroy_call = receiver + ".destroyFramebuffers();";
        const std::size_t destroy_pos = code_only.find(destroy_call);
        EXPECT_NE(destroy_pos, std::string::npos)
          << receiver << " owns framebuffers (destroyFramebuffers() is defined in " << map_it->second
          << ") but recreateSwapChain never calls " << destroy_call
          << " - its framebuffers reference the outgoing swapchain image views and must be destroyed "
             "before vulkanSwapChain.recreate(), or vkDestroyDevice will report them as live objects";
        EXPECT_LT(destroy_pos, recreate_pos)
          << receiver << ".destroyFramebuffers() is called after vulkanSwapChain.recreate() in "
             "recreateSwapChain - its framebuffers reference the outgoing swapchain image views, so the "
             "destroy must happen before the swapchain is recreated, not after";
    }
}

// Texture::createImage takes in_mip_levels but, before this test existed, only
// uploadRgba's own path assigned it to the mip_levels field - every other
// caller (Clouds, SkyBox, CascadedShadowMap) went through createImage
// directly and left mip_levels at its 0 default, so getMipLevel() and the
// sampler's maxLod silently disagreed with the image that was actually
// created. There is no device-free way to construct a Texture and call
// createImage (it needs VMA and a logical device), so this pins the source
// text instead of exercising the function.
TEST(BuildIntegrity, TextureCreateImageRecordsTheMipLevelItWasGiven)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path texture_cpp = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "Texture.cpp";
    const auto lines = readFileLines(texture_cpp);
    ASSERT_TRUE(lines.has_value()) << "could not open " << texture_cpp.string();

    static const std::string kFunctionSignature = "Kataglyphis::Texture::createImage(";
    static const std::string kAssignment = "mip_levels = in_mip_levels;";

    bool in_function = false;
    bool function_started = false;// seen the opening '{' of the definition body
    int brace_depth = 0;
    bool found_assignment = false;
    for (const auto &line : *lines) {
        if (!in_function) {
            if (line.find(kFunctionSignature) == std::string::npos) { continue; }
            in_function = true;
            continue;
        }

        brace_depth += static_cast<int>(std::count(line.begin(), line.end(), '{'));
        brace_depth -= static_cast<int>(std::count(line.begin(), line.end(), '}'));
        if (brace_depth > 0) { function_started = true; }
        if (line.find(kAssignment) != std::string::npos) { found_assignment = true; }
        if (function_started && brace_depth <= 0) { break; }// reached the closing '}'
    }

    ASSERT_TRUE(in_function) << texture_cpp.string() << " has no " << kFunctionSignature
                             << " definition to check - did createImage move or get renamed?";
    EXPECT_TRUE(found_assignment)
      << "Texture::createImage no longer assigns mip_levels from in_mip_levels - getMipLevel() and the "
         "sampler's maxLod would go back to depending on which constructor path ran.";
}

// Every feature bit enabled on features11/features12/features13/features2 in
// VulkanDevice::create_logical_device must come from an availability query
// (available_features11/12/13, availableRayTracingFeatures2, ...), never a
// hardcoded literal - the one place that is allowed to hardcode a bit to true
// is inside the `if (deviceSupportsHardwareAcceleratedRRT)` block, because the
// checks immediately above it already proved those specific bits are
// available on this device. A hardcoded bit anywhere else risks requesting a
// feature vkCreateDevice then rejects with VK_ERROR_FEATURE_NOT_PRESENT on a
// device that does not support it - the engine does not start at all, rather
// than degrading.
TEST(BuildIntegrity, EveryEnabledDeviceFeatureIsCopiedFromAnAvailabilityQuery)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path vulkan_device_cpp = repo_root / "Src" / "GraphicsEngineVulkan" / "vulkan_base" / "VulkanDevice.cpp";
    const auto lines_opt = readFileLines(vulkan_device_cpp);
    ASSERT_TRUE(lines_opt.has_value()) << "could not open " << vulkan_device_cpp.string();
    const auto &lines = *lines_opt;

    static const std::string kGuard = "if (deviceSupportsHardwareAcceleratedRRT)";
    std::size_t guard_start = lines.size();
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index].find(kGuard) != std::string::npos) {
            guard_start = index;
            break;
        }
    }
    ASSERT_LT(guard_start, lines.size())
      << vulkan_device_cpp.string() << " has no " << kGuard << " block to check - did the guard get renamed?";

    std::size_t guard_end = lines.size();
    int brace_depth = 0;
    bool guard_body_started = false;
    for (std::size_t index = guard_start; index < lines.size(); ++index) {
        brace_depth += static_cast<int>(std::count(lines[index].begin(), lines[index].end(), '{'));
        brace_depth -= static_cast<int>(std::count(lines[index].begin(), lines[index].end(), '}'));
        if (brace_depth > 0) { guard_body_started = true; }
        if (guard_body_started && brace_depth <= 0) {
            guard_end = index;
            break;
        }
    }
    ASSERT_LT(guard_end, lines.size()) << "could not find the closing '}' of the " << kGuard << " block";

    static const std::regex kHardcodedFeatureBit(
      R"((features1[123]\.\w+|features2\.features\.\w+)\s*=\s*(VK_TRUE|true)\b)");

    std::vector<std::string> violations;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index >= guard_start && index <= guard_end) { continue; }
        if (std::regex_search(lines[index], kHardcodedFeatureBit)) {
            violations.push_back("VulkanDevice.cpp:" + std::to_string(index + 1) + ": " + lines[index]);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " device feature bit(s) are hardcoded to true outside the " << kGuard
      << " block - every enabled feature must be copied from an availability query so a device that does not "
         "support it degrades instead of failing vkCreateDevice:"
      << joinViolations(violations);
}

// Pins the SHAPE of the cascade light-matrix double-buffering fix, not its
// runtime behaviour: the bug (the CPU rewriting a single UBO while up to
// MAX_FRAME_DRAWS in-flight shadow passes may still be reading it) is a data
// race on host-coherent memory, invisible to both the golden render suites
// and Vulkan synchronization validation - there is no observable symptom to
// assert on short of a flaky multi-frame GPU race repro. So this checks the
// source directly: the buffer is a std::vector (one per swapchain image, like
// globalUBOBuffer/sceneUBOBuffer), and recordCommands binds the set for the
// CURRENT image_index rather than always set 0.
TEST(BuildIntegrity, ShadowLightMatricesAreDoubleBufferedPerSwapchainImage)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path shadow_dir =
      repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "light" / "directional_light";

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
      << "CascadedShadowMap.cpp still binds lightMatricesDescriptors.sets()[0] unconditionally in "
      << cpp_path.string()
      << " - that is the exact bug this test pins: the CPU rewrites the light-matrix UBO for whichever image is "
         "current, while the shadow pass keeps sampling set 0 regardless of image_index.";
}

// dirShadowMap is sized per swapchain image (lightMatricesBuffers, above) but
// recreateSwapChain()'s newImageCount != oldImageCount branch only calls
// reprovisionPerImageResources() - which never touched dirShadowMap - so the
// shadow pass silently stopped rendering for any image added past the
// original count. reinitShadowMapForCurrentSettings() (extracted out of
// handleShadowResolutionChange, which already proved the
// cleanUp()+init()+createGraphicsPipeline() sequence) must be called from
// reprovisionPerImageResources() too, and strictly after
// initDescriptorResources() - CascadedShadowMap::init caches the
// sharedRenderDescriptors layout, and initDescriptorResources() is what
// (re)creates that layout via cleanUpDescriptorResources()/initDescriptorResources()
// in reprovisionPerImageResources() itself.
TEST(BuildIntegrity, EveryPerSwapchainImageSubsystemIsReprovisionedOnImageCountChange)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path renderer_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "VulkanRenderer.cpp";
    const auto renderer_contents_opt = readFileText(renderer_path);
    ASSERT_TRUE(renderer_contents_opt.has_value()) << "could not open " << renderer_path.string();
    const std::string &renderer_contents = *renderer_contents_opt;

    const std::size_t reinit_occurrences = [&renderer_contents] {
        std::size_t count = 0;
        std::size_t pos = 0;
        while ((pos = renderer_contents.find("reinitShadowMapForCurrentSettings", pos)) != std::string::npos) {
            ++count;
            pos += std::string("reinitShadowMapForCurrentSettings").size();
        }
        return count;
    }();

    EXPECT_GE(reinit_occurrences, 3u)
      << "found only " << reinit_occurrences << " occurrence(s) of reinitShadowMapForCurrentSettings in "
      << renderer_path.string()
      << " - expected at least 3 (the definition plus a call from handleShadowResolutionChange and a call from "
         "reprovisionPerImageResources); deleting either call site regresses the swapchain-image-count-change bug "
         "this test guards against.";

    const std::size_t signature_pos = renderer_contents.find("VulkanRenderer::reprovisionPerImageResources(");
    ASSERT_NE(signature_pos, std::string::npos)
      << "VulkanRenderer::reprovisionPerImageResources is no longer defined in " << renderer_path.string();

    const std::size_t body_open = renderer_contents.find('{', signature_pos);
    ASSERT_NE(body_open, std::string::npos)
      << "could not locate the opening brace of VulkanRenderer::reprovisionPerImageResources";

    int brace_depth = 0;
    std::size_t body_close = std::string::npos;
    for (std::size_t i = body_open; i < renderer_contents.size(); ++i) {
        if (renderer_contents[i] == '{') { ++brace_depth; }
        else if (renderer_contents[i] == '}') {
            --brace_depth;
            if (brace_depth == 0) {
                body_close = i;
                break;
            }
        }
    }
    ASSERT_NE(body_close, std::string::npos)
      << "could not brace-match the closing '}' of VulkanRenderer::reprovisionPerImageResources";

    const std::string body = renderer_contents.substr(body_open, body_close - body_open + 1);

    const std::size_t init_descriptor_pos = body.find("initDescriptorResources();");
    ASSERT_NE(init_descriptor_pos, std::string::npos)
      << "VulkanRenderer::reprovisionPerImageResources no longer calls initDescriptorResources() - "
         "reinitShadowMapForCurrentSettings must run after it, since CascadedShadowMap::init caches the "
         "sharedRenderDescriptors layout that call (re)creates.";

    const std::size_t reinit_call_pos = body.find("reinitShadowMapForCurrentSettings();");
    ASSERT_NE(reinit_call_pos, std::string::npos)
      << "VulkanRenderer::reprovisionPerImageResources no longer calls reinitShadowMapForCurrentSettings() - "
         "without it, dirShadowMap is never re-provisioned when the swapchain image count changes, so the shadow "
         "pass silently stops rendering for the added images.";

    EXPECT_GT(reinit_call_pos, init_descriptor_pos)
      << "reinitShadowMapForCurrentSettings() is called before initDescriptorResources() inside "
         "reprovisionPerImageResources - CascadedShadowMap::init caches the sharedRenderDescriptors layout handed "
         "to it, so re-initing before initDescriptorResources() replaces that layout would leave it caching the "
         "layout that is about to be destroyed.";
}

// ObjLoader::uploadParsed and GltfLoader::uploadParsed used to each hand-roll
// their own texture-slot-fill and mesh-range-to-Mesh loop; the only genuine
// difference between them (texture bytes from a file vs. from memory) got
// buried inside that duplication, and the one non-genuine difference
// (GltfLoader forwarding MeshRange::doubleSided, ObjLoader relying on
// add_new_mesh's default argument) went unnoticed for it. ModelAssembly.ixx
// (kataglyphis.vulkan.model_assembly) is now the one place that fills a
// texture slot, builds meshes from MeshRanges, and checks the device-free /
// empty-parse preconditions - this pins that down so a future loader (or
// uploadParsed itself) cannot silently reintroduce a hand-rolled copy that
// drifts from the shared one again.
TEST(BuildIntegrity, ModelUploadGoesThroughTheSharedAssembly)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    // sliceMeshRange's own definition (MeshRange.ixx) and createDefaultTexture's
    // own definition (Texture.ixx/.cpp) are exempt - everyone else, including
    // ModelAssembly.ixx's own callers, must reach them through the shared
    // functions ModelAssembly.ixx wraps them in. ObjLoader.cpp/GltfLoader.cpp
    // are exempt too, but only from uploadPreconditionsMet: they are its two
    // intended callers (uploadParsed's guard), not a hand-rolled duplicate of
    // it - a *third* loader reimplementing the device/vertices checks inline
    // rather than calling the shared guard is what this test still catches.
    static const std::array<const char *, 6> kExemptFiles = {
        "Src/GraphicsEngineVulkan/scene/ModelAssembly.ixx",
        "Src/GraphicsEngineVulkan/scene/MeshRange.ixx",
        "Src/GraphicsEngineVulkan/scene/Texture.ixx",
        "Src/GraphicsEngineVulkan/scene/Texture.cpp",
        "Src/GraphicsEngineVulkan/scene/ObjLoader.cpp",
        "Src/GraphicsEngineVulkan/scene/GltfLoader.cpp"
    };
    static const std::array<const char *, 3> kBannedCalls = {
        "sliceMeshRange(", "createDefaultTexture(", "uploadPreconditionsMet("
    };

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();
        if (std::find(kExemptFiles.begin(), kExemptFiles.end(), relative_file) != kExemptFiles.end()) { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            for (const char *banned : kBannedCalls) {
                if (line.find(banned) == std::string::npos) { continue; }
                violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " call(s) to sliceMeshRange/createDefaultTexture/uploadPreconditionsMet found outside their sanctioned "
         "callers under Src/ - build meshes and texture slots through "
         "addMeshesForRanges/addTextureOrDefault/ensureAtLeastOneTexture and check upload preconditions through "
         "uploadPreconditionsMet (kataglyphis.vulkan.model_assembly) instead:"
      << joinViolations(violations);
}

// CascadedShadowMap::createDescriptorSetAndPipeline() used to spin up a
// throwaway command pool + VulkanBufferManager staging round trip to seed a
// host-visible buffer that uploadLightMatrices() overwrites before the first
// frame anyway. It now takes the renderer's own graphics_command_pool through
// init() (unused today, but kept for parity with every other stage's
// init(..., commandPool) signature) and writes lightMatricesBuffers directly
// through getMappedData(). Pin both halves of that: the renderer stays the
// only place that owns a command pool, and the shadow map file stays free of
// the staging abstraction it no longer needs.
TEST(BuildIntegrity, OnlyTheRendererCreatesACommandPool)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path engine_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(engine_root)) << "missing " << engine_root.string();

    std::vector<std::string> files_with_pool_creation;
    std::error_code error;
    for (fs::recursive_directory_iterator it(engine_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx") { continue; }

        const auto contents = readFileText(path);
        if (!contents) { continue; }
        if (contents->find("createCommandPool(") == std::string::npos) { continue; }

        files_with_pool_creation.push_back(fs::relative(path, repo_root).generic_string());
    }

    static const std::array<const char *, 1> kExpected = { "Src/GraphicsEngineVulkan/renderer/VulkanRenderer.cpp" };
    std::sort(files_with_pool_creation.begin(), files_with_pool_creation.end());

    EXPECT_TRUE(std::equal(files_with_pool_creation.begin(),
      files_with_pool_creation.end(),
      kExpected.begin(),
      kExpected.end()))
      << "expected only VulkanRenderer.cpp to create a vk::CommandPool, found:"
      << joinViolations(files_with_pool_creation);
}

TEST(BuildIntegrity, CascadedShadowMapDoesNotStageThroughABufferManager)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path shadow_map_cpp = repo_root
      / "Src" / "GraphicsEngineVulkan" / "scene" / "light" / "directional_light" / "CascadedShadowMap.cpp";
    ASSERT_TRUE(fs::exists(shadow_map_cpp)) << "missing " << shadow_map_cpp.string();

    const auto contentsOpt = readFileText(shadow_map_cpp);
    ASSERT_TRUE(contentsOpt.has_value()) << "could not open " << shadow_map_cpp.string();
    const std::string &contents = *contentsOpt;

    EXPECT_EQ(contents.find("VulkanBufferManager"), std::string::npos)
      << "CascadedShadowMap.cpp should seed lightMatricesBuffers directly through getMappedData(), not a "
         "VulkanBufferManager staging round trip";
}

// Rasterizer and DeferredRasterizer used to call chooseDepthFormat() once in
// createTextures() and again in createRenderPass(), so the render-pass
// attachment format and the depth image it is paired with were derived
// independently and could silently diverge. CascadedShadowMap already caches
// the result in a member and reads it a second time; this test holds all
// three remaining depth-owning render stages to that invariant. PostStage
// dropped out of this list along with its depth attachment entirely - see
// "Delete the depth attachment that PostStage and SkyBox allocate, clear and
// synchronize but never test or write" - nothing read it, so there is no
// depth_format left to derive.
//
// Rasterizer and DeferredRasterizer now derive it indirectly - through
// Kataglyphis::VulkanRendererInternals::createDepthAttachment
// (renderer/DepthAttachment.ixx), which calls chooseDepthFormat() and
// returns the result - rather than calling chooseDepthFormat() in their own
// text, so the invariant is checked at the one place all three stages still
// share: the member assignment itself. Two occurrences would mean the
// member was derived and (re)assigned twice in the same file, which is
// exactly the divergence this test exists to catch.
TEST(BuildIntegrity, EveryRenderStageDerivesItsDepthFormatOnce)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::array<const char *, 3> kFiles = {
        "Src/GraphicsEngineVulkan/renderer/Rasterizer.cpp",
        "Src/GraphicsEngineVulkan/renderer/DeferredRasterizer.cpp",
        "Src/GraphicsEngineVulkan/scene/light/directional_light/CascadedShadowMap.cpp",
    };

    static const char *const kAssignment = "depth_format =";

    for (const char *relative_path : kFiles) {
        const fs::path path = repo_root / relative_path;
        ASSERT_TRUE(fs::exists(path)) << "missing " << path.string();

        const auto contentsOpt = readFileText(path);
        ASSERT_TRUE(contentsOpt.has_value()) << "could not open " << path.string();
        const std::string &contents = *contentsOpt;

        std::size_t count = 0;
        std::size_t pos = 0;
        while ((pos = contents.find(kAssignment, pos)) != std::string::npos) {
            ++count;
            pos += std::strlen(kAssignment);
        }

        EXPECT_EQ(count, 1U) << relative_path << " assigns depth_format " << count
                             << " time(s); it must be derived exactly once and cached in a member, so the "
                                "render-pass attachment and the image it is paired with cannot diverge";
    }
}

// Every image memory barrier used to be spelled out at each of seven call
// sites across Raytracing.cpp, PathTracing.cpp and FrameCapture.ixx, hand-
// rolled via default construction (`vk::ImageMemoryBarrier name{};`) followed
// by field-by-field assignment. Kataglyphis::buildImageMemoryBarrier
// (common/ImageBarrierHelper.hpp) is now the one place that builds an image
// barrier - this pins that down the same way FramebufferTeardownGoesThrough
// TheSharedHelper does for framebuffer teardown.
//
// The scan looks specifically for the empty-brace default-construction
// idiom, not every mention of the type: a converted call site still declares
// a `const vk::ImageMemoryBarrier` local to hold the helper's return value,
// and that is exactly the pattern this test must NOT flag.
TEST(BuildIntegrity, ImageMemoryBarriersGoThroughTheSharedHelper)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    // Per-file budgets, not whole-file exemptions: VulkanImage.cpp and
    // Texture.cpp each build one vk::ImageMemoryBarrier from a general
    // transition helper's own aspect/mip/layer parameters, not boilerplate.
    // VulkanRenderer.cpp's two cloud-output barriers are the subject of a
    // separate, blocked backlog entry. Every file not listed here has an
    // implicit budget of 0. ImageBarrierHelper.hpp (the helper's own
    // definition) matches nothing, so it needs no entry at all.
    static const std::map<std::string, std::size_t> kBarrierBudgets = {
        { "Src/GraphicsEngineVulkan/vulkan_base/VulkanImage.cpp", 1 },
        { "Src/GraphicsEngineVulkan/scene/Texture.cpp", 1 },
        { "Src/GraphicsEngineVulkan/renderer/VulkanRenderer.cpp", 2 },
    };

    const std::regex hand_rolled_barrier(R"(vk::ImageMemoryBarrier\s+\w+\s*\{\s*\}\s*;)");

    std::map<std::string, std::size_t> counts;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx" && path.extension() != ".hpp") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        for (const auto &line : *lines) {
            if (!std::regex_search(line, hand_rolled_barrier)) { continue; }
            ++counts[relative_file];
        }
    }

    std::vector<std::string> violations;
    for (const auto &[file, count] : counts) {
        const auto budget_it = kBarrierBudgets.find(file);
        const std::size_t budget = budget_it == kBarrierBudgets.end() ? 0 : budget_it->second;
        if (count == budget) { continue; }
        if (count > budget) {
            violations.push_back(file + ": found " + std::to_string(count) + " hand-rolled barrier(s), budget is "
                                  + std::to_string(budget)
                                  + " - route the new one(s) through Kataglyphis::buildImageMemoryBarrier "
                                    "(common/ImageBarrierHelper.hpp)");
        } else {
            violations.push_back(file + ": found " + std::to_string(count) + " hand-rolled barrier(s), budget is "
                                  + std::to_string(budget) + " - lower the budget in this test");
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size() << " budget mismatch(es) for hand-rolled vk::ImageMemoryBarrier declarations under "
         "Src/GraphicsEngineVulkan/:"
      << joinViolations(violations);
}

TEST(BuildIntegrity, BarrierBudgetsNameOnlyFilesThatStillHaveBarriers)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::map<std::string, std::size_t> kBarrierBudgets = {
        { "Src/GraphicsEngineVulkan/vulkan_base/VulkanImage.cpp", 1 },
        { "Src/GraphicsEngineVulkan/scene/Texture.cpp", 1 },
        { "Src/GraphicsEngineVulkan/renderer/VulkanRenderer.cpp", 2 },
    };

    const std::regex hand_rolled_barrier(R"(vk::ImageMemoryBarrier\s+\w+\s*\{\s*\}\s*;)");

    for (const auto &[file, budget] : kBarrierBudgets) {
        EXPECT_GT(budget, 0u) << file << ": a budgeted file must have a non-zero budget, or it is a dead entry";

        const fs::path path = repo_root / file;
        ASSERT_TRUE(fs::exists(path)) << "budgeted file no longer exists: " << file;

        const auto lines = readFileLines(path);
        ASSERT_TRUE(lines.has_value()) << "could not read " << file;
        std::size_t count = 0;
        for (const auto &line : *lines) {
            if (std::regex_search(line, hand_rolled_barrier)) { ++count; }
        }
        EXPECT_EQ(count, budget) << file << ": budget says " << budget << " but the file currently has " << count;
    }
}

// Texture::generateMipMaps' two eShaderReadOnlyOptimal barriers route their
// access mask and pipeline stage through ImageLayoutHelper.hpp's shared rule
// rather than a hand-written eFragmentShader destination stage, so the model
// textures they publish stay synchronized for raytrace.rchit.slang's ray
// queries and the path_tracing.slang compute kernel, not just the raster
// fragment shaders. A reintroduced eFragmentShader literal would silently
// narrow that destination stage back to raster-only.
TEST(BuildIntegrity, TextureUploadDoesNotNarrowItsShaderReadStage)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path texture_path = repo_root / "Src/GraphicsEngineVulkan/scene/Texture.cpp";
    const auto texture_text = readFileText(texture_path);
    ASSERT_TRUE(texture_text.has_value()) << "could not open " << texture_path.string();

    EXPECT_EQ(texture_text->find("eFragmentShader"), std::string::npos)
      << "Texture.cpp reintroduced eFragmentShader as a barrier destination stage - route "
         "eShaderReadOnlyOptimal transitions through Kataglyphis::pipelineStageForLayout instead, "
         "or the compute/ray-tracing readers of the mip chain lose synchronization.";
}

// docs/cpp-renderer-improvements.md's "In progress" section once asked for
// the redundant same-layout swapchain barrier removal as still outstanding
// after the removal had already shipped (2026-07-19) - the doc contradicted
// the source it describes. Follows NoGeneratedWgslSourceClaimsToMirrorItsOutput's
// structure: read both files, assert the contradiction cannot coexist, and
// name both sides in the failure message.
TEST(BuildIntegrity, RendererImprovementLogDoesNotAskForShippedWork)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "cpp-renderer-improvements.md";
    const fs::path renderer_path = repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "VulkanRenderer.cpp";

    const auto doc_content_opt = readFileText(doc_path);
    ASSERT_TRUE(doc_content_opt.has_value()) << "could not open " << doc_path.string();
    const std::string &doc_content = *doc_content_opt;

    const auto renderer_content_opt = readFileText(renderer_path);
    ASSERT_TRUE(renderer_content_opt.has_value()) << "could not open " << renderer_path.string();
    const std::string &renderer_content = *renderer_content_opt;

    const bool renderer_records_the_removal = renderer_content.find("used to sit here") != std::string::npos;
    const bool doc_still_asks_for_the_removal = doc_content.find("remove only after a sync-validation") != std::string::npos;

    EXPECT_FALSE(renderer_records_the_removal && doc_still_asks_for_the_removal)
      << doc_path.string() << " still asks for the same-layout swapchain barrier to be removed "
         "(\"remove only after a sync-validation\"), but " << renderer_path.string()
      << " already records the removal (\"used to sit here\") - the doc is asking for shipped work.";

    const std::string in_progress_marker = "## In progress";
    const auto in_progress_pos = doc_content.find(in_progress_marker);
    ASSERT_NE(in_progress_pos, std::string::npos) << doc_path.string() << " is missing its \"## In progress\" section";
    const auto queued_pos = doc_content.find("## Queued", in_progress_pos);
    ASSERT_NE(queued_pos, std::string::npos) << doc_path.string() << " is missing its \"## Queued\" section";
    const std::string in_progress_section = doc_content.substr(in_progress_pos, queued_pos - in_progress_pos);

    EXPECT_EQ(in_progress_section.find("sync-validated barrier removal"), std::string::npos)
      << doc_path.string() << "'s \"## In progress\" section still lists \"sync-validated barrier removal\" as "
         "remaining queue, but " << renderer_path.string() << " already records the removal (\"used to sit here\").";
}

// Internal linkage in a header gives every translation unit its own copy of
// the function, and an inline function that names one (chooseDepthFormat used
// to call the internal-linkage choose_supported_format) is IFNDR under
// [basic.def.odr] - which is exactly how FormatHelper.hpp got into this state.
// Namespace-scope function definitions in this tree are unindented, while
// static member functions inside a class body are indented, so column 0 is
// the whole discriminator between "internal-linkage free function" and
// "static member function" (the latter is fine and must stay untouched).
TEST(BuildIntegrity, HeadersDoNotDefineStaticFreeFunctions)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        const auto extension = path.extension();
        if (extension != ".hpp" && extension != ".ixx") { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            constexpr std::string_view kStaticPrefix = "static ";
            if (line.compare(0, kStaticPrefix.size(), kStaticPrefix) != 0) { continue; }
            if (line.find('(') == std::string::npos) { continue; }
            if (line.find("static_assert") != std::string::npos) { continue; }

            violations.push_back(
              fs::relative(path, repo_root).generic_string() + ":" + std::to_string(line_number) + ": " + line);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " namespace-scope function definition(s) under Src/ use internal linkage (\"static\") instead of "
         "\"inline\" - internal linkage in a header gives every translation unit its own copy, which is how "
         "chooseDepthFormat (FormatHelper.hpp) ended up calling an internal-linkage function across ten TUs, "
         "an IFNDR ODR violation the compiler is not required to diagnose:"
      << joinViolations(violations);
}

// AGENTS.md states that cleanUp() must be idempotent and safe to call twice.
// The idempotence itself is not source-scannable, but the destructor half of
// the convention is: every class that declares cleanUp() should call it from
// its own destructor, so a caller who forgets the explicit call (or a
// device-lost path that skips it, see App.cpp) still gets torn down. Four
// classes are intentionally exempt - see kExemptClasses below.
TEST(BuildIntegrity, EveryCleanUpIsCalledFromItsDestructor)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    // Mesh, VulkanBufferManager: every member is already RAII, so `= default`
    // is correct - there is nothing for a destructor call to do.
    // VulkanDevice, VulkanInstance: VulkanRenderer::cleanUp() owns the
    // teardown order between the logical device and the instance; a
    // destructor call would let either move independently of that order.
    static const std::array<const char *, 4> kExemptClasses = {
        "Mesh", "VulkanBufferManager", "VulkanDevice", "VulkanInstance"
    };

    // Anchored to the start of a line (no leading whitespace) so prose like
    // "...exactly the class of bug..." in a comment cannot masquerade as a
    // class declaration.
    const std::regex class_pattern(R"(\nclass\s+(\w+))");

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".ixx") { continue; }

        const auto ixx_contents_opt = readFileText(path);
        if (!ixx_contents_opt) { continue; }
        const std::string &ixx_contents = *ixx_contents_opt;

        if (ixx_contents.find("void cleanUp();") == std::string::npos) { continue; }

        std::smatch match;
        if (!std::regex_search(ixx_contents, match, class_pattern)) { continue; }
        const std::string class_name = match[1].str();

        if (std::find(kExemptClasses.begin(), kExemptClasses.end(), class_name) != kExemptClasses.end()) { continue; }

        std::string combined_contents = ixx_contents;
        const fs::path cpp_path = fs::path(path).replace_extension(".cpp");
        if (const auto cpp_contents = readFileText(cpp_path)) { combined_contents += *cpp_contents; }

        // Matches "~Name() { cleanUp(); }" whether spelled inline in the
        // .ixx or out-of-line (possibly namespace-qualified) in the .cpp.
        const std::regex dtor_pattern(
          R"(~)" + class_name + R"(\s*\(\s*\)\s*\{\s*cleanUp\s*\(\s*\)\s*;\s*\})");
        if (std::regex_search(combined_contents, dtor_pattern)) { continue; }

        violations.push_back(fs::relative(path, repo_root).generic_string() + ": class " + class_name
                              + " declares cleanUp() but its destructor does not call it");
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " class(es) under Src/GraphicsEngineVulkan/ declare cleanUp() without a destructor that calls it "
         "(`~Name() { cleanUp(); }`) - AGENTS.md requires cleanUp() to double as the destructor body so a "
         "forgotten explicit call still tears the object down. Add the destructor call, or add the class to "
         "kExemptClasses with a reason if it genuinely cannot call cleanUp() from its destructor:"
      << joinViolations(violations);
}

// App::run() used to unconditionally `return EXIT_SUCCESS;`, so a device-lost
// or fatal-submit run was reported to the OS as a clean quit. The exit code
// must now be derived (via Kataglyphis::appExitCode) from how the frame loop
// actually ended - see appExitCodeSuite.cpp for the derivation itself.
TEST(BuildIntegrity, AppRunDoesNotReturnABareExitSuccess)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path app_cpp = repo_root / "Src" / "GraphicsEngineVulkan" / "app" / "App.cpp";
    ASSERT_TRUE(fs::exists(app_cpp)) << "missing " << app_cpp.string();

    const auto contentsOpt = readFileText(app_cpp);
    ASSERT_TRUE(contentsOpt.has_value()) << "could not open " << app_cpp.string();
    const std::string &contents = *contentsOpt;

    EXPECT_EQ(contents.find("return EXIT_SUCCESS;"), std::string::npos)
      << "App.cpp contains a bare \"return EXIT_SUCCESS;\" - App::run()'s exit code must be derived from "
         "whether the frame loop hit a device loss or fatal frame error (Kataglyphis::appExitCode), not "
         "hard-coded, or a broken run is reported as a clean quit again.";
}

// std::shared_ptr<VulkanDevice> is the single most widely passed object in
// the engine. Passing it by value pays two atomic refcount operations per
// call for a parameter that is usually only read; every non-sink parameter
// must take it as `const std::shared_ptr<VulkanDevice> &`. The three
// genuine sinks (DescriptorSetGroup::create, the GltfLoader ctor, the
// ShaderStagePair ctor) keep it by value because they move it into a
// member, and are marked with a trailing "// DEVICE_SINK_OK: " comment.
TEST(BuildIntegrity, EveryVulkanDeviceParameterIsTakenByConstReference)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *const kMarkerPrefix = "DEVICE_SINK_OK: ";
    const std::regex by_value_device_param(R"(std::shared_ptr<VulkanDevice>\s*[A-Za-z_][A-Za-z0-9_]*\s*[,)])");

    const std::array<fs::path, 2> search_roots = { repo_root / "Src" / "GraphicsEngineVulkan",
        repo_root / "Src" / "shared" };

    std::vector<std::string> violations;
    bool marker_found = false;
    for (const fs::path &src_root : search_roots) {
        ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

        std::error_code error;
        for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
            if (error) { break; }
            const fs::path &path = it->path();
            if (!it->is_regular_file(error)) { continue; }
            if (path.extension() != ".cpp" && path.extension() != ".ixx" && path.extension() != ".hpp") { continue; }

            const std::string relative_file = fs::relative(path, repo_root).generic_string();

            const auto lines = readFileLines(path);
            if (!lines) { continue; }
            std::size_t line_number = 0;
            for (const auto &line : *lines) {
                ++line_number;
                if (line.find(kMarkerPrefix) != std::string::npos) {
                    marker_found = true;
                    continue;
                }
                if (!std::regex_search(line, by_value_device_param)) { continue; }
                violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " by-value std::shared_ptr<VulkanDevice> parameter(s) found under Src/GraphicsEngineVulkan/ and "
         "Src/shared/ - take the parameter as `const std::shared_ptr<VulkanDevice> &` instead, or add a "
         "trailing \"// DEVICE_SINK_OK: <reason>\" comment on the line if the parameter is a deliberate sink "
         "moved into a member:"
      << joinViolations(violations);

    EXPECT_TRUE(marker_found) << "expected to find at least one \"// DEVICE_SINK_OK: ...\" exemption marker "
                                  "(DescriptorSetGroup::create, the GltfLoader ctor, the ShaderStagePair ctor) - "
                                  "if all sinks were removed, delete this check too";
}

// Main.cpp used to carry a hand-rolled parse_command_line()/print_usage()
// pair alongside absl::ParseCommandLine - ~62 dead lines describing a
// --help/--gpu contract main() never called. This gate pins abseil as the
// single CLI front end so the dead parser cannot silently come back.
TEST(BuildIntegrity, MainHasOneCommandLineParser)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path source = repo_root / "Src" / "GraphicsEngineVulkan" / "Main.cpp";
    const auto text = readFileText(source);
    ASSERT_TRUE(text.has_value()) << "could not read " << source.string();

    EXPECT_NE(text->find("absl::ParseCommandLine"), std::string::npos)
      << "Main.cpp must parse arguments via absl::ParseCommandLine in " << source.string();
    EXPECT_EQ(text->find("parse_command_line"), std::string::npos)
      << "Main.cpp must not carry a hand-rolled parse_command_line() alongside absl::ParseCommandLine in "
      << source.string();
    EXPECT_EQ(text->find("print_usage"), std::string::npos)
      << "Main.cpp must not carry a hand-rolled print_usage() alongside absl::ParseCommandLine in "
      << source.string();
}

// buildIntegritySuite, renderPassCreateHelperSuite and sceneAsyncLoadSuite
// each used to carry their own copy of a repo-root walk-up helper and their
// own whole-file stream-iterator slurp - three copies of each, one
// (renderPassCreateHelperSuite's) even with a different failure contract than
// the other two. RepoFiles.hpp collapsed all of that into
// repoRoot()/slangRoot()/spirvRoot()/readFileText(). This scans every
// Test/**/*.cpp and Test/**/*.hpp for the two textual signatures a
// reimplementation would have to contain, so a fourth copy cannot grow back
// silently. The signatures below are deliberately built by runtime
// concatenation rather than spelled out as single literals - otherwise this
// gate would contain, and thus fail, its own banned pattern.
TEST(BuildIntegrity, TestSuitesShareOneRepoRootHelper)
{
    const fs::path test_root = repoRoot() / "Test";
    ASSERT_TRUE(fs::exists(test_root)) << "missing " << test_root.string();

    const std::array<std::string, 2> kBannedPatterns = { std::string("find_repo") + "_root",
        std::string("istreambuf_iter") + "ator<char>" };

    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(test_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        const auto extension = path.extension();
        if (extension != ".cpp" && extension != ".hpp") { continue; }
        if (path.filename() == "RepoFiles.hpp") { continue; }

        const auto text = readFileText(path);
        if (!text) { continue; }

        for (const std::string &pattern : kBannedPatterns) {
            if (text->find(pattern) != std::string::npos) {
                violations.push_back(
                  fs::relative(path, repoRoot()).generic_string() + " contains \"" + pattern + "\"");
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " Test source file(s) reimplement RepoFiles.hpp's repoRoot()/readFileText() instead of including it: "
      << joinViolations(violations);
}

// README.md and docs/source/conf.py drifted to the old "Kataglyphis-Renderer"
// repository slug after the rename to BeschleunigerBallett -
// conf.py's repository_url was fixed but project/breathe_projects/
// breathe_default_project were not, and the README's build badges kept
// pointing at someone else's CI. Nothing else gates prose, so a partial
// rename like that can sit there indefinitely.
TEST(BuildIntegrity, DocsNameThisRepository)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const std::string kStaleSlug = "Kataglyphis-Renderer";

    std::vector<fs::path> candidates = { repo_root / "README.md" };

    const fs::path docs_source = repo_root / "docs" / "source";
    ASSERT_TRUE(fs::exists(docs_source)) << "missing " << docs_source.string();

    std::error_code error;
    for (fs::recursive_directory_iterator it(docs_source, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        const auto extension = path.extension();
        if (extension != ".md" && extension != ".rst" && path.filename() != "conf.py") { continue; }
        candidates.push_back(path);
    }

    std::vector<std::string> violations;
    for (const fs::path &path : candidates) {
        const auto text = readFileText(path);
        if (!text) { continue; }
        if (text->find(kStaleSlug) != std::string::npos) {
            violations.push_back(fs::relative(path, repo_root).generic_string());
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size() << " doc file(s) still reference the old repository slug \"" << kStaleSlug << "\":"
      << joinViolations(violations);
}

// The C++ engine's swapchain is UNORM, not sRGB (SwapchainChoices.hpp's
// chooseBestSurfaceFormat), so there is no hardware encode on present -
// every fragment shader that writes directly into the swapchain
// (post/post.slang, the last stage of the post pass, and skybox/skybox.slang,
// which bypasses the post pass entirely) must apply linear_to_srgb itself.
// If a future change ever prefers an sRGB surface format instead, this gate
// must fail loudly so those shader-side encodes get removed with it - see
// BACKLOG.md's "sRGB-encode every shader that writes the swapchain" entry.
TEST(BuildIntegrity, EverySwapchainWritingShaderEncodesSrgb)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path swapchain_choices_path =
      repo_root / "Src" / "GraphicsEngineVulkan" / "vulkan_base" / "SwapchainChoices.hpp";
    const auto swapchain_choices_text = readFileText(swapchain_choices_path);
    ASSERT_TRUE(swapchain_choices_text.has_value()) << "missing " << swapchain_choices_path.string();

    // Match vk::Format::e...Srgb..., not vk::ColorSpaceKHR::eSrgbNonlinear -
    // the latter is the expected, unrelated presentation color space.
    static const std::regex kSrgbFormat(R"(vk::Format::e\w*Srgb)");
    EXPECT_FALSE(std::regex_search(*swapchain_choices_text, kSrgbFormat))
      << swapchain_choices_path.string()
      << " now prefers an sRGB surface format - the shader-side linear_to_srgb encodes in post.slang and "
         "skybox.slang become a double-encode and must be removed";

    const fs::path slang_root = slangRoot();
    const std::array<fs::path, 2> shaders = { slang_root / "post" / "post.slang",
        slang_root / "skybox" / "skybox.slang" };

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
            if ((*source)[pos] == '{') { ++depth; }
            else if ((*source)[pos] == '}') { --depth; }
        }
        ASSERT_EQ(depth, 0u) << "unbalanced braces in the fragment entry point of " << shader_path.string();

        const std::string body = source->substr(body_start, pos - body_start);
        EXPECT_NE(body.find("linear_to_srgb"), std::string::npos)
          << shader_path.string() << "'s fragment entry point must call linear_to_srgb before writing the swapchain";
    }
}

// Pins the anisotropy fix: every buildSamplerCreateInfo call must derive its
// maxAnisotropy argument from Kataglyphis::resolveMaxAnisotropy(...) (which
// itself clamps to the device's queried limit), never a bare numeric literal
// above 1.0 - a literal above the device's maxSamplerAnisotropy limit is
// VUID-VkSamplerCreateInfo-anisotropyEnable-01071. Also pins that
// vk::PhysicalDevice::getFeatures()/getFeatures2() only run inside
// VulkanDevice.cpp, the one place a device's capabilities should be queried.
TEST(BuildIntegrity, NoSamplerHardCodesItsMaxAnisotropy)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const std::string kSignature = "buildSamplerCreateInfo(";
    // A bare floating-point literal greater than 1 - "1.0F" / "1.0f" is the
    // disabled-anisotropy sentinel and stays allowed.
    static const std::regex kLiteralAboveOne(R"(^\s*(?:[2-9]|\d{2,})(?:\.\d+)?[fF]?\s*$)");
    static const char *const kGetFeatures = "getFeatures";

    std::vector<std::string> anisotropy_violations;
    std::vector<std::string> get_features_violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".ixx" && path.extension() != ".hpp") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();

        const auto contentsOpt = readFileText(path);
        ASSERT_TRUE(contentsOpt.has_value()) << "could not open " << path.string();
        const std::string &contents = *contentsOpt;

        std::size_t sig_pos = 0;
        while ((sig_pos = contents.find(kSignature, sig_pos)) != std::string::npos) {
            const std::size_t args_begin = sig_pos + kSignature.size();

            // Balanced-paren extraction (not a lazy `\);` regex): a `\);`
            // regex would run past the parameter list on the two sites where
            // this text names a declaration/definition rather than a call -
            // `buildSamplerCreateInfo(...) -> vk::SamplerCreateInfo` has no
            // `);` right after its own parameter list, so a lazy regex keeps
            // scanning into unrelated code far below looking for the next one.
            std::size_t pos = args_begin;
            int paren_depth = 1;
            while (pos < contents.size() && paren_depth > 0) {
                if (contents[pos] == '(') { ++paren_depth; }
                else if (contents[pos] == ')') { --paren_depth; }
                ++pos;
            }
            sig_pos = pos;
            ASSERT_EQ(paren_depth, 0)
              << relative_file << ": unbalanced parentheses scanning a buildSamplerCreateInfo(...) argument list";

            const std::string call_args = contents.substr(args_begin, pos - 1 - args_begin);

            // Depth-aware split: a top-level comma separates arguments, but
            // maxAnisotropy is itself a nested call
            // (resolveMaxAnisotropy(anisotropyEnable, device->maxSamplerAnisotropy()))
            // whose internal comma must not be mistaken for one.
            std::vector<std::string> args;
            std::size_t arg_start = 0;
            int depth = 0;
            for (std::size_t i = 0; i < call_args.size(); ++i) {
                const char character = call_args[i];
                if (character == '(') { ++depth; }
                else if (character == ')') { --depth; }
                else if (character == ',' && depth == 0) {
                    args.push_back(call_args.substr(arg_start, i - arg_start));
                    arg_start = i + 1;
                }
            }
            args.push_back(call_args.substr(arg_start));
            ASSERT_GE(args.size(), 5u)
              << relative_file
              << ": buildSamplerCreateInfo(...) does not have the expected (filter, addressMode, maxLod, "
                 "anisotropyEnable, maxAnisotropy, ...) shape - the scan needs updating:\n"
              << call_args;

            if (std::regex_match(args[4], kLiteralAboveOne)) {
                anisotropy_violations.push_back(relative_file + ": maxAnisotropy=" + args[4]);
            }
        }

        if (relative_file == "Src/GraphicsEngineVulkan/vulkan_base/VulkanDevice.cpp") { continue; }
        std::size_t pos = 0;
        while ((pos = contents.find(kGetFeatures, pos)) != std::string::npos) {
            get_features_violations.push_back(relative_file);
            pos += std::strlen(kGetFeatures);
        }
    }

    EXPECT_TRUE(anisotropy_violations.empty())
      << "buildSamplerCreateInfo call(s) hard-code a maxAnisotropy literal above the disabled sentinel - route "
         "through Kataglyphis::resolveMaxAnisotropy(anisotropyEnabled, device->maxSamplerAnisotropy()) instead:"
      << joinViolations(anisotropy_violations);

    EXPECT_TRUE(get_features_violations.empty())
      << "getFeatures()/getFeatures2() called outside VulkanDevice.cpp - device capability queries belong in one "
         "place so availability and the features actually enabled cannot diverge:"
      << joinViolations(get_features_violations);
}

// ASSERT_VULKAN (common/Utilities.hpp) used to expand to a bare unbraced
// `if`, so every call site was a statement fragment rather than a statement -
// 38 of 49 sites omitted the trailing semicolon and relied on that. The macro
// is now wrapped in do/while(false), which makes it a real statement that
// *requires* the semicolon - a missing one is a compile error, so this test
// is cheap insurance rather than the primary check. It also guards against a
// future copy-pasted call site reintroducing the old, inconsistent spelling.
TEST(BuildIntegrity, EveryAssertVulkanCallSiteEndsInASemicolon)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::array<fs::path, 2> kRoots = { repo_root / "Src" / "GraphicsEngineVulkan",
        repo_root / "Src" / "shared" };

    const std::string_view kMacroInvocation = "ASSERT_VULKAN(";
    const fs::path definition_file = repo_root / "Src" / "GraphicsEngineVulkan" / "common" / "Utilities.hpp";

    std::vector<std::string> violations;
    std::error_code error;
    for (const fs::path &root : kRoots) {
        if (!fs::exists(root)) { continue; }
        for (fs::recursive_directory_iterator it(root, error), end; it != end; it.increment(error)) {
            if (error) { break; }
            const fs::path &path = it->path();
            if (!it->is_regular_file(error)) { continue; }
            if (fs::equivalent(path, definition_file, error)) { continue; }
            const auto extension = path.extension();
            if (extension != ".hpp" && extension != ".cpp" && extension != ".ixx") { continue; }

            const auto lines = readFileLines(path);
            if (!lines) { continue; }

            // A call spanning multiple lines only closes its parens on its
            // last line, so track the cumulative paren depth across lines
            // rather than checking each line in isolation - a parenless
            // argument line (e.g. "&allocation,") must not be mistaken for
            // the closing line just because it has zero net parens.
            bool inside_call = false;
            int paren_depth = 0;
            std::size_t call_start_line = 0;
            std::size_t line_number = 0;
            for (const auto &line : *lines) {
                ++line_number;
                std::size_t search_pos = 0;
                if (!inside_call) {
                    const auto found = line.find(kMacroInvocation);
                    if (found == std::string::npos) { continue; }
                    inside_call = true;
                    paren_depth = 0;
                    call_start_line = line_number;
                    search_pos = found;
                }

                std::size_t close_pos = std::string::npos;
                for (std::size_t i = search_pos; i < line.size(); ++i) {
                    if (line[i] == '(') { ++paren_depth; }
                    else if (line[i] == ')') {
                        --paren_depth;
                        if (paren_depth == 0) {
                            close_pos = i;
                            break;
                        }
                    }
                }

                if (close_pos == std::string::npos) { continue; }

                inside_call = false;
                const bool ends_in_semicolon = close_pos + 1 < line.size() && line[close_pos + 1] == ';';
                if (!ends_in_semicolon) {
                    violations.push_back(fs::relative(path, repo_root).generic_string() + ":"
                                          + std::to_string(call_start_line) + "-" + std::to_string(line_number)
                                          + ": " + line);
                }
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " ASSERT_VULKAN call site(s) do not end in \");\" - the macro is a do/while(false) statement and requires "
         "the trailing semicolon:"
      << joinViolations(violations);
}

// glTF 2.0 Section 3.9.4 ("Normals") requires that a back-facing fragment of
// a double-sided material flip its shading normal to face the viewer. All
// three C++ raster shaders already disable culling for doubleSided meshes
// (MeshDrawRecorder.cpp's dynamic eCullMode), so a back face reaching a
// fragment shader is exactly the doubleSided case, and SV_IsFrontFace is the
// cheapest available test for it. This scans the three raster shader sources
// as text and fails if any of them stops reading SV_IsFrontFace or stops
// negating its normal in response.
TEST(BuildIntegrity, DoubleSidedBackFacesFlipTheShadingNormal)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const char *kFailureMessage =
      "glTF 2.0 Section 3.9.4 requires flipping the shading normal on back-facing fragments of doubleSided "
      "materials; both C++ renderers already disable culling for doubleSided meshes, so a fragment shader that "
      "does not test SV_IsFrontFace and negate its normal renders those back faces black";

    const fs::path rasterizer_path = repo_root / "Resources/ShadersSlang/rasterizer/rasterizer.slang";
    const auto rasterizer_text_opt = readFileText(rasterizer_path);
    ASSERT_TRUE(rasterizer_text_opt.has_value()) << "could not open " << rasterizer_path.string();
    const std::string &rasterizer_text = *rasterizer_text_opt;
    EXPECT_NE(rasterizer_text.find("SV_IsFrontFace"), std::string::npos)
      << "rasterizer.slang's fs_main no longer reads SV_IsFrontFace. " << kFailureMessage;
    EXPECT_NE(rasterizer_text.find("N = -N"), std::string::npos)
      << "rasterizer.slang no longer negates N for back-facing fragments. " << kFailureMessage;

    const fs::path deferred_path = repo_root / "Resources/ShadersSlang/deferred/deferred.slang";
    const auto deferred_text_opt = readFileText(deferred_path);
    ASSERT_TRUE(deferred_text_opt.has_value()) << "could not open " << deferred_path.string();
    const std::string &deferred_text = *deferred_text_opt;
    EXPECT_NE(deferred_text.find("SV_IsFrontFace"), std::string::npos)
      << "deferred.slang's geometry_fs_main no longer reads SV_IsFrontFace. " << kFailureMessage;
    EXPECT_NE(deferred_text.find("N = -N"), std::string::npos)
      << "deferred.slang no longer negates N for back-facing fragments before the G-buffer write. "
      << kFailureMessage;

    const fs::path forward_path = repo_root / "Resources/ShadersSlang/forward/forward.slang";
    const auto forward_text_opt = readFileText(forward_path);
    ASSERT_TRUE(forward_text_opt.has_value()) << "could not open " << forward_path.string();
    const std::string &forward_text = *forward_text_opt;
    EXPECT_NE(forward_text.find("SV_IsFrontFace"), std::string::npos)
      << "forward.slang's fs_main no longer reads SV_IsFrontFace. " << kFailureMessage;
    EXPECT_NE(forward_text.find("nGeom = -nGeom"), std::string::npos)
      << "forward.slang no longer negates nGeom for back-facing fragments before t/b are derived. "
      << kFailureMessage;
}

// Mesh used to hold its own `model` matrix plus a getModel()/setModel() pair
// that nothing ever called - the per-model transform is owned by
// Model::set_model/Model::getModel and reached through
// Scene::update_model_matrix/Scene::getModelMatrix. A per-mesh copy would be
// a second source of truth that no draw path reads, so it must not come
// back.
TEST(BuildIntegrity, MeshDoesNotHoldAModelMatrix)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path mesh_ixx_path = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "Mesh.ixx";
    const auto text_opt = readFileText(mesh_ixx_path);
    ASSERT_TRUE(text_opt.has_value()) << "could not read " << mesh_ixx_path.string();
    const std::string &text = *text_opt;

    static const char *kFailureMessage =
      "the per-model transform is owned by Model::set_model/Model::getModel and reached through "
      "Scene::update_model_matrix/Scene::getModelMatrix; a per-mesh copy would be a second source of truth that no "
      "draw path reads";

    EXPECT_EQ(text.find("setModel"), std::string::npos) << "Mesh.ixx must not declare setModel(). " << kFailureMessage;
    EXPECT_EQ(text.find("getModel"), std::string::npos) << "Mesh.ixx must not declare getModel(). " << kFailureMessage;
    EXPECT_EQ(text.find("glm::mat4 model"), std::string::npos)
      << "Mesh.ixx must not hold a glm::mat4 model member. " << kFailureMessage;
}

// Rasterizer and DeferredRasterizer each used to spell out their frame-texture
// teardown twice - once in cleanUp() and once in recreateFrameResources() -
// and the two copies had drifted apart in Rasterizer, where the
// recreateFrameResources() copy dereferenced offscreenTextures/depthBufferImage
// unconditionally while cleanUp()'s copy guarded them. This pins that both
// call sites now go through a single releaseFrameTextures() helper, so a
// future edit cannot paste a third (possibly-diverging) copy back in.
TEST(BuildIntegrity, RasterStagesReleaseFrameTexturesThroughOneHelper)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    struct Target
    {
        fs::path path;
        std::string clean_up_name;
        std::string recreate_name;
    };

    const std::array<Target, 2> targets{ {
      { repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "Rasterizer.cpp", "Rasterizer::cleanUp",
        "Rasterizer::recreateFrameResources" },
      { repo_root / "Src" / "GraphicsEngineVulkan" / "renderer" / "DeferredRasterizer.cpp",
        "DeferredRasterizer::cleanUp", "DeferredRasterizer::recreateFrameResources" },
    } };

    static const std::array<const char *, 3> kTextureCleanUpTokens{
        "tex->cleanUp()",
        "texture->cleanUp()",
        "depthBufferImage->cleanUp()",
    };

    for (const auto &target : targets) {
        const auto text_opt = readFileText(target.path);
        ASSERT_TRUE(text_opt.has_value()) << "could not read " << target.path.string();
        const std::string &text = *text_opt;

        for (const auto &function_name : { target.clean_up_name, target.recreate_name }) {
            const auto span = function_body_span(text, function_name);
            ASSERT_TRUE(span.has_value())
              << target.path.string() << " is missing a body for " << function_name << "(...)";
            const std::string body = text.substr(span->first, span->second - span->first);

            EXPECT_NE(body.find("releaseFrameTextures()"), std::string::npos)
              << target.path.string() << "'s " << function_name
              << " no longer calls releaseFrameTextures() - the frame-texture teardown must go through the "
                 "shared helper rather than being pasted back in inline.";

            for (const char *token : kTextureCleanUpTokens) {
                EXPECT_EQ(body.find(token), std::string::npos)
                  << target.path.string() << "'s " << function_name << " contains an inline \"" << token
                  << "\" - frame-texture teardown must go through releaseFrameTextures() instead of a "
                     "second hand-written copy.";
            }
        }
    }
}

// GltfLoader.cpp used to spell out the has_pbr_metallic_roughness guard three
// times (once for factors/warnings, once for UV-transform reading, once for
// texture-slot assignment) and call readUvTransform / warnUnsupportedTexCoordSet
// / assignTextureSlot once per texture slot by hand. gltfTextureSlots()
// collapses all four slots (base-colour, metallic-roughness, normal,
// emissive) into one table that every caller loops over instead; this pins
// that shape so the guard/call-site duplication cannot silently come back
// slot by slot.
TEST(BuildIntegrity, GltfTextureSlotsAreEnumeratedInOneTable)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path path = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "GltfLoader.cpp";
    const auto lines_opt = readFileLines(path);
    ASSERT_TRUE(lines_opt.has_value()) << "missing " << path.string();

    // Comments stripped: the doc comments on gltfTextureSlots and its callers
    // spell out "has_pbr_metallic_roughness" in prose to explain the guard,
    // which would otherwise be mistaken for a second code occurrence.
    std::string text;
    for (const auto &raw_line : *lines_opt) {
        text += strip_line_comment(raw_line);
        text += '\n';
    }

    const auto slots_span = function_body_span(text, "gltfTextureSlots");
    ASSERT_TRUE(slots_span.has_value())
      << "gltfTextureSlots not found in " << path.string() << " - was the table rewrite reverted?";

    // Everything outside gltfTextureSlots' own body - the guard and the three
    // per-slot calls are only allowed to appear once each there, anchored to
    // the function's body rather than a line number so the exemption tracks
    // the function even if it moves.
    const std::string outside = text.substr(0, slots_span->first) + text.substr(slots_span->second);

    const auto count_occurrences = [](const std::string &haystack, const std::string &needle) {
        std::size_t count = 0;
        for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
             pos = haystack.find(needle, pos + needle.size())) {
            ++count;
        }
        return count;
    };

    EXPECT_LE(count_occurrences(outside, "has_pbr_metallic_roughness"), 1U)
      << "has_pbr_metallic_roughness must be gated inside gltfTextureSlots (fromGltfMaterial's own factor read is "
         "the one allowed exception), not repeated once per texture-slot call site";

    // readUvTransform/warnUnsupportedTexCoordSet are free functions, so their
    // own signatures ("readUvTransform(const char *materialName, ...") also
    // contain the function name followed by "(". Matching "materialName"
    // (the literal argument every call site passes, with no type prefix)
    // right after the opening paren selects call sites only, not the
    // declaration.
    EXPECT_LE(count_occurrences(outside, "readUvTransform(materialName"), 1U)
      << "readUvTransform must be called from one loop over gltfTextureSlots' table, not once per texture slot";
    EXPECT_LE(count_occurrences(outside, "warnUnsupportedTexCoordSet(materialName"), 1U)
      << "warnUnsupportedTexCoordSet must be called from one loop over gltfTextureSlots' table, not once per "
         "texture slot";
    // assignTextureSlot is a lambda ("assignTextureSlot = [&](..."), so every
    // occurrence of "assignTextureSlot(" in the file is a call site.
    EXPECT_LE(count_occurrences(outside, "assignTextureSlot("), 1U)
      << "assignTextureSlot must be called from one loop over gltfTextureSlots' table, not once per texture slot";
}

// endAndSubmitCommandBuffer's contract (CommandBufferManager.cpp) is that
// `false` means the submitted work never happened - the command buffer is
// freed either way, but nothing it recorded ran on the device. Discarding
// that result with static_cast<void>(...) silently treats a submit failure
// the same as success: the 2026-08 BLAS/TLAS/clouds/path-tracing cohort of
// fixes converted every such discard at the time this gate was added, each
// to whatever degraded behaviour its own call site defines. This test stops
// a new discard from being reintroduced. A genuinely intentional discard is
// still allowed via a trailing "// SUBMIT_RESULT_IGNORED_OK: <reason>"
// comment on the same line, the same exemption shape
// NoStageHandRollsTheColorAttachmentChain uses.
TEST(BuildIntegrity, EverySubmitResultIsCheckedOrExplicitlyExempt)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const char *const kCallMarker = "endAndSubmitCommandBuffer";
    static const char *const kDiscardMarker = "static_cast<void>(";
    static const char *const kExemptionPrefix = "SUBMIT_RESULT_IGNORED_OK: ";

    std::vector<std::string> violations;
    bool call_site_seen = false;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        if (path.extension() != ".cpp" && path.extension() != ".hpp" && path.extension() != ".ixx") { continue; }

        const std::string relative_file = fs::relative(path, repo_root).generic_string();

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::size_t line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find(kCallMarker) == std::string::npos) { continue; }
            if (line.find(kDiscardMarker) == std::string::npos) {
                call_site_seen = true;
                continue;
            }
            if (line.find(kExemptionPrefix) != std::string::npos) { continue; }
            violations.push_back(relative_file + ":" + std::to_string(line_number) + ": " + line);
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " endAndSubmitCommandBuffer() call(s) found under Src/ whose [[nodiscard]] result is thrown away with "
         "static_cast<void>(...) - capture the bool and handle the false case (it means the submitted work "
         "never happened), or add a trailing \"// SUBMIT_RESULT_IGNORED_OK: <reason>\" comment on the line if "
         "the discard is a deliberate, reasoned exception:"
      << joinViolations(violations);

    EXPECT_TRUE(call_site_seen) << "expected to find at least one non-discarded endAndSubmitCommandBuffer() call "
                                    "site under Src/ - if this fired, either the function was renamed (update "
                                    "kCallMarker) or every call site started discarding its result, and this gate "
                                    "would be silently checking nothing";
}

// rasterizer.slang's vs_main and deferred.slang's geometry_vs_main share one
// vertex stage, common/raster_geometry.slang's raster_geometry_vs(), because
// goldenRenderSuite.cpp's forward/deferred parity oracle requires the two
// passes to produce identical world position, normal and tangent - a bug in
// that oracle has already shipped once from the two bodies drifting apart.
// This scans both shader sources as text and fails if either stops importing
// raster_geometry or stops calling raster_geometry_vs(), or if either
// contains its own "o.worldTangent =" assignment - the marker for a
// re-hand-rolled vertex body that would reintroduce the drift.
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

// path_tracing.slang's bounce direction used to be assigned unnormalized
// (hitWorldNormal + a unit vector has length in [0, 2]), which quietly
// corrupted three downstream consumers that are all measured in units of
// rayDirection's length: ray.TMin's self-intersection epsilon, ray.TMax's
// visibility distance, and the miss branch's sky gradient (skyT extrapolates
// past [0, 1] once rayDirection.y leaves [-1, 1]). See docs/path-tracing.md's
// Bounce bullet and the "Self-intersection is guarded by a 1e-4 normal
// offset and ray-query t_min = 0.001" sentence, which this normalize keeps
// true.
TEST(BuildIntegrity, PathTracingBounceDirectionIsNormalized)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path path_tracing_path = repo_root / "Resources/ShadersSlang/path_tracing/path_tracing.slang";
    const auto contentsOpt = readFileText(path_tracing_path);
    ASSERT_TRUE(contentsOpt.has_value()) << "missing " << path_tracing_path.string();
    const std::string &contents = *contentsOpt;

    static const char *kFailureMessage =
      "rayDirection must be renormalized after the hemisphere bounce sample - the unnormalized "
      "sum (hitWorldNormal + a unit vector) has length in [0, 2], which corrupts ray.TMin's "
      "self-intersection epsilon, ray.TMax's visibility distance, and the miss branch's sky "
      "gradient, all of which are measured in units of rayDirection's length";

    EXPECT_NE(contents.find("rayDirection = normalize(newDirection);"), std::string::npos)
      << path_tracing_path.string() << " no longer normalizes the bounce direction. " << kFailureMessage;

    EXPECT_EQ(contents.find("rayDirection = newDirection;"), std::string::npos)
      << path_tracing_path.string() << " assigns the bounce direction without normalizing it. " << kFailureMessage;
}

// docs/shader-sharing.md's "Which C++ shading path reads which ObjMaterial
// field" table is the fine-grained sibling of EveryObjMaterialFieldIsReadByAShader
// (above): that gate only asks whether *some* shader mentions material.<field>
// anywhere, so a field read by one shading path and silently dropped by
// another still passes it - precisely the bug shape batches XI-XVII kept
// re-discovering one field at a time. This test parses the ObjMaterial member
// list out of scene_types.slang (parse_obj_material_member_names, reused from
// EveryObjMaterialFieldIsReadByAShader above) and the doc's table, and checks
// (a) exactly one row per member, no extras, (b) every row has five
// non-empty cells, one per shading path, and (c) for every cell that does not
// start with "not read", the corresponding shading path's own shader source
// actually names the member - either directly as material.<name>, or through
// one of the helper functions in kHelperFields below, the no-binding helpers
// (material_rules.slang/base_color.slang/emission.slang/normal_map.slang/
// material_textures.slang) every shading path funnels its ObjMaterial reads
// through instead of dereferencing every field inline.
TEST(BuildIntegrity, ShaderSharingDocCoversEveryObjMaterialFieldPerShadingPath)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path scene_types_path = repo_root / "Resources/ShadersSlang/common/scene_types.slang";
    ASSERT_TRUE(fs::exists(scene_types_path)) << "missing " << scene_types_path.string();
    const std::vector<std::string> members = parse_obj_material_member_names(scene_types_path);
    ASSERT_GT(members.size(), 3U) << "found only " << members.size() << " ObjMaterial member(s) in "
                                   << scene_types_path.string() << " - the parser itself is broken";

    const fs::path doc_path = repo_root / "docs" / "shader-sharing.md";
    const auto doc_content = readFileText(doc_path);
    ASSERT_TRUE(doc_content.has_value()) << "could not open " << doc_path.string();

    const auto section_start = doc_content->find("Which C++ shading path reads which");
    ASSERT_NE(section_start, std::string::npos)
      << doc_path.string() << " is missing its \"Which C++ shading path reads which ObjMaterial field\" section";
    const auto section_end = doc_content->find("\n## ", section_start);
    const std::string section_text = doc_content->substr(
      section_start, section_end == std::string::npos ? std::string::npos : section_end - section_start);

    // Split into "| cell | cell | ... |" rows, each row into pipe-delimited
    // cells (Member, rasterizer.slang, deferred.slang, raytrace.rchit.slang,
    // path_tracing.slang, shadow_map.slang / alpha_test.slang) - same shape
    // as ModelLoadingDocSrgbRowCoversEveryObjTextureDirective above.
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
                cell = first == std::string::npos ? ""
                                                   : cell.substr(first, cell.find_last_not_of(" \t") - first + 1);
                cells.push_back(std::move(cell));
                pos = end + 1;
            }
            rows.push_back(std::move(cells));
        }
    }
    // Row 0 is the header, row 1 the "| --- | ... |" separator; data rows
    // start at index 2.
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

    EXPECT_TRUE(malformed_rows.empty())
      << doc_path.string()
      << "'s per-shading-path table has row(s) with an empty shading-path cell:" << joinViolations(malformed_rows);

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

    // One shading-path source blob per table column, in table-column order.
    // shadow_map.slang and alpha_test.slang share one column - neither ever
    // shades a colour, both are alpha-only ObjMaterial consumers.
    static constexpr std::array<const char *, 5> kColumnNames{ "rasterizer.slang", "deferred.slang",
        "raytrace.rchit.slang", "path_tracing.slang", "shadow_map.slang / alpha_test.slang" };
    std::array<std::string, 5> column_text;
    {
        const std::array<std::vector<const char *>, 5> kColumnFiles{ {
          { "Resources/ShadersSlang/rasterizer/rasterizer.slang" },
          { "Resources/ShadersSlang/deferred/deferred.slang" },
          { "Resources/ShadersSlang/raytracing/raytrace.rchit.slang" },
          { "Resources/ShadersSlang/path_tracing/path_tracing.slang" },
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

    // Helper functions every shading path funnels its ObjMaterial reads
    // through, and the fields each is known to consume - so a cell whose
    // shader source names the helper (not material.<field> directly) still
    // counts as a real read. resolved_emission/resolved_metallic_roughness/
    // resolved_normal cover both their implicit-LOD and _lod0 explicit-LOD
    // forms via substring match (the _lod0 name starts with the un-suffixed
    // one). material_metallic_roughness and sample_metallic_roughness_lod0
    // are path_tracing.slang's documented exception
    // (material_textures.slang's header comment): it calls both directly
    // instead of resolved_metallic_roughness[_lod0](), so those two raw
    // helpers get their own entries.
    const std::vector<std::pair<std::string, std::vector<std::string>>> kHelperFields{
        { "resolved_emission",
          { "emission", "emissiveTextureID", "emissive_uv_transform_row0", "emissive_uv_transform_row1" } },
        { "resolved_metallic_roughness",
          { "metallic", "roughness", "shininess", "metallicRoughnessTextureID", "metallic_roughness_uv_transform_row0",
            "metallic_roughness_uv_transform_row1" } },
        { "resolved_normal",
          { "normalTextureID", "normalScale", "normal_uv_transform_row0", "normal_uv_transform_row1" } },
        { "alpha_masked_out", { "alphaCutoff", "dissolve" } },
        { "material_roughness", { "roughness", "shininess" } },
        { "base_color", { "diffuse" } },
        { "transform_uv", { "uv_transform_row0", "uv_transform_row1" } },
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

// PostStage::createRenderpass builds its single colour attachment with
// vk::AttachmentLoadOp::eLoad (the skybox pass already rendered into this
// swapchain image), so a clear value passed to beginRenderPass can never be
// consumed. If someone later switches the pass back to eClear, this test
// should fail and tell them to update the gate rather than silently passing.
// Alongside that, post.slang's fs_main must sample noisyTxt exactly once -
// two identical fetches for one texel invite the reading that they differ.
TEST(BuildIntegrity, TheLoadingPostPassDeclaresNoClearValue)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path post_stage_path = repo_root / "Src/GraphicsEngineVulkan/renderer/PostStage.cpp";
    const auto post_stage_text_opt = readFileText(post_stage_path);
    ASSERT_TRUE(post_stage_text_opt.has_value()) << "missing " << post_stage_path.string();
    const std::string &post_stage_text = *post_stage_text_opt;

    EXPECT_NE(post_stage_text.find("vk::AttachmentLoadOp::eLoad"), std::string::npos)
      << post_stage_path.string()
      << " no longer declares its colour attachment with vk::AttachmentLoadOp::eLoad - this pass loads its "
         "attachment, so a clear value can never be consumed; if that changed, this gate needs updating instead "
         "of just deleting the assertion below";
    EXPECT_EQ(post_stage_text.find("ClearColorValue"), std::string::npos)
      << post_stage_path.string()
      << " builds a ClearColorValue for a pass whose attachment is eLoad - this pass loads its attachment, so a "
         "clear value can never be consumed";
    // The empty-span idiom (std::span<const vk::ClearValue>{}) still names
    // the type - only a populated array/vector of clear values is dead here.
    EXPECT_EQ(post_stage_text.find("std::array<vk::ClearValue"), std::string::npos)
      << post_stage_path.string()
      << " builds an array of vk::ClearValue for a pass whose attachment is eLoad - this pass loads its "
         "attachment, so a clear value can never be consumed";
    EXPECT_NE(post_stage_text.find("std::span<const vk::ClearValue>{}"), std::string::npos)
      << post_stage_path.string()
      << " no longer passes an empty std::span<const vk::ClearValue> to buildRenderPassBeginInfo";

    const fs::path post_slang_path = slangRoot() / "post" / "post.slang";
    const auto post_slang_text_opt = readFileText(post_slang_path);
    ASSERT_TRUE(post_slang_text_opt.has_value()) << "missing " << post_slang_path.string();
    const std::string &post_slang_text = *post_slang_text_opt;

    std::size_t sample_count = 0;
    std::size_t pos = 0;
    static const std::string kNeedle = "noisyTxt.Sample";
    while ((pos = post_slang_text.find(kNeedle, pos)) != std::string::npos) {
        ++sample_count;
        pos += kNeedle.size();
    }
    EXPECT_EQ(sample_count, 1U)
      << post_slang_path.string() << " samples noisyTxt " << sample_count
      << " time(s) in fs_main - expected exactly one fetch into a float4, with .rgb/.a taken from it";
}

// Parses every `BENCHMARK(NAME)` registration in perfSuite.cpp, together with
// its trailing `->Arg(N)` chain up to the statement's `;`, and expands each
// to the Google-Benchmark name(s) it produces at run time: no `->Arg` yields
// the bare NAME, one or more `->Arg(N)` yields one "NAME/N" per argument.
// Other chained calls (`->Unit(...)`) are matched but ignored - they do not
// change the run name.
std::vector<std::string> parse_registered_benchmark_names(const std::string &perf_suite_text)
{
    static const std::regex kRegistration(
      R"(BENCHMARK\(\s*([A-Za-z_]\w*)\s*\)((?:\s*->\s*\w+\([^)]*\))*)\s*;)");
    static const std::regex kArg(R"(->\s*Arg\(\s*(\d+)\s*\))");

    std::vector<std::string> names;
    for (auto match = std::sregex_iterator(perf_suite_text.begin(), perf_suite_text.end(), kRegistration),
              match_end = std::sregex_iterator();
         match != match_end; ++match) {
        const std::string benchmark_name = (*match)[1].str();
        const std::string chain = (*match)[2].str();

        std::vector<std::string> args;
        for (auto arg_match = std::sregex_iterator(chain.begin(), chain.end(), kArg), arg_end = std::sregex_iterator();
             arg_match != arg_end; ++arg_match) {
            args.push_back((*arg_match)[1].str());
        }

        if (args.empty()) {
            names.push_back(benchmark_name);
        } else {
            for (const auto &arg : args) { names.push_back(benchmark_name + "/" + arg); }
        }
    }
    return names;
}

// perfSuite.cpp is the source of truth for what BENCHMARK_MAIN() actually
// registers; the checked-in baseline (win-9070xt-32core.json) exists to catch
// perf regressions on exactly those functions. The two have drifted before:
// BM_ComputeTangents grew two more ->Arg() rows on 2026-07-31 and the
// baseline was never updated, so Compare-PerfBaseline.ps1 (deliberately
// never-fatal for an unmatched benchmark - see its header comment) silently
// stopped protecting the function it was added to guard. This test is the
// commit-time half of that contract: the baseline must cover the suite, even
// though a single comparison run is allowed not to.
TEST(BuildIntegrity, EveryRegisteredBenchmarkHasAPerfBaselineRow)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path perf_suite_path = repo_root / "Test" / "perf" / "perfSuite.cpp";
    const auto perf_suite_text = readFileText(perf_suite_path);
    ASSERT_TRUE(perf_suite_text.has_value()) << "missing " << perf_suite_path.string();

    const fs::path baseline_path = repo_root / "Test" / "perf" / "baselines" / "win-9070xt-32core.json";
    const auto baseline_text = readFileText(baseline_path);
    ASSERT_TRUE(baseline_text.has_value()) << "missing " << baseline_path.string();

    const nlohmann::json baseline_doc = nlohmann::json::parse(*baseline_text, nullptr, /*allow_exceptions=*/false);
    ASSERT_FALSE(baseline_doc.is_discarded()) << baseline_path.string() << " is not valid JSON";
    ASSERT_TRUE(baseline_doc.contains("benchmarks") && baseline_doc["benchmarks"].is_array())
      << baseline_path.string() << " has no \"benchmarks\" array";

    std::set<std::string> baseline_names;
    for (const auto &entry : baseline_doc["benchmarks"]) {
        ASSERT_TRUE(entry.contains("name") && entry["name"].is_string())
          << baseline_path.string() << " has a benchmarks[] entry with no string \"name\"";
        baseline_names.insert(entry["name"].get<std::string>());
    }

    const std::vector<std::string> registered = parse_registered_benchmark_names(*perf_suite_text);
    ASSERT_FALSE(registered.empty()) << "found no BENCHMARK(...) registrations in " << perf_suite_path.string();
    const std::set<std::string> registered_names(registered.begin(), registered.end());

    std::vector<std::string> missing;
    for (const auto &name : registered_names) {
        if (!baseline_names.contains(name)) { missing.push_back(name); }
    }
    std::vector<std::string> stale;
    for (const auto &name : baseline_names) {
        if (!registered_names.contains(name)) { stale.push_back(name); }
    }

    EXPECT_TRUE(missing.empty()) << perf_suite_path.string() << " registers benchmark(s) with no row in "
                                  << baseline_path.string() << ":" << joinViolations(missing);
    EXPECT_TRUE(stale.empty()) << baseline_path.string()
                                << " has benchmarks[] row(s) for benchmark(s) no longer registered in "
                                << perf_suite_path.string() << " - delete the stale row(s):" << joinViolations(stale);
}

// GUISceneSharedVars.ixx has grown eleven fields since GUI.cpp's shadow block
// was written, and nothing has ever checked that a new one is reachable from
// the GUI. shadow_distance and cascade_split_lambda were the latest to slip
// through - both documented tunables that needed a rebuild to change. This
// hand-maintained list must be kept in sync with GUISceneSharedVars.ixx; a
// member added there needs either a control here or an entry in the
// exemption comment above the list.
TEST(BuildIntegrity, EveryTunableGuiSceneVarHasAControl)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path gui_path = repo_root / "Src" / "GraphicsEngineVulkan" / "gui" / "GUI.cpp";
    const auto gui_text = readFileText(gui_path);
    ASSERT_TRUE(gui_text.has_value()) << "could not open " << gui_path.string();

    // Every GUISceneSharedVars member a user can tune, excluding the
    // *_changed / *_requested latches (written by the GUI, not read from it),
    // selected_model_index (driven by the model list, not a slider), and
    // available_shadow_map_resolutions (labels, not a tunable value).
    static constexpr std::array<const char *, 24> kTunables{ "directional_light_radiance",
        "directional_light_color", "directional_light_direction", "shadow_map_res_index", "num_shadow_cascades",
        "pcf_radius", "cascaded_shadow_intensity", "shadow_distance", "cascade_split_lambda",
        "cloud_num_march_steps", "cloud_num_march_steps_to_light", "cloud_density_multiplier",
        "cloud_coverage_threshold", "cloud_pillowness", "cloud_cirrus_effect", "cloud_powder_effect",
        "clouds_enabled", "cloud_mesh_scale", "cloud_mesh_offset", "shadows_enabled", "skybox_enabled",
        "model_position", "model_rotation", "camera_fov" };

    std::vector<std::string> missing;
    for (const char *member : kTunables) {
        const std::string needle = std::string("guiSceneSharedVars.") + member;
        if (gui_text->find(needle) == std::string::npos) { missing.emplace_back(member); }
    }

    EXPECT_TRUE(missing.empty())
      << gui_path.string()
      << " does not read/write the following GUISceneSharedVars tunable(s) - add a control, or if it is not "
         "meant to be user-facing, add it to the exemption list in this test:"
      << joinViolations(missing);
}

// The right-mouse look mode shipped in 6c4191ab without the "KEY Bindings"
// panel being touched, and ESC has never been listed there either - the
// panel is a hand-written string literal nothing checks against the input
// code it is supposed to describe. This walks CameraController.ixx and
// WindowInputCallbacks.ixx for every GLFW_KEY_*/GLFW_MOUSE_BUTTON_* symbol
// they act on and asserts the panel literal in CommonGuiPanels.ixx mentions
// each one, via a hand-maintained symbol->prose table so a new binding with
// no table entry fails loudly by name instead of being silently invisible.
TEST(BuildIntegrity, KeyBindingsPanelListsEveryBinding)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path camera_controller_path = repo_root / "Src" / "shared" / "frontend" / "CameraController.ixx";
    const fs::path window_input_callbacks_path =
      repo_root / "Src" / "shared" / "frontend" / "WindowInputCallbacks.ixx";
    const fs::path panel_path = repo_root / "Src" / "shared" / "frontend" / "CommonGuiPanels.ixx";

    const auto camera_controller_text = readFileText(camera_controller_path);
    ASSERT_TRUE(camera_controller_text.has_value()) << "could not open " << camera_controller_path.string();
    const auto window_input_callbacks_text = readFileText(window_input_callbacks_path);
    ASSERT_TRUE(window_input_callbacks_text.has_value()) << "could not open " << window_input_callbacks_path.string();
    const auto panel_text = readFileText(panel_path);
    ASSERT_TRUE(panel_text.has_value()) << "could not open " << panel_path.string();

    static const std::regex kKeyOrButtonPattern(R"(GLFW_(?:KEY|MOUSE_BUTTON)_[A-Z0-9_]+)");

    std::set<std::string> bindings;
    for (const auto *text : { &*camera_controller_text, &*window_input_callbacks_text }) {
        for (auto it = std::sregex_iterator(text->begin(), text->end(), kKeyOrButtonPattern);
             it != std::sregex_iterator(); ++it) {
            bindings.insert(it->str());
        }
    }
    ASSERT_FALSE(bindings.empty()) << "found no GLFW_KEY_*/GLFW_MOUSE_BUTTON_* usages in "
                                    << camera_controller_path.string() << " or "
                                    << window_input_callbacks_path.string();

    // Symbol -> the exact substring the panel literal must contain for it.
    static const std::map<std::string, std::string> kSymbolToProse{
        { "GLFW_KEY_W", "W" },
        { "GLFW_KEY_A", "A" },
        { "GLFW_KEY_S", "S" },
        { "GLFW_KEY_D", "D" },
        { "GLFW_KEY_Q", "Q" },
        { "GLFW_KEY_E", "E" },
        { "GLFW_KEY_ESCAPE", "ESC" },
        { "GLFW_MOUSE_BUTTON_RIGHT", "right mouse" },
    };

    // Symbols that act on input but are deliberately not user-facing controls,
    // with the reason each is exempt from needing panel prose. Empty today -
    // every binding CameraController.ixx/WindowInputCallbacks.ixx implements
    // is user-facing.
    static const std::set<std::string> kExemptSymbols{};

    std::vector<std::string> unmapped;
    std::vector<std::string> missing_from_panel;
    for (const auto &symbol : bindings) {
        if (kExemptSymbols.contains(symbol)) { continue; }
        const auto mapping = kSymbolToProse.find(symbol);
        if (mapping == kSymbolToProse.end()) {
            unmapped.push_back(symbol);
            continue;
        }
        if (panel_text->find(mapping->second) == std::string::npos) { missing_from_panel.push_back(symbol); }
    }

    EXPECT_TRUE(unmapped.empty()) << "found GLFW binding(s) with no prose mapping in this test's kSymbolToProse "
                                      "table - add one, or if the binding is deliberately not user-facing, add it "
                                      "to kExemptSymbols with a reason:"
                                  << joinViolations(unmapped);
    EXPECT_TRUE(missing_from_panel.empty())
      << panel_path.string()
      << " does not mention the following key binding(s) - add them to renderCommonKeyBindings():"
      << joinViolations(missing_from_panel);
}

// Parses docs/cpp-renderer-improvements.md's `<!-- commit-suite-test-count: N -->` marker.
std::optional<int> parse_commit_suite_test_count_marker(const fs::path &doc_path)
{
    const auto lines = readFileLines(doc_path);
    if (!lines) { return std::nullopt; }

    static const std::regex kMarkerPattern(R"(<!--\s*commit-suite-test-count:\s*(\d+)\s*-->)");

    for (const auto &line : *lines) {
        std::smatch match;
        if (std::regex_search(line, match, kMarkerPattern)) { return std::stoi(match[1].str()); }
    }
    return std::nullopt;
}

// Counts every TEST(...)/TEST_F(...)/TEST_P(...) definition line across every
// .cpp under Test/commit/VulkanEngine/, mirroring
// `grep -c '^(TEST|TEST_F|TEST_P)\('`.
int count_commit_suite_tests(const fs::path &tests_dir)
{
    static const std::regex kTestPattern(R"(^(TEST|TEST_F|TEST_P)\()");

    int count = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(tests_dir, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".cpp") { continue; }
        const auto lines = readFileLines(it->path());
        if (!lines) { continue; }
        for (const auto &line : *lines) {
            if (std::regex_search(line, kTestPattern)) { ++count; }
        }
    }
    return count;
}

// docs/cpp-renderer-improvements.md's opening paragraph quotes a commit-suite
// test count that nothing enforced, and it drifted 573 tests stale before
// being caught. Fails loudly (not GTEST_SKIP) when the marker is missing, so
// deleting it cannot silently pass the gate - same shape as
// MaxTextureCountInDocsMatchesTheHeader above. Deliberately churny: every
// test-adding change now also touches one line of this doc. That is the
// price of the number in the first paragraph being true, and it is the same
// trade PerfBaselineCoversEveryRegisteredBenchmark already makes.
TEST(BuildIntegrity, ImprovementLogQuotesTheCurrentCommitSuiteTestCount)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path doc_path = repo_root / "docs" / "cpp-renderer-improvements.md";
    const auto marker_value = parse_commit_suite_test_count_marker(doc_path);
    ASSERT_TRUE(marker_value.has_value())
      << doc_path.string()
      << " is missing its '<!-- commit-suite-test-count: N -->' marker line - this is a one-line update, add it "
         "back with the current count from Test/commit/VulkanEngine/.";

    const fs::path tests_dir = repo_root / "Test" / "commit" / "VulkanEngine";
    const int actual_count = count_commit_suite_tests(tests_dir);

    EXPECT_EQ(*marker_value, actual_count)
      << doc_path.string() << "'s commit-suite-test-count marker says " << *marker_value << " but "
      << tests_dir.string() << " actually defines " << actual_count
      << " TEST/TEST_F/TEST_P case(s) - this is a one-line update to the marker in " << doc_path.string() << ".";
}

// VulkanDevice marks every one of its query accessors const; this gate keeps
// a newly-added read-only accessor elsewhere in Src/ from reintroducing the
// drift that left seventeen of them non-const.
//
// "Read-only" here deliberately excludes anything whose return type is a
// non-const lvalue reference (Model::getTextures(), VulkanBuffer::getBuffer(),
// GUI::getGuiSceneSharedVars(), ...): those hand out a mutable handle by
// design - callers write through them - and are a different category from a
// query accessor, out of scope for this gate. A pointer return (Texture*,
// Mesh*, GLFWwindow*) stays in scope: for a unique_ptr<T>/shared_ptr<T>-backed
// accessor, or a plain raw-pointer member, constness does not propagate to
// the pointee, so marking the accessor const costs nothing.
//
// Free functions (e.g. Frustum.ixx's isVisible) are excluded by requiring the
// match to start on an indented line: every inline accessor in Src/ is a
// class member and therefore indented, while every free function here starts
// at column 0.
TEST(BuildIntegrity, ReadOnlyAccessorsAreConst)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(fs::exists(src_root));

    struct Exemption
    {
        std::string file;
        std::string name;
        std::string reason;
    };
    // Accessors proven - by the compiler, not by inspection - unable to
    // become const without also changing their return type, which is a
    // bigger, separate change than this gate makes.
    const std::vector<Exemption> exemptions = { { "Src/GraphicsEngineVulkan/scene/Model.ixx", "getMesh",
      "returns Mesh* via meshes[index] over std::vector<Mesh> - operator[] const yields const Mesh&, so "
      "&meshes[index] cannot convert to the non-const Mesh* this returns (unlike the "
      "unique_ptr<T>::get()/shared_ptr<T>::get()-backed accessors, whose constness does not propagate to the "
      "pointee)" } };

    static const std::regex kAccessorRegex(
      R"re((get[A-Z_]\w*|get_[a-z]\w*|is[A-Z]\w*|is_[a-z]\w*)\s*\(([^()]*)\)\s*(const)?\s*(noexcept)?\s*(\{|;))re");

    int accessors_checked = 0;
    std::vector<std::string> violations;
    std::error_code error;
    for (fs::recursive_directory_iterator it(src_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error)) { continue; }
        const std::string ext = path.extension().string();
        if (ext != ".ixx" && ext != ".hpp") { continue; }

        const auto lines = readFileLines(path);
        if (!lines) { continue; }
        std::string stripped_text;
        for (const auto &raw_line : *lines) {
            stripped_text += strip_line_comment(raw_line);
            stripped_text += '\n';
        }

        const std::string relative_path = fs::relative(path, repo_root).generic_string();

        for (auto match = std::sregex_iterator(stripped_text.begin(), stripped_text.end(), kAccessorRegex);
             match != std::sregex_iterator(); ++match) {
            const auto &found = *match;
            if (found[5].str() != "{") { continue; }// declaration-only, or a call site - not a definition

            const auto match_start = static_cast<std::size_t>(found.position(0));
            const auto line_start_pos = match_start == 0 ? std::string::npos
                                                           : stripped_text.find_last_of('\n', match_start - 1);
            const std::size_t prefix_start = line_start_pos == std::string::npos ? 0 : line_start_pos + 1;
            const std::string prefix = stripped_text.substr(prefix_start, match_start - prefix_start);

            // Free functions in this codebase sit at namespace scope (column
            // 0); every class member accessor is indented.
            if (prefix.empty() || (prefix.front() != ' ' && prefix.front() != '\t')) { continue; }

            const std::size_t trimmed_end = prefix.find_last_not_of(" \t");
            const std::string trimmed_prefix = trimmed_end == std::string::npos ? std::string{} : prefix.substr(0, trimmed_end + 1);
            const bool returns_non_const_reference =
              !trimmed_prefix.empty() && trimmed_prefix.back() == '&' && trimmed_prefix.find("const") == std::string::npos;
            if (returns_non_const_reference) { continue; }// mutable-handle accessor, not read-only - out of scope

            const std::string name = found[1].str();
            ++accessors_checked;
            if (found[3].matched) { continue; }// already const

            const bool is_exempt = std::any_of(exemptions.begin(), exemptions.end(), [&](const Exemption &exemption) {
                return exemption.file == relative_path && exemption.name == name;
            });
            if (is_exempt) { continue; }

            const auto line_number = 1
              + static_cast<std::size_t>(std::count(stripped_text.begin(), stripped_text.begin() + static_cast<std::ptrdiff_t>(match_start), '\n'));
            violations.push_back(
              relative_path + ':' + std::to_string(line_number) + ": " + name + "() is a read-only accessor with no const qualifier");
        }
    }

    EXPECT_GT(accessors_checked, 0) << "scanner found zero candidate accessors under " << src_root.string()
                                     << " - the regex or the indentation heuristic has drifted from the "
                                        "codebase's formatting";

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " read-only accessor(s) are missing const - either add const, or if the compiler rejects it, add a "
         "named Exemption recording the member that forced it: "
      << joinViolations(violations);
}

// This suite used to hand-roll the same "join a vector of strings for a gtest
// failure message" lambda at 102 call sites, three of which had already
// drifted into their own spelling of the separator. Kataglyphis::TestSupport::
// joinViolations (RepoFiles.hpp) is now the one place that loop lives - this
// pins that down the same way ImageMemoryBarriersGoThroughTheSharedHelper does
// for hand-rolled barriers. A flat budget of 0 is correct here, unlike the
// per-file barrier budgets: there is no legitimate reason for this file to
// hand-roll the join again.
TEST(BuildIntegrity, ViolationListsGoThroughTheSharedJoiner)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path this_file = repo_root / "Test" / "commit" / "VulkanEngine" / "buildIntegritySuite.cpp";
    const auto lines = readFileLines(this_file);
    ASSERT_TRUE(lines.has_value()) << "could not read " << this_file.string();

    const std::string kHandRolledJoinDecl = std::string("std::string ") + "joined;";

    std::vector<std::string> violations;
    std::size_t line_number = 0;
    for (const auto &line : *lines) {
        ++line_number;
        if (line.find(kHandRolledJoinDecl) != std::string::npos) {
            violations.push_back(this_file.string() + ':' + std::to_string(line_number)
                                  + ": hand-rolled join loop - use Kataglyphis::TestSupport::joinViolations instead");
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size() << " hand-rolled join loop(s) found, budget is 0: " << joinViolations(violations);
}
