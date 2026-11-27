// Host mirror of OxidANT's ssao.slang SsaoUniforms; BuildIntegrity.SharedStructOffsetsMatchTheCompiledSpirv pins its layout.
#pragma once
#include "common/HostDeviceGlmAliases.hpp"
namespace Kataglyphis::VulkanRendererInternals {

struct SsaoUniforms
{
    mat4 proj;
    mat4 inv_proj;
    // x: radius, y: bias, z: intensity, w: unused.
    vec4 params;
};

}// namespace Kataglyphis::VulkanRendererInternals
