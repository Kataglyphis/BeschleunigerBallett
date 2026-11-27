// A wrong barrier field still renders correctly and only races under specific timing, so pin every field.

#include <gtest/gtest.h>

#include <vulkan/vulkan.hpp>

#include "common/ImageBarrierHelper.hpp"

using Kataglyphis::buildImageMemoryBarrier;

using Stage = vk::PipelineStageFlagBits2;
using Access = vk::AccessFlagBits2;

// vk::Image{} is not constexpr in vulkan-hpp; only the nullptr_t constructor is.
static_assert(buildImageMemoryBarrier(vk::Image(nullptr),
                vk::ImageLayout::eUndefined,
                vk::ImageLayout::eGeneral,
                Stage::eColorAttachmentOutput,
                Access::eNone,
                Stage::eRayTracingShaderKHR,
                Access::eShaderStorageWrite)
                  .newLayout
                == vk::ImageLayout::eGeneral,
  "buildImageMemoryBarrier must be usable in a constant expression");

namespace {

// Asserted per call site so no single site can drift from the engine-wide fields.
void expectEngineWideBarrierInvariants(const vk::ImageMemoryBarrier2 &barrier)
{
    EXPECT_EQ(barrier.srcQueueFamilyIndex, vk::QueueFamilyIgnored);
    EXPECT_EQ(barrier.dstQueueFamilyIndex, vk::QueueFamilyIgnored);
    EXPECT_EQ(barrier.subresourceRange.aspectMask, vk::ImageAspectFlagBits::eColor);
    EXPECT_EQ(barrier.subresourceRange.baseMipLevel, 0U);
    EXPECT_EQ(barrier.subresourceRange.levelCount, 1U);
    EXPECT_EQ(barrier.subresourceRange.baseArrayLayer, 0U);
    EXPECT_EQ(barrier.subresourceRange.layerCount, 1U);
    EXPECT_EQ(barrier.pNext, nullptr);
}

TEST(ImageBarrierHelperUnit, DefaultsToFullColorMipAndLayerRange)
{
    const vk::ImageMemoryBarrier2 barrier = buildImageMemoryBarrier(vk::Image{},
      vk::ImageLayout::eUndefined,
      vk::ImageLayout::eGeneral,
      Stage::eComputeShader,
      Access::eNone,
      Stage::eComputeShader,
      Access::eShaderStorageWrite);

    expectEngineWideBarrierInvariants(barrier);
}

TEST(ImageBarrierHelperUnit, EachSideCarriesItsOwnStageAndAccess)
{
    // Sync2 puts the stage masks in the barrier, so swapping the sides is a field error, not a call-site one.
    const vk::ImageMemoryBarrier2 barrier = buildImageMemoryBarrier(vk::Image{},
      vk::ImageLayout::eColorAttachmentOptimal,
      vk::ImageLayout::eShaderReadOnlyOptimal,
      Stage::eColorAttachmentOutput,
      Access::eColorAttachmentWrite,
      Stage::eFragmentShader | Stage::eComputeShader,
      Access::eShaderSampledRead);

    EXPECT_EQ(barrier.srcStageMask, Stage::eColorAttachmentOutput);
    EXPECT_EQ(barrier.srcAccessMask, Access::eColorAttachmentWrite);
    EXPECT_EQ(barrier.dstStageMask, Stage::eFragmentShader | Stage::eComputeShader);
    EXPECT_EQ(barrier.dstAccessMask, Access::eShaderSampledRead);
    EXPECT_EQ(barrier.oldLayout, vk::ImageLayout::eColorAttachmentOptimal);
    EXPECT_EQ(barrier.newLayout, vk::ImageLayout::eShaderReadOnlyOptimal);
    expectEngineWideBarrierInvariants(barrier);
}

TEST(ImageBarrierHelperUnit, MatchesTheShadowCascadeArrayBarrier)
{
    // CascadedShadowMap::recordCommands: every cascade layer, both depth-stencil aspects of a stencil format.
    const vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
    const vk::ImageMemoryBarrier2 barrier = buildImageMemoryBarrier(vk::Image{},
      vk::ImageLayout::eUndefined,
      vk::ImageLayout::eDepthStencilAttachmentOptimal,
      Stage::eFragmentShader,
      Access::eNone,
      Stage::eEarlyFragmentTests | Stage::eLateFragmentTests,
      Access::eDepthStencilAttachmentRead | Access::eDepthStencilAttachmentWrite,
      aspect,
      0,
      1,
      0,
      3);

    EXPECT_EQ(barrier.subresourceRange.aspectMask, aspect);
    EXPECT_EQ(barrier.subresourceRange.levelCount, 1U);
    EXPECT_EQ(barrier.subresourceRange.baseArrayLayer, 0U);
    EXPECT_EQ(barrier.subresourceRange.layerCount, 3U);
    EXPECT_EQ(barrier.srcStageMask, Stage::eFragmentShader);
    EXPECT_EQ(barrier.dstStageMask, Stage::eEarlyFragmentTests | Stage::eLateFragmentTests);
}

TEST(ImageBarrierHelperUnit, MatchesRaytracingRasterizerToRaytracingBarrier)
{
    // Raytracing::recordCommands, before traceRaysKHR.
    const vk::ImageMemoryBarrier2 barrier = buildImageMemoryBarrier(vk::Image{},
      vk::ImageLayout::eUndefined,
      vk::ImageLayout::eGeneral,
      Stage::eColorAttachmentOutput,
      Access::eNone,
      Stage::eRayTracingShaderKHR,
      Access::eShaderStorageWrite);

    EXPECT_EQ(barrier.srcAccessMask, Access::eNone);
    EXPECT_EQ(barrier.dstAccessMask, Access::eShaderStorageWrite);
    EXPECT_EQ(barrier.oldLayout, vk::ImageLayout::eUndefined);
    EXPECT_EQ(barrier.newLayout, vk::ImageLayout::eGeneral);
    expectEngineWideBarrierInvariants(barrier);
}

TEST(ImageBarrierHelperUnit, MatchesPathTracingAccumulationBarrier)
{
    // Same old and new layout: a read-modify-write hazard, not a transition.
    const vk::ImageMemoryBarrier2 barrier = buildImageMemoryBarrier(vk::Image{},
      vk::ImageLayout::eGeneral,
      vk::ImageLayout::eGeneral,
      Stage::eComputeShader,
      Access::eShaderStorageWrite,
      Stage::eComputeShader,
      Access::eShaderStorageRead | Access::eShaderStorageWrite);

    EXPECT_EQ(barrier.srcAccessMask, Access::eShaderStorageWrite);
    EXPECT_EQ(barrier.dstAccessMask, Access::eShaderStorageRead | Access::eShaderStorageWrite);
    EXPECT_EQ(barrier.oldLayout, barrier.newLayout);
    expectEngineWideBarrierInvariants(barrier);
}

TEST(ImageBarrierHelperUnit, MatchesPostToPresentBarrier)
{
    // PostStage::recordCommands: colour output on the far side too, so a frame capture's barrier chains from it.
    const vk::ImageMemoryBarrier2 barrier = buildImageMemoryBarrier(vk::Image{},
      vk::ImageLayout::eColorAttachmentOptimal,
      vk::ImageLayout::ePresentSrcKHR,
      Stage::eColorAttachmentOutput,
      Access::eColorAttachmentWrite,
      Stage::eColorAttachmentOutput,
      Access::eNone);

    EXPECT_EQ(barrier.dstStageMask, Stage::eColorAttachmentOutput);
    EXPECT_EQ(barrier.dstAccessMask, Access::eNone);
    EXPECT_EQ(barrier.newLayout, vk::ImageLayout::ePresentSrcKHR);
    expectEngineWideBarrierInvariants(barrier);
}

TEST(ImageBarrierHelperUnit, MatchesFrameCaptureToTransferSrcBarrier)
{
    // FrameCapture::record, before the copy.
    const vk::ImageMemoryBarrier2 barrier = buildImageMemoryBarrier(vk::Image{},
      vk::ImageLayout::ePresentSrcKHR,
      vk::ImageLayout::eTransferSrcOptimal,
      Stage::eColorAttachmentOutput,
      Access::eColorAttachmentWrite,
      Stage::eCopy,
      Access::eTransferRead);

    EXPECT_EQ(barrier.srcAccessMask, Access::eColorAttachmentWrite);
    EXPECT_EQ(barrier.dstAccessMask, Access::eTransferRead);
    EXPECT_EQ(barrier.oldLayout, vk::ImageLayout::ePresentSrcKHR);
    EXPECT_EQ(barrier.newLayout, vk::ImageLayout::eTransferSrcOptimal);
    expectEngineWideBarrierInvariants(barrier);
}

TEST(ImageBarrierHelperUnit, MatchesFrameCaptureBackToPresentBarrier)
{
    // Empty dstAccessMask on purpose: host visibility is a separate buffer barrier.
    const vk::ImageMemoryBarrier2 barrier = buildImageMemoryBarrier(vk::Image{},
      vk::ImageLayout::eTransferSrcOptimal,
      vk::ImageLayout::ePresentSrcKHR,
      Stage::eCopy,
      Access::eTransferRead,
      Stage::eBottomOfPipe,
      Access::eNone);

    EXPECT_EQ(barrier.srcAccessMask, Access::eTransferRead);
    EXPECT_EQ(barrier.dstAccessMask, Access::eNone);
    EXPECT_EQ(barrier.oldLayout, vk::ImageLayout::eTransferSrcOptimal);
    EXPECT_EQ(barrier.newLayout, vk::ImageLayout::ePresentSrcKHR);
    expectEngineWideBarrierInvariants(barrier);
}

}// namespace

