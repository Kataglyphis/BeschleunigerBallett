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

#ifdef NDEBUG
const bool ENABLE_VALIDATION_LAYERS = false;
#else
const bool ENABLE_VALIDATION_LAYERS = true;
#endif
}// namespace Kataglyphis