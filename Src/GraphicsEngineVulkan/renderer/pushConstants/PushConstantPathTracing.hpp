// Host mirror of the path-tracing push constants; SharedStructOffsetsMatchTheCompiledSpirv pins its layout.
#pragma once
#include "common/HostDeviceGlmAliases.hpp"
namespace Kataglyphis::VulkanRendererInternals {

struct PushConstantPathTracing
{
    vec4 clearColor;
    uint width;
    uint height;
    // Frames since the last history reset; 0 discards history. Also seeds the RNG.
    uint frame_index;
    // GUI-driven quality: samples per pixel per frame and the bounce cap.
    uint samples_per_pixel;
    uint max_bounces;
    // Furnace mode's white surface (clearColor.w > 0.5): a dielectric at metallic 0, a white metal at 1.
    float furnace_metallic;
    float furnace_roughness;
    // Nonzero when the clouds read this frame's traced depth; otherwise its extra primary ray is skipped.
    uint trace_cloud_depth;
    // Nonzero while the skybox is on: a bounce that misses samples its cube map, else the disabled sky's navy.
    uint skybox_enabled;
};

}// namespace Kataglyphis::VulkanRendererInternals
