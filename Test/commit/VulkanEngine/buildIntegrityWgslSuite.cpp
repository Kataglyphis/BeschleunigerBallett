// Build integrity: checked-in WGSL is newer than its Slang, unedited, versioned and located.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "BuildIntegrityScan.hpp"
#include "common/EnvironmentVariable.hpp"

using namespace Kataglyphis::TestSupport::BuildIntegrity;

// Git may rewrite checkout line endings while the staged compiler output keeps its original spelling.
std::string normalize_crlf(std::string text)
{
    for (auto pos = text.find("\r\n"); pos != std::string::npos; pos = text.find("\r\n", pos)) { text.erase(pos, 1); }
    return text;
}

bool same_text_ignoring_crlf(std::string checked_in, std::string emitted)
{ return normalize_crlf(std::move(checked_in)) == normalize_crlf(std::move(emitted)); }

// The artifact and checkout are distinct trees, so every mapped output must have a readable counterpart.
std::vector<std::string>
  staged_wgsl_differences(const ShaderManifestData &manifest, const fs::path &repo_root, const fs::path &generated_root)
{
    std::vector<std::string> differences;
    for (const auto &mapping : manifest.wgsl_map) {
        const fs::path checked_in = repo_root / mapping.dst_dir / mapping.wgsl_file;
        const fs::path emitted = generated_root / mapping.wgsl_file;
        const auto checked_text = readFileText(checked_in);
        const auto emitted_text = readFileText(emitted);
        if (!checked_text || !emitted_text || !same_text_ignoring_crlf(*checked_text, *emitted_text)) {
            differences.push_back(fs::relative(checked_in, repo_root).string() + " differs from " + emitted.string()
                                  + (!checked_text   ? " (checked-in file missing or unreadable)"
                                     : !emitted_text ? " (generated file missing or unreadable)"
                                                     : " (content differs)"));
        }
    }
    return differences;
}

// Both trees must name the same inputs; build outputs do not belong to the source closure.
std::set<fs::path> slang_input_paths(const fs::path &root, std::vector<std::string> &differences)
{
    std::set<fs::path> paths;
    std::error_code error;
    if (!fs::is_directory(root, error)) {
        differences.push_back("source directory missing: " + root.string());
        return paths;
    }
    for (fs::recursive_directory_iterator it(root, error), end; it != end; it.increment(error)) {
        const bool directory = it->is_directory(error);
        if (error) { break; }
        if (directory && it->path().filename() == "build") {
            it.disable_recursion_pending();
            continue;
        }
        const bool regular = it->is_regular_file(error);
        if (error) { break; }
        if (!regular) { continue; }
        const fs::path path = it->path();
        if (path.extension() == ".slang" || path.filename() == "shader-manifest.json") {
            paths.insert(path.lexically_relative(root));
        }
    }
    if (error) { differences.push_back("cannot enumerate " + root.string() + ": " + error.message()); }
    return paths;
}

// Source and manifest bytes prove the emitted artifact belongs to this checkout's pinned inputs.
std::vector<std::string>
  staged_source_differences(const fs::path &repo_root, const fs::path &artifact_root, int &checked)
{
    std::vector<std::string> differences;
    checked = 0;
    for (const auto &relative_root : { fs::path("Resources/ShadersSlang"), fs::path("third_party/OxidANT/shaders") }) {
        const fs::path host_root = repo_root / relative_root;
        const fs::path staged_root = artifact_root / relative_root;
        const auto host_paths = slang_input_paths(host_root, differences);
        const auto staged_paths = slang_input_paths(staged_root, differences);
        checked += static_cast<int>(host_paths.size());
        for (const auto &relative : host_paths) {
            const fs::path host_file = host_root / relative;
            const fs::path staged_file = staged_root / relative;
            if (!staged_paths.contains(relative)) {
                differences.push_back("staged source missing: " + staged_file.string());
                continue;
            }
            const auto host_text = readFileText(host_file);
            const auto staged_text = readFileText(staged_file);
            if (!host_text || !staged_text || !same_text_ignoring_crlf(*host_text, *staged_text)) {
                differences.push_back(
                  fs::relative(host_file, repo_root).string() + " differs from " + staged_file.string());
            }
        }
        for (const auto &relative : staged_paths) {
            if (!host_paths.contains(relative)) {
                differences.push_back("unexpected staged source: " + (staged_root / relative).string());
            }
        }
    }
    return differences;
}

