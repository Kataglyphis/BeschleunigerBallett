#pragma once

#include <glm/glm.hpp>

#include "renderer/SceneUBO.hpp"

namespace Kataglyphis::VulkanRendererInternals {

// One white surface everywhere under a uniform sky and no lights; the defaults are the Rust editor's White furnace.
struct PathTracingFurnace
{
    bool on{ false };
    float radiance{ 1.0F };
    float metallic{ 0.0F };
    float roughness{ 0.5F };

    friend auto operator==(const PathTracingFurnace &, const PathTracingFurnace &) -> bool = default;
};

// Inputs whose change must reset accumulation; SceneUBO's cloud and shadow fields stay out, as the tracer ignores them.
struct PathTracingHistoryKey
{
    glm::mat4 view{ 1.0F };
    glm::mat4 projection{ 1.0F };
    // w is the Ambient slider, which scales the sky a bounce that misses sees.
    glm::vec4 lightDirectionAndAmbient{ 0.0F };
    glm::vec4 lightColorAndRadiance{ 0.0F };
    int samplesPerPixel{ 0 };
    int maxBounces{ 0 };
    // A camera miss draws the cube map or the navy, and only a drawn cube lights the bounces through the IBL.
    bool skyboxEnabled{ true };
    // Another surface and sky: a mean across the switch would blend the scene into the furnace.
    PathTracingFurnace furnace{};

    friend auto operator==(const PathTracingHistoryKey &, const PathTracingHistoryKey &) -> bool = default;
};

// dirLight.direction.w is the Ambient slider: a bounce that misses reads it, so it restarts the mean too.
inline auto makePathTracingHistoryKey(const glm::mat4 &view,
  const glm::mat4 &projection,
  const DirectionalLightData &light,
  int samplesPerPixel,
  int maxBounces,
  bool skyboxEnabled,
  const PathTracingFurnace &furnace) -> PathTracingHistoryKey
{
    return PathTracingHistoryKey{
        .view = view,
        .projection = projection,
        .lightDirectionAndAmbient = light.direction,
        .lightColorAndRadiance = light.color,
        .samplesPerPixel = samplesPerPixel,
        .maxBounces = maxBounces,
        .skyboxEnabled = skyboxEnabled,
        .furnace = furnace,
    };
}

}// namespace Kataglyphis::VulkanRendererInternals
