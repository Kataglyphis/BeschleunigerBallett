#pragma once

#include <algorithm>
#include <cctype>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

enum class GpuSelectionMode : std::uint8_t { Auto, Dedicated, Integrated };

// typeRank compares first, so no capability lets a lower device type outrank a higher one.
struct PhysicalDeviceScore
{
    int typeRank;// 4 discrete, 3 integrated, 2 virtual, 1 cpu, 0 other
    uint32_t capability;// maxImageDimension2D, tie-break only
    friend auto operator<=>(const PhysicalDeviceScore &, const PhysicalDeviceScore &) = default;
};

inline auto parseGpuSelectionMode(std::string_view mode) -> GpuSelectionMode
{
    if (mode.empty()) { return GpuSelectionMode::Auto; }

    std::string lowered(mode);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });

    if (lowered == "dedicated") { return GpuSelectionMode::Dedicated; }
    if (lowered == "integrated") { return GpuSelectionMode::Integrated; }

    return GpuSelectionMode::Auto;
}

inline auto gpuSelectionModeToString(GpuSelectionMode mode) -> const char *
{
    switch (mode) {
    case GpuSelectionMode::Dedicated:
        return "dedicated";
    case GpuSelectionMode::Integrated:
        return "integrated";
    case GpuSelectionMode::Auto:
    default:
        return "auto";
    }
}

inline auto matchesSelectionMode(const vk::PhysicalDeviceProperties &properties, GpuSelectionMode mode) -> bool
{
    switch (mode) {
    case GpuSelectionMode::Dedicated:
        return properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
    case GpuSelectionMode::Integrated:
        return properties.deviceType == vk::PhysicalDeviceType::eIntegratedGpu;
    case GpuSelectionMode::Auto:
    default:
        return true;
    }
}

// A device may support only the linear bit; requesting quads there fails vkCreateDevice outright.
constexpr bool shouldEnableComputeDerivativeGroupQuads(bool extensionPresent, vk::Bool32 quadsSupported)
{ return extensionPresent && quadsSupported == VK_TRUE; }

// What a device-loss breadcrumb writes: AMD's marker lands in memory per command buffer, NV's checkpoint per queue.
enum class BreadcrumbBackend : std::uint8_t { None, BufferMarker, Checkpoints };

// What the device lists, and the features it reports for the listed extensions that have one.
struct DeviceDiagnosticsOffer
{
    bool bufferMarker = false;
    bool checkpoints = false;
    bool deviceCoherentMemoryListed = false;
    vk::Bool32 deviceCoherentMemoryFeature = VK_FALSE;
    bool deviceFaultListed = false;
    vk::Bool32 deviceFaultFeature = VK_FALSE;
};

struct DeviceDiagnostics
{
    BreadcrumbBackend backend = BreadcrumbBackend::None;
    bool deviceCoherentMemory = false;
    bool deviceFault = false;
};

constexpr DeviceDiagnostics chooseDeviceDiagnostics(const DeviceDiagnosticsOffer &offer)
{
    DeviceDiagnostics chosen{};
    if (offer.bufferMarker) {
        chosen.backend = BreadcrumbBackend::BufferMarker;
    } else if (offer.checkpoints) {
        chosen.backend = BreadcrumbBackend::Checkpoints;
    }
    // Uncached device-coherent memory keeps the last markers out of the caches a hang strands them in.
    chosen.deviceCoherentMemory = chosen.backend == BreadcrumbBackend::BufferMarker && offer.deviceCoherentMemoryListed
                                  && offer.deviceCoherentMemoryFeature == VK_TRUE;
    // The fault query needs the deviceFault feature, so the extension alone buys nothing.
    chosen.deviceFault = offer.deviceFaultListed && offer.deviceFaultFeature == VK_TRUE;
    return chosen;
}

// Empty for a device that offers none of them, so its creation requests exactly what it did before.
inline std::vector<const char *> deviceDiagnosticExtensions(const DeviceDiagnostics &diagnostics)
{
    std::vector<const char *> extensions;
    if (diagnostics.backend == BreadcrumbBackend::BufferMarker) {
        extensions.push_back(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
    } else if (diagnostics.backend == BreadcrumbBackend::Checkpoints) {
        extensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    }
    if (diagnostics.deviceCoherentMemory) { extensions.push_back(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME); }
    if (diagnostics.deviceFault) { extensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME); }
    return extensions;
}

constexpr const char *breadcrumbBackendName(BreadcrumbBackend backend)
{
    switch (backend) {
    case BreadcrumbBackend::BufferMarker:
        return VK_AMD_BUFFER_MARKER_EXTENSION_NAME;
    case BreadcrumbBackend::Checkpoints:
        return VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME;
    case BreadcrumbBackend::None:
    default:
        return "none";
    }
}

// Every pass renders through dynamic rendering and records sync2 barriers: nullptr, or the first thing missing.
constexpr const char *
  firstMissingRenderingRequirement(uint32_t apiVersion, vk::Bool32 dynamicRendering, vk::Bool32 synchronization2)
{
    if (VK_API_VERSION_MAJOR(apiVersion) < 1
        || (VK_API_VERSION_MAJOR(apiVersion) == 1 && VK_API_VERSION_MINOR(apiVersion) < 3)) {
        return "Vulkan 1.3";
    }
    if (dynamicRendering != VK_TRUE) { return "the dynamicRendering feature"; }
    if (synchronization2 != VK_TRUE) { return "the synchronization2 feature"; }
    return nullptr;
}

// Clamped per VUID-VkSamplerCreateInfo-anisotropyEnable-01071; a fixed 16x keeps texture quality equal across GPUs.
constexpr float resolveMaxAnisotropy(bool anisotropyEnabled, float deviceLimit)
{ return anisotropyEnabled ? std::clamp(16.0F, 1.0F, deviceLimit) : 1.0F; }

inline auto scorePhysicalDevice(const vk::PhysicalDeviceProperties &properties) -> PhysicalDeviceScore
{
    int type_rank = 0;

    switch (properties.deviceType) {
    case vk::PhysicalDeviceType::eDiscreteGpu:
        type_rank = 4;
        break;
    case vk::PhysicalDeviceType::eIntegratedGpu:
        type_rank = 3;
        break;
    case vk::PhysicalDeviceType::eVirtualGpu:
        type_rank = 2;
        break;
    case vk::PhysicalDeviceType::eCpu:
        type_rank = 1;
        break;
    default:
        type_rank = 0;
        break;
    }

    return PhysicalDeviceScore{ type_rank, properties.limits.maxImageDimension2D };
}

}// namespace Kataglyphis
