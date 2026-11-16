#pragma once

#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

// The swapchain image's first access is the sky pass's layout transition, whose source stage must be this one.
inline constexpr vk::PipelineStageFlags2 kSwapchainAcquireWaitStage =
  vk::PipelineStageFlagBits2::eColorAttachmentOutput;

// All commands: the present transition and a frame capture's restore to ePresentSrcKHR have no stage of their own.
inline constexpr vk::PipelineStageFlags2 kRenderFinishedSignalStage = vk::PipelineStageFlagBits2::eAllCommands;

// Binary semaphores only: value stays 0 and deviceIndex 0 (single-device).
constexpr vk::SemaphoreSubmitInfo buildSemaphoreSubmitInfo(vk::Semaphore semaphore, vk::PipelineStageFlags2 stage)
{ return vk::SemaphoreSubmitInfo{ semaphore, 0, stage, 0 }; }

}// namespace Kataglyphis
