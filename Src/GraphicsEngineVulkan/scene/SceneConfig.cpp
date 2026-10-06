module;
#include <cstdlib>

#include <filesystem>
#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

module kataglyphis.vulkan.scene_config;

import kataglyphis.vulkan.model_file_kind;
import kataglyphis.shared.util.resource_paths;

namespace sceneConfig {

auto resolveModelPath(const std::string &relativeModelPath) -> std::string
{
    if (const auto resolved = Kataglyphis::Shared::resolveResourceRelativePath(relativeModelPath);
      resolved.has_value()) {
        return resolved->string();
    }

    // Nothing matched: return the build-relative candidate so the loaders log a full path.
    std::error_code filesystem_error;
    const std::filesystem::path current_path = std::filesystem::current_path(filesystem_error);
    if (filesystem_error) { return relativeModelPath; }

    return (std::filesystem::path(current_path.string() + RELATIVE_RESOURCE_PATH) / relativeModelPath).string();
}

namespace {
    auto findResourcesBasePath() -> std::filesystem::path { return Kataglyphis::Shared::resourcesRootOrEmpty(); }

    auto computeModelDisplayName(const std::string &relativePath) -> std::string
    {
        const std::filesystem::path path(relativePath);
        std::string display = path.stem().string();
        const std::filesystem::path parent = path.parent_path();
        if (parent.has_relative_path() && parent != "." && !parent.empty()) {
            display = parent.string() + "/" + display;
        }
        return display;
    }

    std::vector<std::string> s_cached_model_paths;
    std::vector<std::string> s_cached_model_display_names;
    bool s_models_scanned = false;

    void scanAvailableModels()
    {
        if (s_models_scanned) return;

        sceneConfig::ModelScanResult result = sceneConfig::scanModelsUnder(findResourcesBasePath());
        if (!result.complete) return;

        // Latch only after a completed walk, so an early or failed scan retries instead of reporting no models forever.
        s_models_scanned = true;
        s_cached_model_paths = std::move(result.paths);
        s_cached_model_display_names = std::move(result.displayNames);
    }
}// namespace

auto scanModelsUnder(const std::filesystem::path &resourcesRoot) -> ModelScanResult
{
    ModelScanResult result{ .complete = false };

    std::error_code ec;
    const std::filesystem::path models_dir = resourcesRoot / "Models";
    if (!std::filesystem::exists(models_dir, ec) || ec) { return result; }

    std::filesystem::recursive_directory_iterator it(
      models_dir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) { return result; }

    const std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec) || entry_ec) { continue; }
        if (!Kataglyphis::isSupportedModelPath(it->path().string())) { continue; }

        std::error_code rel_ec;
        const std::filesystem::path rel = std::filesystem::relative(it->path(), resourcesRoot, rel_ec);
        if (rel_ec) { continue; }
        result.paths.push_back(rel.string());
        result.displayNames.push_back(computeModelDisplayName(rel.string()));
    }

    result.complete = !ec;
    return result;
}

auto defaultModelRelativePath() -> std::string_view
{
#if NDEBUG
    return "Models/crytek-sponza/sponza_triag.obj";
#else
    // Dinosaurs shows the cascaded shadows: it carries its own ground plane for them to land on.
    return "Models/Dinosaurs/dinosaurs.obj";
#endif
}

auto getModelFile() -> std::string
{
    // Tests pick a scene for what it measures: the default skeleton's thin bones cast too little shadow to gate on.
    if (const char *override_path = std::getenv("KATAGLYPHIS_MODEL_OVERRIDE"); override_path != nullptr) {
        if (*override_path != '\0') { return resolveModelPath(override_path); }
    }

    return resolveModelPath(std::string(defaultModelRelativePath()));
}

// Identity: a big model scale puts the camera inside the geometry and outgrows a cascade's resolution.
auto getModelMatrix() -> glm::mat4 { return glm::mat4(1.0F); }

auto getAvailableModelPaths() -> std::span<const std::string>
{
    scanAvailableModels();
    return s_cached_model_paths;
}

auto getAvailableModelDisplayNames() -> std::span<const std::string>
{
    scanAvailableModels();
    return s_cached_model_display_names;
}

auto defaultSelectedModelIndex(std::span<const std::string> availablePaths, std::string_view preferredRelativePath)
  -> int
{
    // generic_string() on both sides: scanned paths come out backslashed on Windows, the preferred literal does not.
    const std::string preferred = std::filesystem::path(preferredRelativePath).generic_string();
    for (size_t i = 0; i < availablePaths.size(); ++i) {
        if (std::filesystem::path(availablePaths[i]).generic_string() == preferred) { return static_cast<int>(i); }
    }

    if (!availablePaths.empty()) { return 0; }

    return -1;
}

}// namespace sceneConfig
