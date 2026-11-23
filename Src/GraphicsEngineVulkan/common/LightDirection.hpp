#pragma once

#include <cmath>

#include <glm/glm.hpp>

namespace Kataglyphis {

// A zero, inf or NaN direction would be NaN in every shader; straight down instead, as cascades.rs's normalize_or.
inline auto normalizedLightDirection(glm::vec3 raw) -> glm::vec3
{
    const float length = glm::length(raw);
    if (!std::isfinite(length) || length < 1e-6F) { raw = glm::vec3(0.0F, -1.0F, 0.0F); }
    return glm::normalize(raw);
}

// The settings hold the way towards the Sun, as OxidANT's; the shaders and cascades take the way its light travels.
inline auto sunTravelDirection(glm::vec3 towardsSun) -> glm::vec3 { return normalizedLightDirection(-towardsSun); }

}// namespace Kataglyphis
