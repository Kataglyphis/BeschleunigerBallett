module;
#include <cstdint>

#include <algorithm>
#include <optional>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.descriptor_set_group;

// Own TU of the same module, so these helpers are callable and testable without device machinery.

std::vector<vk::DescriptorPoolSize>
  Kataglyphis::deriveDescriptorPoolSizes(std::span<const vk::DescriptorSetLayoutBinding> bindings, uint32_t set_count)
{
    std::vector<vk::DescriptorPoolSize> pool_sizes;
    for (const vk::DescriptorSetLayoutBinding &binding : bindings) {
        const auto existing = std::find_if(pool_sizes.begin(),
          pool_sizes.end(),
          [&](const vk::DescriptorPoolSize &pool_size) { return pool_size.type == binding.descriptorType; });
        const uint32_t descriptor_count = binding.descriptorCount * set_count;
        if (existing != pool_sizes.end()) {
            existing->descriptorCount += descriptor_count;
        } else {
            pool_sizes.push_back(vk::DescriptorPoolSize{ binding.descriptorType, descriptor_count });
        }
    }
    return pool_sizes;
}

bool Kataglyphis::descriptorWriteCountMatchesBinding(const vk::DescriptorSetLayoutBinding &binding, uint32_t writeCount)
{ return binding.descriptorCount == writeCount; }

std::optional<uint32_t> Kataglyphis::firstDuplicateBinding(std::span<const vk::DescriptorSetLayoutBinding> bindings)
{
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        for (std::size_t j = i + 1; j < bindings.size(); ++j) {
            if (bindings[i].binding == bindings[j].binding) { return bindings[i].binding; }
        }
    }
    return std::nullopt;
}
