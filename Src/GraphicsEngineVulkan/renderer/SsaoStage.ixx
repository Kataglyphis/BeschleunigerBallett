module;

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.ssao_stage;

import kataglyphis.vulkan.bloom_stage;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.descriptor_set_group;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.swapchain;
import kataglyphis.vulkan.texture;

export namespace Kataglyphis::VulkanRendererInternals {

// The depth SSAO reads: the forward path's prepass, or the deferred path's G-buffer.
enum class SsaoDepthSource : std::uint8_t { ForwardPrepass = 0, Deferred = 1 };

// OxidANT's ssao.slang at half resolution: depth-only occlusion, then its 3x3 blur, which the lit passes' ambient reads.
class SsaoStage
{
  public:
    static constexpr vk::Format kFormat = vk::Format::eR8Unorm;

    SsaoStage();

    SsaoStage(const SsaoStage &) = delete;
    SsaoStage &operator=(const SsaoStage &) = delete;

    // The pool records the one-off clear that leaves the occlusion white and sampleable before any frame runs SSAO.
    void init(const std::shared_ptr<VulkanDevice> &in_device, VulkanSwapChain *swap_chain, vk::CommandPool commandPool);
    // After a swapchain recreation: the targets follow its extent, the sets and uniforms its image count.
    void recreateFrameResources(vk::CommandPool commandPool);
    void bindDepth(SsaoDepthSource source, vk::ImageView depthView);
    void shaderHotReload();

    // Reads `source`'s depth in eShaderReadOnlyOptimal; leaves the blurred occlusion there for the lit fragments.
    void recordCommands(vk::CommandBuffer commandBuffer,
      uint32_t imageIndex,
      SsaoDepthSource source,
      const glm::mat4 &projection);

    [[nodiscard]] Kataglyphis::Texture &getOcclusion() { return *blurred; }
    [[nodiscard]] vk::Sampler getSampler() const { return sampler; }
    [[nodiscard]] vk::Extent2D getExtent() const { return extent; }

    void cleanUp();

    ~SsaoStage();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };
    VulkanSwapChain *vulkanSwapChain{ nullptr };

    vk::Extent2D extent{};
    uint32_t imageCount{ 0 };
    std::unique_ptr<Kataglyphis::Texture> raw{};
    std::unique_ptr<Kataglyphis::Texture> blurred{};
    std::vector<VulkanBuffer> uniforms{};
    vk::Sampler sampler{};
    // Per source, one occlusion set per swapchain image; then per source the blur's.
    DescriptorSetGroup descriptors;

    vk::PipelineLayout pipelineLayout{};
    vk::Pipeline occlusionPipeline{};
    vk::Pipeline blurPipeline{};

    void createFrameResources(vk::CommandPool commandPool);
    void releaseFrameResources();
    void createPipelines();
    void destroyPipelines();
    [[nodiscard]] uint32_t occlusionSet(SsaoDepthSource source, uint32_t imageIndex) const;
    [[nodiscard]] uint32_t blurSet(SsaoDepthSource source) const;
    [[nodiscard]] bool ready(uint32_t imageIndex) const;
};

}// namespace Kataglyphis::VulkanRendererInternals
