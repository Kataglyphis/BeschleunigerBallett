module;
#include <cstdint>

#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "spdlog/spdlog.h"
#include "common/Utilities.hpp"

module kataglyphis.vulkan.descriptor_set_group;

import kataglyphis.vulkan.device;

// Not = default: a defaulted ctor in the implementation unit can miss the interface's member initializers.
Kataglyphis::DescriptorSetGroup::DescriptorSetGroup()
  : device(nullptr), bindings(), layout(nullptr), pool(nullptr), descriptor_sets()
{
}

Kataglyphis::DescriptorSetGroup::DescriptorSetGroup(DescriptorSetGroup &&other) noexcept
  : device(std::move(other.device)), bindings(std::move(other.bindings)), layout(other.layout), pool(other.pool),
    descriptor_sets(std::move(other.descriptor_sets))
{
    // No clear() on the moved-from vectors: after a module ABI mismatch it overflows the heap.
    other.layout = nullptr;
    other.pool = nullptr;
}

auto Kataglyphis::DescriptorSetGroup::operator=(DescriptorSetGroup &&other) noexcept -> DescriptorSetGroup &
{
    if (this != &other) {
        cleanUp();

        device = std::move(other.device);
        bindings = std::move(other.bindings);
        layout = other.layout;
        pool = other.pool;
        descriptor_sets = std::move(other.descriptor_sets);

        other.layout = nullptr;
        other.pool = nullptr;
        other.bindings.clear();
        other.descriptor_sets.clear();
    }

    return *this;
}

auto Kataglyphis::DescriptorSetGroup::addBinding(uint32_t binding,
  vk::DescriptorType type,
  uint32_t descriptor_count,
  vk::ShaderStageFlags stages) -> DescriptorSetGroup &
{
    vk::DescriptorSetLayoutBinding layout_binding{};
    layout_binding.binding = binding;
    layout_binding.descriptorType = type;
    layout_binding.descriptorCount = descriptor_count;
    layout_binding.stageFlags = stages;
    layout_binding.pImmutableSamplers = nullptr;
    bindings.push_back(layout_binding);
    return *this;
}

bool Kataglyphis::DescriptorSetGroup::create(std::shared_ptr<VulkanDevice> vulkan_device, uint32_t set_count)  // DEVICE_SINK_OK: moved into member
{
    releaseGpuResources();

    device = std::move(vulkan_device);

    if (bindings.empty() || set_count == 0) {
        spdlog::error("DescriptorSetGroup::create called without bindings or with zero sets.");
        return false;
    }

    if (const std::optional<uint32_t> duplicate = firstDuplicateBinding(bindings); duplicate.has_value()) {
        spdlog::error("DescriptorSetGroup::create called with duplicate binding number {}.", *duplicate);
        return false;
    }

    // -- layout
    vk::DescriptorSetLayoutCreateInfo layout_create_info{};
    layout_create_info.bindingCount = static_cast<uint32_t>(bindings.size());
    layout_create_info.pBindings = bindings.data();

    auto layout_result = device->getLogicalDevice().createDescriptorSetLayout(layout_create_info);
    ASSERT_VULKAN(layout_result.result, "Failed to create a descriptor set layout!");
    layout = layout_result.value;

    // -- pool (sizes derived per descriptor type: binding count * set count)
    const std::vector<vk::DescriptorPoolSize> pool_sizes = deriveDescriptorPoolSizes(bindings, set_count);

    vk::DescriptorPoolCreateInfo pool_create_info{};
    pool_create_info.maxSets = set_count;
    pool_create_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_create_info.pPoolSizes = pool_sizes.data();

    auto pool_result = device->getLogicalDevice().createDescriptorPool(pool_create_info);
    ASSERT_VULKAN(pool_result.result, "Failed to create a descriptor pool!");
    pool = pool_result.value;

    // -- sets
    std::vector<vk::DescriptorSetLayout> set_layouts(set_count, layout);

    vk::DescriptorSetAllocateInfo set_alloc_info{};
    set_alloc_info.descriptorPool = pool;
    set_alloc_info.descriptorSetCount = set_count;
    set_alloc_info.pSetLayouts = set_layouts.data();

    auto alloc_result = device->getLogicalDevice().allocateDescriptorSets(set_alloc_info);
    ASSERT_VULKAN(alloc_result.result, "Failed to allocate descriptor sets!");
    descriptor_sets = alloc_result.value;

    return true;
}

auto Kataglyphis::DescriptorSetGroup::findBinding(uint32_t binding) const -> const vk::DescriptorSetLayoutBinding *
{
    for (const vk::DescriptorSetLayoutBinding &layout_binding : bindings) {
        if (layout_binding.binding == binding) { return &layout_binding; }
    }
    spdlog::error("DescriptorSetGroup: binding {} was never declared.", binding);
    return nullptr;
}

