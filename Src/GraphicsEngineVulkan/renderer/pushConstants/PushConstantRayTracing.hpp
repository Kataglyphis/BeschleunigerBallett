// Host mirror of the ray-tracing push constants; SharedStructOffsetsMatchTheCompiledSpirv pins its layout.
#pragma once
#include "common/HostDeviceGlmAliases.hpp"
namespace Kataglyphis::VulkanRendererInternals {

struct PushConstantRaytracing
{
    vec4 clear_color;
};

}// namespace Kataglyphis::VulkanRendererInternals
