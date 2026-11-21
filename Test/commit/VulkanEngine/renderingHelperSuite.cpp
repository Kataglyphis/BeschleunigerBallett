// Every pass begins its vkCmdBeginRendering through these two builders, so a wrong default reaches every pass.

#include <array>

#include <gtest/gtest.h>

#include <vulkan/vulkan.hpp>

#include "common/RenderingHelper.hpp"

using Kataglyphis::buildRenderingAttachmentInfo;
using Kataglyphis::buildRenderingInfo;

namespace {

TEST(RenderingHelperUnit, AttachmentDefaultsAreClearStoreAndUnresolved)
{
    const vk::RenderingAttachmentInfo attachment =
      buildRenderingAttachmentInfo(vk::ImageView{}, vk::ImageLayout::eColorAttachmentOptimal);

    EXPECT_EQ(attachment.imageLayout, vk::ImageLayout::eColorAttachmentOptimal);
    EXPECT_EQ(attachment.loadOp, vk::AttachmentLoadOp::eClear);
    EXPECT_EQ(attachment.storeOp, vk::AttachmentStoreOp::eStore);
    EXPECT_EQ(attachment.resolveMode, vk::ResolveModeFlagBits::eNone);
    EXPECT_EQ(attachment.resolveImageView, vk::ImageView{});
    EXPECT_EQ(attachment.pNext, nullptr);
}

TEST(RenderingHelperUnit, DepthAttachmentCarriesItsClearValue)
{
    // The raster passes and the shadow cascades clear depth to the far plane.
    const vk::RenderingAttachmentInfo attachment = buildRenderingAttachmentInfo(vk::ImageView{},
      vk::ImageLayout::eDepthStencilAttachmentOptimal,
      vk::AttachmentLoadOp::eClear,
      vk::AttachmentStoreOp::eStore,
      vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0F, 0 } });

    EXPECT_EQ(attachment.imageLayout, vk::ImageLayout::eDepthStencilAttachmentOptimal);
    EXPECT_FLOAT_EQ(attachment.clearValue.depthStencil.depth, 1.0F);
    EXPECT_EQ(attachment.clearValue.depthStencil.stencil, 0U);
}

TEST(RenderingHelperUnit, ColorAttachmentCarriesItsClearValue)
{
    const vk::RenderingAttachmentInfo attachment = buildRenderingAttachmentInfo(vk::ImageView{},
      vk::ImageLayout::eColorAttachmentOptimal,
      vk::AttachmentLoadOp::eClear,
      vk::AttachmentStoreOp::eStore,
      vk::ClearValue{ vk::ClearColorValue{ std::array<float, 4>{ 0.0F, 0.0F, 0.0F, 1.0F } } });

    EXPECT_FLOAT_EQ(attachment.clearValue.color.float32[0], 0.0F);
    EXPECT_FLOAT_EQ(attachment.clearValue.color.float32[3], 1.0F);
}

TEST(RenderingHelperUnit, PostStageOverwritesInsteadOfLoadingOrClearing)
{
    // PostStage.cpp: the fullscreen triangle writes every swapchain pixel, so nothing is loaded or cleared.
    const vk::RenderingAttachmentInfo attachment = buildRenderingAttachmentInfo(vk::ImageView{},
      vk::ImageLayout::eColorAttachmentOptimal,
      vk::AttachmentLoadOp::eDontCare,
      vk::AttachmentStoreOp::eStore);

    EXPECT_EQ(attachment.loadOp, vk::AttachmentLoadOp::eDontCare);
    EXPECT_EQ(attachment.storeOp, vk::AttachmentStoreOp::eStore);
}

TEST(RenderingHelperUnit, TracedSkyLoadsTheTraceItFills)
{
    // SkyBox.cpp's traced draw: eLoad keeps the hits, whose alpha tells the blend where the sky goes.
    const vk::RenderingAttachmentInfo attachment = buildRenderingAttachmentInfo(vk::ImageView{},
      vk::ImageLayout::eColorAttachmentOptimal,
      vk::AttachmentLoadOp::eLoad,
      vk::AttachmentStoreOp::eStore);

    EXPECT_EQ(attachment.loadOp, vk::AttachmentLoadOp::eLoad);
    EXPECT_EQ(attachment.storeOp, vk::AttachmentStoreOp::eStore);
}

TEST(RenderingHelperUnit, RenderAreaIsTheFullExtentAtTheOrigin)
{
    const vk::RenderingInfo info = buildRenderingInfo(vk::Extent2D{ 1280U, 720U }, {}, nullptr);

    EXPECT_EQ(info.renderArea.offset.x, 0);
    EXPECT_EQ(info.renderArea.offset.y, 0);
    EXPECT_EQ(info.renderArea.extent.width, 1280U);
    EXPECT_EQ(info.renderArea.extent.height, 720U);
    EXPECT_EQ(info.layerCount, 1U);
    EXPECT_EQ(info.flags, vk::RenderingFlags{});
}

TEST(RenderingHelperUnit, ColorCountAndPointerComeFromTheSpan)
{
    // DeferredRasterizer.cpp's geometry pass: three G-buffer targets.
    const std::array<vk::RenderingAttachmentInfo, 3> colors = {
        buildRenderingAttachmentInfo(vk::ImageView{}, vk::ImageLayout::eColorAttachmentOptimal),
        buildRenderingAttachmentInfo(vk::ImageView{}, vk::ImageLayout::eColorAttachmentOptimal),
        buildRenderingAttachmentInfo(vk::ImageView{}, vk::ImageLayout::eColorAttachmentOptimal),
    };
    const vk::RenderingInfo info = buildRenderingInfo(vk::Extent2D{ 4U, 4U }, colors, nullptr);

    EXPECT_EQ(info.colorAttachmentCount, 3U);
    EXPECT_EQ(info.pColorAttachments, colors.data());
}

TEST(RenderingHelperUnit, DepthPointerRoundTripsAndNullMeansNoDepth)
{
    const vk::RenderingAttachmentInfo depth =
      buildRenderingAttachmentInfo(vk::ImageView{}, vk::ImageLayout::eDepthStencilAttachmentOptimal);
    const vk::RenderingInfo with_depth = buildRenderingInfo(vk::Extent2D{ 4U, 4U }, {}, &depth);
    EXPECT_EQ(with_depth.pDepthAttachment, &depth);
    EXPECT_EQ(with_depth.colorAttachmentCount, 0U);
    // No stencil attachment anywhere: nothing in the engine uses stencil.
    EXPECT_EQ(with_depth.pStencilAttachment, nullptr);

    // The post, sky and deferred lighting passes have no depth attachment.
    const vk::RenderingInfo without_depth = buildRenderingInfo(vk::Extent2D{ 4U, 4U }, {}, nullptr);
    EXPECT_EQ(without_depth.pDepthAttachment, nullptr);
}

TEST(RenderingHelperUnit, ViewMaskIsOffByDefaultAndPassedThroughForTheCascades)
{
    EXPECT_EQ(buildRenderingInfo(vk::Extent2D{ 4U, 4U }, {}, nullptr).viewMask, 0U);

    // CascadedShadowMap.cpp: one bit per cascade layer, the same mask its pipeline names.
    const vk::RenderingInfo multiview = buildRenderingInfo(vk::Extent2D{ 2048U, 2048U }, {}, nullptr, 0b111U);
    EXPECT_EQ(multiview.viewMask, 0b111U);
}

}// namespace
