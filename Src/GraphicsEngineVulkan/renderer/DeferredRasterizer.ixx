module;
#include <optional>

#include "renderer/pushConstants/PushConstantRasterizer.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.deferred_rasterizer;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.swapchain;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.rasterizer;

export namespace Kataglyphis::VulkanRendererInternals {
class DeferredRasterizer
{
  public:
    DeferredRasterizer();

    DeferredRasterizer(const DeferredRasterizer &) = delete;
    DeferredRasterizer &operator=(const DeferredRasterizer &) = delete;

    void init(const std::shared_ptr<VulkanDevice> &in_device,
      VulkanSwapChain *swap_chain,
      std::span<const vk::DescriptorSetLayout> descriptorSetLayouts);

    void shaderHotReload(std::span<const vk::DescriptorSetLayout> descriptor_set_layouts);

    Kataglyphis::Texture &getOffscreenTexture(uint32_t index);
    vk::ImageView getGBufferNormal(uint32_t index) const { return gBufferNormals[index]->getImageView(); }
    vk::ImageView getGBufferAlbedo(uint32_t index) const { return gBufferAlbedos[index]->getImageView(); }
    vk::ImageView getGBufferMaterial(uint32_t index) const { return gBufferMaterials[index]->getImageView(); }
    vk::ImageView getDepthBufferImageView() const { return depthBufferImage->getImageView(); }

    void setPushConstant(PushConstantRasterizer push_constant);

    /// `cameraFrustum` culls meshes whose world-space bounds fall entirely
    /// outside the view. Pass std::nullopt to draw everything - and note that
    /// the SHADOW pass must do exactly that: geometry behind or beside the
    /// camera still casts into view, so culling casters by the camera frustum
    /// deletes shadows rather than saving work.
    void recordCommands(vk::CommandBuffer &commandBuffer,
      uint32_t image_index,
      Kataglyphis::Scene *scene,
      std::span<const vk::DescriptorSet> descriptorSets,
      const std::optional<FrustumPlanes> &cameraFrustum = std::nullopt);

    /// Meshes drawn / considered by the most recent recordCommands call.
    /// Reset at the start of each call, so a frame that records nothing
    /// reports zeros rather than stale counts from the previous frame.
    unsigned int getMeshesDrawn() const { return meshesDrawn; }
    unsigned int getMeshesConsidered() const { return meshesConsidered; }

    void recreateFrameResources();
    void destroyFramebuffers();

    void cleanUp();

    ~DeferredRasterizer();

    // GBuffer / final-target formats, written once here and read by both
    // createTextures() and createRenderPass(): a mismatch between the image
    // and the attachment description backing it is a validation error at
    // framebuffer creation, not a compile error, so both call sites must use
    // these instead of re-spelling the literals. Indices match the attachment
    // order documented in createRenderPass()'s "0: Final Color ... 4: Depth" comment.
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
    VulkanSwapChain *vulkanSwapChain{ nullptr };

    std::vector<vk::Framebuffer> framebuffer;

    // The final color output (offscreen texture, consumed by PostStage)
    std::vector<std::unique_ptr<Kataglyphis::Texture>> offscreenTextures;

    // GBuffer attachments
    std::vector<std::unique_ptr<Kataglyphis::Texture>> gBufferNormals;
    std::vector<std::unique_ptr<Kataglyphis::Texture>> gBufferAlbedos;
    std::vector<std::unique_ptr<Kataglyphis::Texture>> gBufferMaterials;// Metallic, Roughness, AO

    std::unique_ptr<Kataglyphis::Texture> depthBufferImage;
    // Resolved once by createTextures(), which init() always runs before
    // createRenderPass() - reuse it there rather than querying again, so the
    // attachment and the image it is paired with cannot diverge.
    vk::Format depth_format{ vk::Format::eUndefined };

    vk::PushConstantRange push_constant_range{ vk::ShaderStageFlagBits::eAll, 0, 0 };
    PushConstantRasterizer pushConstant{ glm::mat4(1.f) };

    // Geometry Pass
    vk::Pipeline geometryPipeline{};
    vk::PipelineLayout geometryPipelineLayout{};

    // Lighting Pass
    vk::Pipeline lightingPipeline{};
    vk::PipelineLayout lightingPipelineLayout{};

    vk::RenderPass renderPass{};

    void createTextures();
    void releaseFrameTextures();
    void createRenderPass();
    void createPipelines(std::span<const vk::DescriptorSetLayout> descriptorSetLayouts);
    void createFramebuffer();
    void createPushConstantRange();
};
}// namespace Kataglyphis::VulkanRendererInternals
