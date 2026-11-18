module;

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <vulkan/vulkan.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_transform_2d.hpp>

#include "spdlog/spdlog.h"

#include <cgltf.h>

module kataglyphis.vulkan.gltf_loader;

import kataglyphis.vulkan.device;
import kataglyphis.vulkan.model;
import kataglyphis.vulkan.texture;
import kataglyphis.vulkan.mesh_range;
import kataglyphis.vulkan.model_assembly;
import kataglyphis.vulkan.sampler_builder;
import kataglyphis.shared.util.file_reader;

namespace Kataglyphis {

GltfLoader::GltfLoader(std::shared_ptr<VulkanDevice> device,// DEVICE_SINK_OK: moved into member
  vk::CommandPool command_pool)
  : device(std::move(device)), command_pool(command_pool)
{}

std::shared_ptr<Model> GltfLoader::loadModel(const std::string &modelFile)
{
    if (!parseCpu(modelFile)) {
        spdlog::error("Failed to parse glTF: {}", modelFile);
        return nullptr;
    }
    return uploadParsed();
}

void GltfLoader::adoptParsed(GltfLoader &&other)
{
    vertices = std::move(other.vertices);
    indices = std::move(other.indices);
    materials = std::move(other.materials);
    materialIndex = std::move(other.materialIndex);
    textureImages = std::move(other.textureImages);
    textureSamplerDescs = std::move(other.textureSamplerDescs);
    textureSrgb = std::move(other.textureSrgb);
    meshRanges = std::move(other.meshRanges);
    punctualLights = std::move(other.punctualLights);
}

std::shared_ptr<Model> GltfLoader::uploadParsed()
{
    if (!uploadPreconditionsMet(device, vertices.size(), "GltfLoader")) { return nullptr; }

    std::shared_ptr<Model> model = std::make_shared<Model>(device);

    // Upload order must match the textureIDs parseCpu assigned.
    for (std::size_t i = 0; i < textureImages.size(); ++i) {
        const std::vector<unsigned char> &encoded = textureImages[i];
        Texture texture;
        const bool created =
          texture.createFromMemory(device, command_pool, encoded.data(), encoded.size(), textureSrgb[i] != 0);
        addTextureOrDefault(*model, device, command_pool, created, std::move(texture), textureSamplerDescs[i]);
    }
    ensureAtLeastOneTexture(*model, device, command_pool);

    // One mesh per glTF primitive; each shares the full materials array (materialIndex keeps original indices).
    addMeshesForRanges(*model, device, command_pool, vertices, indices, materialIndex, materials, meshRanges);
    model->setPunctualLights(punctualLights);
    return model;
}

TexCoordSetInfo describeTexCoordSet(int texcoord)
{ return { static_cast<unsigned int>(texcoord), texcoord == 0 || texcoord == 1 }; }

namespace {

    // Pinned for every glTF material: roughness is always authored or glTF's default, so shininess is never read.
    constexpr float kFallbackShininess = 1.0F;

    // glTF's defaults for an absent pbrMetallicRoughness block and for the default material (owner, 2026-11-17).
    constexpr float kGltfDefaultMetallic = 1.0F;
    constexpr float kGltfDefaultRoughness = 1.0F;

    /// glTF's default material, for primitives with no material: white, metallic 1, roughness 1.
    ObjMaterial neutralMaterial()
    {
        return ObjMaterial{ .diffuse = glm::vec3(1.0F),
            .shininess = kFallbackShininess,
            .metallic = kGltfDefaultMetallic,
            .roughness = kGltfDefaultRoughness };
    }

    /// The UV set a slot samples: KHR_texture_transform's texCoord overrides the view's; TEXCOORD_2+ warns, uses 0.
    unsigned int slotTexCoordSet(const char *materialName, const cgltf_texture_view &view, const char *slotLabel)
    {
        if (view.texture == nullptr) { return 0U; }
        const int texcoord =
          (view.has_transform != 0 && view.transform.has_texcoord != 0) ? view.transform.texcoord : view.texcoord;
        const TexCoordSetInfo texCoordInfo = describeTexCoordSet(texcoord);
        if (!texCoordInfo.supported) {
            spdlog::warn(
              "GltfLoader: material '{}' {} texture uses TEXCOORD_{}, but only TEXCOORD_0/1 are supported; "
              "sampling with UV0",
              materialName,
              slotLabel,
              texCoordInfo.set);
            return 0U;
        }
        return texCoordInfo.set;
    }

