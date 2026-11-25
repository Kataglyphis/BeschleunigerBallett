#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <span>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "common/LightDirection.hpp"
#include "common/host_device_shared_vars.hpp"
#include "renderer/SceneUBO.hpp"
#include "scene/atmospheric_effects/clouds/CloudDispatch.hpp"
#include "scene/sky_box/CloudAmbient.hpp"

namespace Kataglyphis {

// Flips Y for Vulkan clip space; never flip the cascade matrices to match (the shadow pass's culling relies on it).
inline auto makeVulkanProjection(float fovDegrees, float aspect, float nearPlane, float farPlane) -> glm::mat4
{
    glm::mat4 projection = glm::perspective(glm::radians(fovDegrees), aspect, nearPlane, farPlane);
    projection[1][1] *= -1;
    return projection;
}

// The scene's share of the swapchain image, in fractions of its width and height from the top-left corner.
struct ViewportFraction
{
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 1.0F;
    float y1 = 1.0F;
};

// Below this share per axis the rect is treated as degenerate and the whole image is used instead.
constexpr float kMinViewportFraction = 0.05F;

// GUI, tests and configs may send anything: edges clamp into the image (NaN to 0), a collapsed rect becomes all of it.
constexpr auto sanitizeViewportFraction(float x0, float y0, float x1, float y1) -> ViewportFraction
{
    const auto clamp01 = [](float value) { return (value >= 0.0F) ? ((value <= 1.0F) ? value : 1.0F) : 0.0F; };
    const ViewportFraction rect{ clamp01(x0), clamp01(y0), clamp01(x1), clamp01(y1) };
    if (rect.x1 - rect.x0 < kMinViewportFraction || rect.y1 - rect.y0 < kMinViewportFraction) { return {}; }
    return rect;
}

// 1 for an empty rect, as OxidANT's lens: a minimized or resizing window reports a zero side, an infinite projection.
constexpr auto viewportAspectRatio(uint32_t width, uint32_t height, ViewportFraction rect) -> float
{
    const float rectWidth = static_cast<float>(width) * (rect.x1 - rect.x0);
    const float rectHeight = static_cast<float>(height) * (rect.y1 - rect.y0);
    return (rectWidth > 0.0F && rectHeight > 0.0F) ? rectWidth / rectHeight : 1.0F;
}

// Centres the camera on the rect; every pass still covers the whole image, the docks hide the rest.
inline auto makeViewportRemap(ViewportFraction rect) -> glm::mat4
{
    glm::mat4 remap(1.0F);
    remap[0][0] = rect.x1 - rect.x0;
    remap[1][1] = rect.y1 - rect.y0;
    remap[3][0] = rect.x0 + rect.x1 - 1.0F;
    remap[3][1] = rect.y0 + rect.y1 - 1.0F;
    return remap;
}

// A negative value cast to uint32_t would wrap, skip the tap loop and read as fully shadowed.
constexpr auto clampPcfRadius(int guiValue) -> uint32_t
{ return static_cast<uint32_t>(std::clamp(guiValue, 0, MAX_PCF_RADIUS)); }

// The cloud box's half extents are mesh scale * 10; a zero one collapses the box, a zero density empties it.
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
{ return std::clamp(numMarchSteps, kMinCloudMarchSteps, kMaxCloudMarchSteps); }

constexpr auto clampCloudLightMarchSteps(int numMarchStepsToLight) -> int
{ return std::clamp(numMarchStepsToLight, kMinCloudLightMarchSteps, kMaxCloudLightMarchSteps); }

// The noise volume repeats every 256 units (volumetric_clouds.slang's coarse period), so a wind offset wraps to it.
constexpr double kCloudNoisePeriod = 256.0;

// Wrapped in double, so hours of wind keep float precision; OxidANT's CloudSettings::wind_offset.
inline auto cloudWindOffset(glm::vec3 wind, double timeSeconds) -> glm::vec3
{
    const auto wrap = [timeSeconds](float velocity) {
        double travelled = std::fmod(static_cast<double>(velocity) * timeSeconds, kCloudNoisePeriod);
        if (travelled < 0.0) { travelled += kCloudNoisePeriod; }
        const auto offset = static_cast<float>(travelled);
        // -0 (a negative velocity at time 0) and a float rounded up to the period both mean no shift, bit for bit.
        return (offset == 0.0F || offset >= static_cast<float>(kCloudNoisePeriod)) ? 0.0F : offset;
    };
    return { wrap(wind.x), wrap(wind.y), wrap(wind.z) };
}

// Scalars: the global module fragment cannot name GUISceneSharedVars. See docs/source/clouds.md § UBO packing.
inline void fillSceneUboClouds(VulkanRendererInternals::SceneUBO &ubo,
  glm::vec3 meshScale,
  float densityMultiplier,
  glm::vec3 meshOffset,
  float coverageThreshold,
  int numMarchSteps,
  int numMarchStepsToLight,
  float pillowness,
  float cirrusEffect,
  bool powderEffect,
  glm::vec3 windOffset)
{
    ubo.cloudLightMarch = glm::vec4(
      static_cast<float>(clampCloudLightMarchSteps(numMarchStepsToLight)), windOffset.x, windOffset.y, windOffset.z);
    ubo.cloudMeshScale = clampCloudMeshScale(meshScale, densityMultiplier);
    ubo.cloudMeshOffset = glm::vec4(meshOffset.x, meshOffset.y, meshOffset.z, coverageThreshold);
    ubo.cloudParameters = glm::vec4(
      pillowness, cirrusEffect, powderEffect ? 1.0F : 0.0F, static_cast<float>(clampCloudMarchSteps(numMarchSteps)));
}

// The drawn sky's ambient: the cube map's, or the navy when the skybox is off. Both .w are filler.
inline void
  fillSceneUboCloudAmbient(VulkanRendererInternals::SceneUBO &ubo, bool skyboxEnabled, const CloudAmbient &cubemap)
{
    const CloudAmbient ambient = skyboxEnabled ? cubemap : cloudAmbientOfDisabledSky();
    ubo.cloudAmbientTop = glm::vec4(ambient.top, 0.0F);
    ubo.cloudAmbientBottom = glm::vec4(ambient.bottom, 0.0F);
}

// Both .w are filler: shaders read only .xyz.
inline void fillSceneUboCamera(VulkanRendererInternals::SceneUBO &ubo, glm::vec3 position, glm::vec3 direction)
{
    ubo.view_dir = glm::vec4(direction, 1.0F);
    ubo.cam_pos = glm::vec4(position, 1.0F);
}

// The one place the settings' towards-the-Sun direction turns into the travel direction; direction.w is the ambient.
inline void fillSceneUboDirectionalLight(VulkanRendererInternals::SceneUBO &ubo,
  glm::vec3 towardsSun,
  glm::vec3 color,
  float radiance,
  float ambient)
{
    ubo.dirLight.direction = glm::vec4(sunTravelDirection(towardsSun), ambient);
    ubo.dirLight.color = glm::vec4(color, radiance);
}

// Truncates to the shorter span, since NDEBUG drops the assert; disabled shadows only zero numCascades.
inline auto fillSceneUboCascades(VulkanRendererInternals::SceneUBO &ubo,
  std::span<const float> splitDepths,
  std::span<const glm::mat4> viewProjMatrices,
  bool shadowsEnabled) -> uint32_t
{
    assert(splitDepths.size() == viewProjMatrices.size());

    const size_t activeCascades =
      std::min({ splitDepths.size(), viewProjMatrices.size(), static_cast<size_t>(CASCADE_COUNT) });
    for (size_t i = 0; i < activeCascades; ++i) {
        ubo.cascadeSplits[static_cast<int>(i)] = splitDepths[i];
        ubo.cascadeLightSpaceMatrices[i] = viewProjMatrices[i];
    }

    const auto numCascades = shadowsEnabled ? static_cast<uint32_t>(activeCascades) : 0U;
    ubo.numCascades = numCascades;
    return numCascades;
}

}// namespace Kataglyphis
