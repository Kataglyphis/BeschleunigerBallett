// A wrong semaphore stage still renders on most drivers and only races the presentation engine, so pin it.

#include <gtest/gtest.h>

#include <vulkan/vulkan.hpp>

#include "common/FrameSubmitHelper.hpp"

using Kataglyphis::buildSemaphoreSubmitInfo;
using Kataglyphis::kRenderFinishedSignalStage;
using Kataglyphis::kSwapchainAcquireWaitStage;

using Stage = vk::PipelineStageFlagBits2;

// The post pass's UNDEFINED -> colour-attachment transition chains off this wait only through its source stage.
static_assert(kSwapchainAcquireWaitStage == vk::PipelineStageFlags2(Stage::eColorAttachmentOutput),
  "the acquire wait must be the colour-attachment stage the post pass's first barrier names as its source");

// A narrower signal would leave post's ePresentSrcKHR transition or a capture's restore of it outside the semaphore.
static_assert(kRenderFinishedSignalStage == vk::PipelineStageFlags2(Stage::eAllCommands),
  "the render-finished signal must cover every command, layout transitions included");

static_assert(
  buildSemaphoreSubmitInfo(vk::Semaphore(nullptr), kSwapchainAcquireWaitStage).stageMask == kSwapchainAcquireWaitStage,
  "buildSemaphoreSubmitInfo must be usable in a constant expression");

TEST(FrameSubmitHelperUnit, BinarySemaphoreInfoCarriesOnlyTheStage)
{
    const vk::SemaphoreSubmitInfo info = buildSemaphoreSubmitInfo(vk::Semaphore{}, kRenderFinishedSignalStage);

    EXPECT_EQ(info.stageMask, kRenderFinishedSignalStage);
    // A binary semaphore ignores value; a nonzero deviceIndex would name a device group member that does not exist.
    EXPECT_EQ(info.value, 0U);
    EXPECT_EQ(info.deviceIndex, 0U);
    EXPECT_EQ(info.pNext, nullptr);
}
