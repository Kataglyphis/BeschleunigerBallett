#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include "common/ShaderStageHelper.hpp"

using Kataglyphis::buildGeneralShaderGroup;
using Kataglyphis::buildShaderStageCreateInfo;
using Kataglyphis::buildTrianglesHitGroup;

static_assert(buildShaderStageCreateInfo(vk::ShaderStageFlagBits::eRaygenKHR, vk::ShaderModule(nullptr)).stage
                == vk::ShaderStageFlagBits::eRaygenKHR,
  "buildShaderStageCreateInfo must be usable in a constant expression");

namespace {

TEST(ShaderStageHelperUnit, EntryPointIsMainForEveryStageBitTheEngineUses)
{
    const std::array<vk::ShaderStageFlagBits, 5> stages_used = { vk::ShaderStageFlagBits::eVertex,
        vk::ShaderStageFlagBits::eFragment,
        vk::ShaderStageFlagBits::eRaygenKHR,
        vk::ShaderStageFlagBits::eMissKHR,
        vk::ShaderStageFlagBits::eClosestHitKHR };

    for (const vk::ShaderStageFlagBits stage : stages_used) {
        const vk::PipelineShaderStageCreateInfo info = buildShaderStageCreateInfo(stage, vk::ShaderModule(nullptr));
        EXPECT_EQ(info.stage, stage);
        EXPECT_EQ(std::string_view{ info.pName }, "main");
    }
}

TEST(ShaderStageHelperUnit, FlagsDefaultToNone)
{
    const vk::PipelineShaderStageCreateInfo info =
      buildShaderStageCreateInfo(vk::ShaderStageFlagBits::eVertex, vk::ShaderModule(nullptr));
    EXPECT_EQ(info.flags, vk::PipelineShaderStageCreateFlags{});
}

TEST(ShaderStageHelperUnit, GeneralShaderGroupSetsOnlyTheGeneralIndex)
{
    const vk::RayTracingShaderGroupCreateInfoKHR group = buildGeneralShaderGroup(3);
    EXPECT_EQ(group.type, vk::RayTracingShaderGroupTypeKHR::eGeneral);
    EXPECT_EQ(group.generalShader, 3u);
    EXPECT_EQ(group.closestHitShader, VK_SHADER_UNUSED_KHR);
    EXPECT_EQ(group.anyHitShader, VK_SHADER_UNUSED_KHR);
    EXPECT_EQ(group.intersectionShader, VK_SHADER_UNUSED_KHR);
}

TEST(ShaderStageHelperUnit, TrianglesHitGroupSetsOnlyTheClosestHitIndex)
{
    const vk::RayTracingShaderGroupCreateInfoKHR group = buildTrianglesHitGroup(2);
    EXPECT_EQ(group.type, vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup);
    EXPECT_EQ(group.generalShader, VK_SHADER_UNUSED_KHR);
    EXPECT_EQ(group.closestHitShader, 2u);
    EXPECT_EQ(group.anyHitShader, VK_SHADER_UNUSED_KHR);
    EXPECT_EQ(group.intersectionShader, VK_SHADER_UNUSED_KHR);
}

TEST(ShaderStageHelperUnit, TrianglesHitGroupWithAnyHitSetsBothIndices)
{
    const vk::RayTracingShaderGroupCreateInfoKHR group = buildTrianglesHitGroup(2, 4);
    EXPECT_EQ(group.type, vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup);
    EXPECT_EQ(group.generalShader, VK_SHADER_UNUSED_KHR);
    EXPECT_EQ(group.closestHitShader, 2u);
    EXPECT_EQ(group.anyHitShader, 4u);
    EXPECT_EQ(group.intersectionShader, VK_SHADER_UNUSED_KHR);
}

}// namespace
