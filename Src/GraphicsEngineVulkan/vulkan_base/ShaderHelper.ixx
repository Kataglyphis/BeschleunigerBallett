module;
#include <memory>

#include <array>
#include <span>
#include <string>
#include <vulkan/vulkan.hpp>

#include "common/ComputePipelineHelper.hpp"

export module kataglyphis.vulkan.shader_helper;

import kataglyphis.vulkan.device;

export namespace Kataglyphis {

// Structural check that `code` looks like a compiled SPIR-V module: non-empty,
// a multiple of 4 bytes, and starting with the SPIR-V magic number. Pure - no
// Vulkan calls - so it is unit-testable without a device.
auto validateSpirvBlob(std::span<const char> code) -> bool;

// Reads spvPath, validates it with validateSpirvBlob, and creates the
// vk::ShaderModule. A missing file or a blob that fails validation logs
// critical (with a hint to (re)compile shaders) and aborts - shader loading
// happens during stage init, where this codebase already treats a Vulkan
// creation failure as unrecoverable.
auto loadSpirvShaderModule(const std::shared_ptr<VulkanDevice> &device, const std::string &spvPath) -> vk::ShaderModule;

// Owns the vertex + fragment vk::ShaderModule pair every pipeline-creation
// site loads, plus the two vk::PipelineShaderStageCreateInfo that reference
// them. Loads both modules via loadSpirvShaderModule (aborting exactly as
// that function does on a bad blob) and destroys both on destruction, so a
// call site no longer hand-writes the destroyShaderModule pair. Move-only
// would still leave the create-infos pointing at a module owned by a
// relocated instance for no benefit at these call sites (every use is a
// function-local), so copy AND move are both deleted instead.
class ShaderStagePair
{
  public:
    ShaderStagePair(std::shared_ptr<VulkanDevice> device,// DEVICE_SINK_OK: moved into member
      const std::string &vertexSpvPath,
      const std::string &fragmentSpvPath);

    ShaderStagePair(const ShaderStagePair &) = delete;
    ShaderStagePair &operator=(const ShaderStagePair &) = delete;
    ShaderStagePair(ShaderStagePair &&) = delete;
    ShaderStagePair &operator=(ShaderStagePair &&) = delete;

    ~ShaderStagePair();

    [[nodiscard]] auto stages() const -> std::span<const vk::PipelineShaderStageCreateInfo> { return stages_; }

  private:
    std::shared_ptr<VulkanDevice> device_;
    vk::ShaderModule vertexModule_{ nullptr };
    vk::ShaderModule fragmentModule_{ nullptr };
    std::array<vk::PipelineShaderStageCreateInfo, 2> stages_{};
};

// The layout + pipeline pair every compute-pass createPipeline() produces.
// Returned as a struct rather than via two out-params so a call site can
// assign both members straight from the return value with a plain `=` -
// including reassigning them in-place on a shaderHotReload, which is what
// lets that call site keep being a plain assignment instead of an alias.
struct ComputePipelineHandles
{
    vk::PipelineLayout layout;
    vk::Pipeline pipeline;
};

// Loads spvPath, builds the one shader stage and pipeline layout every
// compute pass in this engine needs, and creates the pipeline - the shape
// Clouds and PathTracing each hand-rolled independently. The shader module
// is destroyed before returning, exactly as every call site already did.
[[nodiscard]] auto createComputePipeline(const std::shared_ptr<VulkanDevice> &device,
  const std::string &spvPath,
  std::span<const vk::DescriptorSetLayout> setLayouts,
  std::span<const vk::PushConstantRange> pushConstantRanges = {},
  const char *layoutErrorMessage = "Failed to create a compute pipeline layout!",
  const char *pipelineErrorMessage = "Failed to create a compute pipeline!") -> ComputePipelineHandles;

}// namespace Kataglyphis
