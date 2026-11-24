module;

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

#include <glm/glm.hpp>

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

/// What a load asks of OxidANT's loader beyond the file; each field's zero is the loader's default.
struct GltfLoadOptions
{
    /// MikkTSpace tangents instead of Lengyel's where the file has none.
    bool mikktspaceTangents = false;
};

/// `KATAGLYPHIS_MIKKTSPACE_TANGENTS` set and not empty means MikkTSpace; App::run reads it once, no loader does.
GltfLoadOptions gltfLoadOptionsFromEnvironment();

/// Loads `.gltf`, `.glb` and `.obj` geometry into flat CPU-side arrays that plug
/// into `Model`/`Mesh` and the `AsyncModelParse` worker. Touches no Vulkan - that
/// is the point, the parse is the expensive part worth moving off the render
/// thread. The parse is OxidANT's Rust readers over the cxx bridge, the WebGPU
/// renderer's own; the bridge picks one by the file's extension.
class GltfLoader
{
  public:
    /// Device-owning: `loadModel` can upload. `parseCpu` alone needs no device,
    /// so the default constructor stays available for the CPU-only path.
    GltfLoader(std::shared_ptr<VulkanDevice> device, vk::CommandPool command_pool);// DEVICE_SINK_OK: moved into member
    GltfLoader() = default;
    GltfLoader(const GltfLoader &) = default;
    GltfLoader &operator=(const GltfLoader &) = default;
    /// A move is adoptParsed: the moved-from loader reads as unparsed, not as views into the parse it gave away.
    GltfLoader(GltfLoader &&other) noexcept;
    GltfLoader &operator=(GltfLoader &&other) noexcept;
    ~GltfLoader() = default;

    /// Parses `modelFile` then builds the Vulkan-side Model from it - i.e.
    /// parseCpu + uploadParsed. Returns nullptr on a device-free loader or a
    /// failed parse.
    std::shared_ptr<Model> loadModel(const std::string &modelFile, const GltfLoadOptions &options = {});

    /// Builds the Vulkan-side Model from the LAST parseCpu result - the GPU half,
    /// separated so a device-free worker can parse and a device-owning loader
    /// upload (the async split). Must run on the thread owning the device;
    /// returns nullptr on a device-free loader or before a successful parse.
    std::shared_ptr<Model> uploadParsed();

    /// Moves another loader's parse results in, so a device-owning loader can
    /// upload what a device-free worker produced without copying the arrays.
    /// Copying a loader shares its parse instead: the arrays are read-only.
    void adoptParsed(GltfLoader &&other);

    /// Parses the document into vertices/indices/materials/materialIndex with
    /// OxidANT's Rust loader. Returns false on a missing or malformed file,
    /// leaving the instance empty.
    bool parseCpu(const std::string &modelFile, const GltfLoadOptions &options = {});

    /// The bridge's own arrays, valid while this loader holds the parse; nothing copies them before the upload.
    std::span<const Vertex> getVertices() const { return vertices; }
    std::span<const unsigned int> getIndices() const { return { indices.data, indices.size }; }
    const std::vector<ObjMaterial> &getMaterials() const { return materials; }
    std::span<const unsigned int> getMaterialIndices() const { return { materialIndex.data, materialIndex.size }; }
    /// Encoded base-colour image bytes (PNG/JPG...), one entry per *distinct*
    /// image; materials sharing an image share a slot, indexed by that
    /// material's textureID. loadModel decodes and uploads them via
    /// Texture::createFromMemory; a test can assert the CPU extraction without
    /// a device.
    const std::vector<std::span<const unsigned char>> &getTextureImages() const { return textureImages; }

    /// The glTF sampler (wrap modes + filters) each entry of getTextureImages()
    /// was authored with, index-parallel with it. Two textures sharing one
    /// image but sampling it differently get distinct slots (see parseCpu's
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

    /// The KHR_lights_punctual lights of every visited node, posed by its world transform, without the Sun.
    const std::vector<PunctualLight> &getPunctualLights() const { return punctualLights; }

    /// The first directional light, with a unit direction: the editor adopts it as its Sun instead of shading it.
    const std::optional<PunctualLight> &getSun() const { return sun; }

  private:
    void clearParsed();

    /// Fills the arrays from OxidANT's `load_gltf_flat` (GltfLoaderRust.cpp).
    bool parseWithRust(const std::string &modelFile, const GltfLoadOptions &options);

    std::shared_ptr<VulkanDevice> device;
    vk::CommandPool command_pool;

    // OxidANT's result, which the views below point into; untyped, as no module interface may include a bridge header.
    std::shared_ptr<const void> bridge;

    // A pointer and a count: a std::span<const unsigned int> member crashed GCC 16.2 in GltfLoader.cpp (an ICE).
    struct IndexView
    {
        const unsigned int *data = nullptr;
        std::size_t size = 0;
    };

    std::span<const Vertex> vertices;
    IndexView indices;
    std::vector<ObjMaterial> materials;
    // Per-face (per-triangle) material id.
    IndexView materialIndex;
    // One encoded image per textured material (see getTextureImages).
    std::vector<std::span<const unsigned char>> textureImages;
    // Index-parallel with textureImages; see getTextureSamplerDescs.
    std::vector<GltfSamplerDesc> textureSamplerDescs;
    // Index-parallel with textureImages; see getTextureSrgbFlags.
    std::vector<unsigned char> textureSrgb;
    // One slice per glTF primitive; see MeshRange / getMeshRanges.
    std::vector<MeshRange> meshRanges;
    std::vector<PunctualLight> punctualLights;
    std::optional<PunctualLight> sun;
};

}// namespace Kataglyphis