// Artifact download rewrites mtimes, so CI proves content while local checkouts keep the timestamp guard.
TEST(BuildIntegrity, CheckedInWgslIsNotOlderThanItsSlangSource)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";
    const auto &manifest = shader_manifest();
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    if (const auto generated_root_env = Kataglyphis::environmentVariable("KATAGLYPHIS_STAGED_WGSL_ROOT")) {
        ASSERT_FALSE(generated_root_env->empty()) << "staged WGSL root is empty";
        const fs::path generated_root = *generated_root_env;
        const fs::path artifact_root = generated_root.parent_path();
        std::error_code error;
        ASSERT_TRUE(fs::is_directory(generated_root, error)) << "generated WGSL root missing: " << generated_root;
        ASSERT_GE(manifest->wgsl_map.size(), 8u) << "staged WGSL comparison has too few manifest rows";
        int checked_sources = 0;
        const auto source_differences = staged_source_differences(repo_root, artifact_root, checked_sources);
        ASSERT_GT(checked_sources, 40) << "staged source comparison covered only " << checked_sources
                                       << " source/manifest file(s)";
        EXPECT_TRUE(source_differences.empty())
          << source_differences.size() << " staged Slang source/manifest file(s) differ from the host checkout: "
          << joinViolations(source_differences);
        const auto differences = staged_wgsl_differences(*manifest, repo_root, generated_root);
        EXPECT_TRUE(differences.empty()) << differences.size()
                                         << " checked-in WGSL file(s) differ from the cross-build's fresh emit: "
                                         << joinViolations(differences);
        return;
    }

    const fs::path slang_root = slangRoot();
    std::vector<std::string> stale;
    int checked = 0;
    for (const auto &mapping : manifest->wgsl_map) {
        const fs::path source = slang_root / mapping.slang_source;
        ASSERT_TRUE(sourceExists(source))
          << "Slang source mapped by the manifest's wgslMap is missing: " << source.string();

        const fs::path dest = repo_root / mapping.dst_dir / mapping.wgsl_file;
        if (!sourceExists(dest)) { continue; }// OxidANT submodule not checked out here

        std::error_code error;
        const auto source_time = fs::last_write_time(shaderSourcePath(source), error);
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
            stale.push_back(fs::relative(dest, repo_root).string() + " (mtime ticks="
                            + std::to_string(dest_time.time_since_epoch().count()) + ") is older than "
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

// A changed shader body must fail even if checkout and compiler line endings differ.
TEST(BuildIntegrity, WgslContentComparisonRejectsChangedCode)
{
    EXPECT_TRUE(same_text_ignoring_crlf("fn shader() {\r\n}\r\n", "fn shader() {\n}\n"));
    EXPECT_FALSE(same_text_ignoring_crlf("fn shader() {\n}\n", "fn changed() {\n}\n"));
    EXPECT_FALSE(same_text_ignoring_crlf("fn shader() {\r}", "fn shader() {}"));
}

// This fixture checks file identity, not whether slangc compiled the shader correctly.
TEST(BuildIntegrity, StagedWgslSourceAuditRejectsAlteredAndExtraSources)
{
    std::error_code error;
    const fs::path root =
      fs::temp_directory_path(error)
      / ("ballett-wgsl-source-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_FALSE(error);
    const fs::path host = root / "host";
    const fs::path artifact = root / "artifact";
    for (const auto &relative : { fs::path("Resources/ShadersSlang"), fs::path("third_party/OxidANT/shaders") }) {
        fs::create_directories(host / relative, error);
        ASSERT_FALSE(error);
        fs::create_directories(artifact / relative, error);
        ASSERT_FALSE(error);
    }
    const fs::path relative = "Resources/ShadersSlang/forward.slang";
    std::ofstream(host / relative, std::ios::binary) << "float4 color = 1;\r\n";
    std::ofstream(artifact / relative, std::ios::binary) << "float4 color = 1;\n";
    int checked = 0;
    const auto baseline_differences = staged_source_differences(host, artifact, checked);
    EXPECT_TRUE(baseline_differences.empty()) << joinViolations(baseline_differences);
    EXPECT_EQ(checked, 1);
    fs::create_directories(artifact / "Resources/ShadersSlang/build", error);
    ASSERT_FALSE(error);
    std::ofstream(artifact / "Resources/ShadersSlang/build/ignored.slang", std::ios::binary) << "build artifact\n";
    const auto ignored_differences = staged_source_differences(host, artifact, checked);
    EXPECT_TRUE(ignored_differences.empty()) << joinViolations(ignored_differences);
    std::ofstream(artifact / relative, std::ios::binary) << "float4 color = 2;\n";
    const auto differences = staged_source_differences(host, artifact, checked);
    EXPECT_EQ(differences.size(), 1u);
    if (!differences.empty()) { EXPECT_NE(differences.front().find("forward.slang"), std::string::npos); }
    std::ofstream(artifact / relative, std::ios::binary) << "float4 color = 1;\n";
    const fs::path extra = "third_party/OxidANT/shaders/extra.slang";
    std::ofstream(artifact / extra, std::ios::binary) << "float4 unexpected = 1;\n";
    const auto extra_differences = staged_source_differences(host, artifact, checked);
    EXPECT_EQ(extra_differences.size(), 1u) << joinViolations(extra_differences);
    if (!extra_differences.empty()) { EXPECT_NE(extra_differences.front().find("extra.slang"), std::string::npos); }
    fs::remove_all(root, error);
    EXPECT_FALSE(error);
}

// WGSL has no string literals and the backend emits no comments, so any "//" is a hand-edit the next regenerate drops.
TEST(BuildIntegrity, CheckedInWgslHasNoHandEdits)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const auto &manifest = shader_manifest();
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    std::vector<std::string> hand_edits;
    int checked = 0;
    for (const auto &mapping : manifest->wgsl_map) {
        const fs::path dest = repo_root / mapping.dst_dir / mapping.wgsl_file;
        if (!sourceExists(dest)) { continue; }// OxidANT submodule not checked out here
        ++checked;

        const auto lines = readFileLines(dest);
        if (!lines) { continue; }

        int line_number = 0;
        for (const auto &line : *lines) {
            ++line_number;
            if (line.find("//") != std::string::npos) {
                hand_edits.push_back(
                  fs::relative(dest, repo_root).string() + ':' + std::to_string(line_number) + ": " + line);
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

// CI backstop for docs/source/shader-build-pipeline.md § The combined WGSL emit needs slangc ≥ 2026.8.
TEST(BuildIntegrity, CheckedInWgslVaryingStructsCarryLocations)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const auto &manifest = shader_manifest();
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
        if (!sourceExists(dest)) { continue; }// OxidANT submodule not checked out here
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

            const bool is_io_struct = std::any_of(
              members.begin(), members.end(), [&](const auto &member) { return is_io_attr(std::get<1>(member)); });
            if (!is_io_struct) { continue; }

            for (const auto &[line_number, attrs, text] : members) {
                if (is_io_attr(attrs)) { continue; }
                violations.push_back(fs::relative(dest, repo_root).generic_string() + ':' + std::to_string(line_number)
                                     + ": struct " + struct_name + ": " + text);
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

    const auto &manifest = shader_manifest();
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";

    EXPECT_FALSE(manifest->min_slangc_version_for_wgsl.empty())
      << "shader-manifest.json has no \"minSlangcVersionForWgsl\" - without it both compile scripts stop "
         "skipping the combined WGSL emit on toolchains whose emit drops varying @location attributes";
    EXPECT_TRUE(std::regex_search(manifest->min_slangc_version_for_wgsl, std::regex(R"(^\d+\.\d+)")))
      << "\"minSlangcVersionForWgsl\" (" << manifest->min_slangc_version_for_wgsl
      << ") must start with MAJOR.MINOR - the compile scripts compare only that prefix and treat anything "
         "unparseable as new enough";
}

// Generated WGSL is a one-way output, so no Slang source may claim to mirror it.
TEST(BuildIntegrity, NoGeneratedWgslSourceClaimsToMirrorItsOutput)
{
    const fs::path repo_root = repoRoot();
    ASSERT_FALSE(repo_root.empty()) << "could not locate the repository root";

    const fs::path slang_root = slangRoot();
    const auto &manifest = shader_manifest();
    ASSERT_TRUE(manifest.has_value()) << "shader-manifest.json is missing or malformed";
    ASSERT_GE(manifest->wgsl_map.size(), 5U)
      << "found only " << manifest->wgsl_map.size() << " wgslMap entr(y/ies) - the manifest parse itself is broken";

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
