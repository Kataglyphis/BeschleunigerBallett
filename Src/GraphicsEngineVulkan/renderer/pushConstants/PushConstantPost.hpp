// Host mirror of the post push constants; BuildIntegrity.SharedStructOffsetsMatchTheCompiledSpirv pins its layout.
#pragma once
#include "common/HostDeviceGlmAliases.hpp"
namespace Kataglyphis::VulkanRendererInternals {

struct PushConstantPost
{
    uint clouds_enabled;
    float bloom_strength;
};

}// namespace Kataglyphis::VulkanRendererInternals
