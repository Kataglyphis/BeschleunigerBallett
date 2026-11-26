// Build integrity: every Slang function is reachable from an entry point and survives into its WGSL.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "BuildIntegrityScan.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

namespace {

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
    ASSERT_TRUE(sourceExists(slang_root)) << "missing " << slang_root.string();

    const std::vector<SlangFunctionDef> functions = collect_slang_functions(slang_root);

    // Floors far below the real counts, so a scan that silently finds nothing cannot pass.
    const int total_functions = static_cast<int>(functions.size());
    const int total_roots =
      static_cast<int>(std::count_if(functions.begin(), functions.end(), [](const auto &fn) { return fn.is_root; }));
    ASSERT_GT(total_functions, 40) << "found only " << total_functions << " Slang function definition(s) under "
                                   << slang_root.string() << " - the definition scan itself is broken";
    ASSERT_GT(total_roots, 20) << "found only " << total_roots << " [shader(\"...\")] entry point(s) under "
                               << slang_root.string() << " - the root scan itself is broken";

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
            dead_exemptions.push_back(
              kUnreachableSlangAllowlist[idx].file + " (" + kUnreachableSlangAllowlist[idx].marker + ")");
        }
    }
    EXPECT_TRUE(dead_exemptions.empty())
      << dead_exemptions.size()
      << " kUnreachableSlangAllowlist entr(y/ies) matched no unreachable function - the exemption is dead and "
         "must be deleted:"
      << joinViolations(dead_exemptions);
}

namespace {

// Transitive imports of `entry_relative`; common/ wins over the importer's own directory, as in import_closure.
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
            if (!std::regex_search(stripped, match, kImportRe)) { continue; }
            const std::string common = "common/" + match[1].str() + ".slang";
            const std::string sibling =
              (fs::path(relative).parent_path() / (match[1].str() + ".slang")).generic_string();
            worklist.push_back(sourceExists(slang_root / common) ? common : sibling);
        }
    }
    return file_set;
}

// WGSL names reachable from `entry_relative`'s entry points within `file_set`: its WGSL holds only its own call graph.
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

    // As Slang names them in WGSL: a method under its struct's name; a [ForceInline] one leaves no function behind.
    std::set<std::string> names;
    for (const std::size_t idx : reachable) {
        const SlangFunctionDef &fn = all_functions[idx];
        if (fn.force_inline) { continue; }
        names.insert(fn.owner.empty() ? fn.name : fn.owner + "_" + fn.name);
    }
    return names;
}

}// namespace

// The CI backstop mtimes cannot be: each reachable function must appear in the WGSL, helpers as `fn <name>_<digits>(`.
TEST(BuildIntegrity, EveryReachableSlangFunctionSurvivesIntoItsCheckedInWgsl)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    const auto &manifest = shader_manifest();
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    const std::vector<SlangFunctionDef> functions = collect_slang_functions(slang_root);
    ASSERT_GT(functions.size(), 40u) << "found only " << functions.size() << " Slang function definition(s) under "
                                     << slang_root.string() << " - the definition scan itself is broken";

    std::vector<std::string> violations;
    int checked_destinations = 0;
    int forward_reachable_count = 0;
    // A module beside its importer (forward/, tracer/) must reach the scan too, not only common/.
    std::map<std::string, std::string> sibling_module_probes{ { "forward.wgsl", "shade_surface" },
        { "traced.wgsl", "load_node" } };

    for (const auto &mapping : manifest->wgsl_map) {
        const fs::path dest = repo_root / mapping.dst_dir / mapping.wgsl_file;
        if (!sourceExists(dest)) { continue; }// OxidANT submodule not checked out here

        const auto dest_text_opt = readFileText(dest);
        ASSERT_TRUE(dest_text_opt.has_value()) << "could not open " << dest.string();
        const std::string &dest_text = *dest_text_opt;

        const std::set<std::string> file_set = resolve_slang_import_closure(slang_root, mapping.slang_source);
        const std::set<std::string> reachable_names =
          slang_function_names_reachable_from_source(functions, file_set, mapping.slang_source);

        ++checked_destinations;
        if (mapping.wgsl_file == "forward.wgsl") { forward_reachable_count = static_cast<int>(reachable_names.size()); }
        const auto probe = sibling_module_probes.find(mapping.wgsl_file);
        if (probe != sibling_module_probes.end()) {
            EXPECT_TRUE(reachable_names.contains(probe->second))
              << mapping.slang_source << ": the reachability scan no longer follows its sibling-module imports to "
              << probe->second;
            sibling_module_probes.erase(probe);
        }

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

    ASSERT_GE(checked_destinations, 8) << "only checked " << checked_destinations << " of " << manifest->wgsl_map.size()
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
