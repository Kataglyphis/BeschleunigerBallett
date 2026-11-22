#pragma once

#include <span>
#include <string_view>
#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

[[nodiscard]] inline bool supportsExtension(std::span<const vk::ExtensionProperties> available, std::string_view name)
{
    for (const auto &extension : available) {
        if (std::string_view(extension.extensionName.data()) == name) { return true; }
    }
    return false;
}

[[nodiscard]] inline bool supportsLayer(std::span<const vk::LayerProperties> available, std::string_view name)
{
    for (const auto &layer : available) {
        if (std::string_view(layer.layerName.data()) == name) { return true; }
    }
    return false;
}

// The labels and object names reach captures whenever the loader lists debug_utils; the validation layer needs it too.
[[nodiscard]] inline bool enablesDebugUtils(std::span<const vk::ExtensionProperties> available, bool validation)
{ return validation || supportsExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME); }

// nullptr when every required name is present; otherwise the first missing one.
[[nodiscard]] inline const char *firstMissingExtension(std::span<const vk::ExtensionProperties> available,
  std::span<const char *const> required)
{
    for (const char *name : required) {
        if (!supportsExtension(available, name)) { return name; }
    }
    return nullptr;
}

}// namespace Kataglyphis
