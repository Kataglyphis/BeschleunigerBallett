#pragma once

#include <cstdint>
#include <span>
#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

// A 0-byte size violates VUID-VkBufferCreateInfo-size-00912, so the caller keeps the uncompacted BLAS.
[[nodiscard]] inline auto compactedSizesAreUsable(std::span<const vk::DeviceSize> sizes) -> bool
{
    if (sizes.empty()) { return false; }
    for (vk::DeviceSize const size : sizes) {
        if (size == 0) { return false; }
    }
    return true;
}

}// namespace Kataglyphis
