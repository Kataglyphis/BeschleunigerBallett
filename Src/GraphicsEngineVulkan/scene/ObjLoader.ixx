module;
#include <memory>
#include <string>
#include <tiny_obj_loader.h>
#include <vector>
#include <vulkan/vulkan.hpp>

export module kataglyphis.vulkan.obj_loader;

import kataglyphis.vulkan.obj_material;
import kataglyphis.vulkan.vertex;
import kataglyphis.vulkan.model;
import kataglyphis.vulkan.device;
// Re-exported: MeshRange appears in getMeshRanges()'s return type, so consumers
// (and the parse tests) that import this module see it without a second import.
export import kataglyphis.vulkan.mesh_range;

export namespace Kataglyphis {

/// docs/model-loading.md's map_Kd resolution rule: normalise `\` to `/` (a
/// Windows-authored .mtl must still resolve when the path is otherwise
/// portable), then try `<baseDir>/<mapKd>` beside the .mtl, then
/// `<baseDir>/textures/<mapKd>`; on neither existing, return the beside-the-
/// .mtl candidate and let the caller warn. Free (not a method) so parseCpu's
/// tests can call it without constructing a device-owning loader. An empty
/// baseDir is treated as "." so the candidates stay relative instead of
/// becoming filesystem-root paths.
std::string resolveObjTexturePath(const std::string &baseDir, const std::string &mapKd);

class ObjLoader
{
  public:
    ObjLoader(const std::shared_ptr<VulkanDevice> &device, vk::CommandPool command_pool);

    /// CPU-only construction, for parsing off the render thread (or in a test).
    /// loadModel() is unavailable on such an instance - it has no device to
    /// upload with.
    ObjLoader() = default;

    std::shared_ptr<Model> loadModel(const std::string &modelFile);

    /// Reads and decodes the OBJ into CPU-side vertex/index/material arrays.
    ///
    /// Touches NO Vulkan, which is the point: this is ~99% of model load time
    /// (measured 1906 ms parse + 867 ms vertex build against 15 ms of GPU
    /// upload on the bundled 27 MB model), so it is the part worth moving to a
    /// worker thread. Returns false on a malformed or missing file, leaving
    /// the instance empty.
    bool parseCpu(const std::string &modelFile);

    /// Builds the Vulkan-side Model from the last parseCpu result.
    ///
    /// MUST run on the thread that owns the device. Returns nullptr on a
    /// device-free loader or before a successful parse - the pairing is a
    /// programming error, not a runtime condition, so it logs rather than
    /// failing silently.
    std::shared_ptr<Model> uploadParsed();

    /// Moves another loader's parse results into this one, so a device-owning
    /// loader can upload what a device-free worker produced. Copying the
    /// arrays instead would duplicate tens of MB for no reason.
    void adoptParsed(ObjLoader &&other)
    {
        vertices = std::move(other.vertices);
        indices = std::move(other.indices);
        materials = std::move(other.materials);
        materialIndex = std::move(other.materialIndex);
        textures = std::move(other.textures);
        textureSrgb = std::move(other.textureSrgb);
        meshRanges = std::move(other.meshRanges);
    }

    /// Results of the last parseCpu / loadModel. Exposed so a worker can hand
    /// the data to the thread that owns the device.
    const std::vector<Vertex> &getVertices() const { return vertices; }
    const std::vector<unsigned int> &getIndices() const { return indices; }
    const std::vector<ObjMaterial> &getMaterials() const { return materials; }
    const std::vector<unsigned int> &getMaterialIndices() const { return materialIndex; }
    const std::vector<std::string> &getTextureNames() const { return textures; }
    const std::vector<unsigned char> &getTextureSrgbFlags() const { return textureSrgb; }

    /// One entry per OBJ shape (`o`/`g` group): its slice of the flat vertices/
    /// indices/materialIndex arrays. `uploadParsed` builds one Mesh per range, so
    /// a multi-shape OBJ becomes a multi-mesh Model (backlog #10, the OBJ parallel
    /// of the glTF primitive split) while the flat getters stay exactly as they
    /// were - the ObjParseUnit tests assert only on those. Unlike glTF primitives,
    /// OBJ shapes share the attribute pool, so a per-shape range is only contiguous
    /// because loadVertices resets the vertex-dedup map per shape (a vertex shared
    /// across shapes is stored once per shape - pixel-identical geometry). A
    /// single-shape OBJ yields one range spanning everything - behaviour-identical
    /// to before. MeshRange itself is the shared kataglyphis.vulkan.mesh_range type.
    const std::vector<MeshRange> &getMeshRanges() const { return meshRanges; }

  private:
    std::shared_ptr<VulkanDevice> device;
    vk::CommandPool command_pool;

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::vector<ObjMaterial> materials;
    std::vector<unsigned int> materialIndex;
    std::vector<std::string> textures;
    // Parallel to textures: 1 = upload sRGB-decoded (diffuse), 0 = linear
    // (normal maps). Same convention as GltfLoader's textureSrgb.
    std::vector<unsigned char> textureSrgb;
    // One slice per OBJ shape; see MeshRange / getMeshRanges.
    std::vector<MeshRange> meshRanges;

    // Both take an ALREADY PARSED reader. The file used to be parsed once per
    // function - twice per model - which for the bundled 27 MB OBJ is around
    // 190 ms of duplicated work at the measured ~7 ms/MB (BM_ObjParse_Suzanne).
    // modelFile is still needed: texture paths resolve relative to it.
    void loadTexturesAndMaterials(const tinyobj::ObjReader &reader, const std::string &modelFile);
    void loadVertices(const tinyobj::ObjReader &reader);
};
}// namespace Kataglyphis
