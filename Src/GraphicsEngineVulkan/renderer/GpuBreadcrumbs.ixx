module;

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "renderer/BreadcrumbTrail.hpp"
#include "vulkan_base/PhysicalDeviceChoices.hpp"

export module kataglyphis.vulkan.gpu_breadcrumbs;

import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.debug;
import kataglyphis.vulkan.device;

export namespace Kataglyphis {

// A GPU marker at each label scope of a frame, under CPU-side names; docs/source/gpu-golden-testing.md § Device loss.
class GpuBreadcrumbs final : public debug::CmdLabelObserver
{
  public:
    GpuBreadcrumbs() = default;
    GpuBreadcrumbs(const GpuBreadcrumbs &) = delete;
    GpuBreadcrumbs &operator=(const GpuBreadcrumbs &) = delete;
    GpuBreadcrumbs(GpuBreadcrumbs &&) = delete;
    GpuBreadcrumbs &operator=(GpuBreadcrumbs &&) = delete;
    ~GpuBreadcrumbs() override = default;

    // One trail and one marker region per swapchain image, as the command buffers are; the old trails go.
    void create(const std::shared_ptr<VulkanDevice> &vulkanDevice, std::uint32_t imageCount);

    void beginRecording(vk::CommandBuffer commandBuffer, std::uint32_t imageIndex);
    void endRecording();
    void markSubmitted(std::uint32_t imageIndex) { ring.markSubmitted(imageIndex); }

    // For a scope with no label of its own, such as the main stage the path tracer records inside.
    void beginScope(vk::CommandBuffer commandBuffer, std::string_view name);
    void endScope(vk::CommandBuffer commandBuffer);

    void labelBegun(vk::CommandBuffer commandBuffer, const char *name) override { beginScope(commandBuffer, name); }
    void labelEnded(vk::CommandBuffer commandBuffer) override { endScope(commandBuffer); }

    // A simulated loss leaves the device live, where the checkpoint and fault queries are invalid usage.
    [[nodiscard]] std::vector<std::string> reportDeviceLoss(bool simulated) const;

    [[nodiscard]] BreadcrumbBackend backend() const { return activeBackend; }

  private:
    void writeMarker(vk::CommandBuffer commandBuffer, const VulkanRendererInternals::BreadcrumbEvent &event);
    [[nodiscard]] VulkanRendererInternals::BreadcrumbPassed markersPassed(std::uint32_t slot) const;
    [[nodiscard]] std::optional<VulkanRendererInternals::BreadcrumbPassed> checkpointsPassed(bool simulated) const;
    [[nodiscard]] VulkanRendererInternals::DeviceFaultInfo queryDeviceFault(bool simulated) const;

    std::shared_ptr<VulkanDevice> device;
    BreadcrumbBackend activeBackend = BreadcrumbBackend::None;
    VulkanRendererInternals::BreadcrumbRing ring;
    // Per image: one top-of-pipe slot per event, then one bottom-of-pipe slot per event.
    VulkanBuffer markerBuffer;
    std::uint32_t *markers = nullptr;
    bool markerWrite2 = false;
    vk::CommandBuffer recording{};
};

}// namespace Kataglyphis
