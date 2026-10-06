module;
#include <optional>

#include "renderer/pushConstants/PushConstantRasterizer.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.rasterizer;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.swapchain;
import kataglyphis.vulkan.command_buffer_manager;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.frustum;

export namespace Kataglyphis::VulkanRendererInternals {
class Rasterizer
{
  public:
    Rasterizer();

    Rasterizer(const Rasterizer &) = delete;
    Rasterizer &operator=(const Rasterizer &) = delete;

    void init(const std::shared_ptr<VulkanDevice> &in_device,
      VulkanSwapChain *swap_chain,
      std::span<const vk::DescriptorSetLayout> descriptorSetLayouts,
      vk::CommandPool &commandPool);

    void shaderHotReload(std::span<const vk::DescriptorSetLayout> descriptor_set_layouts);

    Kataglyphis::Texture &getOffscreenTexture(uint32_t index);

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

    void recreateFrameResources(vk::CommandPool commandPool);
    void destroyFramebuffers();

    void cleanUp();

    ~Rasterizer();

    // HDR offscreen target format. The render pass attachment and the offscreen
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
    VulkanSwapChain *vulkanSwapChain{ nullptr };

    std::vector<vk::Framebuffer> framebuffer;
    std::vector<std::unique_ptr<Kataglyphis::Texture>> offscreenTextures;
    std::unique_ptr<Kataglyphis::Texture> depthBufferImage;
    // Resolved once by createTextures(), which init() always runs before
    // createRenderPass() - reuse it there rather than querying again, so the
    // attachment and the image it is paired with cannot diverge.
    vk::Format depth_format{ vk::Format::eUndefined };

    vk::PushConstantRange push_constant_range{ vk::ShaderStageFlagBits::eAll, 0, 0 };
    PushConstantRasterizer pushConstant{ glm::mat4(1.f) };

    vk::Pipeline graphics_pipeline{};
    vk::PipelineLayout pipeline_layout{};
    vk::RenderPass render_pass{};

    void createTextures(vk::CommandPool &commandPool);
    void releaseFrameTextures();
    void createGraphicsPipeline(std::span<const vk::DescriptorSetLayout> descriptorSetLayouts);
    void createRenderPass();
    void createFramebuffer();
    void createPushConstantRange();
};
}// namespace Kataglyphis::VulkanRendererInternals