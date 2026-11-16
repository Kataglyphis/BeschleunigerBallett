#pragma once

#include <cstdlib>
#include <string_view>
#include <utility>

#include "host_device_shared_vars.hpp"
#include <spdlog/spdlog.h>
#include <vulkan/vulkan.hpp>

namespace Kataglyphis {
// Exceptions are disabled, so a failed creation/allocation call logs critical and aborts instead of throwing.
[[noreturn]] inline void vkFail(std::string_view message)
{
    spdlog::critical(message);
    std::abort();
}

inline void vkCheck(vk::Result result, std::string_view message)
{
    if (result != vk::Result::eSuccess) { vkFail(message); }
}

inline void vkCheck(VkResult result, std::string_view message) { vkCheck(static_cast<vk::Result>(result), message); }

// Unwraps a vulkan.hpp ResultValue, so a call site cannot read .value without checking .result.
template<typename T> [[nodiscard]] auto vkCheck(vk::ResultValue<T> &&resultValue, std::string_view message) -> T
{
    vkCheck(resultValue.result, message);
    return std::move(resultValue.value);
}

// Debug always validates; a Release build when KATAGLYPHIS_VULKAN_VALIDATION is set, as the Windows lavapipe lane does.
inline bool validationLayersEnabled()
{
#ifdef NDEBUG
    static const bool enabled = [] {
        const char *value = std::getenv("KATAGLYPHIS_VULKAN_VALIDATION");
        return value != nullptr && *value != '\0';
    }();
    return enabled;
#else
    return true;
#endif
}
}// namespace Kataglyphis