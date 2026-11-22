module;

#include <memory>
#include <vulkan/vulkan.hpp>

#include "common/FormatHelper.hpp"

export module kataglyphis.vulkan.depth_attachment;

import kataglyphis.vulkan.debug;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.texture;

export namespace Kataglyphis::VulkanRendererInternals {

// Shared creation chain for every raster stage's depth attachment:
// chooseDepthFormat -> createImage(..., eDepthStencilAttachment | extraUsage,
// eDeviceLocal) -> createImageView(..., viewAspect). Every caller keeps its
// own depth_format member (the return value) plus any stage-specific
// follow-up - Rasterizer's initial layout transition, or the "no transition"
// / "exactly one aspect" reasoning PostStage and DeferredRasterizer already
// state at their call sites - since those are caller decisions, not part of
// the chain itself. CascadedShadowMap's shadow map array is a deliberate
// non-goal: a sampled 2D array behind a comparison sampler, not a plain
// attachment, so it stays out of this helper.
vk::Format createDepthAttachment(Texture &target,
  const std::shared_ptr<VulkanDevice> &device,
  vk::Extent2D extent,
  vk::ImageUsageFlags extraUsage,
  vk::ImageAspectFlags viewAspect,
  const char *debugName)
{
    vk::Format format = chooseDepthFormat(device->getPhysicalDevice());

    target.createImage(device,
      extent.width,
      extent.height,
      1,
      format,
      vk::ImageTiling::eOptimal,
      vk::ImageUsageFlagBits::eDepthStencilAttachment | extraUsage,
      vk::MemoryPropertyFlagBits::eDeviceLocal);

    target.createImageView(device, format, viewAspect, 1);
    debug::nameObject(device->getLogicalDevice(), target.getImage(), debugName);
    debug::nameObject(device->getLogicalDevice(), target.getImageView(), debugName);

    return format;
}

}// namespace Kataglyphis::VulkanRendererInternals
