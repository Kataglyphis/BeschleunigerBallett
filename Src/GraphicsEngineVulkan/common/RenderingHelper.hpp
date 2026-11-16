#pragma once

#include <span>

#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

// Single-sample and unresolved; a pass that resolves or multisamples builds its own and says why.
inline vk::RenderingAttachmentInfo buildRenderingAttachmentInfo(vk::ImageView image_view,
  vk::ImageLayout layout,
  vk::AttachmentLoadOp load_op = vk::AttachmentLoadOp::eClear,
  vk::AttachmentStoreOp store_op = vk::AttachmentStoreOp::eStore,
  vk::ClearValue clear_value = {})
{
    vk::RenderingAttachmentInfo attachment{};
    attachment.imageView = image_view;
    attachment.imageLayout = layout;
    attachment.resolveMode = vk::ResolveModeFlagBits::eNone;
    attachment.loadOp = load_op;
    attachment.storeOp = store_op;
    attachment.clearValue = clear_value;
    return attachment;
}

// Always the full extent; borrows color_attachments and depth_attachment until vkCmdBeginRendering consumes it.
inline vk::RenderingInfo buildRenderingInfo(vk::Extent2D extent,
  std::span<const vk::RenderingAttachmentInfo> color_attachments,
  const vk::RenderingAttachmentInfo *depth_attachment,
  uint32_t view_mask = 0)
{
    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{ vk::Offset2D{ 0, 0 }, extent };
    // A multiview pass (view_mask != 0) ignores layerCount: the mask names the layers.
    rendering_info.layerCount = 1;
    rendering_info.viewMask = view_mask;
    rendering_info.colorAttachmentCount = static_cast<uint32_t>(color_attachments.size());
    rendering_info.pColorAttachments = color_attachments.data();
    rendering_info.pDepthAttachment = depth_attachment;
    return rendering_info;
}

}// namespace Kataglyphis
