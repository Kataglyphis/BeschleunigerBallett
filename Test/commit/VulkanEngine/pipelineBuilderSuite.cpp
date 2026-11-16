// buildState() assembles everything build() sets without a device; each *CallSite test re-creates a real caller.

#include <gtest/gtest.h>

#include <vulkan/vulkan.hpp>

import kataglyphis.vulkan.pipeline_builder;

using Kataglyphis::GraphicsPipelineState;
using Kataglyphis::linkGraphicsPipelineCreateInfo;
using Kataglyphis::PipelineBuilder;

namespace {

constexpr vk::Format kColor = vk::Format::eR16G16B16A16Sfloat;
constexpr vk::Format kDepth = vk::Format::eD32Sfloat;

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

    // No default attachment: a pipeline without setRenderingFormats renders into nothing.
    EXPECT_TRUE(state.color_states.empty());
    EXPECT_FALSE(state.use_color_blend_state);
    EXPECT_EQ(state.rendering_create_info.colorAttachmentCount, 0U);
    EXPECT_EQ(state.rendering_create_info.pColorAttachmentFormats, nullptr);
    EXPECT_EQ(state.rendering_create_info.depthAttachmentFormat, vk::Format::eUndefined);
    EXPECT_EQ(state.rendering_create_info.stencilAttachmentFormat, vk::Format::eUndefined);
    EXPECT_EQ(state.rendering_create_info.viewMask, 0U);
}

TEST(PipelineBuilderUnit, DynamicCullingAppendsCullModeAndFrontFace)
{
    const GraphicsPipelineState enabled = PipelineBuilder().setDynamicCulling(true).buildState();
    ASSERT_EQ(enabled.dynamic_states.size(), 4U);
    EXPECT_EQ(enabled.dynamic_states[2], vk::DynamicState::eCullMode);
    EXPECT_EQ(enabled.dynamic_states[3], vk::DynamicState::eFrontFace);

    const GraphicsPipelineState disabled = PipelineBuilder().setDynamicCulling(false).buildState();
    EXPECT_EQ(disabled.dynamic_states.size(), 2U);
}

TEST(PipelineBuilderUnit, AlphaBlendingUsesTheStandardFactors)
{
    const GraphicsPipelineState blending =
      PipelineBuilder().setRenderingFormats({ kColor }).setAlphaBlending(true).buildState();
    ASSERT_EQ(blending.color_states.size(), 1U);
    const vk::PipelineColorBlendAttachmentState &attachment = blending.color_states[0];
    EXPECT_EQ(attachment.blendEnable, VK_TRUE);
    EXPECT_EQ(attachment.srcColorBlendFactor, vk::BlendFactor::eSrcAlpha);
    EXPECT_EQ(attachment.dstColorBlendFactor, vk::BlendFactor::eOneMinusSrcAlpha);
    EXPECT_EQ(attachment.colorBlendOp, vk::BlendOp::eAdd);
    EXPECT_EQ(attachment.srcAlphaBlendFactor, vk::BlendFactor::eOne);
    EXPECT_EQ(attachment.dstAlphaBlendFactor, vk::BlendFactor::eZero);
    EXPECT_EQ(attachment.alphaBlendOp, vk::BlendOp::eAdd);

    const GraphicsPipelineState no_blending = PipelineBuilder().setRenderingFormats({ kColor }).buildState();
    ASSERT_EQ(no_blending.color_states.size(), 1U);
    EXPECT_EQ(no_blending.color_states[0].blendEnable, VK_FALSE);
}

TEST(PipelineBuilderUnit, EachColorFormatGetsOneBlendState)
{
    const GraphicsPipelineState state = PipelineBuilder().setRenderingFormats({ kColor, kColor, kColor }).buildState();
    ASSERT_EQ(state.color_states.size(), 3U);
    EXPECT_EQ(state.color_blending.attachmentCount, 3U);
    EXPECT_TRUE(state.use_color_blend_state);
    for (const vk::PipelineColorBlendAttachmentState &attachment : state.color_states) {
        EXPECT_EQ(attachment.blendEnable, VK_FALSE);
    }
}

