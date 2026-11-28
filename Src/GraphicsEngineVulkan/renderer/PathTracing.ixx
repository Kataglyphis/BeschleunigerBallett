module;
#include <memory>

#include "renderer/PathTracingHistory.hpp"
#include "renderer/pushConstants/PushConstantPathTracing.hpp"
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <tuple>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.path_tracing;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.image;

export namespace Kataglyphis::VulkanRendererInternals {

// What one path-traced frame adds to the running mean.
struct PathTracingFrame
{
    // Samples per pixel the mean held before this frame; 0 starts it over.
    uint64_t accumulated = 0;
    uint32_t samples_per_pixel = 1;
    uint32_t max_bounces = 1;
    PathTracingFurnace furnace{};
    // The clouds read this frame's traced depth.
    bool trace_cloud_depth = false;
    bool skybox_enabled = true;
};

// The editor's White furnace, unless KATAGLYPHIS_PT_FURNACE names a radiance; _METALLIC and _ROUGHNESS pick its surface.
auto resolvePathTracingFurnace(bool editor_furnace) -> PathTracingFurnace;

class PathTracing
{
  public:
    PathTracing();

    PathTracing(const PathTracing &) = delete;
    PathTracing &operator=(const PathTracing &) = delete;

    void init(const std::shared_ptr<VulkanDevice> &in_device,
      std::span<const vk::DescriptorSetLayout> descriptorSetLayouts);

    void shaderHotReload(std::span<const vk::DescriptorSetLayout> descriptor_set_layouts);

    // One dispatch per tile and sample chunk of planPathTracingDispatches, each within its segment budget.
    void recordCommands(vk::CommandBuffer &commandBuffer,
      VulkanImage &vulkanImage,
      VulkanImage &accumulationImage,
      vk::Extent2D extent,
      std::span<const vk::DescriptorSet> descriptorSets,
      const PathTracingFrame &frame);

    void cleanUp();

    ~PathTracing();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };

    vk::PipelineLayout pipeline_layout{};
    vk::Pipeline pipeline{};
    PushConstantPathTracing push_constant{};
    // Dispatches a frame, the samples of its first and the bounces of each, as last logged.
    std::tuple<std::size_t, uint32_t, uint32_t> logged_plan_shape{ 0, 0, 0 };

    // NOTE: this stage used to own a private 2-query timestamp pool that was
    // written every frame but never read back. Per-pass GPU timing now lives
    // centrally in VulkanRenderer (one pool, per-swapchain-image slices, read
    // back a frame later); the renderer's "Main" pass brackets this stage, so
    // the private pool was removed instead of being wired in.

    void createPipeline(std::span<const vk::DescriptorSetLayout> descriptorSetLayouts);
};
}// namespace Kataglyphis::VulkanRendererInternals
