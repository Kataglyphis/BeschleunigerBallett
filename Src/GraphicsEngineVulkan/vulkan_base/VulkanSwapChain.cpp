module;
#include <memory>

#include "renderer/SwapChainDetails.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "GLFW/glfw3.h"
#include "common/Utilities.hpp"
#include "vulkan_base/SwapchainChoices.hpp"

module kataglyphis.vulkan.swapchain;

import kataglyphis.vulkan.queue_family_indices;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.window;

Kataglyphis::VulkanSwapChain::VulkanSwapChain() = default;

void Kataglyphis::VulkanSwapChain::initVulkanContext(const std::shared_ptr<VulkanDevice> &in_device,
  Kataglyphis::Frontend::Window *frontend_window,
  const vk::SurfaceKHR &surface,
  const vk::SwapchainKHR &oldSwapchain)
{
    this->device = in_device;
    this->window = frontend_window;

    // get swap chain details so we can pick the best settings
    Kataglyphis::VulkanRendererInternals::SwapChainDetails const swap_chain_details = device->getSwapchainDetails();

    vk::SurfaceFormatKHR const surface_format = chooseBestSurfaceFormat(swap_chain_details.formats);
    vk::PresentModeKHR const present_mode = chooseBestPresentationMode(swap_chain_details.presentation_mode);

    // A max currentExtent lets the window decide; that needs the live window, so it stays out of SwapchainChoices.
    vk::Extent2D extent{};
    const vk::SurfaceCapabilitiesKHR &surface_capabilities = swap_chain_details.surface_capabilities;
    if (surface_capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        extent = surface_capabilities.currentExtent;
    } else {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window->get_window(), &width, &height);
        extent = clampSwapExtent(surface_capabilities, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }

    // One over the minimum, for triple buffering.
    uint32_t image_count = swap_chain_details.surface_capabilities.minImageCount + 1;

    // if maxImageCount == 0, then limitless
    if (swap_chain_details.surface_capabilities.maxImageCount > 0
        && swap_chain_details.surface_capabilities.maxImageCount < image_count) {
        image_count = swap_chain_details.surface_capabilities.maxImageCount;
    }

    // get queue family indices
    Kataglyphis::VulkanRendererInternals::QueueFamilyIndices const indices = device->getQueueFamilies();

    vk::SwapchainCreateInfoKHR swap_chain_create_info{};
    swap_chain_create_info.surface = surface;// swapchain surface
    swap_chain_create_info.imageFormat = surface_format.format;// swapchain format
    swap_chain_create_info.imageColorSpace = surface_format.colorSpace;// swapchain color space
    swap_chain_create_info.presentMode = present_mode;// swapchain presentation mode
    swap_chain_create_info.imageExtent = extent;// swapchain image extents
    swap_chain_create_info.minImageCount = image_count;// minimum images in swapchain
    swap_chain_create_info.imageArrayLayers = 1;// number of layers for each image in chain
    // Unconditional: every present-capable surface supports color-attachment and transfer-dst use.
    vk::ImageUsageFlags image_usage =
      vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;

    // eSampled and eStorage are optional; requesting an unsupported usage fails createSwapchainKHR.
    const vk::ImageUsageFlags supported_usage = swap_chain_details.surface_capabilities.supportedUsageFlags;
    if (supported_usage & vk::ImageUsageFlagBits::eSampled) { image_usage |= vk::ImageUsageFlagBits::eSampled; }
    if (supported_usage & vk::ImageUsageFlagBits::eStorage) { image_usage |= vk::ImageUsageFlagBits::eStorage; }

    // Without eTransferSrc, frame capture degrades to unsupported instead of failing creation.
    transfer_src_supported = static_cast<bool>(supported_usage & vk::ImageUsageFlagBits::eTransferSrc);
    if (transfer_src_supported) { image_usage |= vk::ImageUsageFlagBits::eTransferSrc; }

    swap_chain_create_info.imageUsage = image_usage;
    swap_chain_create_info.preTransform =
      swap_chain_details.surface_capabilities.currentTransform;// transform to perform on swap chain images
    swap_chain_create_info.compositeAlpha =
      vk::CompositeAlphaFlagBitsKHR::eOpaque;// dont do blending; everything opaque
    swap_chain_create_info.clipped = vk::True;// of course activate clipping ! :)

    // Function scope: createSwapchainKHR below reads pQueueFamilyIndices after the branch has ended.
    std::array<uint32_t, 2> queue_family_indices{};

    // Separate graphics and present families must share the images concurrently.
    if (indices.graphics_family != indices.presentation_family) {
        queue_family_indices = { static_cast<uint32_t>(indices.graphics_family),
            static_cast<uint32_t>(indices.presentation_family) };

        swap_chain_create_info.imageSharingMode = vk::SharingMode::eConcurrent;// image share handling
        swap_chain_create_info.queueFamilyIndexCount = 2;// number of queues to share images between
        swap_chain_create_info.pQueueFamilyIndices = queue_family_indices.data();// array of queues to share between

    } else {
        swap_chain_create_info.imageSharingMode = vk::SharingMode::eExclusive;
        swap_chain_create_info.queueFamilyIndexCount = 0;
        swap_chain_create_info.pQueueFamilyIndices = nullptr;
    }

    // The outgoing swapchain lets the driver recycle its images instead of a fresh allocation on every resize.
    swap_chain_create_info.oldSwapchain = oldSwapchain;

    // create swap chain
    vk::ResultValue<vk::SwapchainKHR> swapchain_result =
      device->getLogicalDevice().createSwapchainKHR(swap_chain_create_info);
    // A resize racing the recreate can fail this; a stored null handle would be UB, so fail fast.
    ASSERT_VULKAN(swapchain_result.result, "Failed to (re)create swapchain!");
    swapchain = swapchain_result.value;

    // store for later reference
    swap_chain_image_format = surface_format.format;
    swap_chain_extent = extent;

    // get swapchain images
    vk::ResultValue<std::vector<vk::Image>> images_result = device->getLogicalDevice().getSwapchainImagesKHR(swapchain);
    ASSERT_VULKAN(images_result.result, "Failed to get swapchain images!");
    std::vector<vk::Image> images = images_result.value;

    swap_chain_images.clear();

    for (size_t i = 0; i < images.size(); i++) {
        vk::Image image = images[static_cast<uint32_t>(i)];
        swap_chain_images.emplace_back();
        Texture &swap_chain_image = swap_chain_images.back();
        swap_chain_image.setImage(image);
        swap_chain_image.createImageView(device, swap_chain_image_format, vk::ImageAspectFlagBits::eColor, 1);// COLOR_ATTACHMENT_CHAIN_OK: view-over-an-image-the-swapchain-owns
    }
}

void Kataglyphis::VulkanSwapChain::destroyImageViews()
{
    // Frees only the views: setImage() marked the swapchain images as not owned.
    for (Texture &image : swap_chain_images) { image.cleanUp(); }
    swap_chain_images.clear();
}

void Kataglyphis::VulkanSwapChain::cleanUp()
{
    if (!device) { return; }
    destroyImageViews();
    device->getLogicalDevice().destroySwapchainKHR(swapchain);
    swapchain = nullptr;
    device.reset();
}

Kataglyphis::VulkanSwapChain::~VulkanSwapChain() { cleanUp(); }

void Kataglyphis::VulkanSwapChain::recreate(const std::shared_ptr<VulkanDevice> &in_device, const vk::SurfaceKHR &surface)
{
    // The old handle survives the create as the oldSwapchain handoff; its views go first, as they reference its images.
    const vk::SwapchainKHR previousSwapchain = swapchain;
    destroyImageViews();
    initVulkanContext(in_device, window, surface, previousSwapchain);
    if (previousSwapchain) { in_device->getLogicalDevice().destroySwapchainKHR(previousSwapchain); }
}
