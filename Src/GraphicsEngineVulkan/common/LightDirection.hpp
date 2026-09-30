#pragma once

#include <glm/glm.hpp>

namespace Kataglyphis {

// The GUI can drag the light direction to zero, which would normalize to NaN; fall back to straight down.
inline auto normalizedLightDirection(glm::vec3 raw) -> glm::vec3
{
    if (glm::length(raw) < 1e-6F) { raw = glm::vec3(0.0F, -1.0F, 0.0F); }
    return glm::normalize(raw);
}

}// namespace Kataglyphis
