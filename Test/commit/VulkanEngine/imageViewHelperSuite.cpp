#include <gtest/gtest.h>

#include <vulkan/vulkan.hpp>

#include "common/ImageViewHelper.hpp"

using Kataglyphis::buildImageViewCreateInfo;

static_assert(
  buildImageViewCreateInfo(vk::Image(nullptr), vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor, 1)
      .subresourceRange.layerCount
    == 1,
  "buildImageViewCreateInfo must be usable in a constant expression");

namespace {

TEST(ImageViewHelperUnit, SubresourceRangeComesFromTheArguments)
{
    const vk::ImageViewCreateInfo info = buildImageViewCreateInfo(
      vk::Image(nullptr), vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor, 4, vk::ImageViewType::eCube, 6);

    EXPECT_EQ(info.subresourceRange.levelCount, 4U);
    EXPECT_EQ(info.subresourceRange.layerCount, 6U);
}

TEST(ImageViewHelperUnit, DefaultsProduce2DWithOneLayer)
{
    const vk::ImageViewCreateInfo info =
      buildImageViewCreateInfo(vk::Image(nullptr), vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor, 1);

    EXPECT_EQ(info.viewType, vk::ImageViewType::e2D);
    EXPECT_EQ(info.subresourceRange.layerCount, 1U);
}

TEST(ImageViewHelperUnit, ComponentsAreAllIdentity)
{
    const vk::ImageViewCreateInfo info =
      buildImageViewCreateInfo(vk::Image(nullptr), vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor, 1);

    EXPECT_EQ(info.components.r, vk::ComponentSwizzle::eIdentity);
    EXPECT_EQ(info.components.g, vk::ComponentSwizzle::eIdentity);
    EXPECT_EQ(info.components.b, vk::ComponentSwizzle::eIdentity);
    EXPECT_EQ(info.components.a, vk::ComponentSwizzle::eIdentity);
}

TEST(ImageViewHelperUnit, BaseMipLevelAndBaseArrayLayerAreZero)
{
    const vk::ImageViewCreateInfo info = buildImageViewCreateInfo(
      vk::Image(nullptr), vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor, 4, vk::ImageViewType::eCube, 6);

    EXPECT_EQ(info.subresourceRange.baseMipLevel, 0U);
    EXPECT_EQ(info.subresourceRange.baseArrayLayer, 0U);
}

}// namespace
