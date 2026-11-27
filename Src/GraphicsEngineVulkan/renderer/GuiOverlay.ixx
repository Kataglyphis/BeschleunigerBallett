module;

#include <functional>
#include <span>

#include <vulkan/vulkan.hpp>

#include "common/RenderingHelper.hpp"
#include <imgui.h>
#include <imgui_impl_vulkan.h>

export module kataglyphis.vulkan.gui_overlay;

import kataglyphis.vulkan.debug;

export namespace Kataglyphis::VulkanRendererInternals {

// Sees the image a pass drew before the editor goes over it, in eColorAttachmentOptimal, and leaves it so.
using BeforeGuiHook = std::function<void(vk::CommandBuffer, vk::Image)>;

// Draws the editor into the caller's open pass on `image`; a hook gets the image first, the editor then a load pass.
inline void recordGuiOverlay(vk::CommandBuffer commandBuffer,
  vk::Image image,
  vk::ImageView view,
  vk::Extent2D extent,
  const BeforeGuiHook &beforeGui)
{
    if (beforeGui) {
        commandBuffer.endRendering();
        beforeGui(commandBuffer, image);
        const vk::RenderingAttachmentInfo color_attachment = Kataglyphis::buildRenderingAttachmentInfo(
          view, vk::ImageLayout::eColorAttachmentOptimal, vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore);
        commandBuffer.beginRendering(Kataglyphis::buildRenderingInfo(
          extent, std::span<const vk::RenderingAttachmentInfo>(&color_attachment, 1), nullptr));
    }

    Kataglyphis::debug::ScopedCmdLabel const gui_label(commandBuffer, "gui", { 0.90F, 0.70F, 0.20F, 1.0F });
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), static_cast<VkCommandBuffer>(commandBuffer));
}

}// namespace Kataglyphis::VulkanRendererInternals
