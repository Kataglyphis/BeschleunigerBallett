module;
#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <vulkan/vulkan.hpp>

#include "renderer/pushConstants/PushConstantRasterizer.hpp"

export module kataglyphis.vulkan.mesh_draw_recorder;

import kataglyphis.vulkan.scene;
import kataglyphis.vulkan.frustum;
import kataglyphis.vulkan.mesh;

export namespace Kataglyphis::VulkanRendererInternals {

/// Meshes drawn vs considered by one walkSceneMeshes/recordSceneMeshDraws
/// call, for the caller's frame-timing/HUD counters.
struct MeshDrawStats
{
    unsigned int drawn;
    unsigned int considered;
};

/// The per-frame model x mesh draw walk, shared by every pass that iterates
/// `scene`'s flattened meshes: forward/deferred rasterization
/// (`recordSceneMeshDraws` below) and `CascadedShadowMap::recordCommands`.
/// Owns the parts that must not drift between callers - the flat mesh index
/// bookkeeping, the findMesh()+unknownBounds fallback, transformAABB, the
/// considered/drawn counters, and the bind-VB/bind-IB/drawIndexed sequence -
/// while leaving genuine per-caller differences to the two callbacks:
///
/// - `onModel(modelMatrix)` runs once per model, before its meshes. Forward
///   rasterization uses it to seed the push constant's model/invModelRows;
///   the shadow pass has nothing model-level to precompute and passes a
///   no-op.
/// - `shouldCull(worldBounds)` runs once per mesh (after `onModel`, before
///   any draw-related side effect) and returning true skips the mesh.
///   Forward rasterization tests one camera frustum; the shadow pass unions
///   `isVisibleAsShadowCaster` over every cascade.
/// - `onMesh(modelMatrix, objectIndex, mesh)` runs once per surviving mesh,
///   before the shared bind+draw. It is where each caller pushes its own
///   push-constant type/content (`PushConstantRasterizer` with
///   inverse-transpose rows and the dynamic `setCullMode`, vs
///   `ShadowPushConstants` with no cull-mode toggle since the shadow
///   pipeline is built `eCullMode = eNone`).
///
/// `mesh` passed to `shouldCull`'s bounds and to `onMesh` may be null (a
/// findMesh() miss); the shared walk treats a null mesh as an empty draw
/// (unknown bounds, zero-size vertex/index buffers) rather than skipping it,
/// matching the callers' former inline behaviour.
template<typename PerModelSetup, typename CullPredicate, typename PerMeshSetup>
MeshDrawStats walkSceneMeshes(vk::CommandBuffer commandBuffer,
  Kataglyphis::Scene *scene,
  PerModelSetup &&onModel,
  CullPredicate &&shouldCull,
  PerMeshSetup &&onMesh)
{
    MeshDrawStats stats{ 0, 0 };

    // object descriptions are flattened one-per-mesh across all models
    // (Scene::add_model), so objectIndex is the running FLAT mesh index, not
    // the model index. It advances for every mesh - culled ones included -
    // to stay aligned with that buffer.
    uint32_t flat_mesh_index = 0;
    for (uint32_t m = 0; m < scene->getModelCount(); m++) {
        const glm::mat4 model_matrix = scene->getModelMatrix(m);
        onModel(model_matrix);

        for (unsigned int k = 0; k < scene->getMeshCount(m); k++) {
            const uint32_t object_index = flat_mesh_index++;
            ++stats.considered;

            // Unreachable with a null mesh: m/k come from getModelCount()/
            // getMeshCount(m), so findMesh() always resolves here. Kept as a
            // fallback rather than an assert so behaviour matches the former
            // per-accessor calls byte-for-byte.
            Mesh *mesh = scene->findMesh(m, k);
            static const AABB unknownBounds{ glm::vec3(1.0F), glm::vec3(-1.0F) };
            const AABB &meshBounds = mesh != nullptr ? mesh->getBounds() : unknownBounds;

            if (shouldCull(transformAABB(model_matrix, meshBounds))) { continue; }

            onMesh(model_matrix, object_index, mesh);

            const vk::Buffer vertex_buffer = mesh != nullptr ? mesh->getVertexBuffer() : vk::Buffer{};
            const vk::DeviceSize offset = 0;
            commandBuffer.bindVertexBuffers(0, 1, &vertex_buffer, &offset);

            commandBuffer.bindIndexBuffer(
              mesh != nullptr ? mesh->getIndexBuffer() : vk::Buffer{}, 0, vk::IndexType::eUint32);

            commandBuffer.drawIndexed(mesh != nullptr ? mesh->getIndexCount() : 0, 1, 0, 0, 0);
            ++stats.drawn;
        }
    }

    return stats;
}

/// Forward/deferred rasterization's use of walkSceneMeshes: seeds
/// `pushConstant`'s model/invModelRows per model, culls against one camera
/// frustum, then pushes `PushConstantRasterizer` and sets the dynamic cull
/// mode per mesh. `pipelineLayout` and `pushConstantStages` are the two
/// genuine differences between the callers - forward pushes
/// eVertex|eFragment through pipeline_layout, deferred pushes eAll through
/// geometryPipelineLayout - and must NOT be collapsed to one value.
/// `pushConstant` is taken by reference so the caller's already-seeded fields
/// (set via setPushConstant) survive; .model / .invModelRows / .objectIndex
/// are (re)written per mesh exactly as the inline loops did.
MeshDrawStats recordSceneMeshDraws(vk::CommandBuffer commandBuffer,
  vk::PipelineLayout pipelineLayout,
  vk::ShaderStageFlags pushConstantStages,
  Kataglyphis::Scene *scene,
  const std::optional<FrustumPlanes> &cameraFrustum,
  PushConstantRasterizer &pushConstant);

}// namespace Kataglyphis::VulkanRendererInternals