TEST(PipelineBuilderUnit, RenderingFormatsReachThePipelineRenderingCreateInfo)
{
    const GraphicsPipelineState state =
      PipelineBuilder().setRenderingFormats({ kColor, vk::Format::eR8G8B8A8Srgb }, kDepth).buildState();

    ASSERT_EQ(state.rendering_create_info.colorAttachmentCount, 2U);
    ASSERT_NE(state.rendering_create_info.pColorAttachmentFormats, nullptr);
    EXPECT_EQ(state.rendering_create_info.pColorAttachmentFormats[0], kColor);
    EXPECT_EQ(state.rendering_create_info.pColorAttachmentFormats[1], vk::Format::eR8G8B8A8Srgb);
    // The formats point into the state itself, so moving it keeps them valid.
    EXPECT_EQ(state.rendering_create_info.pColorAttachmentFormats, state.color_formats.data());
    EXPECT_EQ(state.rendering_create_info.depthAttachmentFormat, kDepth);
    EXPECT_EQ(state.rendering_create_info.stencilAttachmentFormat, vk::Format::eUndefined);
}

TEST(PipelineBuilderUnit, LinkChainsTheRenderingInfoInsteadOfARenderPass)
{
    const GraphicsPipelineState state = PipelineBuilder().setRenderingFormats({ kColor }, kDepth).buildState();
    const vk::GraphicsPipelineCreateInfo info = linkGraphicsPipelineCreateInfo(state, {}, vk::PipelineLayout(nullptr));

    EXPECT_EQ(info.pNext, &state.rendering_create_info);
    EXPECT_EQ(info.renderPass, vk::RenderPass(nullptr));
    EXPECT_EQ(info.subpass, 0U);
    EXPECT_EQ(info.pColorBlendState, &state.color_blending);
    EXPECT_EQ(info.pDepthStencilState, &state.depth_stencil_create_info);
}

TEST(PipelineBuilderUnit, DepthOnlyPipelineHasNoColorBlendState)
{
    const GraphicsPipelineState state = PipelineBuilder().setRenderingFormats({}, kDepth).buildState();
    const vk::GraphicsPipelineCreateInfo info = linkGraphicsPipelineCreateInfo(state, {}, vk::PipelineLayout(nullptr));
    EXPECT_FALSE(state.use_color_blend_state);
    EXPECT_EQ(info.pColorBlendState, nullptr);
    EXPECT_EQ(state.rendering_create_info.colorAttachmentCount, 0U);
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
    // Rasterizer.cpp: dynamic culling serves doubleSided glTF meshes and mirrored models.
    const GraphicsPipelineState state = PipelineBuilder()
                                          .setAlphaBlending(true)
                                          .setDynamicCulling(true)
                                          .setRenderingFormats({ kColor }, kDepth)
                                          .buildState();

    ASSERT_EQ(state.color_states.size(), 1U);
    EXPECT_EQ(state.color_states[0].blendEnable, VK_TRUE);
    ASSERT_EQ(state.dynamic_states.size(), 4U);
    EXPECT_EQ(state.dynamic_states[2], vk::DynamicState::eCullMode);
    EXPECT_EQ(state.dynamic_states[3], vk::DynamicState::eFrontFace);
    EXPECT_EQ(state.rasterizer_create_info.cullMode, vk::CullModeFlagBits::eBack);
    EXPECT_EQ(state.depth_stencil_create_info.depthTestEnable, VK_TRUE);
    EXPECT_EQ(state.rendering_create_info.depthAttachmentFormat, kDepth);
}