    /// Top two rows of a KHR_texture_transform T*R*S matrix; the third is always [0,0,1].
    struct UvTransformRows
    {
        glm::vec3 row0{ 1.0F, 0.0F, 0.0F };
        glm::vec3 row1{ 0.0F, 1.0F, 0.0F };
    };

    /// Identity rows when the slot has no texture or no transform.
    UvTransformRows readUvTransform(const cgltf_texture_view &view)
    {
        if (view.texture == nullptr || view.has_transform == 0) { return {}; }

        const cgltf_texture_transform &transform = view.transform;
        const glm::vec2 offset(transform.offset[0], transform.offset[1]);
        const glm::vec2 scaleVec(transform.scale[0], transform.scale[1]);

        // Same T*R*S as the Rust loader, rotation negated because glTF's is clockwise in UV space.
        const glm::mat3 uvMatrix =
          glm::scale(glm::rotate(glm::translate(glm::mat3(1.0F), offset), -transform.rotation), scaleVec);

        UvTransformRows rows;
        rows.row0 = glm::vec3(uvMatrix[0][0], uvMatrix[1][0], uvMatrix[2][0]);
        rows.row1 = glm::vec3(uvMatrix[0][1], uvMatrix[1][1], uvMatrix[2][1]);
        return rows;
    }

    /// One texture slot; `srgb` is true for colour (base-colour, emissive), false for data (glTF 2.0 SS3.9.2/3).
    struct GltfTextureSlot
    {
        const cgltf_texture_view *view;
        const char *label;
        bool srgb;
    };

    /// The only place the pbr guard lives: without the block, the pbr slots point at an empty view.
    std::array<GltfTextureSlot, 4> gltfTextureSlots(const cgltf_material &material)
    {
        static const cgltf_texture_view kNoView{};
        const bool hasPbr = material.has_pbr_metallic_roughness != 0;
        const cgltf_texture_view &baseColorView = hasPbr ? material.pbr_metallic_roughness.base_color_texture : kNoView;
        const cgltf_texture_view &metallicRoughnessView =
          hasPbr ? material.pbr_metallic_roughness.metallic_roughness_texture : kNoView;
        return { {
          { &baseColorView, "base-colour", true },
          { &metallicRoughnessView, "metallic-roughness", false },
          { &material.normal_texture, "normal", false },
          { &material.emissive_texture, "emissive", true },
        } };
    }

    /// Maps a glTF material to ObjMaterial; texture IDs are assigned later in parseCpu.
    ObjMaterial fromGltfMaterial(const cgltf_material &material)
    {
        // Without a pbr block glTF's defaults apply, as the Rust loader reads them.
        glm::vec3 baseColor(1.0F);
        float baseAlpha = 1.0F;
        float metallic = kGltfDefaultMetallic;
        float authoredRoughness = kGltfDefaultRoughness;
        const char *materialName = material.name != nullptr ? material.name : "<unnamed>";
        if (material.has_pbr_metallic_roughness != 0) {
            const cgltf_pbr_metallic_roughness &pbr = material.pbr_metallic_roughness;
            baseColor = glm::vec3(pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2]);
            baseAlpha = pbr.base_color_factor[3];
            metallic = glm::clamp(pbr.metallic_factor, 0.0F, 1.0F);
            authoredRoughness = glm::clamp(pbr.roughness_factor, 0.0F, 1.0F);
        }
        // KHR_materials_emissive_strength is folded into the factor, so shaders need not know it.
        const float emissiveStrength =
          material.has_emissive_strength != 0 ? material.emissive_strength.emissive_strength : 1.0F;
        const glm::vec3 emission(material.emissive_factor[0] * emissiveStrength,
          material.emissive_factor[1] * emissiveStrength,
          material.emissive_factor[2] * emissiveStrength);

        // BLEND keeps no cutoff (-1) like OPAQUE; the raster passes composite it instead (alphaBlend).
        const float alphaCutoff = (material.alpha_mode == cgltf_alpha_mode_mask) ? material.alpha_cutoff : -1.0F;

        // KHR_texture_transform and texCoord are per slot, so each slot reads its own; slot i is uv1SlotMask bit i.
        const std::array<GltfTextureSlot, 4> textureSlots = gltfTextureSlots(material);
        std::array<UvTransformRows, 4> uvTransforms{};
        int uv1SlotMask = 0;
        for (std::size_t i = 0; i < textureSlots.size(); ++i) {
            if (slotTexCoordSet(materialName, *textureSlots[i].view, textureSlots[i].label) == 1U) {
                uv1SlotMask |= 1 << i;
            }
            uvTransforms[i] = readUvTransform(*textureSlots[i].view);
        }
        const UvTransformRows &baseColorUvTransform = uvTransforms[0];
        const UvTransformRows &metallicRoughnessUvTransform = uvTransforms[1];
        const UvTransformRows &normalUvTransform = uvTransforms[2];
        const UvTransformRows &emissiveUvTransform = uvTransforms[3];

