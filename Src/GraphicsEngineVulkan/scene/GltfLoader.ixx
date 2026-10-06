module;

#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

#include <glm/glm.hpp>

// Forward-declared cgltf types used only in the private processPrimitive
// signature. A private member must be declared in the class here, but the full
// cgltf definitions belong to the implementation unit (which includes
// <cgltf.h>); forward declarations keep them out of this module's interface.
struct cgltf_primitive;
struct cgltf_data;
struct cgltf_node;

export module kataglyphis.vulkan.gltf_loader;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.model;
import kataglyphis.vulkan.obj_material;
import kataglyphis.vulkan.vertex;
// Re-exported: MeshRange appears in getMeshRanges()'s return type, so consumers
// (and the parse tests) that import this module see it without a second import.
export import kataglyphis.vulkan.mesh_range;
// Re-exported: GltfSamplerDesc appears in getTextureSamplerDescs()'s return
// type, for the same reason.
export import kataglyphis.vulkan.sampler_builder;

export namespace Kataglyphis {

/// Which UV set a glTF texture view's `texCoord` index names, and whether
/// this engine can sample it. `Vertex` carries exactly one UV slot (bound to
/// TEXCOORD_0), so set 0 is the only supported value - anything else names a
/// channel the geometry does not carry.
struct TexCoordSetInfo
{
    unsigned int set;
    bool supported;
};

/// Resolves a glTF texture view's `texCoord` index to the UV set it names and
/// whether `GltfLoader` can sample it. Exported so parse tests can assert the
/// decision without a device; also the single definition `fromGltfMaterial`
/// warns from when a material asks for an unsupported set.
TexCoordSetInfo describeTexCoordSet(int texcoord);

/// Loads `.gltf`/`.glb` geometry into the SAME CPU-side arrays `ObjLoader`
/// produces, so it plugs into `Model`/`Mesh` and the `AsyncModelParse` worker
/// unchanged. Touches no Vulkan - that is the point, the parse is the expensive
/// part worth moving off the render thread.
///
/// Increment b: geometry (positions/normals/UVs, indices, baked node
/// transforms) plus a single neutral material. Per-material import and texture
/// resolution are follow-ups (see BACKLOG glTF increments c/d).
class GltfLoader
{
  public:
    /// Device-owning: `loadModel` can upload. `parseCpu` alone needs no device,
    /// so the default constructor stays available for the CPU-only path.
    GltfLoader(std::shared_ptr<VulkanDevice> device, vk::CommandPool command_pool);// DEVICE_SINK_OK: moved into member
    GltfLoader() = default;

    /// Parses `modelFile` then builds the Vulkan-side Model from it - i.e.
    /// parseCpu + uploadParsed. Returns nullptr on a device-free loader or a
    /// failed parse.
    std::shared_ptr<Model> loadModel(const std::string &modelFile);

    /// Builds the Vulkan-side Model from the LAST parseCpu result - the GPU half,
    /// separated so a device-free worker can parse and a device-owning loader
    /// upload (the async split). Must run on the thread owning the device;
    /// returns nullptr on a device-free loader or before a successful parse.
    /// Mirrors ObjLoader so AsyncModelParse can drive glTF the same way.
    std::shared_ptr<Model> uploadParsed();

    /// Moves another loader's parse results in, so a device-owning loader can
    /// upload what a device-free worker produced without copying the arrays.
    void adoptParsed(GltfLoader &&other);

    /// Parses the document into vertices/indices/materials/materialIndex.
    /// Returns false on a missing or malformed file, leaving the instance empty.
    bool parseCpu(const std::string &modelFile);

    const std::vector<Vertex> &getVertices() const { return vertices; }
    const std::vector<unsigned int> &getIndices() const { return indices; }
    const std::vector<ObjMaterial> &getMaterials() const { return materials; }
    const std::vector<unsigned int> &getMaterialIndices() const { return materialIndex; }
    /// Encoded base-colour image bytes (PNG/JPG...), one entry per *distinct*
    /// image; materials sharing an image share a slot, indexed by that
    /// material's textureID. loadModel decodes and uploads them via
    /// Texture::createFromMemory; a test can assert the CPU extraction without
    /// a device.
    const std::vector<std::vector<unsigned char>> &getTextureImages() const { return textureImages; }

