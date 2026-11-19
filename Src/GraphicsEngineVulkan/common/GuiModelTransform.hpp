#pragma once

#include <cmath>
#include <span>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Kataglyphis {

// GUI transform for model 0: unscaled translation, then Z, Y, X rotations; no scale (see SceneConfig.cpp).
inline auto makeGuiModelTransform(std::span<const float, 3> position, std::span<const float, 3> rotationDegrees)
  -> glm::mat4
{
    // A non-finite field counts as zero, as Rust's ModelTransform::matrix does: one NaN would void every vertex.
    const auto finite = [](float value) { return std::isfinite(value) ? value : 0.0F; };

    glm::mat4 modelMatrix = glm::mat4(1.0f);

    modelMatrix[3] = glm::vec4(finite(position[0]), finite(position[1]), finite(position[2]), 1.0f);

    modelMatrix = glm::rotate(modelMatrix, glm::radians(finite(rotationDegrees[2])), glm::vec3(0.0f, 0.0f, 1.0f));
    modelMatrix = glm::rotate(modelMatrix, glm::radians(finite(rotationDegrees[1])), glm::vec3(0.0f, 1.0f, 0.0f));
    modelMatrix = glm::rotate(modelMatrix, glm::radians(finite(rotationDegrees[0])), glm::vec3(1.0f, 0.0f, 0.0f));

    return modelMatrix;
}

}// namespace Kataglyphis
