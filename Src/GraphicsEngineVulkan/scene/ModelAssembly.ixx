module;

#include <cstddef>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "spdlog/spdlog.h"

export module kataglyphis.vulkan.model_assembly;

import kataglyphis.vulkan.model;
import kataglyphis.vulkan.device;
import kataglyphis.vulkan.mesh_range;
import kataglyphis.vulkan.obj_material;
import kataglyphis.vulkan.vertex;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.sampler_builder;

export namespace Kataglyphis {

/// Shared tail of ObjLoader::uploadParsed and GltfLoader::uploadParsed: both
/// loaders differ only in how a texture's bytes reach them (file vs. memory)
/// and in shape-specific parse-time skips, not in what happens to the result.
/// This is that shared shape.

/// The two guards both uploadParsed overrides check before touching the
/// device: device-first, then empty-parse, so a device-free loader that also
/// never parsed reports the more actionable error (the device). Programming
/// errors, not runtime conditions - AsyncModelParse only ever hands parsed
/// state to a device-owning loader, so these only fire when that wiring
/// breaks - hence log-and-return-false rather than silence.
bool uploadPreconditionsMet(const std::shared_ptr<VulkanDevice> &device,
  std::size_t vertexCount,
  const char *loaderName)
{
    if (!device) {
        spdlog::error("{}::uploadParsed called on a device-free loader", loaderName);
        return false;
    }
    if (vertexCount == 0) {
        spdlog::error("{}::uploadParsed called before a successful parseCpu", loaderName);
        return false;
    }
    return true;
}

/// Appends `texture` when `created`, otherwise occupies the slot with the
/// default texture instead. Both loaders need this: textureID is a dense
/// index over "one texture per material slot", so skipping a failed load
/// would shift every later texture down one and push the last textureID past
/// the descriptor array. `samplerDesc` carries the glTF sampler this texture
/// was authored with (default-constructed for OBJ, which has no sampler
/// concept, and for a failed load's default stand-in texture).
void addTextureOrDefault(Model &model,
  const std::shared_ptr<VulkanDevice> &device,
  vk::CommandPool pool,
  bool created,
  Texture &&texture,
  GltfSamplerDesc samplerDesc = {})
{
    if (created) {
        model.addTexture(std::move(texture), samplerDesc);
        return;
    }
    Texture defaultTexture;
    if (!defaultTexture.createDefaultTexture(device, pool)) {
        spdlog::error("Default texture upload failed; slot will hold an empty texture.");
    }
    model.addTexture(std::move(defaultTexture));
}

/// The `getTextureCount() == 0` fallback both loaders fall back to when their
/// document had no textures at all, so textureID 0 is still a valid index.
void ensureAtLeastOneTexture(Model &model, const std::shared_ptr<VulkanDevice> &device, vk::CommandPool pool)
{
    if (model.getTextureCount() != 0) { return; }
    Texture defaultTexture;
    if (!defaultTexture.createDefaultTexture(device, pool)) {
        spdlog::error("Default texture upload failed; slot will hold an empty texture.");
    }
    model.addTexture(std::move(defaultTexture));
}

/// The empty-ranges single-mesh case plus the per-range sliceMeshRange ->
/// add_new_mesh loop, common to both loaders. Always forwards
/// range.doubleSided and range.alphaBlended - MeshRange.ixx documents both as always false for OBJ, so
/// that loader forwarding it is behaviour-identical to the default it used to
/// rely on.
///
/// vertices/indices/materialIndex stay non-const references because
/// Model::add_new_mesh takes them that way.
void addMeshesForRanges(Model &model,
  const std::shared_ptr<VulkanDevice> &device,
  vk::CommandPool pool,
  std::vector<Vertex> &vertices,
  std::vector<unsigned int> &indices,
  std::vector<unsigned int> &materialIndex,
  std::vector<ObjMaterial> &materials,
  const std::vector<MeshRange> &ranges)
{
    if (ranges.empty()) {
        model.add_new_mesh(device, pool, vertices, indices, materialIndex, materials);
        return;
    }
    for (const MeshRange &range : ranges) {
        MeshSlice slice = sliceMeshRange(range, vertices, indices, materialIndex);
        model.add_new_mesh(device,
          pool,
          slice.vertices,
          slice.indices,
          slice.materialIndex,
          materials,
          range.doubleSided,
          range.alphaBlended);
    }
}

}// namespace Kataglyphis
