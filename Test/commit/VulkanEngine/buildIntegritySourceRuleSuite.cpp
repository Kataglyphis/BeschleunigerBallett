// Build integrity: C++ sources: modules imported, includes before imports, no static header functions.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

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
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

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
        const bool imported_elsewhere =
          it != importers.end()
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
        if (!sourceExists(root, error)) { continue; }

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
    if (sourceExists(slang_root, error)) {
        for (const auto &shader_path : shaderSourceFiles(slang_root, error)) {
            if (error) { break; }
            const fs::path &path = shader_path;
            if (path.generic_string().rfind(slang_build_prefix, 0) == 0) { continue; }// compiled output tree
            if (!isReadableRegularFile(shader_path) || path.extension() != ".slang") { continue; }
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

// An inline function naming a static header function is IFNDR; column 0 tells free functions from static members.
TEST(BuildIntegrity, HeadersDoNotDefineStaticFreeFunctions)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

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
        ASSERT_TRUE(sourceExists(src_root)) << "missing " << src_root.string();

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

// No suite may regrow a repo-root or file-slurp helper; the banned signatures are concatenated so this file passes.
TEST(BuildIntegrity, TestSuitesShareOneRepoRootHelper)
{
    const fs::path test_root = repoRoot() / "Test";
    ASSERT_TRUE(sourceExists(test_root)) << "missing " << test_root.string();

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
                violations.push_back(fs::relative(path, repoRoot()).generic_string() + " contains \"" + pattern + "\"");
            }
        }
    }

    EXPECT_TRUE(violations.empty())
      << violations.size()
      << " Test source file(s) reimplement RepoFiles.hpp's repoRoot()/readFileText() instead of including it: "
      << joinViolations(violations);
}

