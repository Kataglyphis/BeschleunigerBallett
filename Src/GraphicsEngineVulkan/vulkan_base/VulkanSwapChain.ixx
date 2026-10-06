module;
#include <memory>

#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.swapchain;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.window;

export namespace Kataglyphis {
class VulkanSwapChain
{
  public:
    VulkanSwapChain();

    void initVulkanContext(const std::shared_ptr<VulkanDevice> &in_device,
      Kataglyphis::Frontend::Window *window,
      const vk::SurfaceKHR &surface,
      const vk::SwapchainKHR &oldSwapchain = nullptr);

    void recreate(const std::shared_ptr<VulkanDevice> &in_device, const vk::SurfaceKHR &surface);

    const vk::SwapchainKHR &getSwapChain() const { return swapchain; };
    uint32_t getNumberSwapChainImages() const { return static_cast<uint32_t>(swap_chain_images.size()); };
    const vk::Extent2D &getSwapChainExtent() const { return swap_chain_extent; };
    const vk::Format &getSwapChainFormat() const { return swap_chain_image_format; };
    Texture &getSwapChainImage(uint32_t index) { return swap_chain_images[index]; };
    // True when the surface advertised eTransferSrc among its supported usage
    // flags and the swapchain was therefore created with it. Only then may a
    // swapchain image be used as the source of a copy (frame capture).
    bool supportsTransferSrc() const { return transfer_src_supported; };

    void cleanUp();

    ~VulkanSwapChain();

  private:
    std::shared_ptr<VulkanDevice> device{ nullptr };
    Kataglyphis::Frontend::Window *window{ nullptr };

    vk::SwapchainKHR swapchain{};

    std::vector<Texture> swap_chain_images;
    vk::Format swap_chain_image_format{ vk::Format::eB8G8R8A8Unorm };
    vk::Extent2D swap_chain_extent{ 0, 0 };
    bool transfer_src_supported{ false };

    // Destroys the per-image views without touching the swapchain handle, so
    // recreate() can keep the old swapchain as the oldSwapchain handoff.
    void destroyImageViews();
};
}// namespace Kataglyphis
