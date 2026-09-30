module;
#include <cstdint>
#include <optional>
#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>

#include "renderer/pushConstants/PushConstantRasterizer.hpp"

module kataglyphis.vulkan.mesh_draw_recorder;

import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.mesh;

Kataglyphis::VulkanRendererInternals::MeshDrawStats
  Kataglyphis::VulkanRendererInternals::recordSceneMeshDraws(vk::CommandBuffer commandBuffer,
    vk::PipelineLayout pipelineLayout,
    vk::ShaderStageFlags pushConstantStages,
    Kataglyphis::Scene *scene,
    const std::optional<FrustumPlanes> &cameraFrustum,
    PushConstantRasterizer &pushConstant)
{
    return walkSceneMeshes(
      commandBuffer,
      scene,
      [&](const glm::mat4 &model_matrix) {
          pushConstant.model = model_matrix;
          // Precomputed here: Slang has no inverse() for SPIR-V.
          const glm::mat4 inv_transpose_model = glm::inverse(glm::transpose(model_matrix));
          for (int row = 0; row < 3; ++row) {
              pushConstant.invModelRows[row] = glm::vec4(
                inv_transpose_model[0][row], inv_transpose_model[1][row], inv_transpose_model[2][row], 0.0F);
          }
      },
      [&](const AABB &worldBounds) {
          // isVisible() treats unknown bounds as visible, so only provably hidden meshes are skipped.
          return cameraFrustum.has_value() && !isVisible(*cameraFrustum, worldBounds);
      },
      [&](const glm::mat4 & /*model_matrix*/, uint32_t object_index, Mesh *mesh) {
          pushConstant.objectIndex = object_index;
          commandBuffer.pushConstants(
            pipelineLayout, pushConstantStages, 0, sizeof(PushConstantRasterizer), &pushConstant);

          // Cull mode is dynamic, so every draw sets it: none for glTF doubleSided, else back.
          commandBuffer.setCullMode(mesh != nullptr && mesh->isDoubleSided() ? vk::CullModeFlagBits::eNone
                                                                              : vk::CullModeFlagBits::eBack);
      });
}
