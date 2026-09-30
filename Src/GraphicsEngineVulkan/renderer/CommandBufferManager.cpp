module;

#include "spdlog/spdlog.h"

#include <array>
#include <cstdint>
#include <string>
#include <vulkan/vulkan.hpp>

module kataglyphis.vulkan.command_buffer_manager;

auto Kataglyphis::VulkanRendererInternals::CommandBufferManager::beginCommandBuffer(vk::Device device,
  vk::CommandPool command_pool) -> vk::CommandBuffer
{
    if (!command_pool) {
        spdlog::error("beginCommandBuffer called with VK_NULL_HANDLE commandPool!");
        return vk::CommandBuffer{};
    }

    // command buffer to hold transfer commands
    vk::CommandBuffer command_buffer{};

    // command buffer details
    vk::CommandBufferAllocateInfo alloc_info{};
    alloc_info.level = vk::CommandBufferLevel::ePrimary;
    alloc_info.commandPool = command_pool;
    alloc_info.commandBufferCount = 1;

    // allocate command buffer from pool
    std::array<vk::CommandBuffer, 1> buffers{};
    vk::Result const result = device.allocateCommandBuffers(&alloc_info, buffers.data());
    if (result != vk::Result::eSuccess) {
        spdlog::default_logger_raw()->log(
          spdlog::level::err, "Failed to allocate command buffer! (vk::Result={})", static_cast<int>(result));
        return vk::CommandBuffer{};
    }
    command_buffer = buffers[0];

    // infromation to begin the command buffer record
    vk::CommandBufferBeginInfo begin_info{};
    // we are only using the command buffer once, so set up for one time submit
    begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;

    // begin recording transfer commands
    vk::Result const begin_result = command_buffer.begin(&begin_info);
    if (begin_result != vk::Result::eSuccess) {
        spdlog::default_logger_raw()->log(
          spdlog::level::err, "Failed to begin command buffer! (vk::Result={})", static_cast<int>(begin_result));
        device.freeCommandBuffers(command_pool, 1, &command_buffer);
        return vk::CommandBuffer{};
    }

    return command_buffer;
}

auto Kataglyphis::VulkanRendererInternals::CommandBufferManager::endAndSubmitCommandBuffer(vk::Device device,
  vk::CommandPool command_pool,
  vk::Queue queue,
  vk::CommandBuffer &command_buffer) -> bool
{
    if (!command_buffer) {
        spdlog::default_logger_raw()->log(spdlog::level::err, "Cannot submit null command buffer.");
        return false;
    }

    // end commands
    static_cast<void>(command_buffer.end());// MSVC ICE workaround: explicit discard of [[nodiscard]] return

    // queue submission information
    vk::SubmitInfo submit_info{};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;

    // A fence waits for this submission only, still synchronous, without serializing a frame in flight like waitIdle().
    vk::FenceCreateInfo fence_create_info{};
    vk::Fence fence{};
    vk::Result const fence_result = device.createFence(&fence_create_info, nullptr, &fence);
    if (fence_result != vk::Result::eSuccess) {
        spdlog::default_logger_raw()->log(spdlog::level::warn,
          "Failed to create submit fence, falling back to queue.waitIdle()! (vk::Result={})",
          static_cast<int>(fence_result));
        fence = vk::Fence{};
    }

    vk::Result const submit_result = queue.submit(1, &submit_info, fence);
    if (submit_result != vk::Result::eSuccess) {
        spdlog::default_logger_raw()->log(
          spdlog::level::err, "Failed to submit to queue! (vk::Result={})", static_cast<int>(submit_result));
        if (fence) { device.destroyFence(fence); }
        // A failed submission leaves nothing pending, so free rather than leak.
        device.freeCommandBuffers(command_pool, 1, &command_buffer);
        command_buffer = vk::CommandBuffer{};
        return false;
    }

    if (fence) {
        vk::Result const wait_result = device.waitForFences(1, &fence, VK_TRUE, UINT64_MAX);
        if (wait_result != vk::Result::eSuccess) {
            spdlog::default_logger_raw()->log(spdlog::level::err,
              "Failed to wait for submit fence, falling back to queue.waitIdle()! (vk::Result={})",
              static_cast<int>(wait_result));
            static_cast<void>(queue.waitIdle());// MSVC ICE workaround: explicit discard of [[nodiscard]] return
        }
        device.destroyFence(fence);
    } else {
        static_cast<void>(queue.waitIdle());// MSVC ICE workaround: explicit discard of [[nodiscard]] return
    }

    // Every path above has waited, so the buffer is not pending and freeing it is legal.
    device.freeCommandBuffers(command_pool, 1, &command_buffer);
    command_buffer = vk::CommandBuffer{};
    return true;
}
