module;

#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.sky_box;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.mesh;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.command_buffer_manager;
import kataglyphis.vulkan.descriptor_set_group;

export namespace Kataglyphis {
class SkyBox
{
  public:
    SkyBox();

    SkyBox(const SkyBox &) = delete;
    SkyBox &operator=(const SkyBox &) = delete;

    void init(const std::shared_ptr<VulkanDevice> &device, vk::CommandPool commandPool);

    void createRenderPass(vk::Format format);
    void createFramebuffers(std::span<const vk::ImageView> imageViews, uint32_t width, uint32_t height);
    void createGraphicsPipeline(vk::DescriptorSetLayout sharedLayout);
    void shaderHotReload(vk::DescriptorSetLayout sharedLayout);
    void recordCommands(vk::CommandBuffer &commandBuffer,
      uint32_t image_index,
      std::span<const vk::DescriptorSet> descriptorSets,
      bool skyboxEnabled);

    void recreateFrameResources(std::span<const vk::ImageView> imageViews, uint32_t width, uint32_t height);
    void destroyFramebuffers();

    void cleanUp();

    Kataglyphis::Mesh *getMesh() const { return skyMesh.get(); }

    ~SkyBox();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };

    std::unique_ptr<Kataglyphis::Mesh> skyMesh;
    std::unique_ptr<Kataglyphis::Texture> cubeMapTexture;

    vk::Pipeline graphicsPipeline{};
    vk::PipelineLayout pipelineLayout{};
    DescriptorSetGroup cubemapDescriptors;
    vk::RenderPass renderPass{};
    std::vector<vk::Framebuffer> framebuffers{};
    uint32_t framebufferWidth{ 0 };
    uint32_t framebufferHeight{ 0 };

    void loadCubeMap(vk::CommandPool commandPool);
    void loadFallbackCubeMap(vk::CommandPool commandPool);
    bool uploadCubeMapFaces(vk::CommandPool commandPool,
      uint32_t width,
      uint32_t height,
      std::span<const unsigned char *const, 6> faceData);
    void createMesh(vk::CommandPool commandPool);
    void createDescriptorSetForCubeMap();
    void updateDescriptorSetForCubeMap();
};
}// namespace Kataglyphis