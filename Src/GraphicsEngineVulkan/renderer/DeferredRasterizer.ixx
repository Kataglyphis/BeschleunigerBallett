module;
#include <functional>
#include <optional>

#include "renderer/pushConstants/PushConstantRasterizer.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.deferred_rasterizer;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.rasterizer;
import kataglyphis.vulkan.mesh_draw_recorder;

export namespace Kataglyphis::VulkanRendererInternals {
class DeferredRasterizer
{
  public:
    DeferredRasterizer();

    DeferredRasterizer(const DeferredRasterizer &) = delete;
    DeferredRasterizer &operator=(const DeferredRasterizer &) = delete;

    // Builds the pipelines only; the G-buffer waits for createFrameTargets, as a forward frame needs none.
    void init(const std::shared_ptr<VulkanDevice> &in_device,
      std::span<const vk::DescriptorSetLayout> descriptorSetLayouts);

    void shaderHotReload(std::span<const vk::DescriptorSetLayout> descriptor_set_layouts);

    // The G-buffer and its depth at the scene extent, which recordCommands then draws at.
    void createFrameTargets(vk::Extent2D extent);
    void releaseFrameTargets() { releaseFrameTextures(); }
    [[nodiscard]] bool hasFrameTargets() const { return depthBufferImage != nullptr; }
    // The images createFrameTargets made, none while released.
    [[nodiscard]] std::vector<vk::Image> frameTargetImages() const;

    vk::ImageView getGBufferNormal() const { return gBufferNormal->getImageView(); }
    vk::ImageView getGBufferAlbedo() const { return gBufferAlbedo->getImageView(); }
    vk::ImageView getGBufferMaterial() const { return gBufferMaterial->getImageView(); }
    vk::ImageView getDepthBufferImageView() const { return depthBufferImage->getImageView(); }
    vk::Format getDepthFormat() const { return depth_format; }

    void setPushConstant(PushConstantRasterizer push_constant);

    /// `eye` orders the glTF BLEND meshes, shaded forward over the lit result, back to front.
    /// `cameraFrustum` culls meshes whose world-space bounds fall entirely
    /// outside the view. Pass std::nullopt to draw everything - and note that
    /// the SHADOW pass must do exactly that: geometry behind or beside the
    /// camera still casts into view, so culling casters by the camera frustum
    /// deletes shadows rather than saving work.
    /// `afterGeometryPass` runs right after the G-buffer pass, `beforeLighting` once its targets are sampleable (SSAO).
    /// `drawSky` records the sky into the lit target before BLEND, inside a pass with the G-buffer depth attached.
    /// `sceneColor` is the HDR target every frame in flight shares, the lighting pass's and BLEND's.
    void recordCommands(vk::CommandBuffer &commandBuffer,
      Kataglyphis::Texture &sceneColor,
      Kataglyphis::Scene *scene,
      std::span<const vk::DescriptorSet> descriptorSets,
      const glm::vec3 &eye,
      const std::optional<FrustumPlanes> &cameraFrustum = std::nullopt,
      const std::function<void(vk::CommandBuffer)> &afterGeometryPass = {},
      const std::function<void(vk::CommandBuffer)> &drawSky = {},
      const std::function<void(vk::CommandBuffer)> &beforeLighting = {});

    /// Meshes drawn / considered by the most recent recordCommands call.
    /// Reset at the start of each call, so a frame that records nothing
    /// reports zeros rather than stale counts from the previous frame.
    unsigned int getMeshesDrawn() const { return meshesDrawn; }
    unsigned int getMeshesConsidered() const { return meshesConsidered; }

    // At a new scene extent; a released path stays released.
    void recreateFrameResources(vk::Extent2D extent);

    void cleanUp();

    ~DeferredRasterizer();

    // GBuffer / final-target formats, written once here and read by both
    // createTextures() and createPipelines(): a mismatch between the image and
    // the pipeline's rendering format is a validation error at vkCmdBeginRendering,
    // not a compile error, so both call sites must use these instead of
    // re-spelling the literals.
    static constexpr vk::Format FINAL_FORMAT = vk::Format::eR16G16B16A16Sfloat;
    static constexpr vk::Format GBUFFER_NORMAL_FORMAT = vk::Format::eR16G16B16A16Sfloat;
    static constexpr vk::Format GBUFFER_ALBEDO_FORMAT = vk::Format::eR8G8B8A8Srgb;
    static constexpr vk::Format GBUFFER_MATERIAL_FORMAT = vk::Format::eR16G16B16A16Sfloat;
    // FINAL_FORMAT backs the same forward-offscreen contract as Rasterizer's
    // offscreen target, so the two must never drift apart silently.
    static_assert(FINAL_FORMAT == Rasterizer::OFFSCREEN_FORMAT);

  private:
    unsigned int meshesDrawn{ 0 };
    unsigned int meshesConsidered{ 0 };
    std::shared_ptr<VulkanDevice> device{ nullptr };

    // GBuffer attachments: written by the geometry pass, sampled by the lighting pass; one for every frame in flight.
    std::unique_ptr<Kataglyphis::Texture> gBufferNormal;
    std::unique_ptr<Kataglyphis::Texture> gBufferAlbedo;
    std::unique_ptr<Kataglyphis::Texture> gBufferMaterial;// Metallic, Roughness, AO

    std::unique_ptr<Kataglyphis::Texture> depthBufferImage;
    // The extent the targets were made at, which every pass here renders at.
    vk::Extent2D targetExtent{};
    // Resolved once by init(), before createPipelines() and any target, so the
    // pipeline's depth format and the image it renders to cannot diverge.
    vk::Format depth_format{ vk::Format::eUndefined };

    vk::PushConstantRange push_constant_range{ vk::ShaderStageFlagBits::eAll, 0, 0 };
    PushConstantRasterizer pushConstant{ glm::mat4(1.f), 0 };

    // Geometry Pass
    vk::Pipeline geometryPipeline{};
    vk::PipelineLayout geometryPipelineLayout{};

    // Lighting Pass
    vk::Pipeline lightingPipeline{};
    vk::PipelineLayout lightingPipelineLayout{};

    // glTF BLEND meshes after the lighting pass; shares geometryPipelineLayout.
    vk::Pipeline transparentPipeline{};

    void createTextures();
    void releaseFrameTextures();
    void createPipelines(std::span<const vk::DescriptorSetLayout> descriptorSetLayouts);
    void destroyTransparentPipeline();
    unsigned int recordSkyAndTransparentPass(vk::CommandBuffer &commandBuffer,
      Kataglyphis::Texture &sceneColor,
      Kataglyphis::Scene *scene,
      std::span<const vk::DescriptorSet> descriptorSets,
      const std::vector<BlendDraw> &blend_draws,
      const vk::Extent2D &extent,
      vk::ImageAspectFlags depth_aspect,
      const std::function<void(vk::CommandBuffer)> &drawSky);
    void createPushConstantRange();
};
}// namespace Kataglyphis::VulkanRendererInternals
