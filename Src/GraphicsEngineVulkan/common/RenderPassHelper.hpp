#pragma once

#include <span>

#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

// Single-sample, stencil don't-care; a multisampled or stencil-using attachment builds its own and says why.
constexpr vk::AttachmentDescription buildAttachmentDescription(vk::Format format,
  vk::ImageLayout final_layout,
  vk::AttachmentLoadOp load_op = vk::AttachmentLoadOp::eClear,
  vk::AttachmentStoreOp store_op = vk::AttachmentStoreOp::eStore,
  vk::ImageLayout initial_layout = vk::ImageLayout::eUndefined)
{
    vk::AttachmentDescription description{};
    description.format = format;
    description.samples = vk::SampleCountFlagBits::e1;
    description.loadOp = load_op;
    description.storeOp = store_op;
    description.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    description.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    description.initialLayout = initial_layout;
    description.finalLayout = final_layout;
    return description;
}

// Always the full extent; a pass needing a partial render area builds its own and says why.
constexpr vk::RenderPassBeginInfo buildRenderPassBeginInfo(vk::RenderPass render_pass,
  vk::Framebuffer framebuffer,
  vk::Extent2D extent,
  std::span<const vk::ClearValue> clear_values)
{
    return vk::RenderPassBeginInfo{ render_pass, framebuffer, vk::Rect2D{ vk::Offset2D{ 0, 0 }, extent },
        static_cast<uint32_t>(clear_values.size()), clear_values.data() };
}

// Borrows both spans and depth_attachment: they must outlive the createRenderPass call.
constexpr vk::SubpassDescription buildSubpassDescription(
  std::span<const vk::AttachmentReference> color_attachments,
  const vk::AttachmentReference *depth_attachment,
  std::span<const vk::AttachmentReference> input_attachments = {})
{
    vk::SubpassDescription subpass{};
    subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
    subpass.colorAttachmentCount = static_cast<uint32_t>(color_attachments.size());
    subpass.pColorAttachments = color_attachments.data();
    subpass.pDepthStencilAttachment = depth_attachment;
    subpass.inputAttachmentCount = static_cast<uint32_t>(input_attachments.size());
    subpass.pInputAttachments = input_attachments.data();
    return subpass;
}

// Borrows all three spans; pNext is left for the caller (CascadedShadowMap chains multiview).
constexpr vk::RenderPassCreateInfo buildRenderPassCreateInfo(
  std::span<const vk::AttachmentDescription> attachments,
  std::span<const vk::SubpassDescription> subpasses,
  std::span<const vk::SubpassDependency> dependencies)
{
    return vk::RenderPassCreateInfo{ vk::RenderPassCreateFlags{}, static_cast<uint32_t>(attachments.size()),
        attachments.data(), static_cast<uint32_t>(subpasses.size()), subpasses.data(),
        static_cast<uint32_t>(dependencies.size()), dependencies.data() };
}

// The depth buffer is shared across frames in flight, so the last write must land before this clear; not by-region.
constexpr vk::SubpassDependency buildExternalColorDepthDependency()
{
    vk::SubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput
                              | vk::PipelineStageFlagBits::eEarlyFragmentTests
                              | vk::PipelineStageFlagBits::eLateFragmentTests;
    dependency.srcAccessMask =
      vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    dependency.dstStageMask =
      vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests;
    dependency.dstAccessMask =
      vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    dependency.dependencyFlags = vk::DependencyFlags{};
    return dependency;
}

// PostStage's colour-only twin: orders its load against SkyBox's earlier write to the same swapchain image.
constexpr vk::SubpassDependency buildExternalColorDependency()
{
    vk::SubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    dependency.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    dependency.dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    dependency.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    dependency.dependencyFlags = vk::DependencyFlags{};
    return dependency;
}

// Idempotent: a null device is a no-op and the handle is nulled through the reference.
inline void destroyRenderPass(vk::Device device, vk::RenderPass &render_pass)
{
    if (!device) { return; }
    if (render_pass) {
        device.destroyRenderPass(render_pass);
        render_pass = nullptr;
    }
}

}// namespace Kataglyphis
