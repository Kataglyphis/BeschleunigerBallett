// Host mirror of the path-tracing push constants; SharedStructOffsetsMatchTheCompiledSpirv pins its layout.
#pragma once
#include "common/HostDeviceGlmAliases.hpp"
namespace Kataglyphis::VulkanRendererInternals {

struct PushConstantPathTracing
{
    vec4 clearColor;
    uint width;
    uint height;
    // The dispatch's tile of the frame (PathTracingDispatch.hpp's plan), in pixels.
    uint tile_x;
    uint tile_y;
    uint tile_width;
    uint tile_height;
    // The first of this dispatch's samples among the frame's; only the frame's first chunk probes the clouds' depth.
    uint first_sample;
    // Samples this dispatch adds to each pixel of its tile.
    uint samples;
    // Samples each pixel held before this dispatch; 0 discards the history. Plus the sample's index, it seeds the RNG.
    uint accumulated;
    // GUI-driven quality: the bounce cap.
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
