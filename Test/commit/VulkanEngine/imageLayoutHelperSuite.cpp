// An unhandled layout must fall through to "no access, bottom of pipe" rather than stall or race.

#include <gtest/gtest.h>

#include "common/ImageLayoutHelper.hpp"

static_assert(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eUndefined) == vk::AccessFlags{},
  "accessFlagsForImageLayout must be usable in a constant expression");

static_assert(Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eUndefined) == vk::PipelineStageFlagBits::eTopOfPipe,
  "pipelineStageForLayout must be usable in a constant expression");

namespace {

TEST(ImageLayoutHelperUnit, UndefinedHasNoAccessAndTopOfPipe)
{
    EXPECT_EQ(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eUndefined), vk::AccessFlags{});
    EXPECT_EQ(Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eUndefined), vk::PipelineStageFlagBits::eTopOfPipe);
}

TEST(ImageLayoutHelperUnit, PreinitializedIsHostWrite)
{
    EXPECT_EQ(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::ePreinitialized), vk::AccessFlagBits::eHostWrite);
    EXPECT_EQ(Kataglyphis::pipelineStageForLayout(vk::ImageLayout::ePreinitialized), vk::PipelineStageFlagBits::eHost);
}

TEST(ImageLayoutHelperUnit, TransferSrcIsTransferRead)
{
    EXPECT_EQ(
      Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eTransferSrcOptimal), vk::AccessFlagBits::eTransferRead);
    EXPECT_EQ(
      Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eTransferSrcOptimal), vk::PipelineStageFlagBits::eTransfer);
}

TEST(ImageLayoutHelperUnit, TransferDstIsTransferWrite)
{
    EXPECT_EQ(
      Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eTransferDstOptimal), vk::AccessFlagBits::eTransferWrite);
    EXPECT_EQ(
      Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eTransferDstOptimal), vk::PipelineStageFlagBits::eTransfer);
}

TEST(ImageLayoutHelperUnit, ColorAttachmentOptimalIsColorAttachmentWrite)
{
    EXPECT_EQ(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eColorAttachmentOptimal),
      vk::AccessFlagBits::eColorAttachmentWrite);
    EXPECT_EQ(Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eColorAttachmentOptimal),
      vk::PipelineStageFlagBits::eColorAttachmentOutput);
}

TEST(ImageLayoutHelperUnit, DepthStencilAttachmentOptimalWidensStageToAllCommands)
{
    // eAllCommands lets this transition be recorded on a non-graphics queue.
    EXPECT_EQ(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal),
      vk::AccessFlagBits::eDepthStencilAttachmentWrite);
    EXPECT_EQ(Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal),
      vk::PipelineStageFlagBits::eAllCommands);
}

TEST(ImageLayoutHelperUnit, ShaderReadOnlyOptimalWidensStageToAllCommands)
{
    // eAllCommands, as for depth/stencil above.
    EXPECT_EQ(
      Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal), vk::AccessFlagBits::eShaderRead);
    EXPECT_EQ(Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eShaderReadOnlyOptimal),
      vk::PipelineStageFlagBits::eAllCommands);
}

TEST(ImageLayoutHelperUnit, GeneralIsTheFiveBitAccessUnionAndAllCommands)
{
    vk::AccessFlags const expected = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite
                                     | vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eTransferRead
                                     | vk::AccessFlagBits::eTransferWrite;
    EXPECT_EQ(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eGeneral), expected);
    EXPECT_EQ(Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eGeneral), vk::PipelineStageFlagBits::eAllCommands);
}

TEST(ImageLayoutHelperUnit, UnhandledLayoutFallsThroughToEmptyAccessAndBottomOfPipe)
{
    EXPECT_EQ(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::ePresentSrcKHR), vk::AccessFlags{});
    EXPECT_EQ(
      Kataglyphis::pipelineStageForLayout(vk::ImageLayout::ePresentSrcKHR), vk::PipelineStageFlagBits::eBottomOfPipe);
}

// The barrier SkyBox::uploadCubeMapFaces once wrote by hand.
TEST(ImageLayoutHelperUnit, ReproducesSkyBoxFirstBarrier)
{
    EXPECT_EQ(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eUndefined), vk::AccessFlags{});
    EXPECT_EQ(
      Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eTransferDstOptimal), vk::AccessFlagBits::eTransferWrite);
    EXPECT_EQ(Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eUndefined), vk::PipelineStageFlagBits::eTopOfPipe);
    EXPECT_EQ(
      Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eTransferDstOptimal), vk::PipelineStageFlagBits::eTransfer);
}

// Compute and ray-tracing shaders also read the mips, and only eAllCommands covers them all.
TEST(ImageLayoutHelperUnit, ShaderReadOnlyDestinationCoversComputeAndRayTracing)
{
    const vk::PipelineStageFlags stage = Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
    EXPECT_EQ(stage, vk::PipelineStageFlagBits::eAllCommands);
    EXPECT_NE(stage, vk::PipelineStageFlagBits::eFragmentShader);
}

}// namespace
