module;

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "InstallPaths.hpp"

export module kataglyphis.shared.util.resource_paths;

export namespace Kataglyphis::Shared {

inline constexpr int kResourceSearchDepth = 8;

// Walks up to `maxDepth` ancestors of `start` (starting at `start` itself),
// testing `<ancestor>/<relative>` at each level. Every filesystem call takes
// an std::error_code - exceptions are disabled project-wide, so the throwing
// overloads would be a terminate on the first unreadable/unstattable
// ancestor. `relative` containing ".." is rejected outright: without that
// guard a caller-controlled fragment could walk the search to a path OUTSIDE
// the tree it is meant to probe.
inline auto searchAncestorsForRelative(const std::filesystem::path &start,
  const std::filesystem::path &relative,
  int maxDepth) -> std::optional<std::filesystem::path>
{
    for (const auto &part : relative) {
        if (part == "..") { return std::nullopt; }
    }

    std::filesystem::path search_path = start;
    for (int depth = 0; depth < maxDepth; ++depth) {
        std::error_code exists_ec;
        const std::filesystem::path candidate = search_path / relative;
        if (std::filesystem::exists(candidate, exists_ec) && !exists_ec) { return candidate; }
        if (exists_ec || !search_path.has_parent_path()) { break; }

        const std::filesystem::path parent = search_path.parent_path();
        if (parent == search_path) { break; }
        search_path = parent;
    }
    return std::nullopt;
}

// The `<ancestor>/Resources` directories searchAncestorsForRelative would probe, nearest first;
// an ancestor without one can never hold `Resources/<relative>`.
inline auto resourceRootsAbove(const std::filesystem::path &start, int maxDepth) -> std::vector<std::filesystem::path>
{
    std::vector<std::filesystem::path> roots;
    std::filesystem::path search_path = start;
    for (int depth = 0; depth < maxDepth; ++depth) {
        // exists() first: status() and is_directory() report a missing path as an error, exists() does not.
        std::error_code exists_ec;
        std::filesystem::path candidate = search_path / "Resources";
        if (std::filesystem::exists(candidate, exists_ec) && !exists_ec
            && std::filesystem::is_directory(candidate, exists_ec) && !exists_ec) {
            roots.push_back(std::move(candidate));
        }
        if (exists_ec || !search_path.has_parent_path()) { break; }

        const std::filesystem::path parent = search_path.parent_path();
        if (parent == search_path) { break; }
        search_path = parent;
    }
    return roots;
}

// Keyed on the working directory; an empty walk is not kept, so a Resources tree created later is still found.
inline auto cachedResourceRootsAbove(const std::filesystem::path &cwd) -> std::vector<std::filesystem::path>
{
    static std::mutex mutex;
    static std::filesystem::path cached_cwd;
    static std::vector<std::filesystem::path> cached_roots;

    const std::scoped_lock lock(mutex);
    if (cached_roots.empty() || cached_cwd != cwd) {
        cached_roots = resourceRootsAbove(cwd, kResourceSearchDepth);
        cached_cwd = cwd;
    }
    return cached_roots;
}

inline auto cachedExecutableDirectory() -> const std::optional<std::filesystem::path> &
{
    static const std::optional<std::filesystem::path> directory = executableDirectory();
    return directory;
}

// Exported names for InstallPaths.hpp: a TU that includes vulkan.hpp must not get <windows.h> and its macros.
inline auto executableInstallRoots() -> std::vector<std::filesystem::path>
{ return installRootsFromExecutable(cachedExecutableDirectory()); }

inline auto userCacheDirectory() -> std::optional<std::filesystem::path>
{ return userDirectory(UserDirectoryKind::Cache); }

inline auto userLogDirectory() -> std::optional<std::filesystem::path>
{ return userDirectory(UserDirectoryKind::Logs); }

#if defined(RELATIVE_RESOURCE_PATH)
// The working directory's roots, for developer runs from the source tree: the build-relative one, then each ancestor's.
inline auto workingDirectoryRoots(const std::filesystem::path &cwd) -> std::vector<std::filesystem::path>
{
    std::vector<std::filesystem::path> roots;
    std::filesystem::path build_relative =
      std::filesystem::path(cwd.string() + RELATIVE_RESOURCE_PATH).lexically_normal();
    if (!build_relative.has_filename()) { build_relative = build_relative.parent_path(); }
    roots.push_back(build_relative.parent_path());
    // A miss then costs one probe per Resources directory instead of one per ancestor.
    for (const std::filesystem::path &resources : cachedResourceRootsAbove(cwd)) {
        roots.push_back(resources.parent_path());
    }
    return roots;
}

inline auto installSearchRoots(const std::optional<std::filesystem::path> &executableDir)
  -> std::vector<std::filesystem::path>
{
    std::error_code current_path_ec;
    const std::filesystem::path cwd = std::filesystem::current_path(current_path_ec);
    return orderedSearchRoots(installRootsFromExecutable(executableDir),
      current_path_ec ? std::vector<std::filesystem::path>{} : workingDirectoryRoots(cwd));
}

// `relative` names a path below the install root, e.g. "Resources/ShadersSlang/build/spirv/post/post.vs_main.spv".
inline auto resolveInstalledPath(std::string_view relative) -> std::optional<std::filesystem::path>
{ return resolveUnderRoots(installSearchRoots(cachedExecutableDirectory()), std::filesystem::path(relative)); }

inline auto resolveResourceRelativePathFrom(const std::optional<std::filesystem::path> &executableDir,
  std::string_view relative) -> std::optional<std::filesystem::path>
{ return resolveUnderRoots(installSearchRoots(executableDir), std::filesystem::path("Resources") / relative); }

// `relative` is below Resources/, as model and skybox paths are.
inline auto resolveResourceRelativePath(std::string_view relative) -> std::optional<std::filesystem::path>
{ return resolveResourceRelativePathFrom(cachedExecutableDirectory(), relative); }

inline auto resourcesRootOrEmpty() -> std::filesystem::path
{
    if (const auto models = resolveResourceRelativePath("Models"); models.has_value()) { return models->parent_path(); }
    return {};
}
#endif

}// namespace Kataglyphis::Shared
