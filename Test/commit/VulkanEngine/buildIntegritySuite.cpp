// Filesystem-only guards against costly build-system bugs; no GPU needed, so they run in any CI container.

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

// Strips a trailing "// ..." comment so prose naming a constant cannot pass for its definition.
std::string strip_line_comment(const std::string &line)
{
    const auto comment_pos = line.find("//");
    return comment_pos == std::string::npos ? line : line.substr(0, comment_pos);
}

// Recursion helper for import_closure; `visited` makes each resolved file recurse exactly once.
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

// Every .slang file `source` transitively imports, excluding itself; common/ wins over the importer's own directory.
std::set<fs::path> import_closure(const fs::path &slang_root, const fs::path &source)
{
    std::set<fs::path> visited;
    std::set<fs::path> closure;
    collect_import_closure(slang_root, source, visited, closure);
    closure.erase(source);
    return closure;
}

// Newest mtime in the import closure and its file, so callers can name the stale import; false if the closure is empty.
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

// shader-manifest.json is both Slang build scripts' single source of truth, so these tests check the files against it.

// One wgslMap row; histogram.wgsl has none, being hand-written (Slang cannot emit its InterlockedAdd to WGSL).
struct WgslMapping
{
    std::string slang_source;// "src": relative to Resources/ShadersSlang/
    std::string dst_dir;     // "dst": relative to the repository root
    std::string wgsl_file;   // "out": destination file name
};

// What this suite needs from shader-manifest.json, parsed once (see shader_manifest below).
struct ShaderManifestData
{
    // Enabled rows targeting spirv (paths relative to Resources/ShadersSlang/): what the C++ renderer consumes.
    std::set<std::string> vulkan_spirv_sources;
    // The build/spirv/ subdirectories; every other shader directory is WGSL-only and never scanned for SPIR-V.
    std::set<std::string> engine_spirv_subdirs;
    // Every enabled row's .slang path whatever its target, so no Slang entry point goes unaccounted for.
    std::set<std::string> all_enabled_manifest_files;
    // Union of targets per file, to classify each source as spirv-only, wgsl-only or both.
    std::map<std::string, std::set<std::string>> file_targets;
    std::vector<WgslMapping> wgsl_map;
    // "depthTexturePatches" output filenames, without the "_comment" documentation keys.
    std::set<std::string> depth_patched_files;
    // "minSlangcVersionForWgsl"; empty when absent, which its own test fails, as no floor re-enables the broken emit.
    std::string min_slangc_version_for_wgsl;
};

// Strict: a malformed row returns std::nullopt rather than being skipped; no-throw parse, since exceptions are off.
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

// Parsed once per process; std::nullopt means missing or malformed, so callers ASSERT on has_value(), never skip.
const std::optional<ShaderManifestData> &shader_manifest(const fs::path &repo_root)
{
    static const std::optional<ShaderManifestData> cached =
      parse_shader_manifest(slangRoot() / "shader-manifest.json");
    return cached;
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
    if (!fs::exists(binary_dir, error)) { return false; }
    for (fs::directory_iterator it(binary_dir, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        if (!it->is_regular_file(error) || it->path().extension() != ".spv") { continue; }
        if (source_for_spirv(it->path(), spirv_root, slang_root) == source) { return true; }
    }
    return false;
}

// Binding constants hand-mirrored between host_device_shared_vars.hpp and scene_types.slang.
const std::vector<std::string> kSharedConstantNames = {
    "MAX_TEXTURE_COUNT", "MAX_CASCADES", "MAX_PCF_RADIUS", "globalUBO_BINDING", "sceneUBO_BINDING", "OBJECT_DESCRIPTION_BINDING",
    "TEXTURES_BINDING", "SAMPLER_BINDING", "SHADOW_MAP_BINDING", "TLAS_BINDING", "OUT_IMAGE_BINDING",
    "ACCUMULATION_IMAGE_BINDING", "GBUFFER_NORMAL_BINDING", "GBUFFER_ALBEDO_BINDING", "GBUFFER_MATERIAL_BINDING",
    "GBUFFER_DEPTH_BINDING"
};

bool is_identifier_char(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_'; }

// The integer after a constant name, for both "#define NAME 3" and "[static] const int NAME = 3;".
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

// Whole-word matches only, so a longer identifier containing a constant's name is ignored.
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

// Every GTest suite name under tests_dir; anchored at line start, so comments and string literals do not count.
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

// Test names of `suite`, anchored like collect_defined_suites; file I/O only, so GPU suites list without a GPU.
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

// The five integers of docs/gpu-golden-testing.md's golden-counts marker line.
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

// std::nullopt if the marker or any field is missing, so a deleted marker fails rather than passes.
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

// The doc's known-issue filter exclusions as (suite, name); only lines with `:-` count, and finding none must fail.
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

// Names in docs/path-tracing.md's pt-goldens marker; std::nullopt if missing, malformed or empty, so deletion fails.
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

// Suite globs of Invoke-WindowsLane.ps1's $gpuOnlySuites array, anchored on its opener and -join closer.
std::optional<std::vector<std::string>> parse_ci_gpu_excluded_suites(const fs::path &lane_path)
{
    const auto lines = readFileLines(lane_path);
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

// Fuzz targets declared via kataglyphis_add_fuzz_test(<name> ...); the definition has a space there, so it never matches.
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

// Benchmark names, as "<name>/<n>" per chained ->Arg(<n>) like Google Benchmark's run names; zero found fails the gate.
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

// benchmarks[].name from a Google Benchmark JSON baseline; no-throw parse, since exceptions are disabled.
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

// Fuzz targets of Invoke-WindowsLane.ps1's seed loop, anchored so no other loop matches; empty means no anchor: fail.
std::optional<std::vector<std::string>> parse_ci_fuzz_targets(const fs::path &lane_path)
{
    const auto lines = readFileLines(lane_path);
    if (!lines) { return std::nullopt; }

    // Escaped when a host shell would expand $t, plain when the command arrives via env; accept both.
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

// Fuzz targets of reusable-linux.yml's "for t in ...; do" loop, anchored likewise; empty means no anchor: fail.
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

// Fuzz executables of Invoke-ClangClDebug.ps1's loop without ".exe", to compare with the declared targets.
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

// SPIR-V parsing; the magic is re-declared because ShaderHelper.cpp's copy is module-private.
constexpr uint32_t kSpirvMagicNumber = 0x07230203;
constexpr std::size_t kSpirvHeaderWordCount = 5;
constexpr uint32_t kOpName = 5;
constexpr uint32_t kOpMemberName = 6;
constexpr uint32_t kOpMemberDecorate = 72;
constexpr uint32_t kDecorationOffset = 35;

// SPIR-V literal strings pack four bytes per word, low byte first, NUL-terminated and word-padded.
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

// Emitted struct name -> member -> Offset decoration; std::nullopt if unreadable or not a SPIR-V module.
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

// A module's execution model (every .spv here has one entry point) and the distinct opcodes it uses.
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

constexpr uint32_t kOpDecorate = 71;
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

// Emitted struct name plus member offsets; strides live on the array types and are deliberately out of scope.
struct SpirvStructContract
{
    std::string spirv_name;
    std::map<std::string, std::size_t> member_offsets;
};

// No PushConstantSkyBox_std430 entry: SkyBox pushes a bare uint32_t with no host struct to compare.
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
        // skybox.slang re-declares GlobalUBO's members under its own name: same host type, another emitted struct.
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
          // The host's cascadeIndex carries the shader's objectIndex (see makeShadowPush); what must agree is the offset.
          { { "model", offsetof(ShadowPushConstants, model) },
            { "objectIndex", offsetof(ShadowPushConstants, cascadeIndex) } } },
    };
}

// First non-comment statement in the body of `qualified_name`; assumes its parameter list holds no parentheses.
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

// Like first_statement_of_function, but returns the [begin, end) span of the whole body.
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

// A reused stale .spv silently ignores shader edits, so no .spv may be older than its .slang source.
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

// A .spv older than a module its source really imports was not recompiled; scoped to the closure, not all of common/.
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

// ssao's closure must not hold material_fetch; rasterizer's reaches scene_types only by recursing through material_fetch.
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

// Implicit LOD needs derivatives, which only Fragment has; outside it the sample is illegal and can lose the device.
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

// create() must release first, or a second call leaks the previous allocation; a text check, as behaviour needs a device.
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

// Likewise create() must open with releaseGpuResources(), or a second call leaks the layout and pool.
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

// VUID-vkDestroyImage-image-01000: the view release must precede the createImage() that frees the old image.
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

// Every entry-point source the engine consumes must compile, or one missing from the manifest surfaces only at runtime.
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

// Every loaded shader needs a .spv, or it fails only at runtime; the list derives from the literals under Src/.
TEST(BuildIntegrity, ActivePipelineShadersHaveCompiledBinaries)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty());

    const fs::path slang_root = slangRoot();
    const fs::path spirv_root = spirvRoot();

    const auto referenced = collect_spirv_paths_referenced_by_sources(repo_root);
    // Catches a scanner that silently finds nothing; raise this floor when a pass is added, never lower it.
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

