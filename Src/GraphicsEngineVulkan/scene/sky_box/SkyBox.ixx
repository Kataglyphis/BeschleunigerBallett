module;

#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "scene/sky_box/CloudAmbient.hpp"
#include "scene/sky_box/SkyUniforms.hpp"

export module kataglyphis.vulkan.sky_box;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.command_buffer_manager;
import kataglyphis.vulkan.descriptor_set_group;

export namespace Kataglyphis {
// OxidANT's sky.slang drawn into the HDR scene target, which post tonemaps with the scene.
class SkyBox
{
  public:
    SkyBox();

    SkyBox(const SkyBox &) = delete;
    SkyBox &operator=(const SkyBox &) = delete;

    void init(const std::shared_ptr<VulkanDevice> &device, vk::CommandPool commandPool, uint32_t imageCount);

    // colorFormat: the HDR target; depthFormat: the raster passes' depth attachment. Kept for shaderHotReload().
    void createGraphicsPipelines(vk::Format colorFormat, vk::Format depthFormat);
    void shaderHotReload();
    // One uniform buffer and descriptor set per swapchain image; again whenever the image count changes.
    void createFrameResources(uint32_t imageCount);
    void uploadUniforms(uint32_t imageIndex, const SkyUniforms &uniforms);

    // Inside a raster pass with its depth attached: depth 1 under LessOrEqual fills what no surface covered.
    void recordRasterDraw(vk::CommandBuffer commandBuffer, uint32_t imageIndex) const;
    // A ray-traced frame's target in eGeneral, alpha 1 where the primary ray hit and tracedDepth is below 1:
    // fills the misses and leaves the target for post to sample.
    void recordTracedDraw(vk::CommandBuffer commandBuffer,
      uint32_t imageIndex,
      vk::Image target,
      vk::ImageView targetView,
      vk::Extent2D extent) const;

    void cleanUp();

    // Null until init(); the path tracer's bounces sample the same cubemap.
    Kataglyphis::Texture *getCubeMapTexture() const { return cubeMapTexture.get(); }

    // The uploaded faces' side in texels; 0 before an upload succeeded.
    uint32_t getCubeFaceSize() const { return cubeFaceSize; }

    // The loaded faces' hemisphere means, computed once per load; the analytic sky's when no faces loaded.
    const CloudAmbient &getCloudAmbient() const { return cloudAmbient; }

    // No skybox faces loaded, so the cubemap holds OxidANT's procedural sky.
    bool usesAnalyticSky() const { return analyticSky; }

    ~SkyBox();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };

    std::unique_ptr<Kataglyphis::Texture> cubeMapTexture;
    CloudAmbient cloudAmbient{};
    bool analyticSky{ false };
    uint32_t cubeFaceSize{ 0 };

    std::vector<VulkanBuffer> uniformBuffers{};
    DescriptorSetGroup descriptors;
    vk::Pipeline rasterPipeline{};
    vk::Pipeline tracedPipeline{};
    vk::PipelineLayout pipelineLayout{};
    vk::Format colorFormat{ vk::Format::eUndefined };
    vk::Format depthFormat{ vk::Format::eUndefined };

    void loadCubeMap(vk::CommandPool commandPool);
    void loadAnalyticSky(vk::CommandPool commandPool);
    bool uploadCubeMapFaces(vk::CommandPool commandPool,
      uint32_t width,
      uint32_t height,
      std::span<const unsigned char *const, 6> faceData);
    void writeCubeMapDescriptors();
    void destroyTracedPipeline();
    void recordDraw(vk::CommandBuffer commandBuffer, vk::Pipeline pipeline, uint32_t imageIndex) const;
};
}// namespace Kataglyphis
