#pragma once

#include <glm/glm.hpp>

namespace Kataglyphis::VulkanRendererInternals {

// Inputs whose change must reset accumulation; SceneUBO's cloud and shadow fields stay out, as the tracer ignores them.
struct PathTracingHistoryKey
{
    glm::mat4 view{ 1.0F };
    glm::mat4 projection{ 1.0F };
    glm::vec4 lightDirection{ 0.0F };
    glm::vec4 lightColorAndRadiance{ 0.0F };
    int samplesPerPixel{ 0 };
    int maxBounces{ 0 };
    // A bounce that misses sees the cube map or the navy, so the toggle changes the converged image.
    bool skyboxEnabled{ true };

    friend auto operator==(const PathTracingHistoryKey &, const PathTracingHistoryKey &) -> bool = default;
};

}// namespace Kataglyphis::VulkanRendererInternals
