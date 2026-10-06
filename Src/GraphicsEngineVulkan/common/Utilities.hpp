#pragma once

#include <cstdlib>

#include "host_device_shared_vars.hpp"
#include <spdlog/spdlog.h>

namespace Kataglyphis {
// Aborts on failure (exceptions are disabled); evaluates val exactly once, as a single statement.
#define ASSERT_VULKAN(val, error_string) \
    do { \
        const vk::Result assert_vulkan_result_ = static_cast<vk::Result>(val); \
        if (assert_vulkan_result_ != vk::Result::eSuccess) { \
            spdlog::critical(error_string); \
            std::abort(); \
        } \
    } while (false)

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