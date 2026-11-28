// BuildIntegrityScan.hpp's function bodies; their caches are function-local statics, one per process.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BuildIntegrityScan.hpp"

namespace Kataglyphis::TestSupport::BuildIntegrity {

std::string strip_line_comment(const std::string &line)
{
    const auto comment_pos = line.find("//");
    return comment_pos == std::string::npos ? line : line.substr(0, comment_pos);
}

void collect_import_closure(const fs::path &slang_root,
  const fs::path &source,
  std::set<fs::path> &visited,
  std::set<fs::path> &closure)
{
    if (visited.contains(source)) { return; }// cycle guard
    visited.insert(source);

    const auto lines = readFileLines(source);
    if (!lines) { return; }

    static const std::regex import_pattern(R"(^\s*(?:__exported\s+)?import\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)");
    // A *_uniform_index twin compiles its base through the preprocessor, so the base and its imports are the twin's.
    static const std::regex include_pattern(R"re(^\s*#include\s+"([A-Za-z0-9_.]+\.slang)")re");
    for (const auto &raw_line : *lines) {
        const std::string line = strip_line_comment(raw_line);
        std::smatch match;
        const bool included = std::regex_search(line, match, include_pattern);
        if (!included && !std::regex_search(line, match, import_pattern)) { continue; }

        const std::string identifier = match[1].str();
        std::error_code error;
        fs::path resolved =
          included ? source.parent_path() / identifier : slang_root / "common" / (identifier + ".slang");
        if (!included && !sourceExists(resolved, error)) { resolved = source.parent_path() / (identifier + ".slang"); }
        if (!sourceExists(resolved, error)) { continue; }// unresolvable import: the compiler's problem, not this gate's

        resolved = shaderSourcePath(resolved);
        closure.insert(resolved);
        collect_import_closure(slang_root, resolved, visited, closure);
    }
}

std::set<fs::path> import_closure(const fs::path &slang_root, const fs::path &source)
{
    std::set<fs::path> visited;
    std::set<fs::path> closure;
    collect_import_closure(slang_root, source, visited, closure);
    closure.erase(source);
    return closure;
}

bool newest_import_for(const fs::path &slang_root,
  const fs::path &source,
  fs::file_time_type &out_time,
  fs::path &out_path)
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

std::optional<ShaderManifestData> parse_shader_manifest(const fs::path &manifest_path)
{
    std::ifstream file(manifest_path);
    if (!file) { return std::nullopt; }

    nlohmann::json doc = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) { return std::nullopt; }
    if (doc.contains("wgslManifest")) {
        const auto owner_text = readFileText(slangRoot() / doc["wgslManifest"].get<std::string>());
        if (!owner_text) { return std::nullopt; }
        const auto owner = nlohmann::json::parse(*owner_text, nullptr, false);
        if (owner.is_discarded()) { return std::nullopt; }
        for (const auto &row : owner["manifest"]) { doc["manifest"].push_back(row); }
        doc["wgslMap"] = owner["wgslMap"];
        for (auto &row : doc["wgslMap"]) {
            row["dst"] = doc["wgslDestinationPrefix"].get<std::string>() + "/" + row["dst"].get<std::string>();
        }
        doc["depthTexturePatches"] = owner["depthTexturePatches"];
        doc["minSlangcVersionForWgsl"] = owner["minSlangcVersionForWgsl"];
    }


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
        const auto entry_it = row.find("entry");
        if (entry_it == row.end() || !entry_it->is_string()) { return std::nullopt; }
        data.spirv_entries.emplace(source, entry_it->get<std::string>());
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

const std::optional<ShaderManifestData> &shader_manifest()
{
    static const std::optional<ShaderManifestData> cached = parse_shader_manifest(slangRoot() / "shader-manifest.json");
    return cached;
}

bool is_identifier_char(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_'; }

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

std::vector<fs::path> shared_constant_files()
{ return { slangRoot() / "common" / "shared_constants.slang", sharedSlangRoot() / "common" / "limits.slang" }; }

std::map<std::string, int> parse_shared_constants()
{
    std::map<std::string, int> merged;
    for (const auto &path : shared_constant_files()) { merged.merge(parse_int_constants(path)); }
    return merged;
}

std::optional<std::pair<std::size_t, std::size_t>> function_body_span(const std::string &text,
  const std::string &qualified_name)
{
    const std::string signature = qualified_name + "(";
    const std::size_t sig_pos = text.find(signature);
    if (sig_pos == std::string::npos) { return std::nullopt; }

    std::size_t pos = sig_pos + signature.size();
    int paren_depth = 1;
    while (pos < text.size() && paren_depth > 0) {
        if (text[pos] == '(') {
            ++paren_depth;
        } else if (text[pos] == ')') {
            --paren_depth;
        }
        ++pos;
    }
    if (paren_depth != 0) { return std::nullopt; }

    const std::size_t brace_pos = text.find('{', pos);
    if (brace_pos == std::string::npos) { return std::nullopt; }

    std::size_t end_pos = brace_pos + 1;
    int brace_depth = 1;
    while (end_pos < text.size() && brace_depth > 0) {
        if (text[end_pos] == '{') {
            ++brace_depth;
        } else if (text[end_pos] == '}') {
            --brace_depth;
        }
        ++end_pos;
    }
    if (brace_depth != 0) { return std::nullopt; }
    return std::make_pair(brace_pos + 1, end_pos - 1);
}

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
            while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) != 0 || text[i] == '.')) {
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

bool is_blank_line(const std::string &line) { return line.find_first_not_of(" \t\r") == std::string::npos; }

bool is_attribute_line(const std::string &line)
{
    const auto first = line.find_first_not_of(" \t\r");
    return first != std::string::npos && line[first] == '[';
}

bool preceded_by_attribute(const std::vector<std::string> &stripped_lines,
  std::size_t def_line_index,
  std::string_view attribute)
{
    std::size_t i = def_line_index;
    while (i > 0) {
        --i;
        const std::string &line = stripped_lines[i];
        if (is_blank_line(line)) { continue; }
        if (!is_attribute_line(line)) { break; }
        if (line.find(attribute) != std::string::npos) { return true; }
    }
    return false;
}

bool preceded_by_shader_attribute(const std::vector<std::string> &stripped_lines, std::size_t def_line_index)
{ return preceded_by_attribute(stripped_lines, def_line_index, "[shader("); }

void collect_functions_from_file(const std::vector<std::string> &stripped_lines,
  const std::vector<std::string> &raw_lines,
  const std::string &relative_file,
  std::vector<SlangFunctionDef> &out)
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
    // A struct's methods are call sites too: a generic reaches them by name, through its interface.
    std::string pending_struct;
    std::string current_struct;
    std::size_t t = 0;
    while (t < tokens.size()) {
        const SlangToken &tok = tokens[t];
        if (!tok.is_identifier) {
            if (tok.text == "{") {
                ++depth;
                if (depth == 1 && !pending_struct.empty()) { current_struct = std::exchange(pending_struct, {}); }
            } else if (tok.text == "}") {
                --depth;
                if (depth == 0) { current_struct.clear(); }
            } else if (tok.text == ";" && depth == 0) {
                pending_struct.clear();
            }
            ++t;
            continue;
        }
        if (depth == 0 && tok.text == "struct" && t + 1 < tokens.size() && tokens[t + 1].is_identifier) {
            pending_struct = tokens[t + 1].text;
            t += 2;
            continue;
        }
        const bool definition_scope = depth == 0 || (depth == 1 && !current_struct.empty());

        // A generic's `<T : I>` stands between its name and its parameters; the tokens drop the brackets.
        std::size_t open = t + 2;
        if (definition_scope && open < tokens.size() && tokens[t + 1].is_identifier) {
            std::size_t after_name = tokens[t + 1].offset + tokens[t + 1].text.size();
            while (after_name < text.size() && std::isspace(static_cast<unsigned char>(text[after_name])) != 0) {
                ++after_name;
            }
            if (after_name < text.size() && text[after_name] == '<') {
                const std::size_t close = text.find('>', after_name);
                while (close != std::string::npos && open < tokens.size() && tokens[open].offset < close) { ++open; }
            }
        }
        if (definition_scope && open < tokens.size() && tokens[t + 1].is_identifier && !tokens[open].is_identifier
            && tokens[open].text == "(") {
            // Find the parameter list's matching ')'.
            int paren_depth = 1;
            std::size_t j = open + 1;
            while (j < tokens.size() && paren_depth > 0) {
                if (!tokens[j].is_identifier && tokens[j].text == "(") {
                    ++paren_depth;
                } else if (!tokens[j].is_identifier && tokens[j].text == ")") {
                    --paren_depth;
                }
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
                        if (!tokens[m].is_identifier && tokens[m].text == "{") {
                            ++body_depth;
                        } else if (!tokens[m].is_identifier && tokens[m].text == "}") {
                            --body_depth;
                        }
                        ++m;
                    }
                    const std::size_t body_begin_offset = tokens[k].offset;
                    const std::size_t body_end_offset =
                      (m > 0 && m <= tokens.size()) ? (tokens[m - 1].offset + 1) : text.size();

                    SlangFunctionDef fn;
                    fn.name = tokens[t + 1].text;
                    fn.owner = depth == 0 ? std::string() : current_struct;
                    fn.relative_file = relative_file;
                    fn.line = line_for_offset(tok.offset);
                    fn.is_root = preceded_by_shader_attribute(stripped_lines, static_cast<std::size_t>(fn.line - 1));
                    fn.force_inline =
                      preceded_by_attribute(stripped_lines, static_cast<std::size_t>(fn.line - 1), "[ForceInline]");
                    fn.body = text.substr(body_begin_offset, body_end_offset - body_begin_offset);
                    fn.raw_def_line = (fn.line >= 1 && static_cast<std::size_t>(fn.line - 1) < raw_lines.size())
                                        ? raw_lines[static_cast<std::size_t>(fn.line - 1)]
                                        : std::string();
                    out.push_back(std::move(fn));

                    // Back to the scope the definition sat in once its own body has closed.
                    t = m;
                    continue;
                }
            }
        }
        ++t;
    }
}

