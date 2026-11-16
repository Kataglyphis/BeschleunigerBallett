module;
#include <algorithm>
#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "renderer/pushConstants/PushConstantRasterizer.hpp"

module kataglyphis.vulkan.mesh_draw_recorder;

import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.mesh;

namespace Kataglyphis::VulkanRendererInternals {
namespace {

    // Seeds the per-model push constant fields and returns the model's front face.
    vk::FrontFace seedModelPushConstant(PushConstantRasterizer &pushConstant, const glm::mat4 &model_matrix)
    {
        pushConstant.model = model_matrix;
        // Precomputed here: Slang has no inverse() for SPIR-V.
        const glm::mat4 inv_transpose_model = glm::inverse(glm::transpose(model_matrix));
        for (int row = 0; row < 3; ++row) {
            pushConstant.invModelRows[row] =
              glm::vec4(inv_transpose_model[0][row], inv_transpose_model[1][row], inv_transpose_model[2][row], 0.0F);
        }
        // A mirroring model matrix reverses the screen-space winding of its front faces (glTF 3.7.4).
        return glm::determinant(glm::mat3(model_matrix)) < 0.0F ? vk::FrontFace::eClockwise
                                                                : vk::FrontFace::eCounterClockwise;
    }

    void pushMeshConstantsAndCulling(vk::CommandBuffer commandBuffer,
      vk::PipelineLayout pipelineLayout,
      vk::ShaderStageFlags pushConstantStages,
      PushConstantRasterizer &pushConstant,
      uint32_t object_index,
      const Kataglyphis::Mesh *mesh,
      vk::FrontFace front_face)
    {
        pushConstant.objectIndex = object_index;
        commandBuffer.pushConstants(
          pipelineLayout, pushConstantStages, 0, sizeof(PushConstantRasterizer), &pushConstant);

        // Culling is dynamic, so every draw sets it: none for glTF doubleSided, else back, on the model's winding.
        commandBuffer.setCullMode(
          mesh != nullptr && mesh->isDoubleSided() ? vk::CullModeFlagBits::eNone : vk::CullModeFlagBits::eBack);
        commandBuffer.setFrontFace(front_face);
    }

}// namespace
}// namespace Kataglyphis::VulkanRendererInternals

Kataglyphis::VulkanRendererInternals::MeshDrawStats Kataglyphis::VulkanRendererInternals::recordSceneMeshDraws(
  vk::CommandBuffer commandBuffer,
  vk::PipelineLayout pipelineLayout,
  vk::ShaderStageFlags pushConstantStages,
  Kataglyphis::Scene *scene,
  const std::optional<FrustumPlanes> &cameraFrustum,
  PushConstantRasterizer &pushConstant)
{
    vk::FrontFace front_face = vk::FrontFace::eCounterClockwise;
    return walkSceneMeshes(
      commandBuffer,
      scene,
      [&](const glm::mat4 &model_matrix) { front_face = seedModelPushConstant(pushConstant, model_matrix); },
      [&](const AABB &worldBounds, const Mesh *mesh) {
          // BLEND meshes wait for the sorted transparent pass.
          if (mesh != nullptr && mesh->isAlphaBlended()) { return true; }
          // isVisible() treats unknown bounds as visible, so only provably hidden meshes are skipped.
          return cameraFrustum.has_value() && !isVisible(*cameraFrustum, worldBounds);
      },
      [&](const glm::mat4 & /*model_matrix*/, uint32_t object_index, Mesh *mesh) {
          pushMeshConstantsAndCulling(
            commandBuffer, pipelineLayout, pushConstantStages, pushConstant, object_index, mesh, front_face);
      });
}

void Kataglyphis::VulkanRendererInternals::sortBackToFront(std::vector<BlendDraw> &draws)
{
    std::stable_sort(draws.begin(), draws.end(), [](const BlendDraw &a, const BlendDraw &b) {
        return a.distanceSquared > b.distanceSquared;
    });
}

std::vector<Kataglyphis::VulkanRendererInternals::BlendDraw> Kataglyphis::VulkanRendererInternals::collectBlendDraws(
  Kataglyphis::Scene *scene,
  const std::optional<FrustumPlanes> &cameraFrustum,
  const glm::vec3 &eye)
{
    std::vector<BlendDraw> draws;
    // The same flat index walkSceneMeshes assigns, culled meshes included.
    uint32_t flat_mesh_index = 0;
    for (uint32_t m = 0; m < scene->getModelCount(); m++) {
        const glm::mat4 model_matrix = scene->getModelMatrix(m);
        for (unsigned int k = 0; k < scene->getMeshCount(m); k++) {
            const uint32_t object_index = flat_mesh_index++;
            const Mesh *mesh = scene->findMesh(m, k);
            if (mesh == nullptr || !mesh->isAlphaBlended()) { continue; }

            const AABB worldBounds = transformAABB(model_matrix, mesh->getBounds());
            if (cameraFrustum.has_value() && !isVisible(*cameraFrustum, worldBounds)) { continue; }

            // An empty mesh's inverted box has no centre; its model origin stands in.
            const glm::vec3 centre =
              worldBounds.isValid() ? (worldBounds.min + worldBounds.max) * 0.5F : glm::vec3(model_matrix[3]);
            const glm::vec3 toCentre = centre - eye;
            draws.push_back(BlendDraw{ m, k, object_index, glm::dot(toCentre, toCentre) });
        }
    }
    sortBackToFront(draws);
    return draws;
}

unsigned int Kataglyphis::VulkanRendererInternals::recordBlendDraws(vk::CommandBuffer commandBuffer,
  vk::PipelineLayout pipelineLayout,
  vk::ShaderStageFlags pushConstantStages,
  Kataglyphis::Scene *scene,
  const std::vector<BlendDraw> &draws,
  PushConstantRasterizer &pushConstant)
{
    unsigned int drawn = 0;
    for (const BlendDraw &draw : draws) {
        Mesh *mesh = scene->findMesh(draw.modelIndex, draw.meshIndex);
        if (mesh == nullptr) { continue; }

        const vk::FrontFace front_face = seedModelPushConstant(pushConstant, scene->getModelMatrix(draw.modelIndex));
        pushMeshConstantsAndCulling(
          commandBuffer, pipelineLayout, pushConstantStages, pushConstant, draw.objectIndex, mesh, front_face);

        const vk::Buffer vertex_buffer = mesh->getVertexBuffer();
        const vk::DeviceSize offset = 0;
        commandBuffer.bindVertexBuffers(0, 1, &vertex_buffer, &offset);
        commandBuffer.bindIndexBuffer(mesh->getIndexBuffer(), 0, vk::IndexType::eUint32);
        commandBuffer.drawIndexed(mesh->getIndexCount(), 1, 0, 0, 0);
        ++drawn;
    }
    return drawn;
}
