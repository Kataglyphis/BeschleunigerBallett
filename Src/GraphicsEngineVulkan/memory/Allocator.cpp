module;

#include "common/Utilities.hpp"
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.hpp>

#ifndef VULKAN_API_VERSION
#define VULKAN_API_VERSION VK_API_VERSION_1_3
#endif

module kataglyphis.vulkan.allocator;

import kataglyphis.vulkan.config;

using namespace Kataglyphis;

Allocator::Allocator() = default;

Allocator::Allocator(Allocator &&other) noexcept : vmaAllocator(other.vmaAllocator)
{
    other.vmaAllocator = VK_NULL_HANDLE;
}

Allocator &Allocator::operator=(Allocator &&other) noexcept
{
    if (this != &other) {
        cleanUp();// release any allocator this instance already owns
        vmaAllocator = other.vmaAllocator;
        other.vmaAllocator = VK_NULL_HANDLE;
    }
    return *this;
}

Allocator::Allocator(const vk::Device &device,
  const vk::PhysicalDevice &physicalDevice,
  const vk::Instance &instance,
  bool enableBufferDeviceAddress)
{
    VmaAllocatorCreateInfo allocatorCreateInfo = {};
    // The BDA flag requires the bufferDeviceAddress feature on the logical device.
    if (enableBufferDeviceAddress) { allocatorCreateInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT; }
    allocatorCreateInfo.vulkanApiVersion = Kataglyphis::RendererConfig::vulkanApiVersion;
    allocatorCreateInfo.physicalDevice = static_cast<VkPhysicalDevice>(physicalDevice);
    allocatorCreateInfo.device = static_cast<VkDevice>(device);
    allocatorCreateInfo.instance = static_cast<VkInstance>(instance);

    ASSERT_VULKAN(vmaCreateAllocator(&allocatorCreateInfo, &vmaAllocator), "Failed to create vma allocator!");
}

void Allocator::cleanUp()
{
    if (vmaAllocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(vmaAllocator);
        vmaAllocator = VK_NULL_HANDLE;
    }
}

Allocator::~Allocator() { cleanUp(); }