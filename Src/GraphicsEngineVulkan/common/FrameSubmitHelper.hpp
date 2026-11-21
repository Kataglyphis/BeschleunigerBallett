#pragma once

#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

// The source stage of the swapchain image's first transition: post's, or FxaaStage's while FXAA filters the frame.
inline constexpr vk::PipelineStageFlags2 kSwapchainAcquireWaitStage =
  vk::PipelineStageFlagBits2::eColorAttachmentOutput;

// All commands: the present transition and a frame capture's restore to ePresentSrcKHR have no stage of their own.
inline constexpr vk::PipelineStageFlags2 kRenderFinishedSignalStage = vk::PipelineStageFlagBits2::eAllCommands;

// Binary semaphores only: value stays 0 and deviceIndex 0 (single-device).
constexpr vk::SemaphoreSubmitInfo buildSemaphoreSubmitInfo(vk::Semaphore semaphore, vk::PipelineStageFlags2 stage)
{ return vk::SemaphoreSubmitInfo{ semaphore, 0, stage, 0 }; }

}// namespace Kataglyphis
