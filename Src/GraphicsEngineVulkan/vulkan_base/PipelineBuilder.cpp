module;
#include <cstdint>

#include <array>
#include <span>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "common/Utilities.hpp"

module kataglyphis.vulkan.pipeline_builder;

Kataglyphis::PipelineBuilder::PipelineBuilder() = default;

Kataglyphis::PipelineBuilder &Kataglyphis::PipelineBuilder::setShaderStages(
  std::vector<vk::PipelineShaderStageCreateInfo> in_shader_stages)
{
    shader_stages = std::move(in_shader_stages);
    return *this;
}

Kataglyphis::PipelineBuilder &Kataglyphis::PipelineBuilder::setVertexInput(
  std::vector<vk::VertexInputBindingDescription> bindings,
  std::vector<vk::VertexInputAttributeDescription> attributes)
{
    vertex_bindings = std::move(bindings);
    vertex_attributes = std::move(attributes);
    return *this;
}

Kataglyphis::PipelineBuilder &Kataglyphis::PipelineBuilder::setCullMode(vk::CullModeFlags in_cull_mode)
{
    cull_mode = in_cull_mode;
    return *this;
}

Kataglyphis::PipelineBuilder &Kataglyphis::PipelineBuilder::setAlphaBlending(bool enable)
{
    alpha_blending = enable;
    return *this;
}

Kataglyphis::PipelineBuilder &Kataglyphis::PipelineBuilder::setColorAttachmentCount(uint32_t count)
{
    color_attachment_count = count;
    return *this;
}

Kataglyphis::PipelineBuilder &Kataglyphis::PipelineBuilder::setUseColorBlendState(bool in_use_color_blend_state)
{
    use_color_blend_state = in_use_color_blend_state;
    return *this;
}

Kataglyphis::PipelineBuilder &Kataglyphis::PipelineBuilder::setDepthTest(bool enable)
{
    depth_test = enable;
    return *this;
}

Kataglyphis::PipelineBuilder &Kataglyphis::PipelineBuilder::setDepthWrite(bool enable)
{
    depth_write = enable;
    return *this;
}

Kataglyphis::PipelineBuilder &Kataglyphis::PipelineBuilder::setDepthCompareOp(vk::CompareOp compare_op)
{
    depth_compare_op = compare_op;
    return *this;
}

Kataglyphis::PipelineBuilder &Kataglyphis::PipelineBuilder::setDepthClamp(bool enable)
{
    depth_clamp = enable;
    return *this;
}

Kataglyphis::PipelineBuilder &Kataglyphis::PipelineBuilder::setDynamicCullMode(bool enable)
{
    dynamic_cull_mode = enable;
    return *this;
}

