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
};

}// namespace Kataglyphis::VulkanRendererInternals
