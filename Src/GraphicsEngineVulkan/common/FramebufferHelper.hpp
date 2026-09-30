#pragma once

#include <span>
#include <vector>

#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

// Borrows attachments.data(): consume the result before the span's storage dies or changes.
constexpr vk::FramebufferCreateInfo buildFramebufferCreateInfo(vk::RenderPass render_pass,
  std::span<const vk::ImageView> attachments,
  vk::Extent2D extent,
  uint32_t layers = 1)
{
    return vk::FramebufferCreateInfo{ vk::FramebufferCreateFlags{}, render_pass,
        static_cast<uint32_t>(attachments.size()), attachments.data(), extent.width, extent.height, layers };
}

// Idempotent: a null device is a no-op, and handles are cleared through the reference so a second call is safe.
inline void destroyFramebuffers(vk::Device device, std::vector<vk::Framebuffer> &framebuffers)
{
    if (!device) { return; }
    for (auto &framebuffer : framebuffers) {
        if (framebuffer) { device.destroyFramebuffer(framebuffer); }
    }
    framebuffers.clear();
}

inline void destroyFramebuffer(vk::Device device, vk::Framebuffer &framebuffer)
{
    if (!device) { return; }
    if (framebuffer) {
        device.destroyFramebuffer(framebuffer);
        framebuffer = nullptr;
    }
}

}// namespace Kataglyphis