    /// The glTF sampler (wrap modes + filters) each entry of getTextureImages()
    /// was authored with, index-parallel with it. Two textures sharing one
    /// image but naming different samplers get distinct slots (see parseCpu's
    /// dedup key), so this stays index-parallel rather than needing its own
    /// lookup.
    const std::vector<GltfSamplerDesc> &getTextureSamplerDescs() const { return textureSamplerDescs; }

    /// Colour space each entry of getTextureImages() was authored in,
    /// index-parallel with it: 1 for sRGB (base colour, emissive), 0 for
    /// linear (normal maps). `unsigned char`, not `std::vector<bool>` - the
    /// proxy reference is a trap in a parallel-array getter. An image used as
    /// both base colour and normal map gets two slots, one per colour space,
    /// since a slot can only carry one image format.
    const std::vector<unsigned char> &getTextureSrgbFlags() const { return textureSrgb; }

    // One entry per glTF primitive (see the shared kataglyphis.vulkan.mesh_range
    // module): its slice of the flat vertices/indices/materialIndex arrays.
    // uploadParsed builds one Mesh per range, so a multi-primitive glTF becomes a
    // multi-mesh Model (backlog #10) while parseCpu still produces the flat arrays
    // the tests assert on.
    const std::vector<MeshRange> &getMeshRanges() const { return meshRanges; }

  private:
    /// Processes a single glTF primitive into the flat arrays: reads its
    /// attributes, bakes positions/normals into world space, gathers and
    /// triangulates its index sequence (strip/fan/list), synthesises flat
    /// normals when NORMAL is absent, fills one material id per emitted
    /// triangle, and records the primitive's MeshRange. Points/lines and
    /// position-less primitives are skipped (early return). `world` and
    /// `normalMatrix` are the owning node's baked transforms; `fallbackMaterial`
    /// is the neutral-material index used when the primitive references none.
    /// `mirrored` is true when `world` has a negative determinant (glTF 2.0
    /// §3.7.4), in which case emitted triangles get their winding reversed so
    /// back-face culling and the flat-normal fallback stay correct.
    void processPrimitive(const cgltf_primitive *primitive,
      const glm::mat4 &world,
      const glm::mat3 &normalMatrix,
      const cgltf_data *data,
      unsigned int fallbackMaterial,
      bool mirrored);

    /// Visits `node` and recurses over its children, processing every
    /// primitive of every mesh reached this way. Shared with
    /// `asset/gltf_loader.rs`'s `default_scene().or_else(|| scenes().next())`
    /// walk - the two must change together. `cgltf_node_transform_world`
    /// walks the parent chain regardless of where the walk started, so
    /// recursion only changes which nodes are visited, never the transform a
    /// visited node gets.
    void visitNode(const cgltf_node *node, const cgltf_data *data, unsigned int fallbackMaterial);

    std::shared_ptr<VulkanDevice> device;
    vk::CommandPool command_pool;

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::vector<ObjMaterial> materials;
    // Per-face (per-triangle) material id, exactly like the OBJ path.
    std::vector<unsigned int> materialIndex;
    // One encoded image per textured material (see getTextureImages).
    std::vector<std::vector<unsigned char>> textureImages;
    // Index-parallel with textureImages; see getTextureSamplerDescs.
    std::vector<GltfSamplerDesc> textureSamplerDescs;
    // Index-parallel with textureImages; see getTextureSrgbFlags.
    std::vector<unsigned char> textureSrgb;
    // One slice per glTF primitive; see MeshRange / getMeshRanges.
    std::vector<MeshRange> meshRanges;
};

}// namespace Kataglyphis
