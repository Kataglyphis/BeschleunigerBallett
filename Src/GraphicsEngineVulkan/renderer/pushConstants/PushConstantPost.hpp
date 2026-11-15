// Host mirror of the post push constants; BuildIntegrity.SharedStructOffsetsMatchTheCompiledSpirv pins its layout.
#pragma once
#include "common/HostDeviceGlmAliases.hpp"
namespace Kataglyphis::VulkanRendererInternals {

struct PushConstantPost
{
    uint clouds_enabled;
    uint skybox_enabled;
};

}// namespace Kataglyphis::VulkanRendererInternals
