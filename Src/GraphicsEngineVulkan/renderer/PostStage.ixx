module;

#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.post_stage;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.swapchain;

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

    // Loads the skybox pass's output in the swapchain image, draws post and the GUI over it, leaves it presentable.
    // descriptorSets: the post set, then the shared render set, whose GlobalUBO aims the sky under the clouds.
    void recordCommands(vk::CommandBuffer &commandBuffer,
      uint32_t image_index,
      std::span<const vk::DescriptorSet> descriptorSets,
      bool cloudsEnabled,
      bool skyboxEnabled);
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