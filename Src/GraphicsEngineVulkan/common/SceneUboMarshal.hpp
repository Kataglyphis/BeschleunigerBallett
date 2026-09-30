#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <span>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "common/LightDirection.hpp"
#include "common/host_device_shared_vars.hpp"
#include "renderer/SceneUBO.hpp"
#include "scene/atmospheric_effects/clouds/CloudDispatch.hpp"

namespace Kataglyphis {

// Guards the zero-height extent a minimized or resizing window reports, which would put NaN/Inf in the projection.
constexpr auto aspectRatioOf(uint32_t width, uint32_t height) -> float
{
    return (height > 0) ? static_cast<float>(width) / static_cast<float>(height) : 1.0F;
}

// Flips Y for Vulkan clip space; never flip the cascade matrices to match (the shadow pass's culling relies on it).
inline auto makeVulkanProjection(float fovDegrees, float aspect, float nearPlane, float farPlane) -> glm::mat4
{
    glm::mat4 projection = glm::perspective(glm::radians(fovDegrees), aspect, nearPlane, farPlane);
    projection[1][1] *= -1;
    return projection;
}

// A negative value cast to uint32_t would wrap, skip the tap loop and read as fully shadowed.
constexpr auto clampPcfRadius(int guiValue) -> uint32_t
{
    return static_cast<uint32_t>(std::clamp(guiValue, 0, MAX_PCF_RADIUS));
}

// clouds.slang divides by mesh scale * density multiplier, so a zero in either gives inf/NaN box hits.
constexpr float kMinCloudMeshExtent = 1e-3F;
constexpr float kMinCloudDensityMultiplier = 1e-3F;

constexpr auto clampCloudMeshScale(glm::vec3 meshScale, float densityMultiplier) -> glm::vec4
{
    return { std::max(meshScale.x, kMinCloudMeshExtent),
        std::max(meshScale.y, kMinCloudMeshExtent),
        std::max(meshScale.z, kMinCloudMeshExtent),
        std::max(densityMultiplier, kMinCloudDensityMultiplier) };
}

// Clamped here because the GUI vars can also come from a test or a config load.
constexpr auto clampCloudMarchSteps(int numMarchSteps) -> int
{
    return std::clamp(numMarchSteps, kMinCloudMarchSteps, kMaxCloudMarchSteps);
}

constexpr auto clampCloudLightMarchSteps(int numMarchStepsToLight) -> int
{
    return std::clamp(numMarchStepsToLight, kMinCloudLightMarchSteps, kMaxCloudLightMarchSteps);
}

// Scalars: the global module fragment cannot name GUISceneSharedVars. See docs/clouds.md § UBO packing.
inline void fillSceneUboClouds(VulkanRendererInternals::SceneUBO &ubo,
  glm::vec3 meshScale,
  float densityMultiplier,
  glm::vec3 meshOffset,
  float coverageThreshold,
  int numMarchSteps,
  int numMarchStepsToLight,
  float pillowness,
  float cirrusEffect,
  bool powderEffect)
{
    ubo.cloudLightMarch = glm::vec4(static_cast<float>(clampCloudLightMarchSteps(numMarchStepsToLight)), 0.0F, 0.0F, 0.0F);
    ubo.cloudMeshScale = clampCloudMeshScale(meshScale, densityMultiplier);
    ubo.cloudMeshOffset = glm::vec4(meshOffset.x, meshOffset.y, meshOffset.z, coverageThreshold);
    ubo.cloudParameters = glm::vec4(
      pillowness, cirrusEffect, powderEffect ? 1.0F : 0.0F, static_cast<float>(clampCloudMarchSteps(numMarchSteps)));
}

// Both .w are filler: shaders read only .xyz.
inline void fillSceneUboCamera(VulkanRendererInternals::SceneUBO &ubo, glm::vec3 position, glm::vec3 direction)
{
    ubo.view_dir = glm::vec4(direction, 1.0F);
    ubo.cam_pos = glm::vec4(position, 1.0F);
}

// color.w carries radiance, the one SceneUBO .w slot a shader must read; direction.w is filler.
inline void fillSceneUboDirectionalLight(
  VulkanRendererInternals::SceneUBO &ubo, glm::vec3 rawDirection, glm::vec3 color, float radiance)
{
    ubo.dirLight.direction = glm::vec4(normalizedLightDirection(rawDirection), 1.0F);
    ubo.dirLight.color = glm::vec4(color, radiance);
}

// Truncates to the shorter span, since NDEBUG drops the assert; disabled shadows only zero numCascades.
inline auto fillSceneUboCascades(VulkanRendererInternals::SceneUBO &ubo,
  std::span<const float> splitDepths,
  std::span<const glm::mat4> viewProjMatrices,
  bool shadowsEnabled) -> uint32_t
{
    assert(splitDepths.size() == viewProjMatrices.size());

    const size_t activeCascades = std::min(
      { splitDepths.size(), viewProjMatrices.size(), static_cast<size_t>(MAX_CASCADES) });
    for (size_t i = 0; i < activeCascades; ++i) {
        ubo.cascadeSplits[static_cast<int>(i)] = splitDepths[i];
        ubo.cascadeLightSpaceMatrices[i] = viewProjMatrices[i];
    }

    const auto numCascades = shadowsEnabled ? static_cast<uint32_t>(activeCascades) : 0U;
    ubo.numCascades = numCascades;
    return numCascades;
}

}// namespace Kataglyphis