        return ObjMaterial{
            .diffuse = baseColor,
            .emission = emission,
            .shininess = kFallbackShininess,
            .dissolve = baseAlpha,// glTF baseColorFactor.a
            // textureID, emissiveTextureID, normalTextureID: assigned in parseCpu.
            .alphaCutoff = alphaCutoff,// glTF MASK cutoff (-1 = OPAQUE/BLEND)
            .uv_transform_row0 = baseColorUvTransform.row0,// KHR_texture_transform T*R*S row 0, base colour
            .uv_transform_row1 = baseColorUvTransform.row1,// KHR_texture_transform T*R*S row 1, base colour
            .metallic = metallic,// glTF pbrMetallicRoughness.metallicFactor
            .roughness = authoredRoughness,// glTF pbrMetallicRoughness.roughnessFactor, 1 without the block
            // cgltf leaves scale zero without a normalTexture, so guard on the pointer.
            .normalScale = material.normal_texture.texture != nullptr ? material.normal_texture.scale : 1.0F,
            .normal_uv_transform_row0 = normalUvTransform.row0,
            .normal_uv_transform_row1 = normalUvTransform.row1,
            .metallic_roughness_uv_transform_row0 = metallicRoughnessUvTransform.row0,
            .metallic_roughness_uv_transform_row1 = metallicRoughnessUvTransform.row1,
            .emissive_uv_transform_row0 = emissiveUvTransform.row0,
            .emissive_uv_transform_row1 = emissiveUvTransform.row1,
            .unlit = (material.unlit != 0) ? 1 : 0,// KHR_materials_unlit
            .uv1SlotMask = uv1SlotMask,
            .alphaBlend = (material.alpha_mode == cgltf_alpha_mode_blend) ? 1 : 0,
        };
    }

    /// Reads a 2-, 3- or 4-component float attribute into `out`, one entry per accessor element.
    template<int N, typename VecT> void readAttribute(const cgltf_accessor *accessor, std::vector<VecT> &out)
    {
        // Pre-filled with 1.0: a VEC3 COLOR_0 read with N=4 leaves alpha unwritten.
        out.assign(accessor->count, VecT(1.0F));
        for (cgltf_size i = 0; i < accessor->count; ++i) {
            cgltf_accessor_read_float(accessor, i, glm::value_ptr(out[i]), N);
        }
    }

