#pragma once

#include "common/EnvironmentVariable.hpp"

#include <cstdlib>
#include <string>

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

namespace Kataglyphis::TestSupport {

/// OxidANT's GpuContext::gpu_required rule: KATAGLYPHIS_REQUIRE_GPU set and non-empty.
inline bool gpuRequired() { return Kataglyphis::environmentFlag("KATAGLYPHIS_REQUIRE_GPU"); }

/// GLFW starts a platform and finds a Vulkan loader; a broken Xvfb fails here, a missing driver only later.
inline bool glfwReportsVulkanSupport()
{
    if (glfwInit() == 0) { return false; }
    const bool supports_vulkan = glfwVulkanSupported() != 0;
    glfwTerminate();
    return supports_vulkan;
}

/// SKIP_WITHOUT_FRAME_CAPTURE for a helper that returns a value: under KATAGLYPHIS_REQUIRE_GPU a missing capture is a
/// failure first, so the caller's GTEST_SKIP cannot pass the test.
template<typename Harness> bool frameCaptureAvailable(const Harness &harness)
{
    if (harness.supportsFrameCapture()) { return true; }
    if (gpuRequired()) { ADD_FAILURE() << "KATAGLYPHIS_REQUIRE_GPU is set, but the surface cannot copy out a frame."; }
    return false;
}

}// namespace Kataglyphis::TestSupport

// A GPU test skips without a Vulkan runtime; under KATAGLYPHIS_REQUIRE_GPU, as CI sets it, it fails instead.
#define SKIP_WITHOUT_GPU()                                                                                     \
    do {                                                                                                       \
        if (!::Kataglyphis::TestSupport::glfwReportsVulkanSupport()) {                                         \
            if (::Kataglyphis::TestSupport::gpuRequired()) {                                                   \
                GTEST_FAIL() << "KATAGLYPHIS_REQUIRE_GPU is set, but the GLFW/Vulkan runtime is unavailable."; \
            }                                                                                                  \
            GTEST_SKIP() << "GLFW/Vulkan runtime is unavailable on this system.";                              \
        }                                                                                                      \
    } while (false)

// A test on a device of its own needs no GLFW; `reason`, why none opened, skips it or, as above, fails it.
#define SKIP_WITHOUT_HEADLESS_DEVICE(reason)                                                              \
    do {                                                                                                  \
        if (const std::string why = (reason); !why.empty()) {                                             \
            if (::Kataglyphis::TestSupport::gpuRequired()) {                                              \
                GTEST_FAIL() << "KATAGLYPHIS_REQUIRE_GPU is set, but no headless device opened: " << why; \
            }                                                                                             \
            GTEST_SKIP() << "no headless device: " << why;                                                \
        }                                                                                                 \
    } while (false)

// Vulkan support does not imply a surface that allows copying out of a swapchain image.
#define SKIP_WITHOUT_FRAME_CAPTURE(harness)                                                                 \
    do {                                                                                                    \
        if (!(harness).supportsFrameCapture()) {                                                            \
            if (::Kataglyphis::TestSupport::gpuRequired()) {                                                \
                GTEST_FAIL() << "KATAGLYPHIS_REQUIRE_GPU is set, but the surface cannot copy out a frame."; \
            }                                                                                               \
            GTEST_SKIP() << "Surface does not support eTransferSrc; frame capture unavailable.";            \
        }                                                                                                   \
    } while (false)
