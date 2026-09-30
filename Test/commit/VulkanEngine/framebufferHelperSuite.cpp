#include <gtest/gtest.h>

#include <array>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "common/FramebufferHelper.hpp"

using Kataglyphis::buildFramebufferCreateInfo;
using Kataglyphis::destroyFramebuffer;
using Kataglyphis::destroyFramebuffers;

namespace {
// Default handle constructors are not constexpr in this vulkan-hpp; the nullptr_t one is.
constexpr std::array<vk::ImageView, 3> kThreeViews{ vk::ImageView(nullptr), vk::ImageView(nullptr),
    vk::ImageView(nullptr) };
}// namespace

static_assert(buildFramebufferCreateInfo(vk::RenderPass(nullptr), std::span<const vk::ImageView>(kThreeViews),
                vk::Extent2D{ 1920, 1080 })
                .layers
    == 1,
  "buildFramebufferCreateInfo must be usable in a constant expression");

namespace {

TEST(FramebufferHelperUnit, AttachmentCountIsDerivedFromTheSpan)
{
    const vk::FramebufferCreateInfo info =
      buildFramebufferCreateInfo(vk::RenderPass{}, std::span<const vk::ImageView>(kThreeViews), vk::Extent2D{ 800, 600 });

    EXPECT_EQ(info.attachmentCount, 3U);
}

TEST(FramebufferHelperUnit, PAttachmentsPointsAtTheCallersStorage)
{
    const vk::FramebufferCreateInfo info =
      buildFramebufferCreateInfo(vk::RenderPass{}, std::span<const vk::ImageView>(kThreeViews), vk::Extent2D{ 800, 600 });

    EXPECT_EQ(info.pAttachments, kThreeViews.data());
}

TEST(FramebufferHelperUnit, WidthAndHeightComeFromTheExtent)
{
    const vk::Extent2D extent{ 1920, 1080 };
    const vk::FramebufferCreateInfo info =
      buildFramebufferCreateInfo(vk::RenderPass{}, std::span<const vk::ImageView>(kThreeViews), extent);

    EXPECT_EQ(info.width, 1920U);
    EXPECT_EQ(info.height, 1080U);
}

TEST(FramebufferHelperUnit, LayersDefaultsToOne)
{
    const vk::FramebufferCreateInfo info =
      buildFramebufferCreateInfo(vk::RenderPass{}, std::span<const vk::ImageView>(kThreeViews), vk::Extent2D{ 800, 600 });

    EXPECT_EQ(info.layers, 1U);
}

TEST(FramebufferHelperUnit, LayersOverrideIsHonoured)
{
    const vk::FramebufferCreateInfo info = buildFramebufferCreateInfo(
      vk::RenderPass{}, std::span<const vk::ImageView>(kThreeViews), vk::Extent2D{ 800, 600 }, 6);

    EXPECT_EQ(info.layers, 6U);
}

TEST(FramebufferHelperUnit, NullDeviceLeavesTheHandlesAlone)
{
    std::vector<vk::Framebuffer> framebuffers{ vk::Framebuffer(nullptr), vk::Framebuffer(nullptr) };
    destroyFramebuffers(vk::Device{}, framebuffers);
    EXPECT_EQ(framebuffers.size(), 2U);

    vk::Framebuffer framebuffer(nullptr);
    destroyFramebuffer(vk::Device{}, framebuffer);
    EXPECT_EQ(framebuffer, vk::Framebuffer(nullptr));
}

}// namespace