// A silent divergence corrupts every binding with no validation error; each name must exist in both files, not just match.
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

// Compares against the compiled header, since a text parse can agree with an edit the build never sees.
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

// The Windows lane runs every suite but the excluded GPU ones, so only that exclusion list can drift.
TEST(BuildIntegrity, WindowsCiExcludesExactlyTheGpuSuites)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path lane_path = repo_root / "scripts" / "windows" / "Invoke-WindowsLane.ps1";
    const auto filter_suites_opt = parse_ci_gpu_excluded_suites(lane_path);
    if (!filter_suites_opt.has_value()) {
        GTEST_SKIP() << "could not open " << lane_path.string() << " - not running from the repo root?";
    }
    const std::vector<std::string> &filter_suites = *filter_suites_opt;
    ASSERT_FALSE(filter_suites.empty())
      << "parsed zero suites out of the $gpuOnlySuites array in " << lane_path.string()
      << " - the anchor text ('$gpuOnlySuites = @(' / \"-join ':'\") may have changed";
    const std::set<std::string> filter_set(filter_suites.begin(), filter_suites.end());

    const std::set<std::string> defined_suites =
      collect_defined_suites(repo_root / "Test" / "commit" / "VulkanEngine");
    ASSERT_FALSE(defined_suites.empty()) << "found zero TEST()/TEST_F() suites under Test/commit/VulkanEngine - "
                                            "the scan itself is broken";

    // The container has a Vulkan loader but no GPU, so these abort in device creation instead of skipping.
    const std::set<std::string> gpu_excluded_suites = { "GoldenRender", "Integration" };

    EXPECT_EQ(filter_set, gpu_excluded_suites)
      << "Invoke-WindowsLane.ps1's $gpuOnlySuites exclusion list no longer matches the expected GPU-suite set - "
         "update either the lane script or this test's gpu_excluded_suites";

    for (const auto &suite : filter_suites) {
        EXPECT_TRUE(defined_suites.contains(suite))
          << "Invoke-WindowsLane.ps1 excludes '" << suite
          << "' from $gpuOnlySuites, but no such suite is defined under Test/commit/VulkanEngine "
             "(renamed or deleted?)";
    }
}

// Fuzz targets have no negative filter, so one missing from the Windows lane's array silently never runs.
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

    const fs::path lane_path = repo_root / "scripts" / "windows" / "Invoke-WindowsLane.ps1";
    const auto ci_targets_opt = parse_ci_fuzz_targets(lane_path);
    if (!ci_targets_opt.has_value()) {
        GTEST_SKIP() << "could not open " << lane_path.string() << " - not running from the repo root?";
    }
    const std::vector<std::string> &ci_targets = *ci_targets_opt;
    ASSERT_FALSE(ci_targets.empty())
      << "parsed zero fuzz targets out of the foreach array in " << lane_path.string()
      << R"( - the anchor text ('foreach (`$t in @(' / '))') may have changed)";
    const std::set<std::string> ci_set(ci_targets.begin(), ci_targets.end());

    // For a future smoke-only target that should not gate CI; empty, since every target runs today.
    const std::set<std::string> excluded_from_ci;

    std::vector<std::string> missing_from_ci;
    for (const auto &target : declared_targets) {
        if (ci_set.contains(target) || excluded_from_ci.contains(target)) { continue; }
        missing_from_ci.push_back(target);
    }
    EXPECT_TRUE(missing_from_ci.empty())
      << missing_from_ci.size()
      << " fuzz target(s) declared in Test/fuzz/CMakeLists.txt are neither in Invoke-WindowsLane.ps1's "
         "fuzz-seed foreach array nor in the smoke-target exclusion list, so they silently do not run in CI: "
      << joinViolations(missing_from_ci);

    std::vector<std::string> dead_ci_entries;
    for (const auto &target : ci_targets) {
        if (!declared_set.contains(target)) { dead_ci_entries.push_back(target); }
    }
    EXPECT_TRUE(dead_ci_entries.empty())
      << dead_ci_entries.size()
      << " entry/entries in Invoke-WindowsLane.ps1's fuzz-seed foreach array do not correspond to any target declared "
         "in Test/fuzz/CMakeLists.txt (renamed or deleted?): "
      << joinViolations(dead_ci_entries);
}

