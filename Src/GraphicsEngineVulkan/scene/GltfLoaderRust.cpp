module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

#include <glm/glm.hpp>

#include "spdlog/spdlog.h"

#include "oxidant_bridge/lib.h"

module kataglyphis.vulkan.gltf_loader;

import kataglyphis.vulkan.mesh_range;
import kataglyphis.vulkan.obj_material;
import kataglyphis.vulkan.punctual_light;
import kataglyphis.vulkan.sampler_builder;
import kataglyphis.vulkan.vertex;

namespace Kataglyphis {

namespace {
    namespace ox = kataglyphis::oxidant;

    // The bridge's vertex is copied into Vertex byte for byte, so the two layouts must agree field by field.
    static_assert(sizeof(ox::GltfVertex) == sizeof(Vertex));
    static_assert(offsetof(ox::GltfVertex, position) == offsetof(Vertex, position));
    static_assert(offsetof(ox::GltfVertex, normal) == offsetof(Vertex, normal));
    static_assert(offsetof(ox::GltfVertex, color) == offsetof(Vertex, color));
    static_assert(offsetof(ox::GltfVertex, uv0) == offsetof(Vertex, texture_coords));
    static_assert(offsetof(ox::GltfVertex, tangent) == offsetof(Vertex, tangent));
    static_assert(offsetof(ox::GltfVertex, uv1) == offsetof(Vertex, texture_coords1));

    // The cgltf path's pinned shininess, which only an OBJ material's roughness falls back to.
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

    // The cgltf path's mapping: a bare NEAREST or LINEAR minification still samples the mip chain.
    GltfSamplerDesc toSamplerDesc(const ox::GltfSampler &sampler)
    {
        GltfSamplerDesc desc{};
        desc.addressModeU = toAddressMode(sampler.wrap_u);
        desc.addressModeV = toAddressMode(sampler.wrap_v);
        desc.magFilter = sampler.mag_nearest ? vk::Filter::eNearest : vk::Filter::eLinear;
        desc.minFilter = sampler.min_nearest ? vk::Filter::eNearest : vk::Filter::eLinear;
        desc.mipmapMode = sampler.mip_nearest ? vk::SamplerMipmapMode::eNearest : vk::SamplerMipmapMode::eLinear;
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

bool GltfLoader::parseWithRust(const std::string &modelFile)
{
    const auto *pathBytes = reinterpret_cast<const std::uint8_t *>(modelFile.data());
    const ox::GltfModel model = ox::load_gltf_flat(rust::Slice<const std::uint8_t>(pathBytes, modelFile.size()));
    if (!model.ok) {
        spdlog::warn("GltfLoader (Rust): {}", std::string(model.error));
        return false;
    }

    vertices.resize(model.vertices.size());
    std::memcpy(vertices.data(), model.vertices.data(), model.vertices.size() * sizeof(Vertex));
    indices.assign(model.indices.begin(), model.indices.end());
    materialIndex.assign(model.material_index.begin(), model.material_index.end());
    for (const ox::GltfMaterial &material : model.materials) { materials.push_back(toObjMaterial(material)); }
    for (const ox::GltfTexture &texture : model.textures) {
        textureImages.emplace_back(texture.encoded.begin(), texture.encoded.end());
        textureSamplerDescs.push_back(toSamplerDesc(texture.sampler));
        textureSrgb.push_back(texture.srgb ? 1 : 0);
    }
    for (const ox::GltfMeshRange &range : model.mesh_ranges) { meshRanges.push_back(toMeshRange(range)); }
    for (const ox::GltfLight &light : model.lights) { punctualLights.push_back(toPunctualLight(light)); }
    return !vertices.empty();
}

}// namespace Kataglyphis
