module;

#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.clouds;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.descriptor_set_group;

export namespace Kataglyphis {
class Clouds
{
  public:
    Clouds() = default;

    Clouds(const Clouds &) = delete;
    Clouds &operator=(const Clouds &) = delete;

    void init(const std::shared_ptr<VulkanDevice> &in_device,
      vk::CommandPool commandPool,
      vk::DescriptorSetLayout sharedLayout,
      uint32_t in_width,
      uint32_t in_height);

    void recordComputeCommands(vk::CommandBuffer &commandBuffer, std::span<const vk::DescriptorSet> descriptorSets);

    // The depth the march stops at: what the main pass drew, as NDC depth in `layout` at dispatch time.
    void setSceneDepth(vk::ImageView sceneDepthView, vk::ImageLayout layout);

    void shaderHotReload(vk::DescriptorSetLayout sharedLayout);

    void recreateFrameResources(vk::CommandPool commandPool, uint32_t new_width, uint32_t new_height);

    void cleanUp();

    Kataglyphis::Texture *getCloudOutputTexture() const { return cloudOutputTexture.get(); }

    ~Clouds() { cleanUp(); }

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };

    std::unique_ptr<Kataglyphis::Texture> cloudNoiseTexture;
    std::unique_ptr<Kataglyphis::Texture> cloudOutputTexture;

    DescriptorSetGroup cloudDescriptors;
    DescriptorSetGroup noiseDescriptors;

    vk::Pipeline noiseComputePipeline;
    vk::PipelineLayout noisePipelineLayout;

    vk::Pipeline cloudComputePipeline;
    vk::PipelineLayout cloudPipelineLayout;

    uint32_t width{ 1920 };
    uint32_t height{ 1080 };

    void createTextures(vk::CommandPool commandPool);
    void createDescriptorSets();
    void createComputePipelines(vk::DescriptorSetLayout sharedLayout);
    void dispatchNoiseGeneration(vk::CommandPool commandPool);// Run once during init

    std::unique_ptr<Kataglyphis::Texture> createStorageTexture(vk::CommandPool commandPool,
      uint32_t w,
      uint32_t h,
      uint32_t depth,
      vk::ImageType type,
      vk::ImageViewType viewType);
};
}// namespace Kataglyphis