TEST(PipelineBuilderUnit, CoverageAccumulationKeepsAnOpaquePixelOpaque)
{
    const GraphicsPipelineState state = PipelineBuilder()
                                          .setRenderingFormats({ kColor })
                                          .setAlphaBlending(true)
                                          .setCoverageAccumulation(true)
                                          .buildState();
    ASSERT_EQ(state.color_states.size(), 1U);
    const vk::PipelineColorBlendAttachmentState &attachment = state.color_states[0];
    EXPECT_EQ(attachment.srcColorBlendFactor, vk::BlendFactor::eSrcAlpha);
    EXPECT_EQ(attachment.dstColorBlendFactor, vk::BlendFactor::eOneMinusSrcAlpha);
    // alpha = src.a + dst.a * (1 - src.a): 1 over an opaque pixel, src.a over a cleared one.
    EXPECT_EQ(attachment.srcAlphaBlendFactor, vk::BlendFactor::eOne);
    EXPECT_EQ(attachment.dstAlphaBlendFactor, vk::BlendFactor::eOneMinusSrcAlpha);
}

TEST(PipelineBuilderUnit, MatchesTransparentSceneCallSite)
{
    // Rasterizer.cpp's buildTransparentScenePipeline: glTF BLEND tests the opaque depth but never writes it.
    const GraphicsPipelineState state = PipelineBuilder()
                                          .setAlphaBlending(true)
                                          .setCoverageAccumulation(true)
                                          .setDepthWrite(false)
                                          .setDynamicCulling(true)
                                          .setRenderingFormats({ kColor }, kDepth)
                                          .buildState();

    ASSERT_EQ(state.color_states.size(), 1U);
    EXPECT_EQ(state.color_states[0].blendEnable, VK_TRUE);
    EXPECT_EQ(state.color_states[0].dstAlphaBlendFactor, vk::BlendFactor::eOneMinusSrcAlpha);
    EXPECT_EQ(state.depth_stencil_create_info.depthTestEnable, VK_TRUE);
    EXPECT_EQ(state.depth_stencil_create_info.depthWriteEnable, VK_FALSE);
    EXPECT_EQ(state.depth_stencil_create_info.depthCompareOp, vk::CompareOp::eLess);
    ASSERT_EQ(state.dynamic_states.size(), 4U);
    EXPECT_EQ(state.rendering_create_info.depthAttachmentFormat, kDepth);
}

TEST(PipelineBuilderUnit, MatchesDeferredGeometryCallSite)
{
    // DeferredRasterizer.cpp geometry pipeline: three G-buffer attachments and depth.
    const GraphicsPipelineState state = PipelineBuilder()
                                          .setDynamicCulling(true)
                                          .setRenderingFormats({ kColor, vk::Format::eR8G8B8A8Srgb, kColor }, kDepth)
                                          .buildState();

    ASSERT_EQ(state.color_states.size(), 3U);
    EXPECT_EQ(state.color_blending.attachmentCount, 3U);
    EXPECT_EQ(state.rendering_create_info.colorAttachmentCount, 3U);
    ASSERT_EQ(state.dynamic_states.size(), 4U);
    EXPECT_EQ(state.dynamic_states[2], vk::DynamicState::eCullMode);
    EXPECT_EQ(state.dynamic_states[3], vk::DynamicState::eFrontFace);
}

TEST(PipelineBuilderUnit, MatchesDeferredLightingCallSite)
{
    // DeferredRasterizer.cpp lighting pipeline: a vertex-less fullscreen triangle with no depth attachment.
    const GraphicsPipelineState state = PipelineBuilder()
                                          .setVertexInput({}, {})
                                          .setCullMode(vk::CullModeFlagBits::eNone)
                                          .setDepthTest(false)
                                          .setDepthWrite(false)
                                          .setRenderingFormats({ kColor })
                                          .buildState();

    EXPECT_EQ(state.vertex_input_create_info.pVertexBindingDescriptions, nullptr);
    EXPECT_EQ(state.rasterizer_create_info.cullMode, vk::CullModeFlagBits::eNone);
    EXPECT_EQ(state.depth_stencil_create_info.depthTestEnable, VK_FALSE);
    EXPECT_EQ(state.depth_stencil_create_info.depthWriteEnable, VK_FALSE);
    EXPECT_EQ(state.rendering_create_info.depthAttachmentFormat, vk::Format::eUndefined);
}

