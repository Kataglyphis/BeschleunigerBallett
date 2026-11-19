module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <vulkan/vulkan.hpp>

#include <glm/glm.hpp>

#include "spdlog/spdlog.h"

#include "oxidant_bridge/gltf.h"

module kataglyphis.vulkan.gltf_loader;

import kataglyphis.shared.util.utf8_path;
import kataglyphis.vulkan.mesh_range;
import kataglyphis.vulkan.obj_material;
import kataglyphis.vulkan.oxidant_diagnostics;
import kataglyphis.vulkan.punctual_light;
import kataglyphis.vulkan.sampler_builder;
import kataglyphis.vulkan.vertex;

namespace Kataglyphis {

namespace {
    namespace ox = kataglyphis::oxidant::gltf;

    // The bridge's vertices are read as Vertex in place, so the two layouts must agree field by field.
    static_assert(sizeof(ox::GltfVertex) == sizeof(Vertex));
    static_assert(alignof(ox::GltfVertex) >= alignof(Vertex));
    static_assert(offsetof(ox::GltfVertex, position) == offsetof(Vertex, position));
    static_assert(offsetof(ox::GltfVertex, normal) == offsetof(Vertex, normal));
    static_assert(offsetof(ox::GltfVertex, color) == offsetof(Vertex, color));
    static_assert(offsetof(ox::GltfVertex, uv0) == offsetof(Vertex, texture_coords));
    static_assert(offsetof(ox::GltfVertex, tangent) == offsetof(Vertex, tangent));
    static_assert(offsetof(ox::GltfVertex, uv1) == offsetof(Vertex, texture_coords1));
    static_assert(std::is_same_v<std::uint32_t, unsigned int> && std::is_same_v<std::uint8_t, unsigned char>);

    // Pinned for every glTF material: only an OBJ material's roughness falls back to shininess.
    constexpr float kGltfShininess = 1.0F;

    glm::vec3 toVec3(const std::array<float, 3> &value) { return { value[0], value[1], value[2] }; }

    ObjMaterial toObjMaterial(const ox::GltfMaterial &material)
    {
        return ObjMaterial{
            .diffuse = toVec3(material.diffuse),
            .emission = toVec3(material.emission),
            .shininess = kGltfShininess,
            .dissolve = material.dissolve,
            .textureID = material.base_texture,
            .alphaCutoff = material.alpha_cutoff,
            .uv_transform_row0 = toVec3(material.base_uv_row0),
            .uv_transform_row1 = toVec3(material.base_uv_row1),
            .metallic = material.metallic,
            .roughness = material.roughness,
            .emissiveTextureID = material.emissive_texture,
            .normalTextureID = material.normal_texture,
            .normalScale = material.normal_scale,
            .metallicRoughnessTextureID = material.metallic_roughness_texture,
            .normal_uv_transform_row0 = toVec3(material.normal_uv_row0),
            .normal_uv_transform_row1 = toVec3(material.normal_uv_row1),
            .metallic_roughness_uv_transform_row0 = toVec3(material.metallic_roughness_uv_row0),
            .metallic_roughness_uv_transform_row1 = toVec3(material.metallic_roughness_uv_row1),
            .emissive_uv_transform_row0 = toVec3(material.emissive_uv_row0),
            .emissive_uv_transform_row1 = toVec3(material.emissive_uv_row1),
            .unlit = material.unlit ? 1 : 0,
            .uv1SlotMask = static_cast<int>(material.uv1_slot_mask),
            .alphaBlend = material.alpha_blend ? 1 : 0,
        };
    }

    vk::SamplerAddressMode toAddressMode(std::uint8_t wrap)
    {
        switch (wrap) {
        case 1:
            return vk::SamplerAddressMode::eMirroredRepeat;
        case 2:
            return vk::SamplerAddressMode::eClampToEdge;
        default:
            return vk::SamplerAddressMode::eRepeat;
        }
    }

    GltfSamplerDesc toSamplerDesc(const ox::GltfSampler &sampler)
    {
        GltfSamplerDesc desc{};
        desc.addressModeU = toAddressMode(sampler.wrap_u);
        desc.addressModeV = toAddressMode(sampler.wrap_v);
        desc.magFilter = sampler.mag_nearest ? vk::Filter::eNearest : vk::Filter::eLinear;
        desc.minFilter = sampler.min_nearest ? vk::Filter::eNearest : vk::Filter::eLinear;
        desc.mipmapMode = sampler.mip_nearest ? vk::SamplerMipmapMode::eNearest : vk::SamplerMipmapMode::eLinear;
        desc.mipmaps = sampler.mipmaps;
        return desc;
    }

    PunctualLight toPunctualLight(const ox::GltfLight &source)
    {
        PunctualLight light;
        light.kind = static_cast<PunctualLightKind>(source.kind);
        light.color = toVec3(source.color);
        light.intensity = source.intensity;
        light.range = source.range;
        light.position = toVec3(source.position);
        light.direction = toVec3(source.direction);
        light.cosInner = source.cos_inner;
        light.cosOuter = source.cos_outer;
        return light;
    }

    MeshRange toMeshRange(const ox::GltfMeshRange &range)
    {
        return MeshRange{ range.vertex_base,
            range.vertex_count,
            range.index_start,
            range.index_count,
            range.tri_start,
            range.tri_count,
            range.double_sided,
            range.alpha_blended };
    }
}// namespace

struct GltfLoader::BridgeModel
{ ox::GltfModel model; };

bool GltfLoader::parseWithRust(const std::string &modelFile, const GltfLoadOptions &options)
{
    const std::vector<std::uint8_t> path = Shared::utf8PathBytes(std::filesystem::path(modelFile));
    const ox::GltfLoadOptions bridgeOptions{ .mikktspace_tangents = options.mikktspaceTangents };
    auto parsed = std::make_shared<BridgeModel>(
      BridgeModel{ ox::load_gltf_flat_with(rust::Slice<const std::uint8_t>(path.data(), path.size()), bridgeOptions) });
    logOxidantRecords();
    const ox::GltfModel &model = parsed->model;
    if (!model.ok) {
        spdlog::warn("GltfLoader (Rust): {}", std::string(model.error));
        return false;
    }

    // Rust's allocation holds implicit-lifetime Vertex objects: no C++ code ever reads it as GltfVertex.
    vertices = { reinterpret_cast<const Vertex *>(model.vertices.data()), model.vertices.size() };
    indices = { model.indices.data(), model.indices.size() };
    materialIndex = { model.material_index.data(), model.material_index.size() };
    for (const ox::GltfMaterial &material : model.materials) { materials.push_back(toObjMaterial(material)); }
    for (const ox::GltfTexture &texture : model.textures) {
        textureImages.emplace_back(texture.encoded.data(), texture.encoded.size());
        textureSamplerDescs.push_back(toSamplerDesc(texture.sampler));
        textureSrgb.push_back(texture.srgb ? 1 : 0);
    }
    for (const ox::GltfMeshRange &range : model.mesh_ranges) { meshRanges.push_back(toMeshRange(range)); }
    for (const ox::GltfLight &light : model.lights) { punctualLights.push_back(toPunctualLight(light)); }
    if (model.has_sun) { sun = toPunctualLight(model.sun); }
    bridge = std::move(parsed);
    return !vertices.empty();
}

}// namespace Kataglyphis
