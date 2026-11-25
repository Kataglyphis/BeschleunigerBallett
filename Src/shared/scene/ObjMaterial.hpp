#ifndef KATAGLYPHIS_SHARED_SCENE_OBJ_MATERIAL_HPP
#define KATAGLYPHIS_SHARED_SCENE_OBJ_MATERIAL_HPP

#include <glm/glm.hpp>
#include <type_traits>

namespace Kataglyphis::ShaderSlots {
// The shaders' own files; a namespace, as their internal-linkage constants cannot be exported from a module.
#include "../../../Resources/ShadersSlang/common/uv1_slots.slang"
#include "../../../third_party/OxidANT/shaders/common/alpha_mode.slang"
}// namespace Kataglyphis::ShaderSlots

// ObjMaterial::alphaMode's values, with external linkage for kataglyphis.vulkan.obj_material's export.
inline constexpr int ALPHA_OPAQUE = Kataglyphis::ShaderSlots::ALPHA_OPAQUE;
inline constexpr int ALPHA_MASK = Kataglyphis::ShaderSlots::ALPHA_MASK;
inline constexpr int ALPHA_BLEND = Kataglyphis::ShaderSlots::ALPHA_BLEND;

struct ObjMaterial
{
    glm::vec3 diffuse{ 0.7F, 0.7F, 0.7F };
    // Zero by default: shading adds emission unattenuated after shadowing, so anything else glows scene-wide.
    glm::vec3 emission{ 0.0F };
    float shininess{ 0.0F };
    float dissolve{ 1.0F };

    int textureID{ -1 };

    // glTF alphaCutoff, read only when alphaMode is ALPHA_MASK. New members go last: scalar layout mirrors the shader struct.
    float alphaCutoff{ -1.0F };

    // Base-colour KHR_texture_transform: the top two rows of T*R*S (the third is 0,0,1); identity = untransformed.
    glm::vec3 uv_transform_row0{ 1.0F, 0.0F, 0.0F };
    glm::vec3 uv_transform_row1{ 0.0F, 1.0F, 0.0F };

    // glTF metallicFactor in [0,1]; 0 (dielectric) when unauthored.
    float metallic{ 0.0F };

    // glTF roughnessFactor in [0,1]; negative means unauthored, so it is derived from shininess.
    float roughness{ -1.0F };

    // Emissive texture slot in the shared texture budget; -1 = none.
    int emissiveTextureID{ -1 };

    // Normal texture slot in the shared texture budget; -1 = none.
    int normalTextureID{ -1 };

    // Scales the sampled normal's XY (glTF 2.0 3.9.3); 1.0 when the material has no normal texture.
    float normalScale{ 1.0F };

    // Metallic-roughness texture slot, uploaded linear (G = roughness, B = metallic); -1 = none.
    int metallicRoughnessTextureID{ -1 };

    // Per-slot transform rows: glTF defines the extension per textureInfo, so slots need not share one.
    glm::vec3 normal_uv_transform_row0{ 1.0F, 0.0F, 0.0F };
    glm::vec3 normal_uv_transform_row1{ 0.0F, 1.0F, 0.0F };
    glm::vec3 metallic_roughness_uv_transform_row0{ 1.0F, 0.0F, 0.0F };
    glm::vec3 metallic_roughness_uv_transform_row1{ 0.0F, 1.0F, 0.0F };
    glm::vec3 emissive_uv_transform_row0{ 1.0F, 0.0F, 0.0F };
    glm::vec3 emissive_uv_transform_row1{ 0.0F, 1.0F, 0.0F };

    // KHR_materials_unlit: base colour as-is, with no lighting, shadowing or emission; 0 = lit.
    int unlit{ 0 };

    // OBJ map_d opacity slot; -1 = none, always for glTF, which keeps opacity in baseColorTexture.a.
    int alphaTextureID{ -1 };

    // Slots sampling TEXCOORD_1, one bit each: 0 base colour (and map_d), 1 metallic-roughness, 2 normal, 3 emissive.
    int uv1SlotMask{ 0 };

    // glTF alphaMode; BLEND draws after the opaque meshes, sorted and alpha-composited, and the tracers keep it opaque.
    int alphaMode{ ALPHA_OPAQUE };
};

// Bit positions of ObjMaterial::uv1SlotMask, with external linkage for kataglyphis.vulkan.obj_material's export.
inline constexpr int UV1_SLOT_BASE_COLOR = Kataglyphis::ShaderSlots::UV1_SLOT_BASE_COLOR;
inline constexpr int UV1_SLOT_METALLIC_ROUGHNESS = Kataglyphis::ShaderSlots::UV1_SLOT_METALLIC_ROUGHNESS;
inline constexpr int UV1_SLOT_NORMAL = Kataglyphis::ShaderSlots::UV1_SLOT_NORMAL;
inline constexpr int UV1_SLOT_EMISSIVE = Kataglyphis::ShaderSlots::UV1_SLOT_EMISSIVE;

// A constructor or base class would silently break designated initializers and the offsetof layout gates.
static_assert(std::is_aggregate_v<ObjMaterial>,
  "ObjMaterial must stay an aggregate for designated-initializer construction");
static_assert(std::is_standard_layout_v<ObjMaterial>,
  "ObjMaterial must stay standard-layout for the offsetof layout gates");

#endif
