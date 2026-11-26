#pragma once

#include <array>
#include <cstdint>
#include <glm/ext/matrix_float4x4.hpp>
#include <span>
#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

// VkAccelerationStructureInstanceKHR::instanceCustomIndex is a 24-bit field.
inline constexpr uint32_t kMaxInstanceCustomIndex = (1U << 24U) - 1U;

// A mesh base past 24 bits would wrap onto another model's meshes in the traced shaders.
[[nodiscard]] constexpr auto meshBasesFitInstanceCustomIndex(std::span<const uint32_t> meshBaseOffsets) -> bool
{ return meshBaseOffsets.empty() || meshBaseOffsets.back() <= kMaxInstanceCustomIndex; }

// glm indexes [column][row]; the instance holds the model matrix's top three rows, row-major.
[[nodiscard]] inline auto tlasInstanceTransform(const glm::mat4 &model) -> vk::TransformMatrixKHR
{
    return vk::TransformMatrixKHR(std::array<std::array<float, 4>, 3>{ {
      { model[0][0], model[1][0], model[2][0], model[3][0] },
      { model[0][1], model[1][1], model[2][1], model[3][1] },
      { model[0][2], model[1][2], model[2][2], model[3][2] },
    } });
}

}// namespace Kataglyphis
