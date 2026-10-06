module;
#include <memory>

#include "renderer/pushConstants/PushConstantPathTracing.hpp"
#include <glm/glm.hpp>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.path_tracing;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.image;
import kataglyphis.vulkan.swapchain;

export namespace Kataglyphis::VulkanRendererInternals {
class PathTracing
{
  public:
    PathTracing();

    PathTracing(const PathTracing &) = delete;
    PathTracing &operator=(const PathTracing &) = delete;

    void init(const std::shared_ptr<VulkanDevice> &in_device,
      std::span<const vk::DescriptorSetLayout> descriptorSetLayouts);

    void shaderHotReload(std::span<const vk::DescriptorSetLayout> descriptor_set_layouts);

    void recordCommands(vk::CommandBuffer &commandBuffer,
      uint32_t image_index,
      VulkanImage &vulkanImage,
      VulkanImage &accumulationImage,
      VulkanSwapChain *vulkanSwapChain,
      std::span<const vk::DescriptorSet> descriptorSets,
      uint32_t frame_index,
      uint32_t samples_per_pixel,
      uint32_t max_bounces);

    void cleanUp();

    ~PathTracing();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };

    vk::PipelineLayout pipeline_layout{};
    vk::Pipeline pipeline{};
    PushConstantPathTracing push_constant{ glm::vec4(0.f), 0, 0 };

    // NOTE: this stage used to own a private 2-query timestamp pool that was
    // written every frame but never read back. Per-pass GPU timing now lives
    // centrally in VulkanRenderer (one pool, per-swapchain-image slices, read
    // back a frame later); the renderer's "Main" pass brackets this stage, so
    // the private pool was removed instead of being wired in.

    void createPipeline(std::span<const vk::DescriptorSetLayout> descriptorSetLayouts);
};
}// namespace Kataglyphis::VulkanRendererInternals
