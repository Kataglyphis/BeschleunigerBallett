#include <gtest/gtest.h>

#include "vulkan_base/SwapchainChoices.hpp"

namespace {

TEST(SwapchainChoicesUnit, PrefersMailboxOverFifo)
{
    const std::vector<vk::PresentModeKHR> modes = {
        vk::PresentModeKHR::eFifo, vk::PresentModeKHR::eMailbox, vk::PresentModeKHR::eImmediate
    };

    EXPECT_EQ(Kataglyphis::chooseBestPresentationMode(modes), vk::PresentModeKHR::eMailbox);
}

TEST(SwapchainChoicesUnit, FallsBackToFifoWhenMailboxIsAbsent)
{
    const std::vector<vk::PresentModeKHR> modes = { vk::PresentModeKHR::eImmediate, vk::PresentModeKHR::eFifoRelaxed };

    EXPECT_EQ(Kataglyphis::chooseBestPresentationMode(modes), vk::PresentModeKHR::eFifo);
}

TEST(SwapchainChoicesUnit, PicksAnSrgbNonlinearUnormFormat)
{
    const std::vector<vk::SurfaceFormatKHR> formats = {
        { vk::Format::eR16G16B16A16Sfloat, vk::ColorSpaceKHR::eSrgbNonlinear },
        { vk::Format::eB8G8R8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear },
        { vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear },
    };

    const vk::SurfaceFormatKHR chosen = Kataglyphis::chooseBestSurfaceFormat(formats);
    EXPECT_EQ(chosen.format, vk::Format::eB8G8R8A8Unorm);
    EXPECT_EQ(chosen.colorSpace, vk::ColorSpaceKHR::eSrgbNonlinear);
}

TEST(SwapchainChoicesUnit, TreatsTheSingleUndefinedEntryAsUnrestricted)
{
    const std::vector<vk::SurfaceFormatKHR> formats = { { vk::Format::eUndefined, vk::ColorSpaceKHR::eSrgbNonlinear } };

    const vk::SurfaceFormatKHR chosen = Kataglyphis::chooseBestSurfaceFormat(formats);
    EXPECT_EQ(chosen.format, vk::Format::eR8G8B8A8Unorm);
    EXPECT_EQ(chosen.colorSpace, vk::ColorSpaceKHR::eSrgbNonlinear);
}

TEST(SwapchainChoicesUnit, AnEmptyFormatListYieldsTheDefaultInsteadOfIndexingPastTheEnd)
{
    const std::vector<vk::SurfaceFormatKHR> formats;

    const vk::SurfaceFormatKHR chosen = Kataglyphis::chooseBestSurfaceFormat(formats);
    EXPECT_EQ(chosen.format, vk::Format::eR8G8B8A8Unorm);
    EXPECT_EQ(chosen.colorSpace, vk::ColorSpaceKHR::eSrgbNonlinear);
}

TEST(SwapchainChoicesUnit, AnExoticOnlySurfaceFallsBackToItsFirstFormat)
{
    const std::vector<vk::SurfaceFormatKHR> formats = {
        { vk::Format::eA2B10G10R10UnormPack32, vk::ColorSpaceKHR::eSrgbNonlinear },
        { vk::Format::eR16G16B16A16Sfloat, vk::ColorSpaceKHR::eSrgbNonlinear },
    };

    const vk::SurfaceFormatKHR chosen = Kataglyphis::chooseBestSurfaceFormat(formats);
    EXPECT_EQ(chosen.format, vk::Format::eA2B10G10R10UnormPack32);
    EXPECT_EQ(chosen.colorSpace, vk::ColorSpaceKHR::eSrgbNonlinear);
}

TEST(SwapchainChoicesUnit, ClampsAnExtentIntoTheSurfaceMinMax)
{
    vk::SurfaceCapabilitiesKHR capabilities{};
    capabilities.minImageExtent = vk::Extent2D{ 64, 64 };
    capabilities.maxImageExtent = vk::Extent2D{ 4096, 2160 };

    const vk::Extent2D too_small = Kataglyphis::clampSwapExtent(capabilities, 1, 1);
    EXPECT_EQ(too_small.width, 64U);
    EXPECT_EQ(too_small.height, 64U);

    const vk::Extent2D too_large = Kataglyphis::clampSwapExtent(capabilities, 8000, 5000);
    EXPECT_EQ(too_large.width, 4096U);
    EXPECT_EQ(too_large.height, 2160U);

    const vk::Extent2D within_bounds = Kataglyphis::clampSwapExtent(capabilities, 1920, 1080);
    EXPECT_EQ(within_bounds.width, 1920U);
    EXPECT_EQ(within_bounds.height, 1080U);
}

}// namespace