TEST(PipelineBuilderUnit, MatchesPostStageCallSite)
{
    // PostStage.cpp: blending composites the tonemapped result over the swapchain.
    const GraphicsPipelineState state = PipelineBuilder()
                                          .setCullMode(vk::CullModeFlagBits::eNone)
                                          .setAlphaBlending(true)
                                          .setDepthTest(false)
                                          .setDepthWrite(false)
                                          .setRenderingFormats({ vk::Format::eB8G8R8A8Unorm })
                                          .buildState();

    EXPECT_EQ(state.rasterizer_create_info.cullMode, vk::CullModeFlagBits::eNone);
    ASSERT_EQ(state.color_states.size(), 1U);
    EXPECT_EQ(state.color_states[0].blendEnable, VK_TRUE);
    EXPECT_EQ(state.depth_stencil_create_info.depthTestEnable, VK_FALSE);
    EXPECT_EQ(state.rendering_create_info.pColorAttachmentFormats[0], vk::Format::eB8G8R8A8Unorm);
}

TEST(PipelineBuilderUnit, MatchesCascadedShadowMapCallSite)
{
    // CascadedShadowMap.cpp: culling must be off, since the cascade projections lack the Y flip.
    const uint32_t three_cascades = 0b111U;
    const GraphicsPipelineState clamp_on = PipelineBuilder()
                                             .setCullMode(vk::CullModeFlagBits::eNone)
                                             .setDepthClamp(true)
                                             .setRenderingFormats({}, kDepth)
                                             .setViewMask(three_cascades)
                                             .buildState();
    EXPECT_EQ(clamp_on.rasterizer_create_info.cullMode, vk::CullModeFlagBits::eNone);
    EXPECT_EQ(clamp_on.rasterizer_create_info.depthClampEnable, VK_TRUE);
    EXPECT_FALSE(clamp_on.use_color_blend_state);
    // Multiview: the pipeline names the same mask as the pass's vkCmdBeginRendering.
    EXPECT_EQ(clamp_on.rendering_create_info.viewMask, three_cascades);
    EXPECT_EQ(clamp_on.rendering_create_info.depthAttachmentFormat, kDepth);

    const GraphicsPipelineState clamp_off = PipelineBuilder()
                                              .setCullMode(vk::CullModeFlagBits::eNone)
                                              .setDepthClamp(false)
                                              .setRenderingFormats({}, kDepth)
                                              .buildState();
    EXPECT_EQ(clamp_off.rasterizer_create_info.depthClampEnable, VK_FALSE);

    const vk::GraphicsPipelineCreateInfo info =
      linkGraphicsPipelineCreateInfo(clamp_on, {}, vk::PipelineLayout(nullptr));
    EXPECT_EQ(info.pColorBlendState, nullptr);
}

TEST(PipelineBuilderUnit, MatchesSkyBoxCallSite)
{
    // SkyBox.cpp: drawn behind everything, so no depth test or write, into the swapchain format.
    const GraphicsPipelineState state = PipelineBuilder()
                                          .setCullMode(vk::CullModeFlagBits::eNone)
                                          .setDepthTest(false)
                                          .setDepthWrite(false)
                                          .setDepthCompareOp(vk::CompareOp::eAlways)
                                          .setRenderingFormats({ vk::Format::eB8G8R8A8Unorm })
                                          .buildState();

    EXPECT_EQ(state.rasterizer_create_info.cullMode, vk::CullModeFlagBits::eNone);
    EXPECT_EQ(state.depth_stencil_create_info.depthTestEnable, VK_FALSE);
    EXPECT_EQ(state.depth_stencil_create_info.depthWriteEnable, VK_FALSE);
    EXPECT_EQ(state.depth_stencil_create_info.depthCompareOp, vk::CompareOp::eAlways);
    EXPECT_EQ(state.rendering_create_info.colorAttachmentCount, 1U);
}

}// namespace