    /// Decodes only well-formed %XX triplets; a bare or truncated escape is kept as is.
    std::string percentDecodeUri(const std::string &uri)
    {
        std::string out;
        out.reserve(uri.size());
        for (std::string::size_type i = 0; i < uri.size(); ++i) {
            if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit(static_cast<unsigned char>(uri[i + 1])) != 0
                && std::isxdigit(static_cast<unsigned char>(uri[i + 2])) != 0) {
                out.push_back(static_cast<char>(std::strtoul(uri.substr(i + 1, 2).c_str(), nullptr, 16)));
                i += 2;
            } else {
                out.push_back(uri[i]);
            }
        }
        return out;
    }

    /// Rejects `..` escapes from untrusted glTFs; lexically_relative avoids "/foo/bar" matching "/foo/barbaz".
    bool isWithinDirectory(const std::filesystem::path &candidate, const std::filesystem::path &base)
    {
        const std::filesystem::path rel = candidate.lexically_relative(base);
        return !rel.empty() && rel.begin()->string() != "..";
    }

    /// Encoded bytes from a buffer view, base64 data URI or sibling file; empty for remote URIs or on failure.
    std::vector<unsigned char> extractImageBytes(const cgltf_image *image,
      const cgltf_options &options,
      const std::filesystem::path &documentDir)
    {
        if (image == nullptr) { return {}; }

        if (image->buffer_view != nullptr && image->buffer_view->buffer != nullptr
            && image->buffer_view->buffer->data != nullptr) {
            const auto *base = static_cast<const unsigned char *>(image->buffer_view->buffer->data);
            const cgltf_size offset = image->buffer_view->offset;
            const cgltf_size size = image->buffer_view->size;
            // Reject a view running past its buffer, written so offset + size cannot overflow.
            const cgltf_size buffer_size = image->buffer_view->buffer->size;
            if (offset > buffer_size || size > buffer_size - offset) { return {}; }
            return std::vector<unsigned char>(base + offset, base + offset + size);
        }

        if (image->uri != nullptr) {
            const std::string uri = image->uri;
            const std::string marker = "base64,";
            const std::string::size_type pos = uri.find(marker);
            if (pos != std::string::npos) {
                const char *b64 = uri.c_str() + pos + marker.size();
                const cgltf_size b64len = uri.size() - pos - marker.size();
                // Anything but a positive multiple of 4 would underflow the unsigned size below.
                if (b64len < 4 || (b64len % 4) != 0) { return {}; }
                cgltf_size padding = 0;
                if (b64[b64len - 1] == '=') { ++padding; }
                if (b64[b64len - 2] == '=') { ++padding; }
                const cgltf_size decoded = (b64len / 4) * 3 - padding;
                void *out = nullptr;
                if (cgltf_load_buffer_base64(&options, decoded, b64, &out) == cgltf_result_success && out != nullptr) {
                    const auto *bytes = static_cast<const unsigned char *>(out);
                    std::vector<unsigned char> result(bytes, bytes + decoded);
                    free(out);// cgltf's default allocator is malloc/free
                    return result;
                }
                return {};
            }

            // Remote and malformed data: URIs must not fall through to a filesystem read.
            if (uri.rfind("data:", 0) == 0 || uri.rfind("http:", 0) == 0 || uri.rfind("https:", 0) == 0) { return {}; }

            std::string decodedUri = percentDecodeUri(uri);
            std::replace(decodedUri.begin(), decodedUri.end(), '\\', '/');

            const std::filesystem::path baseDir = documentDir.empty() ? std::filesystem::path(".") : documentDir;
            const std::filesystem::path candidate = baseDir / decodedUri;

            std::error_code baseEc;
            const std::filesystem::path canonicalBase = std::filesystem::weakly_canonical(baseDir, baseEc);
            std::error_code candidateEc;
            const std::filesystem::path canonicalCandidate = std::filesystem::weakly_canonical(candidate, candidateEc);
            if (baseEc || candidateEc) { return {}; }

            if (!isWithinDirectory(canonicalCandidate, canonicalBase)) {
                spdlog::warn(
                  "GltfLoader: external image URI '{}' resolves outside the document directory ('{}'); rejected",
                  uri,
                  canonicalCandidate.string());
                return {};
            }

            const std::vector<char> fileBytes = Kataglyphis::Shared::readBinaryFile(canonicalCandidate.string());
            if (fileBytes.empty()) {
                spdlog::warn("GltfLoader: external image URI '{}' resolved to '{}' but the file could not be read",
                  uri,
                  canonicalCandidate.string());
                return {};
            }
            return std::vector<unsigned char>(fileBytes.begin(), fileBytes.end());
        }
        return {};
    }

    /// Must match gltf_loader.rs's to_cpu_sampler; no sampler or an undefined filter keeps the defaults.
    GltfSamplerDesc gltfSamplerDesc(const cgltf_sampler *sampler)
    {
        GltfSamplerDesc desc{};
        if (sampler == nullptr) { return desc; }

        const auto wrap = [](cgltf_wrap_mode mode) {
            switch (mode) {
            case cgltf_wrap_mode_clamp_to_edge:
                return vk::SamplerAddressMode::eClampToEdge;
            case cgltf_wrap_mode_mirrored_repeat:
                return vk::SamplerAddressMode::eMirroredRepeat;
            case cgltf_wrap_mode_repeat:
            default:
                return vk::SamplerAddressMode::eRepeat;
            }
        };
        desc.addressModeU = wrap(sampler->wrap_s);
        desc.addressModeV = wrap(sampler->wrap_t);

        desc.magFilter = sampler->mag_filter == cgltf_filter_type_nearest ? vk::Filter::eNearest : vk::Filter::eLinear;

        // Split like to_cpu_sampler; a bare Nearest/Linear counts as its *_mipmap_* sibling.
        switch (sampler->min_filter) {
        case cgltf_filter_type_nearest:
        case cgltf_filter_type_nearest_mipmap_nearest:
            desc.minFilter = vk::Filter::eNearest;
            desc.mipmapMode = vk::SamplerMipmapMode::eNearest;
            break;
        case cgltf_filter_type_nearest_mipmap_linear:
            desc.minFilter = vk::Filter::eNearest;
            desc.mipmapMode = vk::SamplerMipmapMode::eLinear;
            break;
        case cgltf_filter_type_linear_mipmap_nearest:
            desc.minFilter = vk::Filter::eLinear;
            desc.mipmapMode = vk::SamplerMipmapMode::eNearest;
            break;
        case cgltf_filter_type_linear:
        case cgltf_filter_type_linear_mipmap_linear:
        case cgltf_filter_type_undefined:
        default:
            desc.minFilter = vk::Filter::eLinear;
            desc.mipmapMode = vk::SamplerMipmapMode::eLinear;
            break;
        }

        return desc;
    }

}// namespace

