module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "renderer/BreadcrumbTrail.hpp"
#include "spdlog/spdlog.h"
#include "vulkan_base/PhysicalDeviceChoices.hpp"

module kataglyphis.vulkan.gpu_breadcrumbs;

import kataglyphis.vulkan.buffer;
import kataglyphis.vulkan.device;

using namespace Kataglyphis::VulkanRendererInternals;

namespace {

constexpr vk::DeviceSize kMarkerBytes = sizeof(std::uint32_t);

constexpr vk::DeviceSize regionMarkers(std::uint32_t capacity) { return 2U * static_cast<vk::DeviceSize>(capacity); }

// A driver's fixed-size description need not end in a terminator.
std::string fixedString(const char *chars, std::size_t size)
{
    const std::span<const char> text(chars, size);
    return std::string(text.begin(), std::ranges::find(text, '\0'));
}

}// namespace

void Kataglyphis::GpuBreadcrumbs::create(const std::shared_ptr<VulkanDevice> &vulkanDevice, std::uint32_t imageCount)
{
    device = vulkanDevice;
    recording = nullptr;
    markers = nullptr;
    markerBuffer.cleanUp();
    ring.resize(imageCount);
    activeBackend = device->breadcrumbBackend();

    const auto &dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
    if (activeBackend == BreadcrumbBackend::BufferMarker) {
        markerWrite2 = dispatcher.vkCmdWriteBufferMarker2AMD != nullptr;
        if (!markerWrite2 && dispatcher.vkCmdWriteBufferMarkerAMD == nullptr) {
            spdlog::warn("VK_AMD_buffer_marker is enabled but exposes no marker write; device-loss breadcrumbs off.");
            activeBackend = BreadcrumbBackend::None;
            return;
        }
        // Device-coherent and uncached where the device enabled them, so a hang strands no marker in a cache.
        const vk::MemoryPropertyFlags preferred =
          device->supportsDeviceCoherentMemory()
            ? vk::MemoryPropertyFlagBits::eDeviceCoherentAMD | vk::MemoryPropertyFlagBits::eDeviceUncachedAMD
            : vk::MemoryPropertyFlags{};
        const vk::DeviceSize marker_count = regionMarkers(ring.capacity()) * imageCount;
        markerBuffer.create(device,
          marker_count * kMarkerBytes,
          vk::BufferUsageFlagBits::eTransferDst,
          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
          {},
          preferred);
        markers = static_cast<std::uint32_t *>(markerBuffer.getMappedData());
        if (markers == nullptr) {
            spdlog::warn("The breadcrumb marker buffer is not mapped; device-loss breadcrumbs off.");
            markerBuffer.cleanUp();
            activeBackend = BreadcrumbBackend::None;
            return;
        }
        // Marker 0 is never a trail's, so a slot the GPU has not written yet matches nothing.
        std::fill_n(markers, marker_count, 0U);
    } else if (activeBackend == BreadcrumbBackend::Checkpoints && dispatcher.vkCmdSetCheckpointNV == nullptr) {
        spdlog::warn("VK_NV_device_diagnostic_checkpoints is enabled but exposes no checkpoint; breadcrumbs off.");
        activeBackend = BreadcrumbBackend::None;
    }
}

void Kataglyphis::GpuBreadcrumbs::beginRecording(vk::CommandBuffer commandBuffer, std::uint32_t imageIndex)
{
    ring.beginRecording(imageIndex);
    recording = ring.recordingSlot() ? commandBuffer : vk::CommandBuffer{};
}

void Kataglyphis::GpuBreadcrumbs::endRecording()
{
    ring.endRecording();
    recording = nullptr;
}

void Kataglyphis::GpuBreadcrumbs::beginScope(vk::CommandBuffer commandBuffer, std::string_view name)
{
    if (!recording || commandBuffer != recording) { return; }
    if (const auto event = ring.beginScope(name)) { writeMarker(commandBuffer, *event); }
}

void Kataglyphis::GpuBreadcrumbs::endScope(vk::CommandBuffer commandBuffer)
{
    if (!recording || commandBuffer != recording) { return; }
    if (const auto event = ring.endScope()) { writeMarker(commandBuffer, *event); }
}

void Kataglyphis::GpuBreadcrumbs::writeMarker(vk::CommandBuffer commandBuffer, const BreadcrumbEvent &event)
{
    if (activeBackend == BreadcrumbBackend::Checkpoints) {
        commandBuffer.setCheckpointNV(reinterpret_cast<const void *>(static_cast<std::uintptr_t>(event.marker)));
        return;
    }
    const std::optional<std::uint32_t> slot = ring.recordingSlot();
    if (activeBackend != BreadcrumbBackend::BufferMarker || !slot) { return; }

    // Every event has its own two slots, so no two marker writes of a command buffer touch one address.
    const vk::DeviceSize region = static_cast<vk::DeviceSize>(*slot) * regionMarkers(ring.capacity());
    const vk::DeviceSize top_offset = (region + event.index) * kMarkerBytes;
    const vk::DeviceSize bottom_offset = (region + ring.capacity() + event.index) * kMarkerBytes;
    // Top of pipe: the GPU got here; bottom of pipe: every command before it finished.
    vk::Buffer buffer = markerBuffer.getBuffer();
    if (markerWrite2) {
        commandBuffer.writeBufferMarker2AMD(vk::PipelineStageFlagBits2::eTopOfPipe, buffer, top_offset, event.marker);
        commandBuffer.writeBufferMarker2AMD(
          vk::PipelineStageFlagBits2::eBottomOfPipe, buffer, bottom_offset, event.marker);
    } else {
        commandBuffer.writeBufferMarkerAMD(vk::PipelineStageFlagBits::eTopOfPipe, buffer, top_offset, event.marker);
        commandBuffer.writeBufferMarkerAMD(
          vk::PipelineStageFlagBits::eBottomOfPipe, buffer, bottom_offset, event.marker);
    }
}