// Every declared fuzz target must run in the Linux workflow, the Windows lane and the local runner, or it goes unexercised.
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

    const fs::path linux_workflow_path = repo_root / ".github" / "workflows" / "reusable-linux.yml";
    const fs::path windows_lane_path = repo_root / "scripts" / "windows" / "Invoke-WindowsLane.ps1";
    const fs::path local_runner_path = repo_root / "scripts" / "windows" / "Invoke-ClangClDebug.ps1";

    const auto linux_targets_opt = parse_linux_ci_fuzz_targets(linux_workflow_path);
    if (!linux_targets_opt.has_value()) {
        GTEST_SKIP() << "could not open " << linux_workflow_path.string() << " - not running from the repo root?";
    }
    const auto windows_targets_opt = parse_ci_fuzz_targets(windows_lane_path);
    if (!windows_targets_opt.has_value()) {
        GTEST_SKIP() << "could not open " << windows_lane_path.string() << " - not running from the repo root?";
    }
    const auto local_targets_opt = parse_local_runner_fuzz_targets(local_runner_path);
    if (!local_targets_opt.has_value()) {
        GTEST_SKIP() << "could not open " << local_runner_path.string() << " - not running from the repo root?";
    }
    ASSERT_FALSE(linux_targets_opt->empty())
      << "parsed zero fuzz targets out of the for-loop in " << linux_workflow_path.string()
      << " - the anchor text ('for t in ' / '; do') may have changed";
    ASSERT_FALSE(windows_targets_opt->empty())
      << "parsed zero fuzz targets out of the foreach array in " << windows_lane_path.string()
      << R"( - the anchor text ('foreach (`$t in @(' / '))') may have changed)";
    ASSERT_FALSE(local_targets_opt->empty())
      << "parsed zero fuzz targets out of the foreach array in " << local_runner_path.string()
      << R"( - the anchor text ('foreach ($fuzzExecutable in @(' / '))') may have changed)";

    const std::set<std::string> linux_set(linux_targets_opt->begin(), linux_targets_opt->end());
    const std::set<std::string> windows_set(windows_targets_opt->begin(), windows_targets_opt->end());
    const std::set<std::string> local_set(local_targets_opt->begin(), local_targets_opt->end());

    // A target may skip a lane only if listed here with a reason; empty means every target runs everywhere.
    struct NotRunInCi
    {
        std::string target;
        std::string lane;// "reusable-linux.yml", "Invoke-WindowsLane.ps1", or "Invoke-ClangClDebug.ps1"
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
    check_lane("reusable-linux.yml", linux_set);
    check_lane("Invoke-WindowsLane.ps1", windows_set);
    check_lane("Invoke-ClangClDebug.ps1", local_set);

    EXPECT_TRUE(missing.empty())
      << missing.size()
      << " fuzz target(s) declared in Test/fuzz/CMakeLists.txt do not run in every CI lane and are not listed "
         "in kNotRunInCi with a reason: "
      << joinViolations(missing);

    // Nothing else catches a stale local-runner entry, so check both directions here.
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

// Source-shape gate: each Invoke-ClangCl*.ps1 must `exit` with a variable, or a failed run reads as a clean quit.
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

// Compare-PerfBaseline.ps1 ignores one-sided entries, so a benchmark without a baseline row is silently never compared.
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

// Local guard (a clone resets mtimes): WGSL must not predate its source or imports; byte-identical re-emits stay stale.
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

// WGSL has no string literals and the backend emits no comments, so any "//" is a hand-edit the next regenerate drops.
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

// CI backstop for docs/shader-build-pipeline.md § The combined WGSL emit needs slangc ≥ 2026.8.
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

// The scripts read a missing floor as none, re-enabling the broken emit; they compare MAJOR.MINOR only.
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

// Comments must not name the deleted GLSL tree, a GLSL-era stage extension, or a .slang basename that does not exist.
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

    // Trailing slash: without it this also matches the live Resources/ShadersSlang tree.
    static const std::string kDeadPath = "Resources/Shaders/";
    // Only a trailing GLSL-era extension is dead; raytrace.rchit.slang carries one mid-name.
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

    // .slang files: every line, as shader source has no filename string literals.
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

    // C++ sources: comments only, so live .spv string literals are never scanned.
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

// All three raster shaders share alpha_masked_out(); a negative alphaCutoff never discards.
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

        // An untextured MASK material still alpha-tests its factor, so both sides of the textureID branch call it.
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

    // MASK alpha is baseColorFactor.a times texture alpha; without dissolve every caller loses the factor half.
    {
        const fs::path path = repo_root / "Resources/ShadersSlang/common/material_rules.slang";
        const auto textOpt = readFileText(path);
        ASSERT_TRUE(textOpt.has_value()) << "missing " << path.string();
        const std::string &text = *textOpt;

        const std::size_t fn_start = text.find("bool alpha_masked_out(");
        ASSERT_NE(fn_start, std::string::npos) << "alpha_masked_out() definition not found in " << path.string();
        // A fixed window covers the short body without brace matching.
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

// Raw emissive, normal and metallic-roughness samples belong to material_textures.slang, so no path re-wraps them.
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

    // path_tracing keeps its block inline: the helper's fallback would apply metallic (glTF default 1.0) to its kernel.
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

// glTF base colour is factor times texture, so every textured branch must route its sample through base_color(.
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

// f0 = mix(0.04, albedo, metallic); a literal 0.0 there renders every metal as a dielectric.
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

// Every texture sample line must also call transform_uv( or a per-slot accessor, or render modes disagree.
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

// Normal maps perturb shading only through normal_map.slang's shared helper, which must use the sampled normal.
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

    // material_textures.slang is the sole caller of apply_normal_map() and folds in normalScale.
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

// All four alpha-testing sites must fold the map_d texture into the alpha they test.
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

// Transforms are per slot, and the bare transform_uv overload applies the base-colour rows to any slot it samples.
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

    // The OBJ-only map_d slot has no transform of its own, so its two samplers legitimately use the bare overload.
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

// material_textures.slang alone declares the texture bindings and samples the four non-base slots; base colour stays inline.
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

    // An explicit list: a directory walk would have to excuse this file's and material_textures.slang's own mentions.
    static const std::vector<fs::path> kConsumers = {
        slang_root / "rasterizer/rasterizer.slang",
        slang_root / "deferred/deferred.slang",
        slang_root / "rasterizer/shadows/shadow_map.slang",
        slang_root / "raytracing/raytrace.rchit.slang",
        slang_root / "common/alpha_test.slang",
        slang_root / "path_tracing/path_tracing.slang",
    };

    // The base-colour overload of resolve_texture_slot stays inline at each path, so it is not listed.
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

// Row members only inside their accessors and structs: naming them directly can pair one slot's row0 with another's row1.
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

// One uv-set mask read per slot accessor, so a hand-rolled selection cannot diverge from their bit assignment.
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

// Every shading path must swizzle metallic-roughness through material_rules.slang (G = roughness, B = metallic).
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

    // path_tracing keeps its block inline: the shared fallback would apply metallic to its Lambertian-only kernel.
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

// A local cascade-count copy survives a MAX_CASCADES bump; WGSL-only shaders pin theirs on the Rust side, so skip them.
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

// The fullscreen uv's y differs per target, so a hand-written copy of either direction mirrors one renderer.
TEST(BuildIntegrity, NoShaderRedeclaresTheFullscreenUvMapping)
{
    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    // Texel-to-NDC in compute and ray-generation shaders is legitimate, so only fullscreen_vs importers are held to it.
    static const std::vector<std::string> kCopies{ "(y + 1.0) * 0.5", "uv * 2.0 - 1.0", "uv.y * 2.0" };

    std::vector<std::string> violations;
    int importers = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(slang_root, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        const fs::path &path = it->path();
        if (!it->is_regular_file(error) || path.extension() != ".slang") { continue; }
        const std::string relative_path = fs::relative(path, slang_root).generic_string();
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
        }
    }

    EXPECT_GE(importers, 5) << "fewer fullscreen.slang importers than expected - did the import syntax change?";
    EXPECT_TRUE(violations.empty()) << violations.size()
                                    << " fullscreen uv mapping copies - call fullscreen_vs / fullscreen_uv_to_ndc: "
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

// Bindings must use the named constants, or a renumbering moves both headers while a shader literal stays behind.
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

// A traced object-index source and how many instance-plus-geometry index pairs it must contain.
struct TracedObjectIndexFile
{
    std::string relative_path;
    int expected_pair_count;
};

// Update these counts when a traced object-index site is added, removed or moved.
const std::vector<TracedObjectIndexFile> kTracedObjectIndexFiles = {
    {"path_tracing/path_tracing.slang", 3},
    {"raytracing/raytrace.rchit.slang", 1},
    {"raytracing/raytrace.rahit.slang", 1},
};

// The custom index names a model's first mesh, so every traced index must add the geometry index to reach the hit mesh.
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

// InstanceIndex() is the TLAS instance index, not the stamped custom index; presence only, as counts move with inlining.
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

// Only material_fetch.slang declares objectDescription and does the lookup; scene_types.slang just defines the types.
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

// The shader re-clamps what the host clamps, as only its own clamp guards the matrix index and tap loop on the GPU.
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
        EXPECT_GE(pos, update_span->first) << "uploadLightMatrices( call at offset " << pos
                                            << " lies outside update_uniform_buffers";
        EXPECT_LT(pos, update_span->second) << "uploadLightMatrices( call at offset " << pos
                                             << " lies outside update_uniform_buffers";
        ++upload_count;
    }
    EXPECT_GT(upload_count, 0U) << "expected at least one uploadLightMatrices( call in VulkanRenderer.cpp";
}

