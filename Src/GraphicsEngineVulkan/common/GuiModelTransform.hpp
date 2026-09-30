#pragma once

#include <span>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Kataglyphis {

// GUI transform for model 0: unscaled translation, then Z, Y, X rotations; no scale (see SceneConfig.cpp).
inline auto makeGuiModelTransform(
  std::span<const float, 3> position, std::span<const float, 3> rotationDegrees) -> glm::mat4
{
    glm::mat4 modelMatrix = glm::mat4(1.0f);

    modelMatrix[3] = glm::vec4(position[0], position[1], position[2], 1.0f);

    modelMatrix = glm::rotate(modelMatrix, glm::radians(rotationDegrees[2]), glm::vec3(0.0f, 0.0f, 1.0f));
    modelMatrix = glm::rotate(modelMatrix, glm::radians(rotationDegrees[1]), glm::vec3(0.0f, 1.0f, 0.0f));
    modelMatrix = glm::rotate(modelMatrix, glm::radians(rotationDegrees[0]), glm::vec3(1.0f, 0.0f, 0.0f));

    return modelMatrix;
}

}// namespace Kataglyphis