const vk::DescriptorSetLayoutBinding *Kataglyphis::DescriptorSetGroup::beginWrite(
  uint32_t set_index, uint32_t binding, vk::WriteDescriptorSet &out) const
{
    if (!device || set_index >= descriptor_sets.size()) {
        spdlog::error("DescriptorSetGroup: write to binding {} with invalid set index {}.", binding, set_index);
        return nullptr;
    }
    const vk::DescriptorSetLayoutBinding *layout_binding = findBinding(binding);
    if (layout_binding == nullptr) { return nullptr; }

    if (!descriptorWriteCountMatchesBinding(*layout_binding, 1)) {
        spdlog::error("DescriptorSetGroup: single-descriptor write to binding {} (declared descriptorCount {}) "
                      "would under-write; use writeImageArray for array bindings.",
          binding,
          layout_binding->descriptorCount);
        return nullptr;
    }

    out.dstSet = descriptor_sets[set_index];
    out.dstBinding = binding;
    out.dstArrayElement = 0;
    out.descriptorType = layout_binding->descriptorType;
    out.descriptorCount = 1;

    return layout_binding;
}

void Kataglyphis::DescriptorSetGroup::writeBuffer(uint32_t set_index,
  uint32_t binding,
  vk::Buffer buffer,
  vk::DeviceSize range,
  vk::DeviceSize offset)
{
    vk::DescriptorBufferInfo buffer_info{};
    buffer_info.buffer = buffer;
    buffer_info.offset = offset;
    buffer_info.range = range;

    vk::WriteDescriptorSet descriptor_write{};
    if (beginWrite(set_index, binding, descriptor_write) == nullptr) { return; }
    descriptor_write.pBufferInfo = &buffer_info;

    device->getLogicalDevice().updateDescriptorSets(1, &descriptor_write, 0, nullptr);
}

void Kataglyphis::DescriptorSetGroup::writeImage(uint32_t set_index,
  uint32_t binding,
  vk::ImageView image_view,
  vk::ImageLayout image_layout,
  vk::Sampler sampler)
{
    vk::DescriptorImageInfo image_info{};
    image_info.imageLayout = image_layout;
    image_info.imageView = image_view;
    image_info.sampler = sampler;

    vk::WriteDescriptorSet descriptor_write{};
    if (beginWrite(set_index, binding, descriptor_write) == nullptr) { return; }
    descriptor_write.pImageInfo = &image_info;

    device->getLogicalDevice().updateDescriptorSets(1, &descriptor_write, 0, nullptr);
}

void Kataglyphis::DescriptorSetGroup::writeImageArray(uint32_t set_index,
  uint32_t binding,
  std::span<const vk::DescriptorImageInfo> infos)
{
    if (!device || set_index >= descriptor_sets.size()) {
        spdlog::error("DescriptorSetGroup: write to binding {} with invalid set index {}.", binding, set_index);
        return;
    }
    const vk::DescriptorSetLayoutBinding *layout_binding = findBinding(binding);
    if (layout_binding == nullptr) { return; }

    if (!descriptorWriteCountMatchesBinding(*layout_binding, static_cast<uint32_t>(infos.size()))) {
        spdlog::error("DescriptorSetGroup: image array write to binding {} with {} infos (declared {}).",
          binding,
          infos.size(),
          layout_binding->descriptorCount);
        return;
    }

    vk::WriteDescriptorSet descriptor_write{};
    descriptor_write.dstSet = descriptor_sets[set_index];
    descriptor_write.dstBinding = binding;
    descriptor_write.dstArrayElement = 0;
    descriptor_write.descriptorType = layout_binding->descriptorType;
    descriptor_write.descriptorCount = static_cast<uint32_t>(infos.size());
    descriptor_write.pImageInfo = infos.data();

    device->getLogicalDevice().updateDescriptorSets(1, &descriptor_write, 0, nullptr);
}

void Kataglyphis::DescriptorSetGroup::writeAccelerationStructure(uint32_t set_index,
  uint32_t binding,
  const vk::AccelerationStructureKHR &tlas)
{
    vk::WriteDescriptorSetAccelerationStructureKHR acceleration_structure_info{};
    acceleration_structure_info.accelerationStructureCount = 1;
    acceleration_structure_info.pAccelerationStructures = &tlas;

    vk::WriteDescriptorSet descriptor_write{};
    if (beginWrite(set_index, binding, descriptor_write) == nullptr) { return; }
    descriptor_write.pNext = &acceleration_structure_info;

    device->getLogicalDevice().updateDescriptorSets(1, &descriptor_write, 0, nullptr);
}

void Kataglyphis::DescriptorSetGroup::releaseGpuResources()
{
    if (device) {
        if (pool) { device->getLogicalDevice().destroyDescriptorPool(pool); }
        if (layout) { device->getLogicalDevice().destroyDescriptorSetLayout(layout); }
    }

    pool = nullptr;
    layout = nullptr;
    descriptor_sets.clear();
}

void Kataglyphis::DescriptorSetGroup::cleanUp()
{
    releaseGpuResources();
    bindings.clear();
    device = nullptr;
}

Kataglyphis::DescriptorSetGroup::~DescriptorSetGroup() { cleanUp(); }