// resolve_texture_slot() is the texture-slot clamp's one definition; a local copy could drift from it.
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

// dirLight.direction is the travel direction, so toward-light vectors negate it where read, inside normalize(...).
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

                // Walk back over the "scene." or "sceneUBO_lighting." prefix to where the read starts.
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

// Roughness comes from material_roughness() in every shading path, never a literal.
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

    // Three paths reach material_roughness() through material_textures.slang's resolved_metallic_roughness helpers.
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

// GltfLoader pins shininess to a fallback, safe only while material_roughness() is its sole shader reader.
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
        if (tonemap_source[pos] == '(') { ++depth; }
        else if (tonemap_source[pos] == ')') { --depth; }
    }
    ASSERT_EQ(depth, 0u) << "unbalanced parentheses after aces_tonemap( in " << tonemap_path.string();
    const std::string composite_expr = tonemap_source.substr(args_start, pos - 1 - args_start);

    // Split top-level '+' terms, tracking paren depth for any future nested '+'.
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

// Slang call-graph reachability: a tokenizer, not a parser, as no Slang front end is available here.

// One identifier or one of "(){}:;," found in a scan; everything else is skipped.
struct SlangToken
{
    std::string text;
    std::size_t offset = 0;// into the comment/string-stripped text this token came from
    bool is_identifier = false;
};

// Numeric literals are swallowed with their suffix, or the 'u' in "8u" would become an identifier.
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

// Blanks string contents but keeps the quotes, so offsets are unaffected.
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

// Every attribute in this corpus sits on its own line above the declaration it decorates.
bool is_attribute_line(const std::string &line)
{
    const auto first = line.find_first_not_of(" \t\r");
    return first != std::string::npos && line[first] == '[';
}

// Walks the whole attribute run above a definition, since [shader(...)] may sit under other stacked attributes.
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

// A definition is type, name, '(' at depth 0, the matching ')', an optional semantic, then '{', never ';'.
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

// Every function under Resources/ShadersSlang as one corpus, because imports cross files.
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

// A justified exception; its marker must trail the definition line, so it cannot rot after the line moves.
struct UnreachableSlangAllowlistEntry
{
    std::string file;// relative to Resources/ShadersSlang/, forward slashes
    std::string marker;
};

const std::vector<UnreachableSlangAllowlistEntry> kUnreachableSlangAllowlist = {};

const std::string kUnreachableSlangMarkerPrefix = "UNREACHABLE_SLANG_FUNCTION_OK: ";

}// namespace

// Every function must be reachable by name from an entry point; an unreachable one is a lost call site: rewire it.
TEST(BuildIntegrity, EverySlangFunctionIsReachableFromAnEntryPoint)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    ASSERT_TRUE(fs::exists(slang_root)) << "missing " << slang_root.string();

    const std::vector<SlangFunctionDef> functions = collect_slang_functions(slang_root);

    // Floors far below the real counts, so a scan that silently finds nothing cannot pass.
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

// Transitive imports of `entry_relative`; wgslMap sources import only from common/, which is looked up recursively.
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

// Names reachable from `entry_relative`'s entry points within `file_set`: its WGSL holds only its own call graph.
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

// The CI backstop mtimes cannot be: each reachable function must appear in the WGSL, helpers as `fn <name>_<digits>(`.
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

// Struct definitions too, because a module can be imported for a struct alone.
struct SlangStructDef
{
    std::string name;
    std::string relative_file;// relative to Resources/ShadersSlang, forward slashes
};

// `struct` always precedes the type name, so no brace or paren bookkeeping is needed.
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

// Forward-slashed relative paths of every non-generated .slang file under `slang_root`.
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

// The inverse of the manifest-driven checks: every source with an entry point needs an enabled manifest row.
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

// Generated WGSL is a one-way output, so no Slang source may claim to mirror it.
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

// Module name from "<prefix><name>;", or empty if the prefix or the ';' is missing.
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

