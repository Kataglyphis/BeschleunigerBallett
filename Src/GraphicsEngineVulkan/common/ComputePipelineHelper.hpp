#pragma once

#include <vulkan/vulkan.hpp>

#include "common/ShaderStageHelper.hpp"

namespace Kataglyphis {

// Compute-stage shorthand for buildShaderStageCreateInfo().
constexpr vk::PipelineShaderStageCreateInfo buildComputeShaderStageCreateInfo(vk::ShaderModule module)
{
    return buildShaderStageCreateInfo(vk::ShaderStageFlagBits::eCompute, module);
}

// Explicit vk::Pipeline(nullptr): the default argument's non-constexpr constructor breaks constant evaluation.
constexpr vk::ComputePipelineCreateInfo buildComputePipelineCreateInfo(
  const vk::PipelineShaderStageCreateInfo &stage, vk::PipelineLayout layout)
{
    return vk::ComputePipelineCreateInfo{ vk::PipelineCreateFlags{}, stage, layout, vk::Pipeline(nullptr) };
}

}// namespace Kataglyphis
