module;

#include <algorithm>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "common/Globals.hpp"
#include "spdlog/spdlog.h"

export module kataglyphis.vulkan.frame_sync;

export namespace Kataglyphis {

// Owns the renderer's frame-synchronization primitives, extracted verbatim
// from VulkanRenderer. The deliberate split in sizing is the whole point of
// this class and must be preserved:
//   - image_available / in_flight_fences are sized PER FRAME-IN-FLIGHT
//     (frame_sync_count = min(MAX_FRAME_DRAWS, swapchain image count)); the
//     CPU cycles current_frame through this set.
//   - render_finished_by_image / images_in_flight_fences are sized PER
//     SWAPCHAIN IMAGE. A per-frame render-finished semaphore can be waited on
//     before it is signalled across a swapchain recreate, so render-finished
//     is keyed by image, not by frame.
class FrameSync
{
  public:
    // Allocates image_available/in_flight_fences sized frame_sync_count and
    // render_finished_by_image/images_in_flight_fences sized imageCount,
    // recreating from scratch (destroys any existing set first). On any
    // creation failure frame_sync_count is set to 0 and the function returns
    // early, exactly as the renderer did inline.
    void create(vk::Device logicalDevice, uint32_t imageCount)
    {
        resetAndSize(logicalDevice, imageCount);

        vk::SemaphoreCreateInfo semaphore_create_info{};

        vk::FenceCreateInfo fence_create_info{};
        fence_create_info.flags = vk::FenceCreateFlagBits::eSignaled;

        for (uint32_t i = 0; i < frame_sync_count; i++) {
            auto image_available_result_value = logicalDevice.createSemaphore(semaphore_create_info);
            auto image_available_result = image_available_result_value.result;
            auto image_available_handle = image_available_result_value.value;
            auto in_flight_fence_result_value = logicalDevice.createFence(fence_create_info);
            auto in_flight_fence_result = in_flight_fence_result_value.result;
            auto in_flight_fence_handle = in_flight_fence_result_value.value;

            if (image_available_result != vk::Result::eSuccess || in_flight_fence_result != vk::Result::eSuccess
                || !image_available_handle || !in_flight_fence_handle) {
                spdlog::error(
                  fmt::format("Failed to create synchronization objects for frame {} (imageAvailable={}, fence={}).",
                    i,
                    static_cast<int>(image_available_result),
                    static_cast<int>(in_flight_fence_result)));
                cleanUp(logicalDevice);
                return;
            }

            image_available[i] = image_available_handle;
            in_flight_fences[i] = in_flight_fence_handle;
        }

        for (uint32_t image = 0; image < imageCount; ++image) {
            auto render_finished_result_value = logicalDevice.createSemaphore(semaphore_create_info);
            auto render_finished_result = render_finished_result_value.result;
            auto render_finished_handle = render_finished_result_value.value;

            if (render_finished_result != vk::Result::eSuccess || !render_finished_handle) {
                spdlog::error(fmt::format("Failed to create render-finished semaphore for swapchain image {} ({}).",
                  image,
                  static_cast<int>(render_finished_result)));
                cleanUp(logicalDevice);
                return;
            }

            render_finished_by_image[image] = render_finished_handle;
        }

        for (uint32_t image = 0; image < imageCount; ++image) { images_in_flight_fences[image] = nullptr; }

        current_frame = 0;
    }

    // Device-free half of create(): tears down any existing set, computes the
    // frame_sync_count/imageCount split and resizes the four handle vectors
    // to match. cleanUp() zeroes frame_sync_count, so the sizing must be
    // computed after it, not before.
    void resetAndSize(vk::Device logicalDevice, uint32_t imageCount)
    {
        cleanUp(logicalDevice);

        frame_sync_count = std::min<uint32_t>(static_cast<uint32_t>(Kataglyphis::MAX_FRAME_DRAWS), imageCount);

        image_available.resize(frame_sync_count);
        render_finished_by_image.resize(imageCount);
        in_flight_fences.resize(frame_sync_count);
        images_in_flight_fences.resize(imageCount);
    }

    void cleanUp(vk::Device logicalDevice)
    {
        // Iterate each vector on its own: after a partial create failure their
        // lengths can differ, and a shared loop bound would leak whatever the
        // longer vector still holds.
        for (vk::Semaphore semaphore : render_finished_by_image) {
            if (semaphore) { logicalDevice.destroySemaphore(semaphore); }
        }
        render_finished_by_image.clear();

        for (vk::Semaphore semaphore : image_available) {
            if (semaphore) { logicalDevice.destroySemaphore(semaphore); }
        }
        image_available.clear();

        for (vk::Fence fence : in_flight_fences) {
            if (fence) { logicalDevice.destroyFence(fence); }
        }
        in_flight_fences.clear();
        images_in_flight_fences.clear();

        frame_sync_count = 0;
        current_frame = 0;
    }

    // current_frame = (current_frame + 1) % frame_sync_count; frame_sync_count
    // is min(MAX_FRAME_DRAWS, image count), not MAX_FRAME_DRAWS directly, so a
    // swapchain with fewer images than MAX_FRAME_DRAWS still cycles correctly.
    void advanceFrame()
    {
        // create() can fail (and zero frame_sync_count) after the caller's
        // frameSyncCount()==0 guard has already run this frame; guard the
        // modulo here too so that ordering can never divide by zero.
        if (frame_sync_count == 0) {
            current_frame = 0;
            return;
        }
        current_frame = (current_frame + 1) % frame_sync_count;
    }

    [[nodiscard]] uint32_t currentFrame() const { return current_frame; }
    [[nodiscard]] uint32_t frameSyncCount() const { return frame_sync_count; }

    [[nodiscard]] bool inFlightFencesEmpty() const { return in_flight_fences.empty(); }
    [[nodiscard]] size_t imageAvailableCount() const { return image_available.size(); }
    [[nodiscard]] size_t inFlightFenceCount() const { return in_flight_fences.size(); }
    [[nodiscard]] size_t renderFinishedCount() const { return render_finished_by_image.size(); }
    [[nodiscard]] size_t imagesInFlightFenceCount() const { return images_in_flight_fences.size(); }

    // Handles for the current frame-in-flight (indexed by current_frame).
    // Returned by reference so the draw path can take their address for the
    // waitForFences/resetFences/submit calls that expect a pointer to the
    // stored handle.
    vk::Semaphore &imageAvailableSemaphore() { return image_available[current_frame]; }
    vk::Fence &inFlightFence() { return in_flight_fences[current_frame]; }

    // Handles keyed by swapchain image index.
    vk::Semaphore &renderFinishedSemaphore(uint32_t image_index) { return render_finished_by_image[image_index]; }
    vk::Fence &imageInFlightFence(uint32_t image_index) { return images_in_flight_fences[image_index]; }

  private:
    uint32_t current_frame{ 0 };
    uint32_t frame_sync_count{ 1 };
    std::vector<vk::Semaphore> image_available;
    std::vector<vk::Semaphore> render_finished_by_image;
    std::vector<vk::Fence> in_flight_fences;
    std::vector<vk::Fence> images_in_flight_fences;
};
}// namespace Kataglyphis