// Every `export module <name>;` under `src_root`; no partitions, at most one module per file.
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

// Module name -> the .cpp/.ixx files that import or re-export it.
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

// Every .ixx is globbed into the build and rescanned each configure, so one nothing imports is dead weight.
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

    // A rootless module needs a written reason here; none today.
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

// A justified exception; its marker must trail the exempted line, so it cannot rot after the line moves.
struct AllowlistEntry
{
    std::string file;
    std::string marker;
};

// Empty: an explicit ".result" check near a ".value" read already satisfies the gate.
const std::vector<AllowlistEntry> kCheckedResultAllowlist = {};

const std::string kUncheckedResultMarkerPrefix = "UNCHECKED_VULKAN_RESULT_OK: ";

// Matched by marker text, not line number, so an edit above an exempted line cannot shift the exemption.
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

// A create or allocate keyword at a word start, then an uppercase letter (camelCase, not snake_case), then '('.
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

// The same for query verbs, which return vk::ResultValue too but slip past the creation matcher.
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

// Exceptions are off, so each ".value" read needs an ASSERT_VULKAN in a window on both sides (else-branches sit below).
TEST(BuildIntegrity, VulkanCreationResultsAreChecked)
{
    // Prove the matcher fires first, so a neutered one cannot report zero violations forever.
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
                // Queries may check ".result" explicitly, but never on the ".value" line itself.
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

// The one non-call "beginCommandBuffer(" under Src/ is its own definition.
bool is_begin_command_buffer_definition_line(const std::string &line)
{
    return line.find("beginCommandBuffer(vk::Device device") != std::string::npos;
}

// The name declared as "vk::CommandBuffer <name>" on the call line or up to `lookback` lines above, else empty.
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

// A null command buffer is beginCommandBuffer's only failure signal, so each result needs a null check.
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

// Each submit result is assigned, tested, or explicitly discarded with static_cast<void>, so a failure is never invisible.
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

// A checked null is not a handled failure: createTLAS must not index a short BLAS vector, nor clouds hand out null.
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

// uploadRgba must consume the submit result, or a failed upload reports success with an unwritten image bound.
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

// Buffer and cubemap uploads must surface a failed submit, or shaders read undefined contents as geometry.
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

// A post-acquire early return must go through an abort helper, or the next acquire reuses a signaled semaphore.
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

// Counts typed into the doc's prose outside the marker: the second copy that drifts while the marker stays right.
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

// Pins docs/path-tracing.md's pt-goldens marker to the PathTracing and Raytraced goldens the suite defines.
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

// std::nullopt if the max-texture-count marker or its value is missing, so a deleted marker fails.
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

// Plain file I/O, not an include: the point is to catch the header changing under the doc.
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

// Pins docs/model-loading.md's max-texture-count marker to the header constant.
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

// Cite symbols, not line numbers, which silently point at unrelated code once a function moves; bare colons rot too.
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

// Every ObjMaterial member needs a row in the doc's material table; this list fails alongside ObjMaterial_natural's.
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

    // A row pair shares one table row, so "_row1" counts in full or as the shorthand on its row0 line.
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

// The hand-summarised srgb row must name every map_... directive the *TextureID rows name.
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
                cell = first == std::string::npos ? ""
                                                   : cell.substr(first, cell.find_last_not_of(" \t") - first + 1);
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

// Every known C++/Rust glTF loader divergence needs a doc row; presence only, not accuracy.
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

// std::nullopt if the format-drift-denominator marker or its value is missing, so a deleted marker fails.
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

// Counts what Get-ProjectCppFiles tracks, skipping build* directories like its git-less fallback.
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

// Pins only the drift figure's denominator to a live count; the numerator needs clang-format and a human.
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

// The shader-targets marker table; std::nullopt without the markers, so an empty map cannot match vacuously.
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

// Each source's documented target must match shader-manifest.json; dual-emit tests/ and hand-written histogram are out.
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

// The shared-module table must equal the targets recomputed from the import graph, in both directions.
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

// The first [numthreads(X, Y, Z)]; std::nullopt when absent, so a renamed attribute cannot match zero times.
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

// Dispatch constants have no compiler link to [numthreads]; a mismatch leaves most of the noise volume undefined.
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

// [min, max] of the num_march_steps clamp; std::nullopt when the expression is not found.
std::optional<std::pair<float, float>> parse_cloud_march_steps_range(const std::string &contents)
{
    static const std::regex kPattern(
      R"(num_march_steps\s*=\s*int\(\s*clamp\(\s*scene\.cloudParameters\.w\s*,\s*([0-9.]+)\s*,\s*([0-9.]+)\s*\)\s*\))");
    std::smatch match;
    if (!std::regex_search(contents, match, kPattern)) { return std::nullopt; }
    return std::make_pair(std::stof(match[1].str()), std::stof(match[2].str()));
}

// [min, max] of the num_march_steps_to_light clamp.
std::optional<std::pair<float, float>> parse_cloud_light_march_steps_range(const std::string &contents)
{
    static const std::regex kPattern(R"(num_march_steps_to_light\s*=\s*int\(\s*clamp\(\s*scene\.cloudLightMarch\.x\s*,)"
                                      R"(\s*([0-9.]+)\s*,\s*([0-9.]+)\s*\)\s*\))");
    std::smatch match;
    if (!std::regex_search(contents, match, kPattern)) { return std::nullopt; }
    return std::make_pair(std::stof(match[1].str()), std::stof(match[2].str()));
}

// The shader's defensive clamps must match CloudDispatch.hpp, which the GUI slider and host packer also use.
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

// Operands of the noiseVolume float4 write, split on top-level commas; std::nullopt when absent.
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

// Body of the first function whose declaration contains `signature_needle`.
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

// The noise must span the whole volume and fill every channel sample_density reads; neither gap is compiler-visible.
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

    // (b) No float4() operand may be a bare numeric literal, a hard-coded channel.
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

    // sample_density must read exactly r, g, b and a, so a changed consumer fails instead of checking a stale set.
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

// The eExclusive noise image belongs to the graphics family, so it is written there, with no compute queue or pool.
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

// abs(fmod(x, N)) maps -x and +x onto one texel, a mirror; frac() wraps negative positions correctly.
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

// Density scales by a constant step length, not distance travelled, or the quality slider becomes a density slider.
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

// Text pins with no numerical oracle: the phase denominator's sign, and powder kept off transmittance.
TEST(BuildIntegrity, CloudScatteringKeepsItsPhaseSignAndItsMonotonicTransmittance)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path clouds_path = slangRoot() / "compute" / "clouds.slang";
    const auto contentsOpt = readFileText(clouds_path);
    ASSERT_TRUE(contentsOpt.has_value()) << "missing " << clouds_path.string();
    const std::string &contents = *contentsOpt;

    // Positive g peaks toward the sun (cosTheta = +1) only if 2*g*cosTheta is subtracted.
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

    // Powder belongs on lightEnergy, never in a transmittance assignment.
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

    // In the march loop transmittance only decreases, through the Beer-Lambert `*= exp(-...)`.
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

// Slang has no SPIR-V inverse(), so clouds_main forms the box inverse itself; no dead forward matrix, no CPU claim.
TEST(BuildIntegrity, CloudBoxInverseIsFormedInTheShaderNotOnTheHost)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path clouds_path = slangRoot() / "compute" / "clouds.slang";
    const auto clouds_contents_opt = readFileText(clouds_path);
    ASSERT_TRUE(clouds_contents_opt.has_value()) << "missing " << clouds_path.string();
    const std::string &clouds_contents = *clouds_contents_opt;

    // ECMAScript regex has no lookbehind, so compare counts: every "model_to_world" must be an "inv_model_to_world".
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

// The host-to-shader cloud UBO pairs, shared by the unpack test and the doc-table test so their truths cannot diverge.
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

// Offsets are pinned elsewhere, but a component swap is not; each host component must land in its shader field.
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

// Owning counterpart of CloudUboFieldPair, whose const char* fields may only point at literals.
struct ParsedCloudUboRow
{
    std::string cloud_field;
    std::string scene_field;
    std::string component;
};

// docs/clouds.md's cloud-ubo marker table; std::nullopt without the markers.
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

// docs/clouds.md's cloud-constants marker table; std::nullopt without the markers.
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

// Both hand-maintained clouds.md tables must match their sources, kCloudUboFieldPairs and the compiled constants.
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

// The dispatch constants have no compiler link to [numthreads]; a mismatch under-covers the image every frame.
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

// A barrier into eShaderReadOnlyOptimal must name the stage that samples the image, or the real hazard goes unguarded.
TEST(BuildIntegrity, OffscreenImageBarriersNameTheStageThatConsumesThem)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::array<const char *, 2> kFiles = {
        "Src/GraphicsEngineVulkan/renderer/PathTracing.cpp",
        "Src/GraphicsEngineVulkan/renderer/Raytracing.cpp",
    };

    // Barriers come from buildImageMemoryBarrier, whose third argument is newLayout.
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

// A stray control byte makes grep treat the whole file as binary and skip it; UTF-8 bytes and BOMs are fine.
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

// The compiled SPIR-V is the layout truth: each contracted member's Offset must equal the host offsetof().
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

// Only C++ compiles the shared headers now, so the GLSL dual-compile shim (`#ifdef __cplusplus`, KTG_VEC*) is dead.
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

// The text between <decl>'s opening brace and its matching close, or nothing when either is missing.
std::optional<std::string> cpp_struct_body(const std::string &text, const std::string &decl)
{
    const std::size_t struct_pos = text.find(decl);
    if (struct_pos == std::string::npos) { return std::nullopt; }
    const std::size_t open_brace = text.find('{', struct_pos);
    if (open_brace == std::string::npos) { return std::nullopt; }
    int depth = 1;
    for (std::size_t pos = open_brace + 1; pos < text.size(); ++pos) {
        if (text[pos] == '{') {
            ++depth;
        } else if (text[pos] == '}' && --depth == 0) {
            return text.substr(open_brace + 1, pos - open_brace - 1);
        }
    }
    return std::nullopt;
}

// Declared member names of a plain C++ struct: the identifier before its initializer, array bound or semicolon.
std::vector<std::string> parse_cpp_struct_member_names(const fs::path &header_path, const std::string &decl)
{
    const auto struct_body = cpp_struct_body(readFileText(header_path).value_or(std::string{}), decl);
    if (!struct_body) { return {}; }
    // A type of words, scopes, templates and pointers first: a static_assert or its continuation never matches.
    static const std::regex kMemberDecl(R"(^\s*[\w:<>,\s\*&]+?\s+(\w+)\s*(=|;|\{|\[))");
    std::vector<std::string> names;
    std::istringstream body_stream(*struct_body);
    std::string line;
    while (std::getline(body_stream, line)) {
        const auto comment_pos = line.find("//");
        if (comment_pos != std::string::npos) { line.resize(comment_pos); }
        std::smatch match;
        if (std::regex_search(line, match, kMemberDecl)) { names.push_back(match[1].str()); }
    }
    return names;
}

// Member names from SceneUBO's C++ text, as an unread member never reaches SPIR-V; _pad filler is skipped.
std::vector<std::string> parse_scene_ubo_member_names(const fs::path &header_path)
{
    const auto struct_body = cpp_struct_body(readFileText(header_path).value_or(std::string{}), "struct SceneUBO");
    if (!struct_body) { return {}; }
    const std::string &body = *struct_body;
    const std::regex array_suffix(R"(\[[^\]]*\])");
    // A declaration ends in a bare identifier; anything else, like a wrapped static_assert message, is skipped.
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

// Member names of scene_types.slang's scalar-layout ObjMaterial mirror, brace-matched like the SceneUBO parser.
std::vector<std::string> parse_obj_material_member_names(const fs::path &scene_types_path)
{
    const auto struct_body = cpp_struct_body(readFileText(scene_types_path).value_or(std::string{}), "struct ObjMaterial");
    if (!struct_body) { return {}; }
    const std::string &body = *struct_body;
    static const std::regex kMemberDecl(R"(\b(?:float3|float|int)\s+([A-Za-z_]\w*)\s*;)");

    std::vector<std::string> names;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), kMemberDecl); it != std::sregex_iterator(); ++it) {
        names.push_back((*it)[1].str());
    }
    return names;
}

}// namespace

