// A wrong barrier field still renders correctly and only races under specific timing, so pin every field.

#include <gtest/gtest.h>

#include <vulkan/vulkan.hpp>

#include "common/ImageBarrierHelper.hpp"

using Kataglyphis::buildImageMemoryBarrier;

// vk::Image{} is not constexpr in vulkan-hpp; only the nullptr_t constructor is.
static_assert(buildImageMemoryBarrier(vk::Image(nullptr),
                vk::ImageLayout::eUndefined,
                vk::ImageLayout::eGeneral,
                {},
                vk::AccessFlagBits::eShaderWrite)
                  .newLayout
                == vk::ImageLayout::eGeneral,
  "buildImageMemoryBarrier must be usable in a constant expression");

namespace {

// Asserted per call site so no single site can drift from the engine-wide fields.
void expectEngineWideBarrierInvariants(const vk::ImageMemoryBarrier &barrier)
{
    EXPECT_EQ(barrier.srcQueueFamilyIndex, vk::QueueFamilyIgnored);
    EXPECT_EQ(barrier.dstQueueFamilyIndex, vk::QueueFamilyIgnored);
    EXPECT_EQ(barrier.subresourceRange.aspectMask, vk::ImageAspectFlagBits::eColor);
    EXPECT_EQ(barrier.subresourceRange.baseMipLevel, 0U);
    EXPECT_EQ(barrier.subresourceRange.levelCount, 1U);
    EXPECT_EQ(barrier.subresourceRange.baseArrayLayer, 0U);
    EXPECT_EQ(barrier.subresourceRange.layerCount, 1U);
}

TEST(ImageBarrierHelperUnit, DefaultsToFullColorMipAndLayerRange)
{
    const vk::ImageMemoryBarrier barrier = buildImageMemoryBarrier(
      vk::Image{}, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, {}, vk::AccessFlagBits::eShaderWrite);

    expectEngineWideBarrierInvariants(barrier);
}

TEST(ImageBarrierHelperUnit, MatchesRaytracingRasterizerToRaytracingBarrier)
{
    // Raytracing::recordCommands, rasterizerToRaytracingImageBarrier.
    const vk::ImageMemoryBarrier barrier = buildImageMemoryBarrier(
      vk::Image{}, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, {}, vk::AccessFlagBits::eShaderWrite);

    EXPECT_EQ(barrier.srcAccessMask, vk::AccessFlags{});
    EXPECT_EQ(barrier.dstAccessMask, vk::AccessFlagBits::eShaderWrite);
    EXPECT_EQ(barrier.oldLayout, vk::ImageLayout::eUndefined);
    EXPECT_EQ(barrier.newLayout, vk::ImageLayout::eGeneral);
    expectEngineWideBarrierInvariants(barrier);
}

TEST(ImageBarrierHelperUnit, MatchesRaytracingRaytracingToPostBarrier)
{
    // Raytracing::recordCommands, raytracingToPostImageBarrier.
    const vk::ImageMemoryBarrier barrier = buildImageMemoryBarrier(vk::Image{},
      vk::ImageLayout::eGeneral,
      vk::ImageLayout::eShaderReadOnlyOptimal,
      vk::AccessFlagBits::eShaderWrite,
      vk::AccessFlagBits::eShaderRead);

    EXPECT_EQ(barrier.srcAccessMask, vk::AccessFlagBits::eShaderWrite);
    EXPECT_EQ(barrier.dstAccessMask, vk::AccessFlagBits::eShaderRead);
    EXPECT_EQ(barrier.oldLayout, vk::ImageLayout::eGeneral);
    EXPECT_EQ(barrier.newLayout, vk::ImageLayout::eShaderReadOnlyOptimal);
    expectEngineWideBarrierInvariants(barrier);
}

TEST(ImageBarrierHelperUnit, MatchesPathTracingPresentToPathTracingBarrier)
{
    // PathTracing::recordCommands, presentToPathTracingImageBarrier.
    const vk::ImageMemoryBarrier barrier = buildImageMemoryBarrier(
      vk::Image{}, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, {}, vk::AccessFlagBits::eShaderWrite);

    EXPECT_EQ(barrier.srcAccessMask, vk::AccessFlags{});
    EXPECT_EQ(barrier.dstAccessMask, vk::AccessFlagBits::eShaderWrite);
    EXPECT_EQ(barrier.oldLayout, vk::ImageLayout::eUndefined);
    EXPECT_EQ(barrier.newLayout, vk::ImageLayout::eGeneral);
    expectEngineWideBarrierInvariants(barrier);
}

TEST(ImageBarrierHelperUnit, MatchesPathTracingAccumulationBarrier)
{
    // Same old and new layout: a read-modify-write hazard, not a transition.
    const vk::ImageMemoryBarrier barrier = buildImageMemoryBarrier(vk::Image{},
      vk::ImageLayout::eGeneral,
      vk::ImageLayout::eGeneral,
      vk::AccessFlagBits::eShaderWrite,
      vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);

    EXPECT_EQ(barrier.srcAccessMask, vk::AccessFlagBits::eShaderWrite);
    EXPECT_EQ(barrier.dstAccessMask, vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
    EXPECT_EQ(barrier.oldLayout, barrier.newLayout);
    expectEngineWideBarrierInvariants(barrier);
}

TEST(ImageBarrierHelperUnit, MatchesPathTracingToPresentBarrier)
{
    // PathTracing::recordCommands, pathTracingToPresentImageBarrier.
    const vk::ImageMemoryBarrier barrier = buildImageMemoryBarrier(vk::Image{},
      vk::ImageLayout::eGeneral,
      vk::ImageLayout::eShaderReadOnlyOptimal,
      vk::AccessFlagBits::eShaderWrite,
      vk::AccessFlagBits::eShaderRead);

    EXPECT_EQ(barrier.srcAccessMask, vk::AccessFlagBits::eShaderWrite);
    EXPECT_EQ(barrier.dstAccessMask, vk::AccessFlagBits::eShaderRead);
    EXPECT_EQ(barrier.oldLayout, vk::ImageLayout::eGeneral);
    EXPECT_EQ(barrier.newLayout, vk::ImageLayout::eShaderReadOnlyOptimal);
    expectEngineWideBarrierInvariants(barrier);
}

TEST(ImageBarrierHelperUnit, MatchesFrameCaptureToTransferSrcBarrier)
{
    // FrameCapture::record, to_transfer_src.
    const vk::ImageMemoryBarrier barrier = buildImageMemoryBarrier(vk::Image{},
      vk::ImageLayout::ePresentSrcKHR,
      vk::ImageLayout::eTransferSrcOptimal,
      vk::AccessFlagBits::eColorAttachmentWrite,
      vk::AccessFlagBits::eTransferRead);

    EXPECT_EQ(barrier.srcAccessMask, vk::AccessFlagBits::eColorAttachmentWrite);
    EXPECT_EQ(barrier.dstAccessMask, vk::AccessFlagBits::eTransferRead);
    EXPECT_EQ(barrier.oldLayout, vk::ImageLayout::ePresentSrcKHR);
    EXPECT_EQ(barrier.newLayout, vk::ImageLayout::eTransferSrcOptimal);
    expectEngineWideBarrierInvariants(barrier);
}

TEST(ImageBarrierHelperUnit, MatchesFrameCaptureBackToPresentBarrier)
{
    // Empty dstAccessMask on purpose: host visibility is a separate buffer barrier.
    const vk::ImageMemoryBarrier barrier = buildImageMemoryBarrier(vk::Image{},
      vk::ImageLayout::eTransferSrcOptimal,
      vk::ImageLayout::ePresentSrcKHR,
      vk::AccessFlagBits::eTransferRead,
      vk::AccessFlags{});

    EXPECT_EQ(barrier.srcAccessMask, vk::AccessFlagBits::eTransferRead);
    EXPECT_EQ(barrier.dstAccessMask, vk::AccessFlags{});
    EXPECT_EQ(barrier.oldLayout, vk::ImageLayout::eTransferSrcOptimal);
    EXPECT_EQ(barrier.newLayout, vk::ImageLayout::ePresentSrcKHR);
    expectEngineWideBarrierInvariants(barrier);
}

}// namespace