namespace {
    /// Any unit vector perpendicular to `n`, for a tangent with no direction of its own.
    glm::vec3 anyPerpendicular(const glm::vec3 &n)
    {
        glm::vec3 axis = glm::cross(n, glm::vec3(0.0F, 1.0F, 0.0F));
        if (glm::dot(axis, axis) <= 1e-12F) { axis = glm::cross(n, glm::vec3(1.0F, 0.0F, 0.0F)); }
        return glm::dot(axis, axis) <= 1e-12F ? glm::vec3(1.0F, 0.0F, 0.0F) : glm::normalize(axis);
    }

    /// A file's zero or non-finite TANGENT would normalize to NaN; it gets a perpendicular, and w a sign (Rust's repair).
    void repairTangents(std::vector<Vertex> &local)
    {
        for (Vertex &v : local) {
            const glm::vec3 t(v.tangent);
            const float lengthSquared = glm::dot(t, t);
            if (!std::isfinite(lengthSquared) || lengthSquared <= 1e-12F) {
                const glm::vec3 axis = anyPerpendicular(v.normal);
                v.tangent = glm::vec4(axis, v.tangent.w);
            }
            if (v.tangent.w != 1.0F && v.tangent.w != -1.0F) { v.tangent.w = v.tangent.w < 0.0F ? -1.0F : 1.0F; }
        }
    }

    /// A tangent lies in the surface, so it takes `world`, not normalMatrix; handedness flips with mirroring.
    Vertex bakeToWorld(const Vertex &v, const glm::mat4 &world, const glm::mat3 &normalMatrix, bool mirrored)
    {
        Vertex out = v;
        out.position = glm::vec3(world * glm::vec4(v.position, 1.0F));
        out.normal = glm::normalize(normalMatrix * v.normal);
        out.tangent =
          glm::vec4(glm::normalize(glm::mat3(world) * glm::vec3(v.tangent)), mirrored ? -v.tangent.w : v.tangent.w);
        return out;
    }
}// namespace

