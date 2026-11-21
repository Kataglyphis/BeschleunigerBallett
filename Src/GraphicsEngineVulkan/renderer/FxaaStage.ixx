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
// OxidANT's fxaa.slang after the tonemap: post renders into a UNORM intermediate per swapchain image, this stage
// filters it into the swapchain image and draws the GUI over it.
class FxaaStage
{
  public:
    FxaaStage();

    FxaaStage(const FxaaStage &) = delete;
    FxaaStage &operator=(const FxaaStage &) = delete;

    void init(const std::shared_ptr<VulkanDevice> &in_device, VulkanSwapChain *swap_chain);
    // After a swapchain recreation: the intermediates, their sets and the texel size follow its extent and count.
    void recreateFrameResources();
    void shaderHotReload();

    // Post's target while FXAA filters the frame, in the swapchain's UNORM format.
    Kataglyphis::Texture &getIntermediate(uint32_t index) { return *intermediates[index]; }

    // Whether recordCommands can filter image `index`; fxaaFilters asks, so post keeps the swapchain image otherwise.
    [[nodiscard]] bool ready(uint32_t index) const
    { return index < descriptors.sets().size() && index < intermediates.size() && static_cast<bool>(pipeline); }

    // Takes the swapchain image from the acquire and the intermediate post left for sampling; leaves it presentable.
    void recordCommands(vk::CommandBuffer commandBuffer, uint32_t imageIndex);

    void cleanUp();

    ~FxaaStage();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };
    VulkanSwapChain *vulkanSwapChain{ nullptr };

    std::vector<std::unique_ptr<Kataglyphis::Texture>> intermediates{};
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
