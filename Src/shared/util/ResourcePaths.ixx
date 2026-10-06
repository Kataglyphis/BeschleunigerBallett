module;

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

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

    return searchAncestorsForRelative(cwd, std::filesystem::path("Resources") / relativePath, kResourceSearchDepth);
}

inline auto resourcesRootOrEmpty() -> std::filesystem::path
{
    if (const auto models = resolveResourceRelativePath("Models"); models.has_value()) { return models->parent_path(); }
    return {};
}
#endif

}// namespace Kataglyphis::Shared
