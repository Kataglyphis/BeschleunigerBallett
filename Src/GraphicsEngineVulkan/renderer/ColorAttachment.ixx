module;

#include <memory>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.color_attachment;

import kataglyphis.vulkan.debug;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.texture;

export namespace Kataglyphis::VulkanRendererInternals {

// Shared creation chain for every raster stage's plain colour attachment:
// createImage(..., eColorAttachment | extraUsage, eDeviceLocal) ->
// createImageView(..., eColor). Every caller keeps its own follow-up work -
// Rasterizer's and VulkanRenderer's layout transitions are caller decisions,
// not part of the chain itself. Five deliberate non-goals stay out of this
// helper because they are not plain single-layer 2D colour attachments:
// Clouds::createStorageTexture (a 3D/array storage texture that passes
// vk::ImageType, depth and a view type), SkyBox (six layers +
// eCubeCompatible), CascadedShadowMap (a sampled depth 2D array behind a
// comparison sampler, not a colour attachment at all), Texture::createFromRgba
// (a real mip chain) and VulkanSwapChain (a view over an image the swapchain
// owns).
void createColorAttachment(Texture &target,
  const std::shared_ptr<VulkanDevice> &device,
  vk::Extent2D extent,
  vk::Format format,
  vk::ImageUsageFlags usage,
  const char *debugName)
{
    target.createImage(device,
      extent.width,
      extent.height,
      1,
      format,
      vk::ImageTiling::eOptimal,
      usage,
      vk::MemoryPropertyFlagBits::eDeviceLocal);

    target.createImageView(device, format, vk::ImageAspectFlagBits::eColor, 1);
    debug::nameObject(device->getLogicalDevice(), target.getImage(), debugName);
    debug::nameObject(device->getLogicalDevice(), target.getImageView(), debugName);
}

}// namespace Kataglyphis::VulkanRendererInternals