auto Kataglyphis::GpuBreadcrumbs::markersPassed(std::uint32_t slot) const -> BreadcrumbPassed
{
    const BreadcrumbTrail &trail = ring.trail(slot);
    const std::uint32_t events = std::min(trail.eventCount(), ring.capacity());
    const std::uint32_t *region = markers + (static_cast<std::size_t>(slot) * regionMarkers(ring.capacity()));
    // Copied once: the memory is uncached, and a plain read keeps the comparison free of a GPU still writing.
    std::vector<std::uint32_t> top(region, region + events);
    std::vector<std::uint32_t> bottom(region + ring.capacity(), region + ring.capacity() + events);
    return BreadcrumbPassed{ .top = lastWrittenMarker(top, trail.firstMarker()),
        .bottom = lastWrittenMarker(bottom, trail.firstMarker()) };
}

auto Kataglyphis::GpuBreadcrumbs::checkpointsPassed(bool simulated) const -> std::optional<BreadcrumbPassed>
{
    // VUID-vkGetQueueCheckpointData2NV-queue-03892: only a lost device holds checkpoint data.
    if (simulated) { return std::nullopt; }
    const auto &dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
    const vk::Queue queue = device->getGraphicsQueue();
    std::vector<CheckpointSample> samples;
    if (dispatcher.vkGetQueueCheckpointData2NV != nullptr) {
        for (const vk::CheckpointData2NV &data : queue.getCheckpointData2NV()) {
            samples.push_back(CheckpointSample{ .stage = data.stage,
              .marker = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(data.pCheckpointMarker)) });
        }
    } else if (dispatcher.vkGetQueueCheckpointDataNV != nullptr) {
        for (const vk::CheckpointDataNV &data : queue.getCheckpointDataNV()) {
            // The two stages the report reads keep their bit values in the synchronization2 flags.
            samples.push_back(
              CheckpointSample{ .stage = vk::PipelineStageFlags2(static_cast<VkPipelineStageFlags2>(data.stage)),
                .marker = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(data.pCheckpointMarker)) });
        }
    } else {
        return std::nullopt;
    }
    return passedFromCheckpoints(samples);
}

auto Kataglyphis::GpuBreadcrumbs::queryDeviceFault(bool simulated) const -> DeviceFaultInfo
{
    DeviceFaultInfo info{};
    if (!device || !device->supportsDeviceFault() || VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT == nullptr) {
        return info;
    }
    // VUID-vkGetDeviceFaultInfoEXT-device-07336: the device must be lost.
    if (simulated) {
        info.query = DeviceFaultQuery::NotQueriedSimulated;
        return info;
    }

    const vk::Device logical_device = device->getLogicalDevice();
    vk::DeviceFaultCountsEXT counts{};
    info.result = logical_device.getFaultInfoEXT(&counts, nullptr);
    if (info.result != vk::Result::eSuccess && info.result != vk::Result::eIncomplete) {
        info.query = DeviceFaultQuery::Failed;
        return info;
    }

    std::vector<vk::DeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<vk::DeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
    // The vendor binary feature is off, so no buffer is offered for a dump.
    counts.vendorBinarySize = 0;
    vk::DeviceFaultInfoEXT fault{};
    fault.pAddressInfos = addresses.data();
    fault.pVendorInfos = vendors.data();
    fault.pVendorBinaryData = nullptr;
    info.result = logical_device.getFaultInfoEXT(&counts, &fault);
    if (info.result != vk::Result::eSuccess && info.result != vk::Result::eIncomplete) {
        info.query = DeviceFaultQuery::Failed;
        return info;
    }

    info.query = DeviceFaultQuery::Reported;
    info.description = fixedString(fault.description.data(), fault.description.size());
    for (std::uint32_t index = 0; index < counts.addressInfoCount && index < addresses.size(); ++index) {
        info.addresses.push_back(DeviceFaultAddress{ .type = addresses[index].addressType,
          .address = addresses[index].reportedAddress,
          .precision = addresses[index].addressPrecision });
    }
    for (std::uint32_t index = 0; index < counts.vendorInfoCount && index < vendors.size(); ++index) {
        info.vendors.push_back(DeviceFaultVendor{
          .description = fixedString(vendors[index].description.data(), vendors[index].description.size()),
          .code = vendors[index].vendorFaultCode,
          .data = vendors[index].vendorFaultData });
    }
    return info;
}

std::vector<std::string> Kataglyphis::GpuBreadcrumbs::reportDeviceLoss(bool simulated) const
{
    std::optional<BreadcrumbPassed> queue_passed;
    bool markers_read = activeBackend == BreadcrumbBackend::BufferMarker;
    if (activeBackend == BreadcrumbBackend::Checkpoints) {
        queue_passed = checkpointsPassed(simulated);
        markers_read = queue_passed.has_value();
    }

    std::vector<BreadcrumbFrameReport> frames;
    for (const std::uint32_t slot : ring.submittedSlots()) {
        BreadcrumbFrameReport frame{ .slot = slot, .trail = &ring.trail(slot), .progress = std::nullopt };
        if (activeBackend == BreadcrumbBackend::BufferMarker) {
            frame.progress = analyzeBreadcrumbs(*frame.trail, markersPassed(slot));
        } else if (queue_passed) {
            frame.progress = analyzeBreadcrumbs(*frame.trail, *queue_passed);
        }
        frames.push_back(frame);
    }
    return formatDeviceLossReport(activeBackend, markers_read, frames, queryDeviceFault(simulated));
}
