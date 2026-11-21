// Host mirror of OxidANT's fxaa.slang FxaaUniforms; BuildIntegrity.SharedStructOffsetsMatchTheCompiledSpirv pins its layout.
#pragma once
#include "common/HostDeviceGlmAliases.hpp"
namespace Kataglyphis::VulkanRendererInternals {

struct FxaaUniforms
{
    // xy: 1 / target size, z: 1 when the frame samples linear values (an sRGB target), w: unused.
    vec4 params;
};

}// namespace Kataglyphis::VulkanRendererInternals
