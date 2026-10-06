#pragma once

#include <span>

#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

// Borrows both spans' data(): they must outlive the createPipelineLayout call.
constexpr vk::PipelineLayoutCreateInfo buildPipelineLayoutCreateInfo(
  std::span<const vk::DescriptorSetLayout> set_layouts,
  std::span<const vk::PushConstantRange> push_constant_ranges = {})
{
    return vk::PipelineLayoutCreateInfo{ vk::PipelineLayoutCreateFlags{},
        static_cast<uint32_t>(set_layouts.size()),
        set_layouts.data(),
        static_cast<uint32_t>(push_constant_ranges.size()),
        push_constant_ranges.data() };
}

// Nulls both handles through the references, so a second call (cleanUp, then destructor) is a no-op.
inline void destroyPipelineAndLayout(vk::Device device, vk::Pipeline &pipeline, vk::PipelineLayout &layout)
{
    if (!device) { return; }
    if (pipeline) {
        device.destroyPipeline(pipeline);
        pipeline = nullptr;
    }
    if (layout) {
        device.destroyPipelineLayout(layout);
        layout = nullptr;
    }
}

}// namespace Kataglyphis
