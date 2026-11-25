// Host mirror of the raster push constants; BuildIntegrity.SharedStructOffsetsMatchTheCompiledSpirv pins its layout.
#pragma once
#include "common/HostDeviceGlmAliases.hpp"
namespace Kataglyphis::VulkanRendererInternals {

// Push constant structure for the raster
struct PushConstantRasterizer
{
    // The vertex stage derives the normal transform from it (gltf_material.slang's normal_transform).
    mat4 model;
    // This draw's object_description entry.
    uint objectIndex;
};

}// namespace Kataglyphis::VulkanRendererInternals