// Every SceneUBO member, uploaded per image every frame, must be read by some shader.
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

// Every ObjMaterial member must be read as `material.<name>` somewhere, or it only costs bytes.
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

// A .w that packs real data rather than a literal filler must be read as `<field>.w` by some shader.
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

// With exceptions off, throwing filesystem overloads terminate on any OS error; pass an error_code or mark NO_EC_OK.
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

    // A substring search for this codebase's error_code naming idioms, not a parser.
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

// Descriptor triads go through DescriptorSetGroup; the allowlist is anchored on file paths, not line numbers.
TEST(BuildIntegrity, DescriptorSetsAreCreatedThroughDescriptorSetGroup)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const std::array<const char *, 3> kRawDescriptorTypeNames = {
        "vk::DescriptorSetLayoutCreateInfo", "vk::DescriptorPoolCreateInfo", "vk::DescriptorSetAllocateInfo"
    };

    // The class's own file is exempt outright; GUI.cpp's one ImGui pool gets a budget, so a second triad still fails.
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

// AS rebuilds go through refreshAfterSceneChange, or descriptors keep the destroyed TLAS (VUID-vkCmdDispatch-None-08114).
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
                        // Signature and brace share this line; the brace counting below clears it when the body closes.
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

// The renderer must call every stage's shaderHotReload, or reload silently skips it; counted, not hand-mapped.
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