void GltfLoader::processPrimitive(const cgltf_primitive *primitive,
  const glm::mat4 &world,
  const glm::mat3 &normalMatrix,
  const cgltf_data *data,
  unsigned int fallbackMaterial,
  bool mirrored)
{
    // Strips and fans are triangulated below; points and lines are skipped.
    const cgltf_primitive_type primType = primitive->type;
    if (primType != cgltf_primitive_type_triangles && primType != cgltf_primitive_type_triangle_strip
        && primType != cgltf_primitive_type_triangle_fan) {
        return;
    }

    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<glm::vec2> uvs1;
    std::vector<glm::vec4> colors;
    std::vector<glm::vec4> tangents;

    for (cgltf_size a = 0; a < primitive->attributes_count; ++a) {
        const cgltf_attribute *attribute = &primitive->attributes[a];
        switch (attribute->type) {
        case cgltf_attribute_type_position:
            readAttribute<3>(attribute->data, positions);
            break;
        case cgltf_attribute_type_normal:
            readAttribute<3>(attribute->data, normals);
            break;
        case cgltf_attribute_type_texcoord:
            if (attribute->index == 0) { readAttribute<2>(attribute->data, uvs); }
            if (attribute->index == 1) { readAttribute<2>(attribute->data, uvs1); }
            break;
        case cgltf_attribute_type_color:
            // COLOR_0 multiplies the base colour, alpha included; absent means white.
            if (attribute->index == 0) { readAttribute<4>(attribute->data, colors); }
            break;
        case cgltf_attribute_type_tangent:
            // glTF TANGENT is always a VEC4 accessor (xyz + w handedness).
            readAttribute<4>(attribute->data, tangents);
            break;
        default:
            break;
        }
    }
    if (positions.empty()) { return; }

    // The mesh's own space: normals and tangents are finished there, as the Rust loader does, then baked.
    const auto makeVertex = [&](std::size_t i) {
        // Placeholder when NORMAL is absent: flat normals are computed below once the indices are known.
        const glm::vec3 normal = i < normals.size() ? normals[i] : glm::vec3(0.0F, 1.0F, 0.0F);
        const glm::vec2 uv = i < uvs.size() ? uvs[i] : glm::vec2(0.0F);
        // Without TEXCOORD_1 a slot naming set 1 samples UV0, as the Rust loader does.
        const glm::vec2 uv1 = i < uvs1.size() ? uvs1[i] : uv;
        const glm::vec4 vcolor = i < colors.size() ? colors[i] : glm::vec4(1.0F);
        const glm::vec4 vtangent = i < tangents.size() ? tangents[i] : glm::vec4(0.0F);
        return Vertex(positions[i], normal, vcolor, uv, vtangent, uv1);
    };

    // A non-indexed primitive uses the implicit sequence 0..N-1.
    std::vector<unsigned int> localSeq;
    if (primitive->indices != nullptr) {
        const cgltf_accessor *idx = primitive->indices;
        localSeq.reserve(idx->count);
        for (cgltf_size i = 0; i < idx->count; ++i) {
            localSeq.push_back(static_cast<unsigned int>(cgltf_accessor_read_index(idx, i)));
        }
    } else {
        localSeq.reserve(positions.size());
        for (std::size_t i = 0; i < positions.size(); ++i) { localSeq.push_back(static_cast<unsigned int>(i)); }
    }

    const auto vertexCount = positions.size();
    std::size_t droppedTriangles = 0;
    // Out-of-range triangles are dropped here, before flat normals and the BLAS read their indices.
    std::vector<std::array<unsigned int, 3>> triangles;
    const auto addTri = [&](unsigned int a, unsigned int b, unsigned int c) {
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount) {
            ++droppedTriangles;
            return;
        }
        triangles.push_back({ a, b, c });
    };
    if (primType == cgltf_primitive_type_triangle_strip) {
        // Alternate winding each step to keep a consistent front face.
        for (std::size_t i = 0; i + 2 < localSeq.size(); ++i) {
            if ((i & 1U) == 0U) {
                addTri(localSeq[i], localSeq[i + 1], localSeq[i + 2]);
            } else {
                addTri(localSeq[i + 1], localSeq[i], localSeq[i + 2]);
            }
        }
    } else if (primType == cgltf_primitive_type_triangle_fan) {
        // Fan: vertex 0 is shared by every triangle.
        for (std::size_t i = 1; i + 1 < localSeq.size(); ++i) { addTri(localSeq[0], localSeq[i], localSeq[i + 1]); }
    } else {
        for (std::size_t i = 0; i + 2 < localSeq.size(); i += 3) {
            addTri(localSeq[i], localSeq[i + 1], localSeq[i + 2]);
        }
    }

    if (droppedTriangles > 0) {
        spdlog::warn("GltfLoader: dropped {} out-of-range triangle(s) in a primitive with {} vertices",
          droppedTriangles,
          vertexCount);
    }

    // Flat shading needs a vertex per corner: a shared corner would keep only its last face's normal.
    std::vector<Vertex> local;
    std::vector<unsigned int> localIndices;
    localIndices.reserve(triangles.size() * 3);
    if (normals.empty()) {
        for (const std::array<unsigned int, 3> &tri : triangles) {
            for (const unsigned int index : tri) {
                localIndices.push_back(static_cast<unsigned int>(local.size()));
                local.push_back(makeVertex(index));
            }
        }
        // The glTF spec requires flat normals when NORMAL is absent.
        vertex::computeFlatNormals(local, localIndices, 0);
    } else {
        for (std::size_t i = 0; i < vertexCount; ++i) { local.push_back(makeVertex(i)); }
        for (const std::array<unsigned int, 3> &tri : triangles) {
            localIndices.insert(localIndices.end(), tri.begin(), tri.end());
        }
    }

    const unsigned int primitiveMaterial = primitive->material != nullptr
                                             ? static_cast<unsigned int>(primitive->material - data->materials)
                                             : fallbackMaterial;

    // Generated only when TANGENT is absent, after the final normals exist, from the normal texture's UV set.
    if (tangents.empty()) {
        const bool normalOnUv1 = (materials[primitiveMaterial].uv1SlotMask & (1 << UV1_SLOT_NORMAL)) != 0;
        vertex::computeTangents(local, localIndices, 0, normalOnUv1 ? 1U : 0U);
    } else {
        repairTangents(local);
    }

    const auto base = static_cast<unsigned int>(vertices.size());
    for (const Vertex &v : local) { vertices.push_back(bakeToWorld(v, world, normalMatrix, mirrored)); }

    // A mirrored node reverses each triangle's winding.
    const std::size_t primIndexStart = indices.size();
    for (std::size_t i = 0; i + 2 < localIndices.size(); i += 3) {
        indices.push_back(base + localIndices[i]);
        indices.push_back(base + localIndices[mirrored ? i + 2 : i + 1]);
        indices.push_back(base + localIndices[mirrored ? i + 1 : i + 2]);
    }

    // One id per emitted triangle: the raw index count over-counts strips and fans.
    const std::size_t triStart = materialIndex.size();
    const std::size_t emittedTriangles = (indices.size() - primIndexStart) / 3;
    materialIndex.insert(materialIndex.end(), emittedTriangles, primitiveMaterial);

    // This primitive's slice of the flat arrays, built as its own mesh.
    const bool doubleSided = primitive->material != nullptr && primitive->material->double_sided != 0;
    const bool alphaBlended =
      primitive->material != nullptr && primitive->material->alpha_mode == cgltf_alpha_mode_blend;
    meshRanges.push_back(MeshRange{ static_cast<std::size_t>(base),
      vertices.size() - base,
      primIndexStart,
      indices.size() - primIndexStart,
      triStart,
      emittedTriangles,
      doubleSided,
      alphaBlended });
}

