#pragma once

// The build-integrity suites' shared scanners: the shader manifest, SPIR-V, Slang tokens and doc markers.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "RepoFiles.hpp"

namespace Kataglyphis::TestSupport::BuildIntegrity {

namespace fs = std::filesystem;

using Kataglyphis::TestSupport::joinViolations;
using Kataglyphis::TestSupport::readFileLines;
using Kataglyphis::TestSupport::readFileText;
using Kataglyphis::TestSupport::repoRoot;
using Kataglyphis::TestSupport::slangRoot;
using Kataglyphis::TestSupport::spirvRoot;
using Kataglyphis::TestSupport::sharedSlangRoot;
using Kataglyphis::TestSupport::shaderSourcePath;
using Kataglyphis::TestSupport::shaderRelativePath;
using Kataglyphis::TestSupport::shaderSourceFiles;
using Kataglyphis::TestSupport::isReadableRegularFile;
using Kataglyphis::TestSupport::sourceExists;

// Strips a trailing "// ..." comment so prose naming a constant cannot pass for its definition.
std::string strip_line_comment(const std::string &line);

// Recursion helper for import_closure; `visited` makes each resolved file recurse exactly once.
void collect_import_closure(const fs::path &slang_root,
  const fs::path &source,
  std::set<fs::path> &visited,
  std::set<fs::path> &closure);

// Every .slang file `source` transitively imports, excluding itself; common/ wins over the importer's own directory.
std::set<fs::path> import_closure(const fs::path &slang_root, const fs::path &source);

// Newest mtime in the import closure and its file, so callers can name the stale import; false if the closure is empty.
bool newest_import_for(const fs::path &slang_root,
  const fs::path &source,
  fs::file_time_type &out_time,
  fs::path &out_path);

// shader-manifest.json is both Slang build scripts' single source of truth, so these tests check the files against it.

// One wgslMap row: the Slang source, the directory and the WGSL file it writes.
struct WgslMapping
{
    std::string slang_source;// "src": relative to Resources/ShadersSlang/
    std::string dst_dir;// "dst": relative to the repository root
    std::string wgsl_file;// "out": destination file name
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
std::optional<ShaderManifestData> parse_shader_manifest(const fs::path &manifest_path);

// Parsed once per process; std::nullopt means missing or malformed, so callers ASSERT on has_value(), never skip.
const std::optional<ShaderManifestData> &shader_manifest();

// Limits and binding slots of shared_constants.slang and OxidANT's limits.slang, which C++ and the shaders compile.
inline const std::vector<std::string> kSharedConstantNames = { "MAX_TEXTURE_COUNT",
    "CASCADE_COUNT",
    "MAX_PCF_RADIUS",
    "globalUBO_BINDING",
    "sceneUBO_BINDING",
    "OBJECT_DESCRIPTION_BINDING",
    "TEXTURES_BINDING",
    "SAMPLER_BINDING",
    "SHADOW_MAP_BINDING",
    "PUNCTUAL_LIGHTS_BINDING",
    "PUNCTUAL_SHADOW_MAP_BINDING",
    "TLAS_BINDING",
    "OUT_IMAGE_BINDING",
    "ACCUMULATION_IMAGE_BINDING",
    "TRACED_DEPTH_IMAGE_BINDING",
    "SKYBOX_CUBEMAP_BINDING",
    "GBUFFER_NORMAL_BINDING",
    "GBUFFER_ALBEDO_BINDING",
    "GBUFFER_MATERIAL_BINDING",
    "GBUFFER_DEPTH_BINDING" };

bool is_identifier_char(char ch);

// The integer after a constant name, for both "#define NAME 3" and "[static] const int NAME = 3;".
std::optional<int> parse_int_after(const std::string &line, std::size_t name_end);

// Whole-word matches only, so a longer identifier containing a constant's name is ignored.
std::map<std::string, int> parse_int_constants(const fs::path &path);

// The files host_device_shared_vars.hpp includes, in its include order.
std::vector<fs::path> shared_constant_files();

// kSharedConstantNames as both shared files declare them.
std::map<std::string, int> parse_shared_constants();

// SPIR-V parsing; the magic is re-declared because ShaderHelper.cpp's copy is module-private.
constexpr uint32_t kSpirvMagicNumber = 0x07230203;
constexpr std::size_t kSpirvHeaderWordCount = 5;

constexpr uint32_t kOpDecorate = 71;

// Like first_statement_of_function, but returns the [begin, end) span of the whole body.
std::optional<std::pair<std::size_t, std::size_t>> function_body_span(const std::string &text,
  const std::string &qualified_name);

// Slang call-graph reachability: a tokenizer, not a parser, as no Slang front end is available here.

// One identifier or one of "(){}:;," found in a scan; everything else is skipped.
struct SlangToken
{
    std::string text;
    std::size_t offset = 0;// into the comment/string-stripped text this token came from
    bool is_identifier = false;
};

// Numeric literals are swallowed with their suffix, or the 'u' in "8u" would become an identifier.
std::vector<SlangToken> tokenize_slang(const std::string &text);

// Blanks string contents but keeps the quotes, so offsets are unaffected.
std::string strip_string_literals(const std::string &line);

// True once `line`, trimmed, is empty.
bool is_blank_line(const std::string &line);

// Every attribute in this corpus sits on its own line above the declaration it decorates.
bool is_attribute_line(const std::string &line);

// Walks the whole attribute run above a definition, since [shader(...)] may sit under other stacked attributes.
bool preceded_by_attribute(const std::vector<std::string> &stripped_lines,
  std::size_t def_line_index,
  std::string_view attribute);

bool preceded_by_shader_attribute(const std::vector<std::string> &stripped_lines, std::size_t def_line_index);

// One function definition found somewhere under Resources/ShadersSlang.
struct SlangFunctionDef
{
    std::string name;
    std::string owner;// the struct a method belongs to, empty for a free function
    std::string relative_file;// relative to Resources/ShadersSlang, forward slashes
    int line = 0;// 1-based, the line the return type starts on
    bool is_root = false;// preceded by a [shader("...")] attribute
    bool force_inline = false;// preceded by [ForceInline], so no generated code keeps it as a function
    std::string body;// comment/string-stripped body text, for call-graph edges
    std::string raw_def_line;// original (unstripped) text of `line`, for allowlist marker lookup
};

// A definition is type, name, '(' at depth 0 or in a struct's body, the matching ')', an optional semantic, then '{'.
void collect_functions_from_file(const std::vector<std::string> &stripped_lines,
  const std::vector<std::string> &raw_lines,
  const std::string &relative_file,
  std::vector<SlangFunctionDef> &out);

// Every function under Resources/ShadersSlang as one corpus, because imports cross files.
std::vector<SlangFunctionDef> collect_slang_functions(const fs::path &slang_root);

// Forward-slashed relative paths of every non-generated .slang file under `slang_root`.
std::vector<std::string> collect_all_slang_relative_paths(const fs::path &slang_root);

// The first [numthreads(X, Y, Z)]; std::nullopt when absent, so a renamed attribute cannot match zero times.
std::optional<std::array<int, 3>> parse_numthreads(const fs::path &path);

// The cloud math both renderers march; the C++ entry points only bind their resources to it.
fs::path sharedCloudsPath();

// Body of the first function whose declaration contains `signature_needle`.
std::string extract_function_body(const std::string &contents, const std::string &signature_needle);

// The text between <decl>'s opening brace and its matching close, or nothing when either is missing.
std::optional<std::string> cpp_struct_body(const std::string &text, const std::string &decl);

// Member names of scene_types.slang's scalar-layout ObjMaterial mirror, brace-matched like the SceneUBO parser.
std::vector<std::string> parse_obj_material_member_names(const fs::path &scene_types_path);

}// namespace Kataglyphis::TestSupport::BuildIntegrity
