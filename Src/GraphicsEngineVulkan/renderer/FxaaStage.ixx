module;

#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.fxaa_stage;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.swapchain;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.descriptor_set_group;

export namespace Kataglyphis::VulkanRendererInternals {
// OxidANT's fxaa.slang after the tonemap: post renders into one UNORM intermediate every frame in flight shares, this
// stage filters it into the swapchain image and draws the GUI over it.
class FxaaStage
{
  public:
    FxaaStage();

    FxaaStage(const FxaaStage &) = delete;
    FxaaStage &operator=(const FxaaStage &) = delete;

    void init(const std::shared_ptr<VulkanDevice> &in_device, VulkanSwapChain *swap_chain);
    // After a swapchain recreation: the intermediate and the texel size follow its extent.
    void recreateFrameResources();
    void shaderHotReload();

    // Post's target while FXAA filters the frame, in the swapchain's UNORM format.
    Kataglyphis::Texture &getIntermediate() { return *intermediate; }
    // The one intermediate, for the scene-target footprint; null before init.
    [[nodiscard]] vk::Image intermediateImage() const { return intermediate ? intermediate->getImage() : vk::Image{}; }

    // Whether recordCommands can filter a frame; fxaaFilters asks, so post keeps the swapchain image otherwise.
    [[nodiscard]] bool ready() const
    { return !descriptors.sets().empty() && intermediate != nullptr && static_cast<bool>(pipeline); }

    // Takes the swapchain image from the acquire and the intermediate post left for sampling; leaves it presentable.
    void recordCommands(vk::CommandBuffer commandBuffer, uint32_t imageIndex);

    void cleanUp();

    ~FxaaStage();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };
    VulkanSwapChain *vulkanSwapChain{ nullptr };

    std::unique_ptr<Kataglyphis::Texture> intermediate{};
    VulkanBuffer uniformBuffer;
    vk::Sampler sampler{};
    DescriptorSetGroup descriptors;
    vk::Pipeline pipeline{};
    vk::PipelineLayout pipelineLayout{};

    void createFrameResources();
    void releaseFrameResources();
    void createPipeline();
};
}// namespace Kataglyphis::VulkanRendererInternals
