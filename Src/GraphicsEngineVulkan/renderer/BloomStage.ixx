module;

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.bloom_stage;

import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.descriptor_set_group;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.swapchain;
import kataglyphis.vulkan.texture;

export namespace Kataglyphis::VulkanRendererInternals {

// The half extent bloom and SSAO draw at, rounded down as OxidANT's post_half, never below one texel.
constexpr vk::Extent2D postHalfExtent(vk::Extent2D extent)
{
    return vk::Extent2D{ extent.width / 2U > 0U ? extent.width / 2U : 1U,
        extent.height / 2U > 0U ? extent.height / 2U : 1U };
}

// A one-off submit that clears `images` to `color` and leaves them for fragment reads, before any pass writes them.
void clearToShaderRead(const std::shared_ptr<VulkanDevice> &device,
  vk::CommandPool commandPool,
  std::span<const vk::Image> images,
  vk::ClearColorValue color);

// OxidANT's bloom.slang at half resolution: a bright pass, then a separable 9-tap Gaussian through two RGBA16F targets.
class BloomStage
{
  public:
    static constexpr vk::Format kFormat = vk::Format::eR16G16B16A16Sfloat;

    BloomStage();

    BloomStage(const BloomStage &) = delete;
    BloomStage &operator=(const BloomStage &) = delete;

    // The pool records the one-off clear that leaves both targets sampleable and black before any frame blooms.
    void init(const std::shared_ptr<VulkanDevice> &in_device,
      VulkanSwapChain *swap_chain,
      vk::CommandPool commandPool,
      vk::Extent2D sceneExtent);
    // The targets at half the scene extent, the bright pass's sets and buffers at the swapchain's image count.
    void recreateFrameResources(vk::CommandPool commandPool, vk::Extent2D sceneExtent);
    // The bright pass's inputs for one swapchain image: its HDR target, the clouds and the exposure buffer.
    void bindInputs(uint32_t imageIndex, vk::ImageView hdrView, vk::ImageView cloudView, vk::Buffer exposure);
    void shaderHotReload();

    // Leaves the blurred result in eShaderReadOnlyOptimal for post's fragment shader.
    void recordCommands(vk::CommandBuffer commandBuffer, uint32_t imageIndex, bool cloudsEnabled);

    // Post samples it every frame; until a frame blooms it is black.
    [[nodiscard]] Kataglyphis::Texture &getOutput() { return *output; }
    [[nodiscard]] vk::Sampler getSampler() const { return sampler; }
    [[nodiscard]] vk::Extent2D getExtent() const { return extent; }
    // Both half-resolution targets, for the scene-target footprint.
    [[nodiscard]] std::array<vk::Image, 2> targetImages() const
    { return { output ? output->getImage() : vk::Image{}, scratch ? scratch->getImage() : vk::Image{} }; }

    void cleanUp();

    ~BloomStage();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };
    VulkanSwapChain *vulkanSwapChain{ nullptr };

    vk::Extent2D extent{};
    uint32_t imageCount{ 0 };
    std::unique_ptr<Kataglyphis::Texture> output{};
    std::unique_ptr<Kataglyphis::Texture> scratch{};
    std::vector<VulkanBuffer> uniforms{};
    vk::Sampler sampler{};
    // One bright-pass set per swapchain image, then the horizontal and the vertical blur's.
    DescriptorSetGroup descriptors;

    vk::PipelineLayout pipelineLayout{};
    vk::Pipeline brightPipeline{};
    vk::Pipeline blurHorizontalPipeline{};
    vk::Pipeline blurVerticalPipeline{};

    void createFrameResources(vk::CommandPool commandPool, vk::Extent2D sceneExtent);
    void releaseFrameResources();
    void createPipelines();
    void destroyPipelines();
    void recordFullscreen(vk::CommandBuffer commandBuffer,
      vk::Pipeline pipeline,
      vk::DescriptorSet set,
      Kataglyphis::Texture &target);
    [[nodiscard]] bool ready(uint32_t imageIndex) const;
};

}// namespace Kataglyphis::VulkanRendererInternals
