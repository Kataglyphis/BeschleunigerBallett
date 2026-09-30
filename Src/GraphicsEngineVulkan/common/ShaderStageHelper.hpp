#pragma once

#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

// No entry-point parameter: Slang always emits "main", whatever the Slang function is called.
constexpr vk::PipelineShaderStageCreateInfo buildShaderStageCreateInfo(vk::ShaderStageFlagBits stage,
  vk::ShaderModule module)
{ return vk::PipelineShaderStageCreateInfo{ vk::PipelineShaderStageCreateFlags{}, stage, module, "main" }; }

// A "general" group (raygen, miss): one shader index, the other three slots unused.
constexpr vk::RayTracingShaderGroupCreateInfoKHR buildGeneralShaderGroup(uint32_t generalShader)
{
    return vk::RayTracingShaderGroupCreateInfoKHR{ vk::RayTracingShaderGroupTypeKHR::eGeneral,
        generalShader,
        VK_SHADER_UNUSED_KHR,
        VK_SHADER_UNUSED_KHR,
        VK_SHADER_UNUSED_KHR };
}

// A triangles-hit group; the engine has no procedural-intersection shaders.
constexpr vk::RayTracingShaderGroupCreateInfoKHR buildTrianglesHitGroup(uint32_t closestHitShader,
  uint32_t anyHitShader = VK_SHADER_UNUSED_KHR)
{
    return vk::RayTracingShaderGroupCreateInfoKHR{ vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup,
        VK_SHADER_UNUSED_KHR,
        closestHitShader,
        anyHitShader,
        VK_SHADER_UNUSED_KHR };
}

}// namespace Kataglyphis
