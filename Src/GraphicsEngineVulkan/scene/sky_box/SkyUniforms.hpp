// Host mirror of OxidANT's sky.slang SkyUniforms; BuildIntegrity.SharedStructOffsetsMatchTheCompiledSpirv pins its layout.
#pragma once

#include <glm/glm.hpp>

#include "common/HostDeviceGlmAliases.hpp"

namespace Kataglyphis {

// sky.slang's mode.x: 1 the cube map, 2 the switched-off navy (0, its procedural sky, is Rust's).
inline constexpr float kSkyModeCubeMap = 1.0F;
inline constexpr float kSkyModeSwitchedOff = 2.0F;

struct SkyUniforms
{
    mat4 inv_view_proj;
    // xyz: towards the light, w: its intensity.
    vec4 light_dir_intensity;
    vec4 mode;
};

// The cube map always: without faces it holds AnalyticSky.hpp's bake, which mode 1 untonemaps like a photo.
inline auto makeSkyUniforms(const glm::mat4 &projection,
  const glm::mat4 &view,
  glm::vec3 lightTravelDirection,
  float lightIntensity,
  bool skyboxEnabled) -> SkyUniforms
{
    SkyUniforms uniforms{};
    uniforms.inv_view_proj = glm::inverse(projection * view);
    uniforms.light_dir_intensity = glm::vec4(-lightTravelDirection, lightIntensity);
    uniforms.mode = glm::vec4(skyboxEnabled ? kSkyModeCubeMap : kSkyModeSwitchedOff, 0.0F, 0.0F, 0.0F);
    return uniforms;
}

}// namespace Kataglyphis