namespace {
    // The spec ignores a skinned node's transform; without joint animation it stays in bind pose.
    glm::mat4 nodeWorld(const cgltf_node *node)
    {
        if (node->skin != nullptr) { return glm::mat4(1.0F); }
        cgltf_float worldRaw[16];
        cgltf_node_transform_world(node, worldRaw);
        return glm::make_mat4(worldRaw);
    }
}// namespace

void GltfLoader::readLight(const cgltf_node *node, const glm::mat4 &world)
{
    const cgltf_light *source = node->light;
    if (source == nullptr) { return; }
    PunctualLight light;
    switch (source->type) {
    case cgltf_light_type_point:
        light.kind = PunctualLightKind::Point;
        break;
    case cgltf_light_type_spot:
        light.kind = PunctualLightKind::Spot;
        light.cosInner = std::cos(source->spot_inner_cone_angle);
        light.cosOuter = std::cos(source->spot_outer_cone_angle);
        break;
    case cgltf_light_type_directional:
        light.kind = PunctualLightKind::Directional;
        break;
    default:
        return;
    }
    light.color = glm::vec3(source->color[0], source->color[1], source->color[2]);
    light.intensity = source->intensity;
    light.range = source->range;
    light.position = glm::vec3(world * glm::vec4(0.0F, 0.0F, 0.0F, 1.0F));
    const glm::vec3 direction = glm::mat3(world) * glm::vec3(0.0F, 0.0F, -1.0F);
    const float length = glm::length(direction);
    light.direction = length > 0.0F ? direction / length : glm::vec3(0.0F, 0.0F, -1.0F);
    punctualLights.push_back(light);
}

void GltfLoader::visitNode(const cgltf_node *node, const cgltf_data *data, unsigned int fallbackMaterial)
{
    if (node->mesh != nullptr || node->light != nullptr) {
        const glm::mat4 world = nodeWorld(node);
        readLight(node, world);
        if (node->mesh != nullptr) {
            const glm::mat3 normalMatrix = glm::inverseTranspose(glm::mat3(world));
            // A negative determinant means a mirrored node (glTF 2.0 SS3.7.4); skinned nodes never are.
            const bool mirrored = glm::determinant(glm::mat3(world)) < 0.0F;
            for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
                processPrimitive(&node->mesh->primitives[p], world, normalMatrix, data, fallbackMaterial, mirrored);
            }
        }
    }

    for (cgltf_size c = 0; c < node->children_count; ++c) { visitNode(node->children[c], data, fallbackMaterial); }
}

GltfParser gltfParserFromEnvironment()
{
    const char *choice = std::getenv("KATAGLYPHIS_GLTF_LOADER");
    return (choice != nullptr && std::string_view(choice) == "cgltf") ? GltfParser::Cgltf : GltfParser::Rust;
}

void GltfLoader::clearParsed()
{
    vertices.clear();
    indices.clear();
    materials.clear();
    materialIndex.clear();
    textureImages.clear();
    textureSamplerDescs.clear();
    textureSrgb.clear();
    meshRanges.clear();
    punctualLights.clear();
}