// The exposure buffer's barriers: the whole buffer, no queue transfer, each side's stage and access kept apart.
TEST(ImageBarrierHelperUnit, BufferBarrierCoversTheWholeBufferOnOneQueue)
{
    const vk::BufferMemoryBarrier2 barrier = Kataglyphis::buildBufferMemoryBarrier(vk::Buffer(nullptr),
      Stage::eComputeShader,
      Access::eShaderStorageWrite,
      Stage::eFragmentShader,
      Access::eShaderStorageRead);
    EXPECT_EQ(barrier.srcStageMask, vk::PipelineStageFlags2(Stage::eComputeShader));
    EXPECT_EQ(barrier.srcAccessMask, vk::AccessFlags2(Access::eShaderStorageWrite));
    EXPECT_EQ(barrier.dstStageMask, vk::PipelineStageFlags2(Stage::eFragmentShader));
    EXPECT_EQ(barrier.dstAccessMask, vk::AccessFlags2(Access::eShaderStorageRead));
    EXPECT_EQ(barrier.srcQueueFamilyIndex, vk::QueueFamilyIgnored);
    EXPECT_EQ(barrier.dstQueueFamilyIndex, vk::QueueFamilyIgnored);
    EXPECT_EQ(barrier.offset, 0U);
    EXPECT_EQ(barrier.size, vk::WholeSize);
    EXPECT_EQ(barrier.pNext, nullptr);
}
