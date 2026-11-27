module;

#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.post_stage;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.swapchain;
import kataglyphis.vulkan.texture;

export namespace Kataglyphis::VulkanRendererInternals {
class PostStage
{
  public:
    PostStage();

    PostStage(const PostStage &) = delete;
    PostStage &operator=(const PostStage &) = delete;

    void init(const std::shared_ptr<VulkanDevice> &in_device,
      VulkanSwapChain *vulkanSwapChain,
      std::span<const vk::DescriptorSetLayout> descriptorSetLayouts);

    void shaderHotReload(std::span<const vk::DescriptorSetLayout> descriptor_set_layouts);

    vk::Sampler &getOffscreenSampler() { return offscreenTextureSampler; };

    // Overwrites its target with the tonemapped scene. Without fxaaInput that is the swapchain image, taken from the
    // acquire, with the GUI drawn over it and left presentable; with it, FXAA's intermediate, left for sampling.
    // descriptorSets: the post set.
    void recordCommands(vk::CommandBuffer &commandBuffer,
      uint32_t image_index,
      std::span<const vk::DescriptorSet> descriptorSets,
      bool cloudsEnabled,
      float bloomStrength,
      Kataglyphis::Texture *fxaaInput = nullptr);
    void cleanUp();

    ~PostStage();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };
    VulkanSwapChain *vulkanSwapChain{ nullptr };

    vk::Sampler offscreenTextureSampler{};
    void createOffscreenTextureSampler();

    vk::PushConstantRange push_constant_range{ vk::ShaderStageFlagBits::eAll, 0, 0 };
    vk::Pipeline graphics_pipeline{};
    vk::PipelineLayout pipeline_layout{};

    void createPushConstantRange();
    void createGraphicsPipeline(std::span<const vk::DescriptorSetLayout> descriptorSetLayouts);
};
}// namespace Kataglyphis::VulkanRendererInternals