bool GltfLoader::parseCpu(const std::string &modelFile) { return parseCpu(modelFile, gltfParserFromEnvironment()); }

bool GltfLoader::parseCpu(const std::string &modelFile, GltfParser parser)
{
    // A reused loader starts empty, and a failed parse leaves it empty.
    clearParsed();
    const bool parsed = parser == GltfParser::Rust ? parseWithRust(modelFile) : parseWithCgltf(modelFile);
    if (!parsed) { clearParsed(); }
    return parsed;
}

bool GltfLoader::parseWithCgltf(const std::string &modelFile)
{
    cgltf_options options{};
    cgltf_data *data = nullptr;
    if (cgltf_parse_file(&options, modelFile.c_str(), &data) != cgltf_result_success) { return false; }
    // The GUI feeds arbitrary files, and the walk below trusts cgltf_validate's invariants.
    if (cgltf_validate(data) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }
    if (cgltf_load_buffers(&options, data, modelFile.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }

    // Slots dedup on (image, sampler, srgb) to save the MAX_TEXTURE_COUNT budget; colour space needs its own slot.
    const std::filesystem::path documentDir = std::filesystem::path(modelFile).parent_path();

    std::map<std::tuple<const cgltf_image *, const cgltf_sampler *, bool>, int> imageSlot;

    // Returns -1 when the view has no texture or its bytes cannot be extracted.
    const auto assignTextureSlot = [&](const cgltf_texture_view &view, bool srgb) -> int {
        if (view.texture == nullptr) { return -1; }
        const cgltf_texture *tex = view.texture;
        const cgltf_image *img = tex->image;
        const auto key = std::make_tuple(img, tex->sampler, srgb);
        const auto existing = imageSlot.find(key);
        if (existing != imageSlot.end()) { return existing->second; }

        std::vector<unsigned char> bytes = extractImageBytes(img, options, documentDir);
        if (bytes.empty()) { return -1; }
        const int slot = static_cast<int>(textureImages.size());
        imageSlot[key] = slot;
        textureImages.push_back(std::move(bytes));
        textureSamplerDescs.push_back(gltfSamplerDesc(tex->sampler));
        textureSrgb.push_back(srgb ? 1 : 0);
        return slot;
    };

    for (cgltf_size m = 0; m < data->materials_count; ++m) {
        const cgltf_material &material = data->materials[m];
        ObjMaterial objMaterial = fromGltfMaterial(material);
        // Slot order: base-colour, metallic-roughness, normal, emissive.
        const std::array<GltfTextureSlot, 4> textureSlots = gltfTextureSlots(material);
        std::array<int, 4> assignedSlots{};
        for (std::size_t i = 0; i < textureSlots.size(); ++i) {
            assignedSlots[i] = assignTextureSlot(*textureSlots[i].view, textureSlots[i].srgb);
        }
        objMaterial.textureID = assignedSlots[0];
        objMaterial.metallicRoughnessTextureID = assignedSlots[1];
        objMaterial.normalTextureID = assignedSlots[2];
        objMaterial.emissiveTextureID = assignedSlots[3];
        materials.push_back(objMaterial);
    }
    const auto fallbackMaterial = static_cast<unsigned int>(materials.size());
    materials.push_back(neutralMaterial());

    // Must change together with gltf_loader.rs. See docs/source/model-loading.md § Which nodes a glTF load walks.
    const cgltf_scene *scene =
      data->scene != nullptr ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
    if (scene != nullptr) {
        for (cgltf_size n = 0; n < scene->nodes_count; ++n) { visitNode(scene->nodes[n], data, fallbackMaterial); }
    } else {
        // No scenes: walk every node once without recursion, as children are already in data->nodes.
        spdlog::warn("GltfLoader: {} has no scenes; loading every node in the document", modelFile);
        for (cgltf_size n = 0; n < data->nodes_count; ++n) {
            const cgltf_node *node = &data->nodes[n];
            if (node->mesh == nullptr && node->light == nullptr) { continue; }
            const glm::mat4 world = nodeWorld(node);
            readLight(node, world);
            if (node->mesh == nullptr) { continue; }
            const glm::mat3 normalMatrix = glm::inverseTranspose(glm::mat3(world));
            const bool mirrored = glm::determinant(glm::mat3(world)) < 0.0F;

            for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
                processPrimitive(&node->mesh->primitives[p], world, normalMatrix, data, fallbackMaterial, mirrored);
            }
        }
    }

    cgltf_free(data);

    return !vertices.empty();
}

}// namespace Kataglyphis
