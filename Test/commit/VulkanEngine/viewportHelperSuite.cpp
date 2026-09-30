// Every raster pass shares this convention: origin (0, 0), unflipped y, depth [0, 1].

#include <gtest/gtest.h>

#include "common/ViewportHelper.hpp"

static_assert(Kataglyphis::fullExtentViewport(vk::Extent2D{ 1920, 1080 }).x == 0.0F,
  "fullExtentViewport must be usable in a constant expression");

static_assert(Kataglyphis::fullExtentScissor(vk::Extent2D{ 1920, 1080 }).extent == vk::Extent2D{ 1920, 1080 },
  "fullExtentScissor must be usable in a constant expression");

namespace {

TEST(ViewportHelperUnit, ViewportOriginIsZeroZero)
{
    const vk::Viewport viewport = Kataglyphis::fullExtentViewport(vk::Extent2D{ 800, 600 });
    EXPECT_EQ(viewport.x, 0.0F);
    EXPECT_EQ(viewport.y, 0.0F);
}

TEST(ViewportHelperUnit, ViewportSizeMatchesExtent)
{
    const vk::Viewport viewport = Kataglyphis::fullExtentViewport(vk::Extent2D{ 800, 600 });
    EXPECT_EQ(viewport.width, 800.0F);
    EXPECT_EQ(viewport.height, 600.0F);
}

TEST(ViewportHelperUnit, ViewportDepthRangeIsZeroToOne)
{
    const vk::Viewport viewport = Kataglyphis::fullExtentViewport(vk::Extent2D{ 800, 600 });
    EXPECT_EQ(viewport.minDepth, 0.0F);
    EXPECT_EQ(viewport.maxDepth, 1.0F);
}

TEST(ViewportHelperUnit, ScissorCoversExactlyThePassedNonSquareExtent)
{
    const vk::Extent2D extent{ 1920, 1080 };
    const vk::Rect2D scissor = Kataglyphis::fullExtentScissor(extent);
    EXPECT_EQ(scissor.offset.x, 0);
    EXPECT_EQ(scissor.offset.y, 0);
    EXPECT_EQ(scissor.extent, extent);
}

}// namespace
