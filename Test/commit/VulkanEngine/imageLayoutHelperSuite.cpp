// An unhandled layout must fall through to "no access, bottom of pipe" rather than stall or race.

#include <gtest/gtest.h>

#include "common/ImageLayoutHelper.hpp"

static_assert(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eUndefined) == vk::AccessFlags2{},
  "accessFlagsForImageLayout must be usable in a constant expression");

static_assert(
  Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eUndefined) == vk::PipelineStageFlagBits2::eTopOfPipe,
  "pipelineStageForLayout must be usable in a constant expression");

namespace {

TEST(ImageLayoutHelperUnit, UndefinedHasNoAccessAndTopOfPipe)
{
    EXPECT_EQ(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eUndefined), vk::AccessFlags2{});
    EXPECT_EQ(Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eUndefined), vk::PipelineStageFlagBits2::eTopOfPipe);
}

TEST(ImageLayoutHelperUnit, PreinitializedIsHostWrite)
{
    EXPECT_EQ(
      Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::ePreinitialized), vk::AccessFlagBits2::eHostWrite);
    EXPECT_EQ(Kataglyphis::pipelineStageForLayout(vk::ImageLayout::ePreinitialized), vk::PipelineStageFlagBits2::eHost);
}

TEST(ImageLayoutHelperUnit, TransferSrcIsTransferRead)
{
    EXPECT_EQ(
      Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eTransferSrcOptimal), vk::AccessFlagBits2::eTransferRead);
    EXPECT_EQ(
      Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eTransferSrcOptimal), vk::PipelineStageFlagBits2::eTransfer);
}

TEST(ImageLayoutHelperUnit, TransferDstIsTransferWrite)
{
    EXPECT_EQ(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eTransferDstOptimal),
      vk::AccessFlagBits2::eTransferWrite);
    EXPECT_EQ(
      Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eTransferDstOptimal), vk::PipelineStageFlagBits2::eTransfer);
}

TEST(ImageLayoutHelperUnit, ColorAttachmentOptimalIsColorAttachmentWrite)
{
    EXPECT_EQ(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eColorAttachmentOptimal),
      vk::AccessFlagBits2::eColorAttachmentWrite);
    EXPECT_EQ(Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eColorAttachmentOptimal),
      vk::PipelineStageFlagBits2::eColorAttachmentOutput);
}

TEST(ImageLayoutHelperUnit, DepthStencilAttachmentOptimalWidensStageToAllCommands)
{
    // eAllCommands lets this transition be recorded on a non-graphics queue.
    EXPECT_EQ(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal),
      vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
    EXPECT_EQ(Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal),
      vk::PipelineStageFlagBits2::eAllCommands);
}

TEST(ImageLayoutHelperUnit, ShaderReadOnlyOptimalWidensStageToAllCommands)
{
    // eAllCommands, as for depth/stencil above.
    EXPECT_EQ(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal),
      vk::AccessFlagBits2::eShaderRead);
    EXPECT_EQ(Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eShaderReadOnlyOptimal),
      vk::PipelineStageFlagBits2::eAllCommands);
}

TEST(ImageLayoutHelperUnit, GeneralIsTheFiveBitAccessUnionAndAllCommands)
{
    vk::AccessFlags2 const expected = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite
                                      | vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eTransferRead
                                      | vk::AccessFlagBits2::eTransferWrite;
    EXPECT_EQ(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eGeneral), expected);
    EXPECT_EQ(Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eGeneral), vk::PipelineStageFlagBits2::eAllCommands);
}

TEST(ImageLayoutHelperUnit, UnhandledLayoutFallsThroughToEmptyAccessAndBottomOfPipe)
{
    EXPECT_EQ(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::ePresentSrcKHR), vk::AccessFlags2{});
    EXPECT_EQ(
      Kataglyphis::pipelineStageForLayout(vk::ImageLayout::ePresentSrcKHR), vk::PipelineStageFlagBits2::eBottomOfPipe);
}

// The barrier SkyBox::uploadCubeMapFaces once wrote by hand.
TEST(ImageLayoutHelperUnit, ReproducesSkyBoxFirstBarrier)
{
    EXPECT_EQ(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eUndefined), vk::AccessFlags2{});
    EXPECT_EQ(Kataglyphis::accessFlagsForImageLayout(vk::ImageLayout::eTransferDstOptimal),
      vk::AccessFlagBits2::eTransferWrite);
    EXPECT_EQ(Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eUndefined), vk::PipelineStageFlagBits2::eTopOfPipe);
    EXPECT_EQ(
      Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eTransferDstOptimal), vk::PipelineStageFlagBits2::eTransfer);
}

// Compute and ray-tracing shaders also read the mips, and only eAllCommands covers them all.
TEST(ImageLayoutHelperUnit, ShaderReadOnlyDestinationCoversComputeAndRayTracing)
{
    const vk::PipelineStageFlags2 stage = Kataglyphis::pipelineStageForLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
    EXPECT_EQ(stage, vk::PipelineStageFlagBits2::eAllCommands);
    EXPECT_NE(stage, vk::PipelineStageFlagBits2::eFragmentShader);
}

}// namespace
