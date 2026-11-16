module;

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "common/FormatHelper.hpp"
#include "common/ImageBarrierHelper.hpp"
#include "spdlog/spdlog.h"

export module kataglyphis.vulkan.frame_capture;

import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.swapchain;
import kataglyphis.vulkan.texture;

export namespace Kataglyphis {

// Owns the renderer's headless frame-capture state, extracted verbatim from
// VulkanRenderer, mirroring the FrameSync/GpuTimingSubsystem extractions.
//
// The capture is recorded *inside* the frame's own command buffer, right
// after the post stage transitions the swapchain image to ePresentSrcKHR and
// before the present. Copying the image after vkQueuePresentKHR would touch
// an image owned by the presentation engine, so the copy is armed one frame
// ahead: request() arms it, record() copies the swapchain image into a
// host-visible staging buffer inside the next recorded command buffer,
// bindSubmitFence() ties that copy to the frame's in-flight fence, and take()
// waits on exactly that fence before handing back the pixels.
class FrameCapture
{
  public:
    [[nodiscard]] bool supportsCapture(const VulkanSwapChain &swapChain, bool deviceLostDetected) const
    {
        return !deviceLostDetected && swapChain.supportsTransferSrc()
               && isCapturableSwapchainFormat(swapChain.getSwapChainFormat());
    }

    [[nodiscard]] bool isArmed() const { return armed; }

    // Arms a capture for the next recorded frame. Callers are expected to have
    // already checked supportsCapture() (VulkanRenderer::requestFrameCapture
    // logs a warning and skips arming when unsupported - this class stays
    // silent so it has no logging policy of its own to get wrong).
    void request()
    {
        armed = true;
        pending = false;
        fence = vk::Fence{};
    }

    // Copies the swapchain image at image_index into the staging buffer,
    // recorded into commandBuffer. Growing the staging buffer only happens
    // when the extent grew or it does not exist yet - safe because a resize
    // goes through VulkanRenderer::recreateSwapChain(), which waits idle and
    // invalidates any pending capture's fence first.
    void record(const std::shared_ptr<VulkanDevice> &device,
      vk::CommandBuffer &commandBuffer,
      VulkanSwapChain &swapChain,
      uint32_t image_index,
      bool deviceLostDetected)
    {
        armed = false;

        if (!device || !supportsCapture(swapChain, deviceLostDetected)) { return; }

        recordCopy(device,
          commandBuffer,
          swapChain.getSwapChainImage(image_index).getImage(),
          swapChain.getSwapChainExtent(),
          swapChain.getSwapChainFormat(),
          CopySource{ .layout = vk::ImageLayout::ePresentSrcKHR,
            .lastStages = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .lastAccess = vk::AccessFlagBits2::eColorAttachmentWrite,
            .nextStages = vk::PipelineStageFlagBits2::eBottomOfPipe });
    }

    // The same copy from a colour image the frame leaves in `layout` after `lastStages` last touched it.
    void recordImage(const std::shared_ptr<VulkanDevice> &device,
      vk::CommandBuffer &commandBuffer,
      vk::Image image,
      vk::Extent2D extent,
      vk::Format imageFormat,
      vk::ImageLayout layout,
      vk::PipelineStageFlags2 lastStages,
      bool deviceLostDetected)
    {
        armed = false;

        if (!device || deviceLostDetected || !image) { return; }

        // Every later use of the image starts its own barrier, so the restore must chain into all of them.
        recordCopy(device,
          commandBuffer,
          image,
          extent,
          imageFormat,
          CopySource{ .layout = layout,
            .lastStages = lastStages,
            .lastAccess = vk::AccessFlagBits2::eNone,
            .nextStages = vk::PipelineStageFlagBits2::eAllCommands });
    }

    // Ties the just-submitted frame's in-flight fence to a pending capture, so
    // take() knows exactly what to wait on. A no-op when nothing is pending.
    void bindSubmitFence(vk::Fence submitFence)
    {
        if (pending) { fence = submitFence; }
    }

    // Drops a stale fence across a swapchain recreation (createSynchronization
    // destroys and recreates every fence). The caller must have already
    // waited idle - that already guarantees a pending capture's copy has
    // completed, so the staged pixels stay readable; only the now-dangling
    // fence handle goes away.
    void invalidateFence() { fence = vk::Fence{}; }

