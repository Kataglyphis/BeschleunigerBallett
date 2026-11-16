module;

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

#if defined(RELATIVE_RESOURCE_PATH)
// Resources-tree resolver shared by scene-config model lookup and the skybox
// texture loader: try the build-relative RELATIVE_RESOURCE_PATH candidate
// first (matches the IDE/debugger working directory), then walk ancestors
// looking for a `Resources/<relative>` sibling (matches launching from an
// arbitrary subdirectory of the repo/install tree).
inline auto resolveResourceRelativePath(std::string_view relative) -> std::optional<std::filesystem::path>
{
    const std::filesystem::path relativePath(relative);
    for (const auto &part : relativePath) {
        if (part == "..") { return std::nullopt; }
    }

    std::error_code current_path_ec;
    const std::filesystem::path cwd = std::filesystem::current_path(current_path_ec);
    if (current_path_ec) { return std::nullopt; }

    const std::filesystem::path direct_candidate =
      std::filesystem::path(cwd.string() + RELATIVE_RESOURCE_PATH) / relativePath;
    std::error_code exists_ec;
    if (std::filesystem::exists(direct_candidate, exists_ec) && !exists_ec) { return direct_candidate; }

    // A miss then costs one probe per Resources directory instead of one per ancestor.
    for (const std::filesystem::path &root : cachedResourceRootsAbove(cwd)) {
        const std::filesystem::path candidate = root / relativePath;
        if (std::filesystem::exists(candidate, exists_ec) && !exists_ec) { return candidate; }
        if (exists_ec) { break; }
    }
    return std::nullopt;
}

inline auto resourcesRootOrEmpty() -> std::filesystem::path
{
    if (const auto models = resolveResourceRelativePath("Models"); models.has_value()) { return models->parent_path(); }
    return {};
}
#endif

}// namespace Kataglyphis::Shared
