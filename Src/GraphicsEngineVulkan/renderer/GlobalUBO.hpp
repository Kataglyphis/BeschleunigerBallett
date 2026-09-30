// Host mirror of the global UBO; BuildIntegrity.SharedStructOffsetsMatchTheCompiledSpirv pins its layout.
#pragma once
#include "common/HostDeviceGlmAliases.hpp"
namespace Kataglyphis::VulkanRendererInternals {

struct GlobalUBO
{
    mat4 projection;
    mat4 view;
    // Precomputed per frame: inverse() per pixel in the clouds shader would be ruinous.
    mat4 inv_projection;
    mat4 inv_view;
};
}// namespace Kataglyphis::VulkanRendererInternals