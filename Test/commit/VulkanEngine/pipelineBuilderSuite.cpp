// buildState() assembles everything build() sets without a device; each *CallSite test re-creates a real caller.

#include <gtest/gtest.h>

#include <vulkan/vulkan.hpp>

import kataglyphis.vulkan.pipeline_builder;

using Kataglyphis::GraphicsPipelineState;
using Kataglyphis::linkGraphicsPipelineCreateInfo;
using Kataglyphis::PipelineBuilder;

namespace {

TEST(PipelineBuilderUnit, DefaultStateMatchesTheDocumentedDefaults)
{
    const GraphicsPipelineState state = PipelineBuilder().buildState();

    EXPECT_EQ(state.input_assembly.topology, vk::PrimitiveTopology::eTriangleList);
    EXPECT_EQ(state.input_assembly.primitiveRestartEnable, VK_FALSE);

    EXPECT_EQ(state.viewport_state_create_info.viewportCount, 1U);
    EXPECT_EQ(state.viewport_state_create_info.scissorCount, 1U);
    EXPECT_EQ(state.viewport_state_create_info.pViewports, nullptr);
    EXPECT_EQ(state.viewport_state_create_info.pScissors, nullptr);

    ASSERT_EQ(state.dynamic_states.size(), 2U);
    EXPECT_EQ(state.dynamic_states[0], vk::DynamicState::eViewport);
    EXPECT_EQ(state.dynamic_states[1], vk::DynamicState::eScissor);

    EXPECT_EQ(state.rasterizer_create_info.polygonMode, vk::PolygonMode::eFill);
    EXPECT_FLOAT_EQ(state.rasterizer_create_info.lineWidth, 1.0F);
    EXPECT_EQ(state.rasterizer_create_info.cullMode, vk::CullModeFlagBits::eBack);
    EXPECT_EQ(state.rasterizer_create_info.frontFace, vk::FrontFace::eCounterClockwise);
    EXPECT_EQ(state.rasterizer_create_info.depthClampEnable, VK_FALSE);

    EXPECT_EQ(state.multisample_create_info.rasterizationSamples, vk::SampleCountFlagBits::e1);

    EXPECT_EQ(state.depth_stencil_create_info.depthTestEnable, VK_TRUE);
    EXPECT_EQ(state.depth_stencil_create_info.depthWriteEnable, VK_TRUE);
    EXPECT_EQ(state.depth_stencil_create_info.depthCompareOp, vk::CompareOp::eLess);

    ASSERT_EQ(state.color_states.size(), 1U);
    EXPECT_EQ(state.color_states[0].blendEnable, VK_FALSE);
    EXPECT_TRUE(state.use_color_blend_state);
}

TEST(PipelineBuilderUnit, DynamicCullModeAppendsExactlyOneState)
{
    const GraphicsPipelineState enabled = PipelineBuilder().setDynamicCullMode(true).buildState();
    ASSERT_EQ(enabled.dynamic_states.size(), 3U);
    EXPECT_EQ(enabled.dynamic_states[2], vk::DynamicState::eCullMode);

    const GraphicsPipelineState disabled = PipelineBuilder().setDynamicCullMode(false).buildState();
    EXPECT_EQ(disabled.dynamic_states.size(), 2U);
}

TEST(PipelineBuilderUnit, AlphaBlendingUsesTheStandardFactors)
{
    const GraphicsPipelineState blending = PipelineBuilder().setAlphaBlending(true).buildState();
    ASSERT_EQ(blending.color_states.size(), 1U);
    const vk::PipelineColorBlendAttachmentState &attachment = blending.color_states[0];
    EXPECT_EQ(attachment.blendEnable, VK_TRUE);
    EXPECT_EQ(attachment.srcColorBlendFactor, vk::BlendFactor::eSrcAlpha);
    EXPECT_EQ(attachment.dstColorBlendFactor, vk::BlendFactor::eOneMinusSrcAlpha);
    EXPECT_EQ(attachment.colorBlendOp, vk::BlendOp::eAdd);
    EXPECT_EQ(attachment.srcAlphaBlendFactor, vk::BlendFactor::eOne);
    EXPECT_EQ(attachment.dstAlphaBlendFactor, vk::BlendFactor::eZero);
    EXPECT_EQ(attachment.alphaBlendOp, vk::BlendOp::eAdd);

    const GraphicsPipelineState no_blending = PipelineBuilder().buildState();
    EXPECT_EQ(no_blending.color_states[0].blendEnable, VK_FALSE);
}

TEST(PipelineBuilderUnit, ColorAttachmentCountReplicatesTheBlendState)
{
    const GraphicsPipelineState state = PipelineBuilder().setColorAttachmentCount(3).buildState();
    ASSERT_EQ(state.color_states.size(), 3U);
    EXPECT_EQ(state.color_blending.attachmentCount, 3U);
    for (const vk::PipelineColorBlendAttachmentState &attachment : state.color_states) {
        EXPECT_EQ(attachment.blendEnable, VK_FALSE);
    }
}

TEST(PipelineBuilderUnit, DepthOnlyPipelineHasNoColorBlendState)
{
    const GraphicsPipelineState state = PipelineBuilder().setUseColorBlendState(false).buildState();
    const vk::GraphicsPipelineCreateInfo info =
      linkGraphicsPipelineCreateInfo(state, {}, vk::PipelineLayout(nullptr), vk::RenderPass(nullptr), 0);
    EXPECT_EQ(info.pColorBlendState, nullptr);
}

TEST(PipelineBuilderUnit, EmptyVertexInputCarriesNullPointers)
{
    const GraphicsPipelineState state = PipelineBuilder().setVertexInput({}, {}).buildState();
    EXPECT_EQ(state.vertex_input_create_info.vertexBindingDescriptionCount, 0U);
    EXPECT_EQ(state.vertex_input_create_info.vertexAttributeDescriptionCount, 0U);
    EXPECT_EQ(state.vertex_input_create_info.pVertexBindingDescriptions, nullptr);
    EXPECT_EQ(state.vertex_input_create_info.pVertexAttributeDescriptions, nullptr);
}

TEST(PipelineBuilderUnit, MatchesForwardRasterizerCallSite)
{
    // Rasterizer.cpp: dynamic cull mode serves doubleSided glTF meshes.
    const GraphicsPipelineState state = PipelineBuilder().setAlphaBlending(true).setDynamicCullMode(true).buildState();

    EXPECT_EQ(state.color_states[0].blendEnable, VK_TRUE);
    ASSERT_EQ(state.dynamic_states.size(), 3U);
    EXPECT_EQ(state.dynamic_states[2], vk::DynamicState::eCullMode);
    EXPECT_EQ(state.rasterizer_create_info.cullMode, vk::CullModeFlagBits::eBack);
    EXPECT_EQ(state.depth_stencil_create_info.depthTestEnable, VK_TRUE);
}

TEST(PipelineBuilderUnit, MatchesDeferredGeometryCallSite)
{
    // DeferredRasterizer.cpp geometry pipeline: three G-buffer attachments.
    const GraphicsPipelineState state =
      PipelineBuilder().setColorAttachmentCount(3).setDynamicCullMode(true).buildState();

    ASSERT_EQ(state.color_states.size(), 3U);
    EXPECT_EQ(state.color_blending.attachmentCount, 3U);
    ASSERT_EQ(state.dynamic_states.size(), 3U);
    EXPECT_EQ(state.dynamic_states[2], vk::DynamicState::eCullMode);
}

TEST(PipelineBuilderUnit, MatchesDeferredLightingCallSite)
{
    // DeferredRasterizer.cpp lighting pipeline: a vertex-less fullscreen triangle.
    const GraphicsPipelineState state = PipelineBuilder()
                                          .setVertexInput({}, {})
                                          .setCullMode(vk::CullModeFlagBits::eNone)
                                          .setDepthTest(false)
                                          .setDepthWrite(false)
                                          .buildState();

    EXPECT_EQ(state.vertex_input_create_info.pVertexBindingDescriptions, nullptr);
    EXPECT_EQ(state.rasterizer_create_info.cullMode, vk::CullModeFlagBits::eNone);
    EXPECT_EQ(state.depth_stencil_create_info.depthTestEnable, VK_FALSE);
    EXPECT_EQ(state.depth_stencil_create_info.depthWriteEnable, VK_FALSE);
}

TEST(PipelineBuilderUnit, MatchesPostStageCallSite)
{
    // PostStage.cpp: blending composites the tonemapped result over the swapchain.
    const GraphicsPipelineState state = PipelineBuilder()
                                          .setCullMode(vk::CullModeFlagBits::eNone)
                                          .setAlphaBlending(true)
                                          .setDepthCompareOp(vk::CompareOp::eLessOrEqual)
                                          .buildState();

    EXPECT_EQ(state.rasterizer_create_info.cullMode, vk::CullModeFlagBits::eNone);
    EXPECT_EQ(state.color_states[0].blendEnable, VK_TRUE);
    EXPECT_EQ(state.depth_stencil_create_info.depthCompareOp, vk::CompareOp::eLessOrEqual);
}

TEST(PipelineBuilderUnit, MatchesCascadedShadowMapCallSite)
{
    // CascadedShadowMap.cpp: culling must be off, since the cascade projections lack the Y flip.
    const GraphicsPipelineState clamp_on = PipelineBuilder()
                                             .setCullMode(vk::CullModeFlagBits::eNone)
                                             .setDepthClamp(true)
                                             .setUseColorBlendState(false)
                                             .buildState();
    EXPECT_EQ(clamp_on.rasterizer_create_info.cullMode, vk::CullModeFlagBits::eNone);
    EXPECT_EQ(clamp_on.rasterizer_create_info.depthClampEnable, VK_TRUE);
    EXPECT_FALSE(clamp_on.use_color_blend_state);

    const GraphicsPipelineState clamp_off = PipelineBuilder()
                                              .setCullMode(vk::CullModeFlagBits::eNone)
                                              .setDepthClamp(false)
                                              .setUseColorBlendState(false)
                                              .buildState();
    EXPECT_EQ(clamp_off.rasterizer_create_info.depthClampEnable, VK_FALSE);

    const vk::GraphicsPipelineCreateInfo info =
      linkGraphicsPipelineCreateInfo(clamp_on, {}, vk::PipelineLayout(nullptr), vk::RenderPass(nullptr), 0);
    EXPECT_EQ(info.pColorBlendState, nullptr);
}

TEST(PipelineBuilderUnit, MatchesSkyBoxCallSite)
{
    // SkyBox.cpp: drawn behind everything, so no depth test or write.
    const GraphicsPipelineState state = PipelineBuilder()
                                          .setCullMode(vk::CullModeFlagBits::eNone)
                                          .setDepthTest(false)
                                          .setDepthWrite(false)
                                          .setDepthCompareOp(vk::CompareOp::eAlways)
                                          .buildState();

    EXPECT_EQ(state.rasterizer_create_info.cullMode, vk::CullModeFlagBits::eNone);
    EXPECT_EQ(state.depth_stencil_create_info.depthTestEnable, VK_FALSE);
    EXPECT_EQ(state.depth_stencil_create_info.depthWriteEnable, VK_FALSE);
    EXPECT_EQ(state.depth_stencil_create_info.depthCompareOp, vk::CompareOp::eAlways);
}

}// namespace