// Every SPIR-V loading subsystem needs shaderHotReload; found by its SPIR-V directory, not a hand-kept file list.
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

// Hot reload must destroy the old pipeline layout before recreating it, or each reload leaks one.
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

// Shader group handles are valid only for their own pipeline, so a hot reload must rebuild the SBT too.
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

// Compute pipelines are built only by createComputePipeline.
TEST(BuildIntegrity, ComputePipelinesAreCreatedThroughTheSharedHelper)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const char *const kRawComputePipelineTypeName = "vk::ComputePipelineCreateInfo";

    // The pure builder and the device-side helper's own implementation.
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

// Stages and shader groups go through the shared builders: a wrong pName or UNUSED_KHR sentinel compiles fine.
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
    // Local declarations only: span or vector parameters of the type construct nothing, so no '<' may precede it.
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

// Pipelines and their layouts are destroyed only through destroyPipelineAndLayout.
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

// Depth attachments come from createDepthAttachment; the shadow array is exempt by its line marker, not by file.
TEST(BuildIntegrity, NoStageHandRollsTheDepthAttachmentChain)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    // The image-usage spelling only; the access, layout and format-feature spellings are unrelated.
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

// Raster stages share buildExternalColorDepthDependency; SkyBox and CascadedShadowMap keep a genuinely different edge.
TEST(BuildIntegrity, NoRasterStageHandRollsItsExternalSubpassDependency)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const char *const kExternalAssign = "srcSubpass = VK_SUBPASS_EXTERNAL";
    static const char *const kStageAssign = "srcStageMask =";
    // The helper's own definition and the two passes with a genuinely different dependency.
    static const std::set<std::string> kAllowedFiles = {
        "Src/GraphicsEngineVulkan/common/RenderPassHelper.hpp",
        "Src/GraphicsEngineVulkan/scene/sky_box/SkyBox.cpp",
        "Src/GraphicsEngineVulkan/scene/light/directional_light/CascadedShadowMap.cpp"
    };
    // Hand-rolled sites set srcStageMask within a couple of lines of srcSubpass.
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

// Plain colour attachments come from createColorAttachment; other views are exempt only by their line marker.
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

// The framebuffer reuses the shadow array's sampled view instead of a byte-identical second one.
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

// Framebuffers are destroyed only through destroyFramebuffers and destroyFramebuffer.
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

// Render passes are destroyed only through destroyRenderPass.
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

// recreateSwapChain() destroys stage framebuffers first; a missed one leaks per resize, seen only at vkDestroyDevice.
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

    // Blank out comment lines, offsets kept, so a commented-out call cannot satisfy the checks.
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

    // Receiver -> the file defining its destroyFramebuffers(); an unmapped receiver fails, so no stage slips in.
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

// createImage must record mip_levels itself, or getMipLevel() and maxLod disagree; text-checked, as it needs a device.
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

// Feature bits come from availability queries; only the proven hardware-RT block may hardcode true, or startup fails.
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

// A single light-matrix UBO races in-flight passes invisibly to goldens and validation, so the source shape is pinned.
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

// An image-count change must reinit the shadow map too, after initDescriptorResources() recreates the layout it caches.
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

// Texture slots, meshes and upload preconditions go through ModelAssembly.ixx, so the loaders cannot drift apart.
TEST(BuildIntegrity, ModelUploadGoesThroughTheSharedAssembly)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    // The wrapped functions' own definitions, and the two loaders as the shared guard's intended callers.
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

// Only the renderer owns a command pool, and the shadow map seeds its host-visible buffers without staging.
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

// Each depth-owning stage assigns depth_format once, so the render-pass format and depth image cannot diverge.
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

// Image barriers come from buildImageMemoryBarrier; only empty-brace default construction is flagged, not the type.
TEST(BuildIntegrity, ImageMemoryBarriersGoThroughTheSharedHelper)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    // Per-file budgets rather than exemptions; every unlisted file has a budget of 0.
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

// RT and compute also sample the mips, so an eFragmentShader literal in Texture.cpp would narrow the stage.
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

// The log must not ask for the swapchain barrier removal the source has already shipped.
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

// An inline function naming a static header function is IFNDR; column 0 tells free functions from static members.
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

// Every class with cleanUp() calls it from its destructor, so a skipped explicit call still tears down.
TEST(BuildIntegrity, EveryCleanUpIsCalledFromItsDestructor)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    // Mesh and VulkanBufferManager are fully RAII; VulkanRenderer owns the device and instance teardown order.
    static const std::array<const char *, 4> kExemptClasses = {
        "Mesh", "VulkanBufferManager", "VulkanDevice", "VulkanInstance"
    };

    // Anchored at line start, so prose mentioning a class cannot pass for a declaration.
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

        // Matches "~Name() { cleanUp(); }" inline or out-of-line, possibly namespace-qualified.
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

// App::run() derives its exit code via appExitCode, or a lost device reads as a clean quit.
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

// By value costs two atomic refcount ops per call, so non-sinks take const &; sinks carry a DEVICE_SINK_OK marker.
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

// abseil is the one CLI front end, so no dead hand-rolled parser can come back.
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

// No suite may regrow its own repo-root or file-slurp helper; the banned signatures are concatenated so this file passes.
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

// The docs must name this repository, not its old slug: nothing else gates prose after a rename.
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

// maxAnisotropy comes from resolveMaxAnisotropy (VUID-VkSamplerCreateInfo-anisotropyEnable-01071); features are queried in VulkanDevice.cpp only.
TEST(BuildIntegrity, NoSamplerHardCodesItsMaxAnisotropy)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src" / "GraphicsEngineVulkan";
    ASSERT_TRUE(fs::exists(src_root)) << "missing " << src_root.string();

    static const std::string kSignature = "buildSamplerCreateInfo(";
    // A literal above 1; "1.0F" or "1.0f" is the disabled-anisotropy sentinel.
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

            // Balanced parens, not a lazy regex, which would run past a declaration's parameter list into unrelated code.
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

            // Split on top-level commas only: maxAnisotropy is itself a nested call with its own comma.
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

// ASSERT_VULKAN is a do/while(false) statement, so every call site ends in ';'; the compiler is the primary check.
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

            // Track paren depth across lines: a paren-free argument line is not the call's closing line.
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

// glTF 2.0 3.9.4: double-sided back faces flip their normal; with culling off for them, SV_IsFrontFace detects them.
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

// Model owns the transform; a per-mesh matrix would be a second truth that no draw path reads.
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

