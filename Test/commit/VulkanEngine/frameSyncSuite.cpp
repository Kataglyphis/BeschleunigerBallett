// A failed create() leaves frame_sync_count at 0, so advanceFrame() must never divide by it.

#include <gtest/gtest.h>

#include <vulkan/vulkan.hpp>

#include "common/Globals.hpp"

import kataglyphis.vulkan.frame_sync;

namespace {

TEST(FrameSyncUnit, DefaultStateCyclesWithinOneFrame)
{
    Kataglyphis::FrameSync frame_sync;

    EXPECT_EQ(frame_sync.frameSyncCount(), 1U);
    EXPECT_EQ(frame_sync.currentFrame(), 0U);

    frame_sync.advanceFrame();

    EXPECT_EQ(frame_sync.currentFrame(), 0U);
}

TEST(FrameSyncUnit, CleanUpReportsAFullyTornDownState)
{
    Kataglyphis::FrameSync frame_sync;

    frame_sync.cleanUp(vk::Device{});

    EXPECT_EQ(frame_sync.frameSyncCount(), 0U);
    EXPECT_EQ(frame_sync.currentFrame(), 0U);
    EXPECT_TRUE(frame_sync.inFlightFencesEmpty());
    EXPECT_EQ(frame_sync.imageAvailableCount(), 0U);
    EXPECT_EQ(frame_sync.inFlightFenceCount(), 0U);
    EXPECT_EQ(frame_sync.renderFinishedCount(), 0U);
    EXPECT_EQ(frame_sync.imagesInFlightFenceCount(), 0U);
}

TEST(FrameSyncUnit, AdvanceFrameIsSafeWhenSyncCreationFailed)
{
    Kataglyphis::FrameSync frame_sync;
    frame_sync.cleanUp(vk::Device{});

    // Modulo by the zeroed count; must be a no-op.
    frame_sync.advanceFrame();

    EXPECT_EQ(frame_sync.currentFrame(), 0U);
}

TEST(FrameSyncUnit, ResetAndSizeSurvivesCleanUpsCounterReset)
{
    Kataglyphis::FrameSync frame_sync;

    // resetAndSize() calls cleanUp(), which must not clobber the size set afterwards.
    frame_sync.resetAndSize(vk::Device{}, 3);

    EXPECT_EQ(frame_sync.frameSyncCount(), 3U);
    EXPECT_EQ(frame_sync.imageAvailableCount(), 3U);
    EXPECT_EQ(frame_sync.inFlightFenceCount(), 3U);
    EXPECT_EQ(frame_sync.renderFinishedCount(), 3U);
    EXPECT_EQ(frame_sync.imagesInFlightFenceCount(), 3U);

    Kataglyphis::FrameSync single_image_frame_sync;
    single_image_frame_sync.resetAndSize(vk::Device{}, 1);

    EXPECT_EQ(single_image_frame_sync.frameSyncCount(), 1U);
    EXPECT_EQ(single_image_frame_sync.renderFinishedCount(), 1U);
    EXPECT_EQ(single_image_frame_sync.imagesInFlightFenceCount(), 1U);
}

TEST(FrameSyncUnit, ResetAndSizeClampsToMaxFrameDraws)
{
    Kataglyphis::FrameSync frame_sync;

    uint32_t const large_image_count = static_cast<uint32_t>(Kataglyphis::MAX_FRAME_DRAWS) + 10;
    frame_sync.resetAndSize(vk::Device{}, large_image_count);

    EXPECT_EQ(frame_sync.frameSyncCount(), static_cast<uint32_t>(Kataglyphis::MAX_FRAME_DRAWS));
    EXPECT_EQ(frame_sync.imageAvailableCount(), static_cast<uint32_t>(Kataglyphis::MAX_FRAME_DRAWS));
    EXPECT_EQ(frame_sync.inFlightFenceCount(), static_cast<uint32_t>(Kataglyphis::MAX_FRAME_DRAWS));
    EXPECT_EQ(frame_sync.renderFinishedCount(), large_image_count);
    EXPECT_EQ(frame_sync.imagesInFlightFenceCount(), large_image_count);
}

}// namespace