Kataglyphis::GraphicsPipelineState Kataglyphis::PipelineBuilder::buildState() const
{
    GraphicsPipelineState state;

    state.vertex_input_create_info.vertexBindingDescriptionCount = static_cast<uint32_t>(vertex_bindings.size());
    state.vertex_input_create_info.pVertexBindingDescriptions =
      vertex_bindings.empty() ? nullptr : vertex_bindings.data();
    state.vertex_input_create_info.vertexAttributeDescriptionCount =
      static_cast<uint32_t>(vertex_attributes.size());
    state.vertex_input_create_info.pVertexAttributeDescriptions =
      vertex_attributes.empty() ? nullptr : vertex_attributes.data();

    state.input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
    state.input_assembly.primitiveRestartEnable = VK_FALSE;

    state.viewport_state_create_info.viewportCount = 1;
    state.viewport_state_create_info.pViewports = nullptr;
    state.viewport_state_create_info.scissorCount = 1;
    state.viewport_state_create_info.pScissors = nullptr;

    // Dynamic cull mode is opt-in, so only the pass that needs per-draw culling pays for vkCmdSetCullMode.
    state.dynamic_states = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
    if (dynamic_cull_mode) { state.dynamic_states.push_back(vk::DynamicState::eCullMode); }
    state.dynamic_state_create_info.dynamicStateCount = static_cast<uint32_t>(state.dynamic_states.size());
    state.dynamic_state_create_info.pDynamicStates = state.dynamic_states.data();

    state.rasterizer_create_info.depthClampEnable = depth_clamp ? VK_TRUE : VK_FALSE;
    state.rasterizer_create_info.rasterizerDiscardEnable = VK_FALSE;
    state.rasterizer_create_info.polygonMode = vk::PolygonMode::eFill;
    state.rasterizer_create_info.lineWidth = 1.0F;
    state.rasterizer_create_info.cullMode = cull_mode;
    state.rasterizer_create_info.frontFace = kFrontFace;
    state.rasterizer_create_info.depthBiasEnable = VK_FALSE;

    state.multisample_create_info.sampleShadingEnable = VK_FALSE;
    state.multisample_create_info.rasterizationSamples = vk::SampleCountFlagBits::e1;

    state.depth_stencil_create_info.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
    state.depth_stencil_create_info.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
    state.depth_stencil_create_info.depthCompareOp = depth_compare_op;
    state.depth_stencil_create_info.depthBoundsTestEnable = VK_FALSE;
    state.depth_stencil_create_info.stencilTestEnable = VK_FALSE;

    vk::PipelineColorBlendAttachmentState color_state;
    color_state.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
                                 | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    if (alpha_blending) {
        color_state.blendEnable = VK_TRUE;
        color_state.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
        color_state.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
        color_state.colorBlendOp = vk::BlendOp::eAdd;
        color_state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        color_state.dstAlphaBlendFactor = vk::BlendFactor::eZero;
        color_state.alphaBlendOp = vk::BlendOp::eAdd;
    } else {
        color_state.blendEnable = VK_FALSE;
    }

    state.color_states.assign(color_attachment_count, color_state);

    state.color_blending.logicOpEnable = VK_FALSE;
    state.color_blending.attachmentCount = static_cast<uint32_t>(state.color_states.size());
    state.color_blending.pAttachments = state.color_states.data();
    state.use_color_blend_state = use_color_blend_state;

    return state;
}

vk::GraphicsPipelineCreateInfo Kataglyphis::linkGraphicsPipelineCreateInfo(const GraphicsPipelineState &state,
  std::span<const vk::PipelineShaderStageCreateInfo> stages,
  vk::PipelineLayout layout,
  vk::RenderPass render_pass,
  uint32_t subpass)
{
    vk::GraphicsPipelineCreateInfo graphics_pipeline_create_info;
    graphics_pipeline_create_info.stageCount = static_cast<uint32_t>(stages.size());
    graphics_pipeline_create_info.pStages = stages.data();
    graphics_pipeline_create_info.pVertexInputState = &state.vertex_input_create_info;
    graphics_pipeline_create_info.pInputAssemblyState = &state.input_assembly;
    graphics_pipeline_create_info.pViewportState = &state.viewport_state_create_info;
    graphics_pipeline_create_info.pDynamicState = &state.dynamic_state_create_info;
    graphics_pipeline_create_info.pRasterizationState = &state.rasterizer_create_info;
    graphics_pipeline_create_info.pMultisampleState = &state.multisample_create_info;
    graphics_pipeline_create_info.pDepthStencilState = &state.depth_stencil_create_info;
    graphics_pipeline_create_info.pColorBlendState = state.use_color_blend_state ? &state.color_blending : nullptr;
    graphics_pipeline_create_info.layout = layout;
    graphics_pipeline_create_info.renderPass = render_pass;
    graphics_pipeline_create_info.subpass = subpass;
    graphics_pipeline_create_info.basePipelineHandle = nullptr;
    graphics_pipeline_create_info.basePipelineIndex = -1;
    return graphics_pipeline_create_info;
}

vk::Pipeline Kataglyphis::PipelineBuilder::build(vk::Device device,
  vk::PipelineLayout pipeline_layout,
  vk::RenderPass render_pass,
  vk::PipelineCache pipeline_cache,
  uint32_t subpass,
  const char *error_message) const
{
    const GraphicsPipelineState state = buildState();
    const vk::GraphicsPipelineCreateInfo graphics_pipeline_create_info =
      linkGraphicsPipelineCreateInfo(state, shader_stages, pipeline_layout, render_pass, subpass);

    auto pipeline_result = device.createGraphicsPipelines(pipeline_cache, graphics_pipeline_create_info);
    if (pipeline_result.result != vk::Result::eSuccess) {
        ASSERT_VULKAN(pipeline_result.result, error_message);
    }
    return pipeline_result.value.front();
}