// cleanUp() and recreateFrameResources() share releaseFrameTextures(), so their teardowns cannot drift apart.
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

// gltfTextureSlots() lists the four slots once, so per-slot guards and calls cannot multiply again.
TEST(BuildIntegrity, GltfTextureSlotsAreEnumeratedInOneTable)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path path = repo_root / "Src" / "GraphicsEngineVulkan" / "scene" / "GltfLoader.cpp";
    const auto lines_opt = readFileLines(path);
    ASSERT_TRUE(lines_opt.has_value()) << "missing " << path.string();

    // Comments stripped, since doc comments name the guard in prose.
    std::string text;
    for (const auto &raw_line : *lines_opt) {
        text += strip_line_comment(raw_line);
        text += '\n';
    }

    const auto slots_span = function_body_span(text, "gltfTextureSlots");
    ASSERT_TRUE(slots_span.has_value())
      << "gltfTextureSlots not found in " << path.string() << " - was the table rewrite reverted?";

    // Everything outside gltfTextureSlots' body, anchored on the function rather than a line number.
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

    // "materialName" right after the paren selects calls, since the free functions' own signatures match too.
    EXPECT_LE(count_occurrences(outside, "readUvTransform(materialName"), 1U)
      << "readUvTransform must be called from one loop over gltfTextureSlots' table, not once per texture slot";
    EXPECT_LE(count_occurrences(outside, "warnUnsupportedTexCoordSet(materialName"), 1U)
      << "warnUnsupportedTexCoordSet must be called from one loop over gltfTextureSlots' table, not once per "
         "texture slot";
    // assignTextureSlot is a lambda, so every "assignTextureSlot(" is a call.
    EXPECT_LE(count_occurrences(outside, "assignTextureSlot("), 1U)
      << "assignTextureSlot must be called from one loop over gltfTextureSlots' table, not once per texture slot";
}

// A false submit means nothing ran, so a discard treats failure as success; only a SUBMIT_RESULT_IGNORED_OK marker allows one.
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

// TMin, TMax and the sky gradient assume a unit bounce direction (see docs/path-tracing.md's Bounce bullet).
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

// Per shading path, since a field dropped by one path still passes the any-shader check; reads may go through helpers.
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
                cell = first == std::string::npos ? ""
                                                   : cell.substr(first, cell.find_last_not_of(" \t") - first + 1);
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

    // One source blob per column, in order; shadow_map and alpha_test share one, both alpha-only consumers.
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

    // Helpers and the fields they read, so naming a helper counts; substring matching covers the _lod0 forms.
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

// The post pass loads its attachment, so a clear value is dead; fs_main samples noisyTxt exactly once.
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
    // The empty-span idiom still names the type; only a populated clear-value container is dead.
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

// Run names per BENCHMARK: the bare NAME, or "NAME/N" per ->Arg(N); other chained calls leave the name alone.
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

// The baseline must cover every registered run name, since Compare-PerfBaseline.ps1 never fails on a missing one.
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

namespace {

// GUI.cpp must read or write every member of a GUI shared-vars struct, or the member has a reason in <exempt>.
void expect_every_member_has_a_gui_control(const std::string &struct_name,
  const std::string &header_rel,
  const std::string &gui_prefix,
  std::size_t min_members,
  const std::map<std::string, std::string> &exempt)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path header_path = repo_root / fs::path(header_rel);
    const fs::path gui_path = repo_root / "Src" / "GraphicsEngineVulkan" / "gui" / "GUI.cpp";
    const auto gui_text = readFileText(gui_path);
    ASSERT_TRUE(gui_text.has_value()) << "could not open " << gui_path.string();

    const std::vector<std::string> members = parse_cpp_struct_member_names(header_path, "struct " + struct_name);
    // A regex that silently matches nothing would pass every member below.
    ASSERT_GE(members.size(), min_members) << "parsed only " << members.size() << " member(s) of " << struct_name
                                           << " from " << header_path.string() << " - the parse broke, not the GUI";

    std::vector<std::string> missing;
    for (const auto &member : members) {
        if (exempt.contains(member)) { continue; }
        if (gui_text->find(gui_prefix + member) == std::string::npos) { missing.push_back(member); }
    }
    std::vector<std::string> stale;
    for (const auto &[name, reason] : exempt) {
        if (std::find(members.begin(), members.end(), name) == members.end()) { stale.push_back(name + " (" + reason + ")"); }
    }

    EXPECT_TRUE(missing.empty()) << gui_path.string() << " does not read/write the following " << struct_name
                                 << " member(s) - add a control, or an exemption with its reason in this test:"
                                 << joinViolations(missing);
    EXPECT_TRUE(stale.empty()) << "exempted member(s) no longer in " << struct_name << " - delete the exemption:"
                               << joinViolations(stale);
}

}// namespace

// Every tunable scene var needs a GUI control; the member list is the header's, so a new member is checked on arrival.
TEST(BuildIntegrity, EveryTunableGuiSceneVarHasAControl)
{
    const std::map<std::string, std::string> kExempt{
        { "shadow_resolution_changed", "a latch the resolution combo raises for the renderer, not a tunable" },
        { "model_reload_requested", "a latch the model picker raises, not a tunable" },
        { "model_transform_changed", "a latch the transform controls raise, not a tunable" },
        { "selected_model_index", "the model picker's selection, not a tunable" },
        { "available_shadow_map_resolutions", "the resolution combo's labels, a constant table" },
    };
    expect_every_member_has_a_gui_control("GUISceneSharedVars",
      "Src/GraphicsEngineVulkan/scene/GUISceneSharedVars.ixx",
      "guiSceneSharedVars.",
      20,
      kExempt);
}

// The renderer's shared vars had no gate; all of them are read or written by the GUI today, and this keeps it so.
TEST(BuildIntegrity, EveryGuiRendererSharedVarHasAControl)
{
    expect_every_member_has_a_gui_control("GUIRendererSharedVars",
      "Src/GraphicsEngineVulkan/renderer/GUIRendererSharedVars.ixx",
      "guiRendererSharedVars.",
      8,
      {});
}

// The key-bindings panel is a hand-written literal, so every bound key and button must map to prose in it.
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

    // Bound inputs deliberately not user-facing, each with a reason; none today.
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

// Counts TEST, TEST_F and TEST_P definition lines across the commit suite's .cpp files.
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

// The log's quoted commit-suite test count must be current: churny by design, and a missing marker fails.
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

// Read-only accessors are const; mutable-reference returns are out of scope, pointer returns are not.
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
    // Accessors the compiler proved cannot become const without a return-type change.
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

            // Free functions sit at column 0; class member accessors are indented.
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

// Violation lists go through joinViolations; a flat budget of 0, as this file never needs its own join.
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
