module;
#include <cstdint>

#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.descriptor_set_group;

import kataglyphis.vulkan.device;

export namespace Kataglyphis {
// Derives descriptor pool sizes from a set of layout bindings: one entry per
// distinct descriptor type, accumulated as descriptorCount * set_count
// across all bindings that share the type, in first-seen order. Pure and
// device-free so it can be unit-tested without a VulkanDevice.
std::vector<vk::DescriptorPoolSize> deriveDescriptorPoolSizes(std::span<const vk::DescriptorSetLayoutBinding> bindings,
  uint32_t set_count);

// True when a write of writeCount descriptors matches what the binding
// declared. Named (rather than an inline comparison) so beginWrite's
// single-descriptor writers and writeImageArray's array writer cannot drift.
bool descriptorWriteCountMatchesBinding(const vk::DescriptorSetLayoutBinding &binding, uint32_t writeCount);

// Returns the first binding number that appears more than once in
// `bindings`, or std::nullopt if every binding number is unique. Pure and
// device-free, matching deriveDescriptorPoolSizes, so create() can reject a
// duplicate binding before it produces an invalid layout.
std::optional<uint32_t> firstDuplicateBinding(std::span<const vk::DescriptorSetLayoutBinding> bindings);

// Owns ONE descriptor set layout + descriptor pool + N descriptor sets
// (typically one per swapchain image). Replaces the four structurally
// identical layout/pool/set triads that used to live in VulkanRenderer.
//
// Usage: declare the bindings with addBinding(), then create(device, N).
// Pool sizes are derived from the bindings (count * set_count per type)
// via deriveDescriptorPoolSizes(). The write helpers look up descriptor
// type and array count from the declared binding, so call sites stay
// declarative.
//
// cleanUp() is idempotent and also clears the recorded bindings, so a
// swapchain recreation can rebuild the group from a fresh declarative
// binding list. Move-only with a destructor safety net, matching the
// established leaf-RAII pattern (VulkanBuffer et al.).
class DescriptorSetGroup
{
  public:
    DescriptorSetGroup();
    DescriptorSetGroup(const DescriptorSetGroup &) = delete;
    DescriptorSetGroup &operator=(const DescriptorSetGroup &) = delete;
    DescriptorSetGroup(DescriptorSetGroup &&other) noexcept;
    DescriptorSetGroup &operator=(DescriptorSetGroup &&other) noexcept;

    DescriptorSetGroup &
      addBinding(uint32_t binding, vk::DescriptorType type, uint32_t descriptor_count, vk::ShaderStageFlags stages);

    // Creates layout + pool and allocates set_count sets. Releases any
    // layout/pool from a previous create() first, so calling create() again
    // on an already-created instance replaces rather than leaks it. Logs and
    // returns false if called without bindings, with zero sets, or with a
    // duplicate binding number, before anything is created; every later
    // failure goes through ASSERT_VULKAN, which logs critical and aborts, so
    // there is no partial-cleanup path.
    [[nodiscard]] bool create(std::shared_ptr<VulkanDevice> vulkan_device,// DEVICE_SINK_OK: moved into member
      uint32_t set_count);

    // -- write helpers (thin wrappers around vkUpdateDescriptorSets; the
    // descriptor type / array count come from the declared binding)
    void writeBuffer(uint32_t set_index,
      uint32_t binding,
      vk::Buffer buffer,
      vk::DeviceSize range,
      vk::DeviceSize offset = 0);
    // sampler may be null (sampled images, storage images, input attachments)
    void writeImage(uint32_t set_index,
      uint32_t binding,
      vk::ImageView image_view,
      vk::ImageLayout image_layout,
      vk::Sampler sampler = nullptr);
    // Writes the binding's whole descriptor array; infos.size() must match
    // the declared descriptor count.
    void writeImageArray(uint32_t set_index, uint32_t binding, std::span<const vk::DescriptorImageInfo> infos);
    void writeAccelerationStructure(uint32_t set_index, uint32_t binding, const vk::AccelerationStructureKHR &tlas);

    [[nodiscard]] vk::DescriptorSetLayout getLayout() const { return layout; }
    [[nodiscard]] const std::vector<vk::DescriptorSet> &sets() const { return descriptor_sets; }

    // Destroys pool + layout, frees the sets and forgets the declared
    // bindings. Safe to call multiple times.
    void cleanUp();

    ~DescriptorSetGroup();

  private:
    // Returns nullptr (and logs) when the binding was never declared.
    const vk::DescriptorSetLayoutBinding *findBinding(uint32_t binding) const;
    // Validates set_index/binding and looks up the declared binding, filling
    // the common fields shared by every write (dstSet, dstBinding,
    // dstArrayElement = 0, descriptorType, descriptorCount = 1). Returns the
    // found binding, or nullptr (having already logged) on failure; callers
    // must return early in that case.
    const vk::DescriptorSetLayoutBinding *
      beginWrite(uint32_t set_index, uint32_t binding, vk::WriteDescriptorSet &out) const;

    // Destroys pool + layout (guarded on device) and clears descriptor_sets,
    // leaving bindings and device alone. The GPU-resource half of cleanUp();
    // called as create()'s first statement so a second create() releases
    // instead of leaking, and by cleanUp() itself so the two cannot drift.
    void releaseGpuResources();

    std::shared_ptr<VulkanDevice> device{ nullptr };

    // Value-initialize ({}) so the vectors are empty regardless of which
    // translation unit constructs this class. Without the braces, a C++23
    // module ABI mismatch can leave the vectors with garbage internal state
    // (the module interface unit's default init and the implementation
    // unit's constructor definition disagree on whether to zero-init).
    std::vector<vk::DescriptorSetLayoutBinding> bindings{};

    vk::DescriptorSetLayout layout{};
    vk::DescriptorPool pool{};
    std::vector<vk::DescriptorSet> descriptor_sets{};
};
}// namespace Kataglyphis
