#include <gtest/gtest.h>

#include <array>
#include <span>
#include <vulkan/vulkan.hpp>

#include "common/PipelineLayoutHelper.hpp"

using Kataglyphis::buildPipelineLayoutCreateInfo;

namespace {
// Default handle constructors are not constexpr in this vulkan-hpp; the nullptr_t one is.
constexpr std::array<vk::DescriptorSetLayout, 2> kTwoLayouts{ vk::DescriptorSetLayout(nullptr),
    vk::DescriptorSetLayout(nullptr) };
constexpr std::array<vk::DescriptorSetLayout, 3> kThreeLayouts{ vk::DescriptorSetLayout(nullptr),
    vk::DescriptorSetLayout(nullptr),
    vk::DescriptorSetLayout(nullptr) };
constexpr std::array<vk::PushConstantRange, 1> kOneRange{ vk::PushConstantRange{} };
constexpr std::array<vk::PushConstantRange, 2> kTwoRanges{ vk::PushConstantRange{}, vk::PushConstantRange{} };
}// namespace

static_assert(buildPipelineLayoutCreateInfo(std::span<const vk::DescriptorSetLayout>(kTwoLayouts)).setLayoutCount == 2U,
  "buildPipelineLayoutCreateInfo must be usable in a constant expression");

namespace {

TEST(PipelineLayoutHelperUnit, SetLayoutCountIsDerivedFromTheSpan)
{
    const vk::PipelineLayoutCreateInfo two_layout_info =
      buildPipelineLayoutCreateInfo(std::span<const vk::DescriptorSetLayout>(kTwoLayouts));
    EXPECT_EQ(two_layout_info.setLayoutCount, 2U);

    const vk::PipelineLayoutCreateInfo three_layout_info =
      buildPipelineLayoutCreateInfo(std::span<const vk::DescriptorSetLayout>(kThreeLayouts));
    EXPECT_EQ(three_layout_info.setLayoutCount, 3U);
}

TEST(PipelineLayoutHelperUnit, PushConstantRangeCountIsDerivedFromTheSpan)
{
    const vk::PipelineLayoutCreateInfo one_range_info = buildPipelineLayoutCreateInfo(
      std::span<const vk::DescriptorSetLayout>(kTwoLayouts), std::span<const vk::PushConstantRange>(kOneRange));
    EXPECT_EQ(one_range_info.pushConstantRangeCount, 1U);

    const vk::PipelineLayoutCreateInfo two_range_info = buildPipelineLayoutCreateInfo(
      std::span<const vk::DescriptorSetLayout>(kTwoLayouts), std::span<const vk::PushConstantRange>(kTwoRanges));
    EXPECT_EQ(two_range_info.pushConstantRangeCount, 2U);
}

TEST(PipelineLayoutHelperUnit, OmittedPushConstantsGiveZeroAndNullptr)
{
    // The Clouds / deferred-lighting shape: no push constants at all.
    const vk::PipelineLayoutCreateInfo info =
      buildPipelineLayoutCreateInfo(std::span<const vk::DescriptorSetLayout>(kTwoLayouts));

    EXPECT_EQ(info.pushConstantRangeCount, 0U);
    EXPECT_EQ(info.pPushConstantRanges, nullptr);
}

TEST(PipelineLayoutHelperUnit, PointersPointAtTheCallersStorage)
{
    const vk::PipelineLayoutCreateInfo info = buildPipelineLayoutCreateInfo(
      std::span<const vk::DescriptorSetLayout>(kTwoLayouts), std::span<const vk::PushConstantRange>(kOneRange));

    EXPECT_EQ(info.pSetLayouts, kTwoLayouts.data());
    EXPECT_EQ(info.pPushConstantRanges, kOneRange.data());
}

TEST(PipelineLayoutHelperUnit, FlagsAreDefaulted)
{
    const vk::PipelineLayoutCreateInfo info =
      buildPipelineLayoutCreateInfo(std::span<const vk::DescriptorSetLayout>(kTwoLayouts));

    EXPECT_EQ(info.flags, vk::PipelineLayoutCreateFlags{});
}

}// namespace