// Read-only accessors are const; mutable-reference returns are out of scope, pointer returns are not.
TEST(BuildIntegrity, ReadOnlyAccessorsAreConst)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path src_root = repo_root / "Src";
    ASSERT_TRUE(sourceExists(src_root));

    struct Exemption
    {
        std::string file;
        std::string name;
        std::string reason;
    };
    // Accessors the compiler proved cannot become const without a return-type change.
    const std::vector<Exemption> exemptions = { { "Src/GraphicsEngineVulkan/scene/Model.ixx",
      "getMesh",
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
          match != std::sregex_iterator();
          ++match) {
            const auto &found = *match;
            if (found[5].str() != "{") { continue; }// declaration-only, or a call site - not a definition

            const auto match_start = static_cast<std::size_t>(found.position(0));
            const auto line_start_pos =
              match_start == 0 ? std::string::npos : stripped_text.find_last_of('\n', match_start - 1);
            const std::size_t prefix_start = line_start_pos == std::string::npos ? 0 : line_start_pos + 1;
            const std::string prefix = stripped_text.substr(prefix_start, match_start - prefix_start);

            // Free functions sit at column 0; class member accessors are indented.
            if (prefix.empty() || (prefix.front() != ' ' && prefix.front() != '\t')) { continue; }

            const std::size_t trimmed_end = prefix.find_last_not_of(" \t");
            const std::string trimmed_prefix =
              trimmed_end == std::string::npos ? std::string{} : prefix.substr(0, trimmed_end + 1);
            const bool returns_non_const_reference = !trimmed_prefix.empty() && trimmed_prefix.back() == '&'
                                                     && trimmed_prefix.find("const") == std::string::npos;
            if (returns_non_const_reference) { continue; }// mutable-handle accessor, not read-only - out of scope

            const std::string name = found[1].str();
            ++accessors_checked;
            if (found[3].matched) { continue; }// already const

            const bool is_exempt = std::any_of(exemptions.begin(), exemptions.end(), [&](const Exemption &exemption) {
                return exemption.file == relative_path && exemption.name == name;
            });
            if (is_exempt) { continue; }

            const auto line_number =
              1
              + static_cast<std::size_t>(std::count(
                stripped_text.begin(), stripped_text.begin() + static_cast<std::ptrdiff_t>(match_start), '\n'));
            violations.push_back(relative_path + ':' + std::to_string(line_number) + ": " + name
                                 + "() is a read-only accessor with no const qualifier");
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

// Violation lists go through joinViolations; a flat budget of 0, as no build-integrity file needs its own join.
TEST(BuildIntegrity, ViolationListsGoThroughTheSharedJoiner)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path tests_dir = repo_root / "Test" / "commit" / "VulkanEngine";
    const std::string kHandRolledJoinDecl = std::string("std::string ") + "joined;";

    std::vector<std::string> violations;
    std::size_t scanned = 0;
    std::error_code error;
    for (fs::directory_iterator it(tests_dir, error), end; it != end; it.increment(error)) {
        if (error) { break; }
        // The suite's subject files and its shared scanners: buildIntegrity*Suite.cpp and BuildIntegrityScan.*.
        const std::string name = it->path().filename().string();
        if (!name.starts_with("buildIntegrity") && !name.starts_with("BuildIntegrity")) { continue; }
        const auto lines = readFileLines(it->path());
        ASSERT_TRUE(lines.has_value()) << "could not read " << it->path().string();
        ++scanned;
        for (std::size_t index = 0; index < lines->size(); ++index) {
            if ((*lines)[index].find(kHandRolledJoinDecl) != std::string::npos) {
                violations.push_back(
                  name + ':' + std::to_string(index + 1)
                  + ": hand-rolled join loop - use Kataglyphis::TestSupport::joinViolations instead");
            }
        }
    }

    EXPECT_GE(scanned, 20U) << "found only " << scanned << " build-integrity source(s) in " << tests_dir.string()
                            << " - renamed, or the scan broke";
    EXPECT_TRUE(violations.empty()) << violations.size()
                                    << " hand-rolled join loop(s) found, budget is 0: " << joinViolations(violations);
}

// GCC 16 redefines the standard library when a textual include follows an import that brought it (Main.cpp did).
TEST(BuildIntegrity, NoTranslationUnitIncludesAHeaderAfterAnImport)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    static const std::regex kImport(R"(^import\s+[\w.:]+\s*;)");
    static const std::regex kModuleDeclaration(R"(^(export\s+)?module(\s+[\w.:]+)?\s*;)");
    static const std::regex kQuotedInclude(R"re(^#include\s+"([^"]+)")re");
    std::vector<std::string> late_includes;
    std::size_t importers = 0;
    std::error_code error;
    // A header that imports, as GoldenRenderHarness.hpp does, is an import where a .cpp includes it.
    std::set<std::string> importing_headers;
    for (const fs::path &root : { repo_root / "Src", repo_root / "Test" }) {
        for (fs::recursive_directory_iterator it(root, error), end; it != end; it.increment(error)) {
            if (error) { break; }
            if (!it->is_regular_file(error) || it->path().extension() != ".hpp") { continue; }
            const auto lines = readFileLines(it->path());
            if (lines && std::any_of(lines->begin(), lines->end(), [](const std::string &line) {
                    return std::regex_search(line, kImport);
                })) {
                importing_headers.insert(it->path().filename().string());
            }
        }
    }
    EXPECT_TRUE(importing_headers.contains("GoldenRenderHarness.hpp")) << "the importing-header scan broke";
    const auto includes_an_importing_header = [&importing_headers](const std::string &line) {
        std::smatch match;
        return std::regex_search(line, match, kQuotedInclude)
               && importing_headers.contains(fs::path(match[1].str()).filename().string());
    };
    for (const fs::path &root : { repo_root / "Src", repo_root / "Test" }) {
        for (fs::recursive_directory_iterator it(root, error), end; it != end; it.increment(error)) {
            if (error) { break; }
            if (!it->is_regular_file(error) || it->path().extension() != ".cpp") { continue; }
            const auto lines = readFileLines(it->path());
            // A module unit keeps its includes in the global module fragment, before any import.
            if (!lines || std::any_of(lines->begin(), lines->end(), [](const std::string &line) {
                    return std::regex_search(line, kModuleDeclaration);
                })) {
                continue;
            }
            bool imported = false;
            for (std::size_t index = 0; index < lines->size(); ++index) {
                const std::string &line = (*lines)[index];
                if (imported && line.starts_with("#include")) {
                    late_includes.push_back(
                      fs::relative(it->path(), repo_root).generic_string() + ":" + std::to_string(index + 1));
                }
                if (std::regex_search(line, kImport) || includes_an_importing_header(line)) {
                    importers += imported ? 0U : 1U;
                    imported = true;
                }
            }
        }
    }

    EXPECT_GT(importers, 10U) << "found only " << importers << " .cpp file(s) that import a module - the scan broke";
    EXPECT_TRUE(late_includes.empty())
      << "move these #includes above the file's first import or importing header, or the gcc job fails:"
      << joinViolations(late_includes);
}
