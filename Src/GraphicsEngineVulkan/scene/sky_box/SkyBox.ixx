module;

#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "scene/sky_box/CloudAmbient.hpp"

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

    // colorFormat: the swapchain format this pass renders into; kept for shaderHotReload().
    void createGraphicsPipeline(vk::DescriptorSetLayout sharedLayout, vk::Format colorFormat);
    void shaderHotReload(vk::DescriptorSetLayout sharedLayout);
    // Clears target (a swapchain image, from UNDEFINED) and draws the sky; leaves it for the post pass to load.
    void recordCommands(vk::CommandBuffer &commandBuffer,
      vk::Image target,
      vk::ImageView targetView,
      vk::Extent2D extent,
      std::span<const vk::DescriptorSet> descriptorSets,
      bool skyboxEnabled);

    void cleanUp();

    Kataglyphis::Mesh *getMesh() const { return skyMesh.get(); }

    // Null until init(); the post pass samples the same cubemap to put the sky under the clouds.
    Kataglyphis::Texture *getCubeMapTexture() const { return cubeMapTexture.get(); }

    // The loaded faces' hemisphere means, computed once per load; zero for the black fallback.
    const CloudAmbient &getCloudAmbient() const { return cloudAmbient; }

    ~SkyBox();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };

    std::unique_ptr<Kataglyphis::Mesh> skyMesh;
    std::unique_ptr<Kataglyphis::Texture> cubeMapTexture;
    CloudAmbient cloudAmbient{};

    vk::Pipeline graphicsPipeline{};
    vk::PipelineLayout pipelineLayout{};
    DescriptorSetGroup cubemapDescriptors;
    vk::Format colorFormat{ vk::Format::eUndefined };

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