#pragma once

// The one copy of the repo-root and file-reading helpers every source-scanning suite needs.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Kataglyphis::TestSupport {

/// Walks up at most six levels to the directory holding Resources/ShadersSlang; an empty path if none does.
inline std::filesystem::path repoRoot()
{
    std::filesystem::path candidate = std::filesystem::current_path();
    for (int depth = 0; depth < 6; ++depth) {
        if (std::filesystem::exists(candidate / "Resources" / "ShadersSlang")) { return candidate; }
        if (!candidate.has_parent_path()) { break; }
        candidate = candidate.parent_path();
    }
    return {};
}

/// The Vulkan source root; shared and WebGPU sources belong to the pinned OxidANT tree.
inline std::filesystem::path slangRoot() { return repoRoot() / "Resources" / "ShadersSlang"; }

inline std::filesystem::path sharedSlangRoot() { return repoRoot() / "third_party" / "OxidANT" / "shaders"; }

/// OxidANT's glTF fixtures, the one set both renderers load; outside Resources/, so resolveModelPath cannot reach them.
inline std::filesystem::path oxidantGltfFixtureDir()
{ return repoRoot() / "third_party" / "OxidANT" / "crates" / "assets" / "tests" / "assets"; }

inline std::string oxidantGltfFixture(std::string_view name) { return (oxidantGltfFixtureDir() / name).string(); }

inline std::filesystem::path shaderSourcePath(const std::filesystem::path &path)
{
    const auto relative = path.lexically_normal().lexically_relative(slangRoot().lexically_normal());
    if (!relative.empty() && *relative.begin() != "..") {
        std::error_code error;
        if (!std::filesystem::exists(path, error)) { return sharedSlangRoot() / relative; }
    }
    return path;
}

inline bool sourceExists(const std::filesystem::path &path, std::error_code &error)
{ return std::filesystem::exists(shaderSourcePath(path), error); }

inline bool sourceExists(const std::filesystem::path &path)
{
    std::error_code error;
    return sourceExists(path, error);
}

inline std::filesystem::path shaderRelativePath(const std::filesystem::path &path, const std::filesystem::path &root)
{
    const auto owned = path.lexically_normal().lexically_relative(sharedSlangRoot().lexically_normal());
    if (!owned.empty() && *owned.begin() != "..") { return owned; }
    return path.lexically_normal().lexically_relative(root.lexically_normal());
}

inline std::vector<std::filesystem::path> shaderSourceFiles(const std::filesystem::path &root, std::error_code &error)
{
    std::vector<std::filesystem::path> result;
    for (const auto &tree : { root, sharedSlangRoot() }) {
        for (std::filesystem::recursive_directory_iterator it(tree, error), end; it != end; it.increment(error)) {
            if (error) { return result; }
            if (it->is_directory(error) && it->path().filename() == "build") { it.disable_recursion_pending(); }
            if (it->is_regular_file(error) && it->path().extension() == ".slang") { result.push_back(it->path()); }
        }
        if (error) { return result; }
    }
    return result;
}

/// slangRoot()'s compiled-SPIR-V output directory.
inline std::filesystem::path spirvRoot() { return slangRoot() / "build" / "spirv"; }

/// True only for a regular file; a directory opens fine on Linux and would read as empty.
/// The error_code overload, because exceptions are disabled and the throwing form would terminate.
inline bool isReadableRegularFile(const std::filesystem::path &path)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(shaderSourcePath(path), ec);
}

/// Reads `path` in full, or std::nullopt if it is not a readable regular file.
inline std::optional<std::string> readFileText(const std::filesystem::path &path)
{
    if (!isReadableRegularFile(path)) { return std::nullopt; }
    std::ifstream file(shaderSourcePath(path), std::ios::binary);
    if (!file) { return std::nullopt; }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

/// Reads `path` by line (an empty file gives an empty vector, not std::nullopt).
/// Binary mode plus a stripped trailing '\r', so CRLF files parse the same on Linux and Windows.
inline std::optional<std::vector<std::string>> readFileLines(const std::filesystem::path &path)
{
    if (!isReadableRegularFile(path)) { return std::nullopt; }
    std::ifstream file(shaderSourcePath(path), std::ios::binary);
    if (!file) { return std::nullopt; }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        lines.push_back(std::move(line));
    }
    return lines;
}

/// Joins gate violations for a gtest failure message.
inline std::string joinViolations(const std::vector<std::string> &entries,
  std::string_view prefix = "\n  ",
  std::string_view suffix = {})
{
    std::string joined;
    for (const auto &entry : entries) { joined.append(prefix).append(entry).append(suffix); }
    return joined;
}

}// namespace Kataglyphis::TestSupport
