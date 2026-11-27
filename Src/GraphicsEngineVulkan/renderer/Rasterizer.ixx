module;
#include <functional>
#include <optional>

#include "renderer/pushConstants/PushConstantRasterizer.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.rasterizer;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.command_buffer_manager;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.frustum;

export namespace Kataglyphis::VulkanRendererInternals {

/// The glTF BLEND pipeline both raster modes draw their sorted transparent meshes with: rasterizer.slang's
/// forward shading, src-alpha blending that accumulates coverage, depth test on and depth writes off.
[[nodiscard]] vk::Pipeline buildTransparentScenePipeline(const std::shared_ptr<VulkanDevice> &device,
  vk::PipelineLayout layout,
  vk::Format color_format,
  vk::Format depth_format);

class Rasterizer
{
  public:
    Rasterizer();

    Rasterizer(const Rasterizer &) = delete;
    Rasterizer &operator=(const Rasterizer &) = delete;

    // Builds the pipelines only; the depth targets wait for createFrameTargets, as a deferred frame needs none.
    void init(const std::shared_ptr<VulkanDevice> &in_device,
      std::span<const vk::DescriptorSetLayout> descriptorSetLayouts);

    void shaderHotReload(std::span<const vk::DescriptorSetLayout> descriptor_set_layouts);

    // The depth and prepass depth at the scene extent, which recordCommands then draws at.
    void createFrameTargets(vk::Extent2D extent, vk::CommandPool commandPool);
    void releaseFrameTargets() { releaseFrameTextures(); }
    [[nodiscard]] bool hasFrameTargets() const { return depthBufferImage != nullptr; }
    // The images createFrameTargets made, none while released.
    [[nodiscard]] std::vector<vk::Image> frameTargetImages() const;

    vk::ImageView getDepthImageView() const { return depthBufferImage->getImageView(); }
    // SSAO's depth: the prepass's, which the forward pass does not share, left in eShaderReadOnlyOptimal.
    vk::ImageView getPrepassDepthImageView() const { return prepassDepthImage->getImageView(); }
    vk::Format getDepthFormat() const { return depth_format; }

    void setPushConstant(PushConstantRasterizer push_constant);

    /// `cameraFrustum` culls meshes whose world-space bounds fall entirely
    /// outside the view. Pass std::nullopt to draw everything - and note that
    /// the SHADOW pass must do exactly that: geometry behind or beside the
    /// camera still casts into view, so culling casters by the camera frustum
    /// deletes shadows rather than saving work.
    /// `eye` orders the glTF BLEND meshes, drawn after the opaque ones, back to front.
    /// `drawSky` records the sky between the two, inside the pass, with its depth attached.
    /// `sceneColor` is the HDR target every frame in flight shares, so its barrier waits out the last frame's reads.
    void recordCommands(vk::CommandBuffer &commandBuffer,
      Kataglyphis::Texture &sceneColor,
      Kataglyphis::Scene *scene,
      std::span<const vk::DescriptorSet> descriptorSets,
      const glm::vec3 &eye,
      const std::optional<FrustumPlanes> &cameraFrustum = std::nullopt,
      const std::function<void(vk::CommandBuffer)> &drawSky = {});

    /// The opaque and MASK meshes' depth alone, for SSAO before the forward pass shades; culled as recordCommands culls.
    void recordDepthPrepass(vk::CommandBuffer &commandBuffer,
      Kataglyphis::Scene *scene,
      std::span<const vk::DescriptorSet> descriptorSets,
      const std::optional<FrustumPlanes> &cameraFrustum = std::nullopt);

    /// Meshes drawn / considered by the most recent recordCommands call.
    /// Reset at the start of each call, so a frame that records nothing
    /// reports zeros rather than stale counts from the previous frame.
    unsigned int getMeshesDrawn() const { return meshesDrawn; }
    unsigned int getMeshesConsidered() const { return meshesConsidered; }

    // At a new scene extent; a released path stays released.
    void recreateFrameResources(vk::Extent2D extent, vk::CommandPool commandPool);

    void cleanUp();

    ~Rasterizer();

    // HDR offscreen target format. The pipeline's rendering format and the offscreen
    // textures MUST agree on this, so it lives in one place: lighting scales
    // with the GUI light radiance (default 10), so the lit scene exceeds 1.0
    // everywhere - a UNORM target clamps it flat before post's tonemap runs
    // (measured: everything at the 186 ceiling). FP16 keeps the range for
    // Reinhard. Public: DeferredRasterizer's FINAL_FORMAT is static-asserted
    // equal to this one, since both back the same forward-offscreen contract.
    static constexpr vk::Format OFFSCREEN_FORMAT = vk::Format::eR16G16B16A16Sfloat;

  private:
    unsigned int meshesDrawn{ 0 };
    unsigned int meshesConsidered{ 0 };
    std::shared_ptr<VulkanDevice> device{ nullptr };

    std::unique_ptr<Kataglyphis::Texture> depthBufferImage;
    std::unique_ptr<Kataglyphis::Texture> prepassDepthImage;
    // The extent the targets were made at, which both passes render at.
    vk::Extent2D targetExtent{};
    // Resolved once by init(), before createGraphicsPipeline() and any target, so the
    // pipeline's depth format and the image it renders to cannot diverge.
    vk::Format depth_format{ vk::Format::eUndefined };

    vk::PushConstantRange push_constant_range{ vk::ShaderStageFlagBits::eAll, 0, 0 };
    PushConstantRasterizer pushConstant{ glm::mat4(1.f), 0 };

    vk::Pipeline graphics_pipeline{};
    // glTF BLEND meshes, after the opaque ones; shares pipeline_layout.
    vk::Pipeline transparent_pipeline{};
    // SSAO's depth prepass; shares pipeline_layout.
    vk::Pipeline prepass_pipeline{};
    vk::PipelineLayout pipeline_layout{};

    void createTextures(vk::CommandPool commandPool);
    void releaseFrameTextures();
    void createGraphicsPipeline(std::span<const vk::DescriptorSetLayout> descriptorSetLayouts);
    void destroyTransparentPipeline();
    void createPushConstantRange();
};
}// namespace Kataglyphis::VulkanRendererInternals