std::vector<SlangFunctionDef> collect_slang_functions(const fs::path &slang_root)
{
    std::vector<SlangFunctionDef> functions;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        const fs::path &path = shader_path;
        if (!isReadableRegularFile(shader_path) || path.extension() != ".slang") { continue; }
        const std::string relative_path = shaderRelativePath(path, slang_root).generic_string();
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

std::vector<std::string> collect_all_slang_relative_paths(const fs::path &slang_root)
{
    std::vector<std::string> paths;
    std::error_code error;
    for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
        if (error) { break; }
        const fs::path &path = shader_path;
        if (!isReadableRegularFile(shader_path) || path.extension() != ".slang") { continue; }
        const std::string relative_path = shaderRelativePath(path, slang_root).generic_string();
        if (relative_path.starts_with("build/")) { continue; }
        paths.push_back(relative_path);
    }
    return paths;
}

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

fs::path sharedCloudsPath() { return sharedSlangRoot() / "common" / "volumetric_clouds.slang"; }

std::string extract_function_body(const std::string &contents, const std::string &signature_needle)
{
    const size_t sig_pos = contents.find(signature_needle);
    if (sig_pos == std::string::npos) { return {}; }
    const size_t brace_pos = contents.find('{', sig_pos);
    if (brace_pos == std::string::npos) { return {}; }

    int depth = 1;
    size_t pos = brace_pos + 1;
    for (; pos < contents.size() && depth > 0; ++pos) {
        if (contents[pos] == '{') {
            ++depth;
        } else if (contents[pos] == '}') {
            --depth;
        }
    }
    return contents.substr(brace_pos, pos - brace_pos);
}

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

std::vector<std::string> parse_obj_material_member_names(const fs::path &scene_types_path)
{
    const auto struct_body =
      cpp_struct_body(readFileText(scene_types_path).value_or(std::string{}), "struct ObjMaterial");
    if (!struct_body) { return {}; }
    const std::string &body = *struct_body;
    static const std::regex kMemberDecl(R"(\b(?:float3|float|int)\s+([A-Za-z_]\w*)\s*;)");

    std::vector<std::string> names;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), kMemberDecl); it != std::sregex_iterator(); ++it) {
        names.push_back((*it)[1].str());
    }
    return names;
}

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

}// namespace Kataglyphis::TestSupport::BuildIntegrity
