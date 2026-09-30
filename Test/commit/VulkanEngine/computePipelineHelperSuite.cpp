#include <gtest/gtest.h>

#include <string_view>

#include "common/ComputePipelineHelper.hpp"

using Kataglyphis::buildComputePipelineCreateInfo;
using Kataglyphis::buildComputeShaderStageCreateInfo;

static_assert(buildComputeShaderStageCreateInfo(vk::ShaderModule(nullptr)).stage == vk::ShaderStageFlagBits::eCompute,
  "buildComputeShaderStageCreateInfo must be usable in a constant expression");

// Handle operator== is not constexpr in this vulkan-hpp, so .flags proves the call is a constant expression.
static_assert(
  buildComputePipelineCreateInfo(buildComputeShaderStageCreateInfo(vk::ShaderModule(nullptr)), vk::PipelineLayout(nullptr))
      .flags
    == vk::PipelineCreateFlags{},
  "buildComputePipelineCreateInfo must be usable in a constant expression");

namespace {

TEST(ComputePipelineHelperUnit, StageIsComputeAndEntryPointIsMain)
{
    const vk::PipelineShaderStageCreateInfo info = buildComputeShaderStageCreateInfo(vk::ShaderModule(nullptr));
    EXPECT_EQ(info.stage, vk::ShaderStageFlagBits::eCompute);
    EXPECT_EQ(std::string_view{ info.pName }, "main");
}

TEST(ComputePipelineHelperUnit, CreateInfoCarriesTheStageAndLayoutUnchanged)
{
    const vk::PipelineShaderStageCreateInfo stage = buildComputeShaderStageCreateInfo(vk::ShaderModule(nullptr));
    const vk::PipelineLayout layout(nullptr);

    const vk::ComputePipelineCreateInfo info = buildComputePipelineCreateInfo(stage, layout);

    EXPECT_EQ(info.stage.stage, stage.stage);
    EXPECT_EQ(info.stage.module, stage.module);
    EXPECT_EQ(info.layout, layout);
}

TEST(ComputePipelineHelperUnit, FlagsDefaultToNone)
{
    const vk::PipelineShaderStageCreateInfo stage = buildComputeShaderStageCreateInfo(vk::ShaderModule(nullptr));
    const vk::ComputePipelineCreateInfo info = buildComputePipelineCreateInfo(stage, vk::PipelineLayout(nullptr));

    EXPECT_EQ(info.flags, vk::PipelineCreateFlags{});
}

}// namespace
