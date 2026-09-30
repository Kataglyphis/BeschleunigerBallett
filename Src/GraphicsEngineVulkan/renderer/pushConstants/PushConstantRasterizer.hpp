// Host mirror of the raster push constants; BuildIntegrity.SharedStructOffsetsMatchTheCompiledSpirv pins its layout.
#pragma once
#include "common/HostDeviceGlmAliases.hpp"
namespace Kataglyphis::VulkanRendererInternals {

// Push constant structure for the raster
struct PushConstantRasterizer
{
    mat4 model;// matrix of the instance
    // Inverse-transpose rows of the upper 3x3, read across GLM's columns; a full mat4 would exceed 128 bytes.
    vec4 invModelRows[3];
    // This draw's object_description entry.
    uint objectIndex;
};

}// namespace Kataglyphis::VulkanRendererInternals
