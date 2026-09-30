#include <gtest/gtest.h>

#include "vulkan_base/PhysicalDeviceChoices.hpp"

namespace {

vk::PhysicalDeviceProperties makeProperties(vk::PhysicalDeviceType type, uint32_t maxImageDimension2D)
{
    vk::PhysicalDeviceProperties properties{};
    properties.deviceType = type;
    properties.limits.maxImageDimension2D = maxImageDimension2D;
    return properties;
}

TEST(PhysicalDeviceChoicesUnit, DeviceTypeOutranksTheCapabilityTieBreak)
{
    const vk::PhysicalDeviceProperties discrete = makeProperties(vk::PhysicalDeviceType::eDiscreteGpu, 8192);
    const vk::PhysicalDeviceProperties integrated = makeProperties(vk::PhysicalDeviceType::eIntegratedGpu, 32768);

    EXPECT_GT(Kataglyphis::scorePhysicalDevice(discrete), Kataglyphis::scorePhysicalDevice(integrated));
}

TEST(PhysicalDeviceChoicesUnit, CapabilityBreaksTiesWithinOneType)
{
    const vk::PhysicalDeviceProperties higher = makeProperties(vk::PhysicalDeviceType::eDiscreteGpu, 16384);
    const vk::PhysicalDeviceProperties lower = makeProperties(vk::PhysicalDeviceType::eDiscreteGpu, 8192);

    EXPECT_GT(Kataglyphis::scorePhysicalDevice(higher), Kataglyphis::scorePhysicalDevice(lower));
}

TEST(PhysicalDeviceChoicesUnit, ACpuDeviceNeverOutranksAnyGpu)
{
    const vk::PhysicalDeviceProperties cpu = makeProperties(vk::PhysicalDeviceType::eCpu, 32768);
    const vk::PhysicalDeviceProperties virtual_gpu = makeProperties(vk::PhysicalDeviceType::eVirtualGpu, 4096);

    EXPECT_LT(Kataglyphis::scorePhysicalDevice(cpu), Kataglyphis::scorePhysicalDevice(virtual_gpu));
}

TEST(PhysicalDeviceChoicesUnit, SelectionModeParsingIsCaseInsensitiveAndDefaultsToAuto)
{
    EXPECT_EQ(Kataglyphis::parseGpuSelectionMode("Dedicated"), Kataglyphis::GpuSelectionMode::Dedicated);
    EXPECT_EQ(Kataglyphis::parseGpuSelectionMode("INTEGRATED"), Kataglyphis::GpuSelectionMode::Integrated);
    EXPECT_EQ(Kataglyphis::parseGpuSelectionMode(""), Kataglyphis::GpuSelectionMode::Auto);
    EXPECT_EQ(Kataglyphis::parseGpuSelectionMode("nonsense"), Kataglyphis::GpuSelectionMode::Auto);
}

TEST(PhysicalDeviceChoicesUnit, ComputeDerivativeQuadsNeedBothTheExtensionAndTheFeature)
{
    EXPECT_TRUE(Kataglyphis::shouldEnableComputeDerivativeGroupQuads(true, VK_TRUE));
    EXPECT_FALSE(Kataglyphis::shouldEnableComputeDerivativeGroupQuads(true, VK_FALSE));
    EXPECT_FALSE(Kataglyphis::shouldEnableComputeDerivativeGroupQuads(false, VK_TRUE));
    EXPECT_FALSE(Kataglyphis::shouldEnableComputeDerivativeGroupQuads(false, VK_FALSE));
}

TEST(PhysicalDeviceChoicesUnit, MaxAnisotropyIsClampedToTheDeviceLimit)
{
    EXPECT_FLOAT_EQ(Kataglyphis::resolveMaxAnisotropy(false, 16.0F), 1.0F);
    EXPECT_FLOAT_EQ(Kataglyphis::resolveMaxAnisotropy(true, 16.0F), 16.0F);
    EXPECT_FLOAT_EQ(Kataglyphis::resolveMaxAnisotropy(true, 2.0F), 2.0F);
    EXPECT_FLOAT_EQ(Kataglyphis::resolveMaxAnisotropy(true, 1.0F), 1.0F);
}

}// namespace
