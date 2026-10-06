module;
#include <memory>

#include "renderer/pushConstants/PushConstantRayTracing.hpp"
#include <glm/glm.hpp>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.raytracing;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.image;
import kataglyphis.vulkan.swapchain;

export namespace Kataglyphis::VulkanRendererInternals {
class Raytracing
{
  public:
    Raytracing();

    Raytracing(const Raytracing &) = delete;
    Raytracing &operator=(const Raytracing &) = delete;

    void init(const std::shared_ptr<VulkanDevice> &in_device,
      std::span<const vk::DescriptorSetLayout> descriptorSetLayouts,
      VulkanSwapChain *swapchain);

    void shaderHotReload(std::span<const vk::DescriptorSetLayout> descriptor_set_layouts);

    void recordCommands(vk::CommandBuffer &commandBuffer,
      VulkanImage &renderImage,
      std::span<const vk::DescriptorSet> descriptorSets);

    void cleanUp();

    ~Raytracing();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };
    VulkanSwapChain *vulkanSwapChain{ nullptr };

    vk::Pipeline graphicsPipeline{};
    vk::PipelineLayout pipeline_layout{};
    PushConstantRaytracing pc{ glm::vec4(0.f) };
    vk::PushConstantRange pc_ranges{ vk::ShaderStageFlagBits::eAll, 0, 0 };

    std::vector<vk::RayTracingShaderGroupCreateInfoKHR> shader_groups;
    VulkanBuffer shaderBindingTableBuffer;
    VulkanBuffer raygenShaderBindingTableBuffer;
    VulkanBuffer missShaderBindingTableBuffer;
    VulkanBuffer hitShaderBindingTableBuffer;

    vk::StridedDeviceAddressRegionKHR rgen_region{};
    vk::StridedDeviceAddressRegionKHR miss_region{};
    vk::StridedDeviceAddressRegionKHR hit_region{};
    vk::StridedDeviceAddressRegionKHR call_region{};

    vk::PhysicalDeviceRayTracingPipelinePropertiesKHR raytracing_properties{};

    void createPCRange();
    void createGraphicsPipeline(std::span<const vk::DescriptorSetLayout> descriptorSetLayouts);
    void createSBT();
    void recreateSBT();
};
}// namespace Kataglyphis::VulkanRendererInternals