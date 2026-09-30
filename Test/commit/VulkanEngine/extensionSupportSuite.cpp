// Name lookups must compare the full null-terminated string, never a prefix.

#include <array>
#include <cstdio>

#include <gtest/gtest.h>

#include "common/ExtensionSupport.hpp"

namespace {

vk::ExtensionProperties makeExtension(const char *name)
{
    vk::ExtensionProperties props{};
    std::snprintf(props.extensionName.data(), props.extensionName.size(), "%s", name);
    return props;
}

vk::LayerProperties makeLayer(const char *name)
{
    vk::LayerProperties props{};
    std::snprintf(props.layerName.data(), props.layerName.size(), "%s", name);
    return props;
}

TEST(ExtensionSupportUnit, PresentNameIsFound)
{
    std::array<vk::ExtensionProperties, 2> available = { makeExtension("VK_KHR_swapchain"),
        makeExtension("VK_KHR_maintenance4") };
    EXPECT_TRUE(Kataglyphis::supportsExtension(available, "VK_KHR_swapchain"));
}

TEST(ExtensionSupportUnit, AbsentNameIsNotFound)
{
    std::array<vk::ExtensionProperties, 1> available = { makeExtension("VK_KHR_swapchain") };
    EXPECT_FALSE(Kataglyphis::supportsExtension(available, "VK_KHR_maintenance4"));
}

TEST(ExtensionSupportUnit, PrefixOfAPresentNameIsNotFound)
{
    std::array<vk::ExtensionProperties, 1> available = { makeExtension("VK_KHR_swapchain") };
    EXPECT_FALSE(Kataglyphis::supportsExtension(available, "VK_KHR_swap"));
}

TEST(ExtensionSupportUnit, FirstMissingExtensionReturnsNullptrWhenAllPresent)
{
    std::array<vk::ExtensionProperties, 2> available = { makeExtension("VK_KHR_swapchain"),
        makeExtension("VK_KHR_maintenance4") };
    std::array<const char *, 2> required = { "VK_KHR_swapchain", "VK_KHR_maintenance4" };
    EXPECT_EQ(Kataglyphis::firstMissingExtension(available, required), nullptr);
}

TEST(ExtensionSupportUnit, FirstMissingExtensionNamesTheFirstAbsentOne)
{
    std::array<vk::ExtensionProperties, 1> available = { makeExtension("VK_KHR_swapchain") };
    std::array<const char *, 3> required = { "VK_KHR_swapchain", "VK_KHR_maintenance4", "VK_KHR_ray_query" };
    const char *missing = Kataglyphis::firstMissingExtension(available, required);
    ASSERT_NE(missing, nullptr);
    EXPECT_STREQ(missing, "VK_KHR_maintenance4");
}

TEST(ExtensionSupportUnit, EmptyAvailableListFindsNothing)
{
    std::array<vk::ExtensionProperties, 0> available{};
    EXPECT_FALSE(Kataglyphis::supportsExtension(available, "VK_KHR_swapchain"));
    std::array<const char *, 1> required = { "VK_KHR_swapchain" };
    const char *missing = Kataglyphis::firstMissingExtension(available, required);
    ASSERT_NE(missing, nullptr);
    EXPECT_STREQ(missing, "VK_KHR_swapchain");
}

TEST(ExtensionSupportUnit, SupportsLayerFindsExactMatchOnly)
{
    std::array<vk::LayerProperties, 1> available = { makeLayer("VK_LAYER_KHRONOS_validation") };
    EXPECT_TRUE(Kataglyphis::supportsLayer(available, "VK_LAYER_KHRONOS_validation"));
    EXPECT_FALSE(Kataglyphis::supportsLayer(available, "VK_LAYER_KHRONOS_valid"));
}

}// namespace
