module;
#include <cstdint>

#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.pipeline_builder;

export namespace Kataglyphis {

// The device-free half of PipelineBuilder::build: every sub-state a
// vk::GraphicsPipelineCreateInfo points at, assembled without touching a
// vk::Device. Freely movable - dynamic_states/color_states are owned here,
// and moving a std::vector never reallocates its buffer, so the pointers
// dynamic_state_create_info.pDynamicStates and color_blending.pAttachments
// stay valid across the move out of PipelineBuilder::buildState().
struct GraphicsPipelineState
{
    vk::PipelineVertexInputStateCreateInfo vertex_input_create_info;
    vk::PipelineInputAssemblyStateCreateInfo input_assembly;
    vk::PipelineViewportStateCreateInfo viewport_state_create_info;
    std::vector<vk::DynamicState> dynamic_states;
    vk::PipelineDynamicStateCreateInfo dynamic_state_create_info;
    vk::PipelineRasterizationStateCreateInfo rasterizer_create_info;
    vk::PipelineMultisampleStateCreateInfo multisample_create_info;
    vk::PipelineDepthStencilStateCreateInfo depth_stencil_create_info;
    std::vector<vk::PipelineColorBlendAttachmentState> color_states;
    vk::PipelineColorBlendStateCreateInfo color_blending;
    bool use_color_blend_state = true;
};

// Wires the shader stages, pipeline layout, render pass and subpass onto a
// GraphicsPipelineState and returns the top-level create-info ready for
// vk::Device::createGraphicsPipelines. Borrows into both state and stages -
// both must outlive the createGraphicsPipelines call that consumes the
// returned value, the same lifetime rule PipelineLayoutHelper.hpp documents.
[[nodiscard]] vk::GraphicsPipelineCreateInfo linkGraphicsPipelineCreateInfo(const GraphicsPipelineState &state,
  std::span<const vk::PipelineShaderStageCreateInfo> stages,
  vk::PipelineLayout layout,
  vk::RenderPass render_pass,
  uint32_t subpass);

// Fluent builder for the graphics pipeline construction that was previously
// copy-pasted across the render stages. Defaults match the most common
// variant: triangle list, dynamic viewport/scissor, fill mode, back-face
// culling, counter-clockwise front face, no blending on a single color
// attachment, depth test + write with CompareOp::eLess, single sample.
// Pipeline LAYOUT creation stays in the stages; this builder only builds
// the vk::Pipeline.
class PipelineBuilder
{
  public:
    PipelineBuilder();

    PipelineBuilder &setShaderStages(std::vector<vk::PipelineShaderStageCreateInfo> shader_stages);
    PipelineBuilder &setVertexInput(std::vector<vk::VertexInputBindingDescription> bindings,
      std::vector<vk::VertexInputAttributeDescription> attributes);
    PipelineBuilder &setCullMode(vk::CullModeFlags cull_mode);
    // When enabled uses the standard alpha blending factors
    // (srcAlpha/oneMinusSrcAlpha for color, one/zero for alpha, both eAdd).
    PipelineBuilder &setAlphaBlending(bool enable);
    PipelineBuilder &setColorAttachmentCount(uint32_t count);
    // Pass false for depth-only pipelines (no pColorBlendState at all).
    PipelineBuilder &setUseColorBlendState(bool use_color_blend_state);
    PipelineBuilder &setDepthTest(bool enable);
    PipelineBuilder &setDepthWrite(bool enable);
    PipelineBuilder &setDepthCompareOp(vk::CompareOp compare_op);
    // Depth clamp ("shadow pancaking"): fragments nearer than the near plane
    // clamp to depth 0 instead of being CLIPPED. Only legal when the device
    // feature is enabled - pass VulkanDevice::supportsDepthClamp().
    PipelineBuilder &setDepthClamp(bool enable);
    // Adds VK_DYNAMIC_STATE_CULL_MODE and VK_DYNAMIC_STATE_FRONT_FACE, so every
    // draw sets both (vkCmdSetCullMode, vkCmdSetFrontFace). Core in Vulkan 1.3,
    // which the engine targets. Opt-in: off by default, so every other pipeline
    // keeps its static state. Used by the forward and deferred geometry passes:
    // doubleSided glTF meshes skip culling, mirrored models wind clockwise.
    PipelineBuilder &setDynamicCulling(bool enable);

    // Assembles every sub-state build() needs without touching a vk::Device -
    // see GraphicsPipelineState. Pair with linkGraphicsPipelineCreateInfo to
    // reproduce build()'s result one call site at a time in a CPU-only test.
    [[nodiscard]] GraphicsPipelineState buildState() const;

    // Creates the pipeline via createGraphicsPipelines(pipeline_cache, ...);
    // aborts through ASSERT_VULKAN with error_message on failure.
    // pipeline_cache may be null (no caching).
    [[nodiscard]] vk::Pipeline build(vk::Device device,
      vk::PipelineLayout pipeline_layout,
      vk::RenderPass render_pass,
      vk::PipelineCache pipeline_cache = nullptr,
      uint32_t subpass = 0,
      const char *error_message = "Failed to create a graphics pipeline!") const;

  private:
    std::vector<vk::PipelineShaderStageCreateInfo> shader_stages;
    std::vector<vk::VertexInputBindingDescription> vertex_bindings;
    std::vector<vk::VertexInputAttributeDescription> vertex_attributes;
    // No setter - CascadedShadowMap.cpp reasons about this exact default
    // explicitly (culling is disabled there instead of flipping the front face
    // to match the projection's Y-flip). setDynamicCulling overrides it per draw.
    static constexpr vk::FrontFace kFrontFace = vk::FrontFace::eCounterClockwise;

    vk::CullModeFlags cull_mode = vk::CullModeFlagBits::eBack;
    bool alpha_blending = false;
    uint32_t color_attachment_count = 1;
    bool use_color_blend_state = true;
    bool depth_test = true;
    bool depth_write = true;
    vk::CompareOp depth_compare_op = vk::CompareOp::eLess;
    bool depth_clamp = false;
    bool dynamic_culling = false;
};
}// namespace Kataglyphis
