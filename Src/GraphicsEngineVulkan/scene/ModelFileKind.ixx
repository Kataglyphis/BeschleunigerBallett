module;

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>

export module kataglyphis.vulkan.model_file_kind;

namespace {

/// Lowercased `std::filesystem::path::extension()` - the classification key
/// isSupportedModelPath uses. Deliberately not `ends_with` on the whole path,
/// which misroutes a path like "C:/assets.glb/model.obj".
std::string lowercaseExtension(std::string_view path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return extension;
}

}// namespace

export namespace Kataglyphis {

/// True for every model format the engine can load: `.obj`, `.gltf` and `.glb`.
bool isSupportedModelPath(std::string_view path)
{
    const std::string extension = lowercaseExtension(path);
    return extension == ".obj" || extension == ".gltf" || extension == ".glb";
}

}// namespace Kataglyphis
