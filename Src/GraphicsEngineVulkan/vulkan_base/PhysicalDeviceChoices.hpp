#pragma once

#include <algorithm>
#include <cctype>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <vulkan/vulkan.hpp>

namespace Kataglyphis {

enum class GpuSelectionMode : std::uint8_t { Auto, Dedicated, Integrated };

// typeRank compares first, so no capability lets a lower device type outrank a higher one.
struct PhysicalDeviceScore {
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
{
    return extensionPresent && quadsSupported == VK_TRUE;
}

// Clamped per VUID-VkSamplerCreateInfo-anisotropyEnable-01071; a fixed 16x keeps texture quality equal across GPUs.
constexpr float resolveMaxAnisotropy(bool anisotropyEnabled, float deviceLimit)
{
    return anisotropyEnabled ? std::clamp(16.0F, 1.0F, deviceLimit) : 1.0F;
}

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
