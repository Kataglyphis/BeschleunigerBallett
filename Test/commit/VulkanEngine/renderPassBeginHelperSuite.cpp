#include <gtest/gtest.h>

#include <array>
#include <span>
#include <vulkan/vulkan.hpp>

#include "common/RenderPassHelper.hpp"

using Kataglyphis::buildRenderPassBeginInfo;

namespace {
// Default handle constructors are not constexpr in this vulkan-hpp; the nullptr_t one is.
constexpr std::array<vk::ClearValue, 3> kThreeClearValues{
    vk::ClearValue{}, vk::ClearValue{}, vk::ClearValue{}
};
}// namespace

static_assert(buildRenderPassBeginInfo(vk::RenderPass(nullptr), vk::Framebuffer(nullptr), vk::Extent2D{ 1920, 1080 },
                std::span<const vk::ClearValue>(kThreeClearValues))
                .clearValueCount
    == 3U,
  "buildRenderPassBeginInfo must be usable in a constant expression");

namespace {

TEST(RenderPassBeginHelperUnit, ClearValueCountIsDerivedFromTheSpan)
{
    const vk::RenderPassBeginInfo info = buildRenderPassBeginInfo(vk::RenderPass{}, vk::Framebuffer{},
      vk::Extent2D{ 800, 600 }, std::span<const vk::ClearValue>(kThreeClearValues));

    EXPECT_EQ(info.clearValueCount, 3U);
}

TEST(RenderPassBeginHelperUnit, EmptySpanYieldsZeroClearValueCountAndNullPointer)
{
    const vk::RenderPassBeginInfo info = buildRenderPassBeginInfo(
      vk::RenderPass{}, vk::Framebuffer{}, vk::Extent2D{ 800, 600 }, std::span<const vk::ClearValue>{});

    EXPECT_EQ(info.clearValueCount, 0U);
    EXPECT_EQ(info.pClearValues, nullptr);
}

TEST(RenderPassBeginHelperUnit, PClearValuesPointsAtTheCallersStorage)
{
    const vk::RenderPassBeginInfo info = buildRenderPassBeginInfo(vk::RenderPass{}, vk::Framebuffer{},
      vk::Extent2D{ 800, 600 }, std::span<const vk::ClearValue>(kThreeClearValues));

    EXPECT_EQ(info.pClearValues, kThreeClearValues.data());
}

TEST(RenderPassBeginHelperUnit, RenderAreaOffsetIsAlwaysZero)
{
    const vk::RenderPassBeginInfo info = buildRenderPassBeginInfo(vk::RenderPass{}, vk::Framebuffer{},
      vk::Extent2D{ 800, 600 }, std::span<const vk::ClearValue>(kThreeClearValues));

    EXPECT_EQ(info.renderArea.offset.x, 0);
    EXPECT_EQ(info.renderArea.offset.y, 0);
}

TEST(RenderPassBeginHelperUnit, RenderAreaExtentComesFromTheExtent)
{
    const vk::Extent2D extent{ 1920, 1080 };
    const vk::RenderPassBeginInfo info = buildRenderPassBeginInfo(
      vk::RenderPass{}, vk::Framebuffer{}, extent, std::span<const vk::ClearValue>(kThreeClearValues));

    EXPECT_EQ(info.renderArea.extent.width, 1920U);
    EXPECT_EQ(info.renderArea.extent.height, 1080U);
}

}// namespace