    // Waits on the recorded fence (never waitIdle) and returns tightly packed
    // RGBA8 pixels, swizzled from BGRA when the swapchain used a B8G8R8A8
    // format, or the raw RGBA16F texels of a recordImage() copy of the HDR
    // target. Returns an empty vector when nothing is pending, the device is
    // lost, or the wait fails. On eErrorDeviceLost, sets deviceLostDetected so
    // the caller's own device-lost bookkeeping stays in sync.
    std::vector<uint8_t> take(const std::shared_ptr<VulkanDevice> &device,
      bool &deviceLostDetected,
      uint32_t &outWidth,
      uint32_t &outHeight)
    {
        outWidth = 0;
        outHeight = 0;

        if (!pending || deviceLostDetected || !device) { return {}; }

        pending = false;

        if (fence) {
            const vk::Result wait_result =
              device->getLogicalDevice().waitForFences(1, &fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
            if (wait_result != vk::Result::eSuccess) {
                spdlog::error(
                  "Failed to wait for the frame capture fence (vk::Result={})", static_cast<int>(wait_result));
                if (wait_result == vk::Result::eErrorDeviceLost) { deviceLostDetected = true; }
                return {};
            }
        }

        const void *mapped = buffer.getMappedData();
        if (mapped == nullptr || width == 0 || height == 0) { return {}; }

        const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
        std::vector<uint8_t> pixels(pixel_count * capturedTexelBytes(format));
        std::memcpy(pixels.data(), mapped, pixels.size());

        // Normalize to RGBA8 regardless of the swapchain's channel order.
        if (capturedFormatIsBgra(format)) {
            for (size_t i = 0; i < pixels.size(); i += 4U) { std::swap(pixels[i], pixels[i + 2U]); }
        }

        outWidth = width;
        outHeight = height;
        return pixels;
    }

    void cleanUp()
    {
        buffer.cleanUp();
        buffer_size = 0;
        armed = false;
        pending = false;
        fence = vk::Fence{};
        width = 0;
        height = 0;
    }

  private:
    // Where the image is when the copy starts, and the stages on either side of it.
    struct CopySource
    {
        vk::ImageLayout layout;
        vk::PipelineStageFlags2 lastStages;
        vk::AccessFlags2 lastAccess;
        vk::PipelineStageFlags2 nextStages;
    };

    void recordCopy(const std::shared_ptr<VulkanDevice> &device,
      vk::CommandBuffer &commandBuffer,
      vk::Image image,
      vk::Extent2D extent,
      vk::Format imageFormat,
      const CopySource &source)
    {
        const uint32_t texel_bytes = capturedTexelBytes(imageFormat);
        if (extent.width == 0 || extent.height == 0 || texel_bytes == 0U) { return; }

        const vk::DeviceSize required_size =
          static_cast<vk::DeviceSize>(extent.width) * static_cast<vk::DeviceSize>(extent.height) * texel_bytes;

        if (buffer_size < required_size) {
            // Redundant since create() now releases the previous allocation itself,
            // but kept: it documents that destroying here (before the new create)
            // is synchronisation-safe.
            buffer.cleanUp();
            buffer.create(device,
              required_size,
              vk::BufferUsageFlagBits::eTransferDst,
              vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
            buffer_size = required_size;
        }

        if (buffer.getMappedData() == nullptr) {
            spdlog::error("Frame capture staging buffer is not host mapped; capture skipped.");
            return;
        }

        const vk::ImageMemoryBarrier2 to_transfer_source = Kataglyphis::buildImageMemoryBarrier(image,
          source.layout,
          vk::ImageLayout::eTransferSrcOptimal,
          source.lastStages,
          source.lastAccess,
          vk::PipelineStageFlagBits2::eCopy,
          vk::AccessFlagBits2::eTransferRead);

        // A re-armed capture copies into the buffer an earlier, still-running frame may be writing (WAW).
        vk::BufferMemoryBarrier2 previous_copy{};
        previous_copy.srcStageMask = vk::PipelineStageFlagBits2::eCopy;
        previous_copy.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        previous_copy.dstStageMask = vk::PipelineStageFlagBits2::eCopy;
        previous_copy.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
        previous_copy.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
        previous_copy.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
        previous_copy.buffer = buffer.getBuffer();
        previous_copy.offset = 0;
        previous_copy.size = required_size;

        vk::DependencyInfo before_copy{};
        before_copy.bufferMemoryBarrierCount = 1;
        before_copy.pBufferMemoryBarriers = &previous_copy;
        before_copy.imageMemoryBarrierCount = 1;
        before_copy.pImageMemoryBarriers = &to_transfer_source;
        commandBuffer.pipelineBarrier2(before_copy);

        vk::BufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;// tightly packed
        region.bufferImageHeight = 0;
        region.imageSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
        region.imageOffset = vk::Offset3D{ 0, 0, 0 };
        region.imageExtent = vk::Extent3D{ extent.width, extent.height, 1 };

        commandBuffer.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer.getBuffer(), 1, &region);

        // Restore the layout the image's next user expects.
        const vk::ImageMemoryBarrier2 back_to_source = Kataglyphis::buildImageMemoryBarrier(image,
          vk::ImageLayout::eTransferSrcOptimal,
          source.layout,
          vk::PipelineStageFlagBits2::eCopy,
          vk::AccessFlagBits2::eTransferRead,
          source.nextStages,
          vk::AccessFlagBits2::eNone);

        // Make the copy visible to host reads of the staging buffer as well.
        vk::BufferMemoryBarrier2 buffer_to_host{};
        buffer_to_host.srcStageMask = vk::PipelineStageFlagBits2::eCopy;
        buffer_to_host.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        buffer_to_host.dstStageMask = vk::PipelineStageFlagBits2::eHost;
        buffer_to_host.dstAccessMask = vk::AccessFlagBits2::eHostRead;
        buffer_to_host.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
        buffer_to_host.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
        buffer_to_host.buffer = buffer.getBuffer();
        buffer_to_host.offset = 0;
        buffer_to_host.size = required_size;

        vk::DependencyInfo after_copy{};
        after_copy.bufferMemoryBarrierCount = 1;
        after_copy.pBufferMemoryBarriers = &buffer_to_host;
        after_copy.imageMemoryBarrierCount = 1;
        after_copy.pImageMemoryBarriers = &back_to_source;
        commandBuffer.pipelineBarrier2(after_copy);

        width = extent.width;
        height = extent.height;
        format = imageFormat;
        pending = true;
        // Set to the frame's in-flight fence by bindSubmitFence() right after
        // the submit.
        fence = vk::Fence{};
    }

    bool armed{ false };
    bool pending{ false };
    vk::Fence fence{};
    VulkanBuffer buffer;
    vk::DeviceSize buffer_size{ 0 };
    uint32_t width{ 0 };
    uint32_t height{ 0 };
    vk::Format format{ vk::Format::eUndefined };
};
}// namespace Kataglyphis
