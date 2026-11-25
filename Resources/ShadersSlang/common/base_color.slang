import scene_types;
import gltf_material;

// glTF base colour = baseColorFactor (material.diffuse) * sampled texture; binding-free so every path can import it.
float3 base_color(ObjMaterial material, float3 sampled)
{
    return sampled * material.diffuse;
}

// Bit positions of ObjMaterial.uv1SlotMask, which C++ includes too.
__exported import uv1_slots;

// Both UV sets of a surface point: xy = TEXCOORD_0, zw = TEXCOORD_1.
float4 uv_sets(float2 uv0, float2 uv1)
{
    return float4(uv0, uv1);
}

// The UV set a slot's glTF texCoord names.
float2 slot_uv(float4 uvs, ObjMaterial material, int slot)
{
    return ((material.uv1SlotMask >> slot) & 1) != 0 ? uvs.zw : uvs.xy;
}

// Base-colour slot; the other slots use their own named accessors below.
float2 base_color_uv(float4 uvs, ObjMaterial material)
{
    return transform_uv(slot_uv(uvs, material, UV1_SLOT_BASE_COLOR), material.uv_transform_row0, material.uv_transform_row1);
}

// Normal slot's UV set and KHR_texture_transform.
float2 normal_uv(float4 uvs, ObjMaterial material)
{
    return transform_uv(slot_uv(uvs, material, UV1_SLOT_NORMAL), material.normal_uv_transform_row0, material.normal_uv_transform_row1);
}

// Metallic-roughness slot's UV set and KHR_texture_transform.
float2 metallic_roughness_uv(float4 uvs, ObjMaterial material)
{
    return transform_uv(slot_uv(uvs, material, UV1_SLOT_METALLIC_ROUGHNESS), material.metallic_roughness_uv_transform_row0, material.metallic_roughness_uv_transform_row1);
}

// Emissive slot's UV set and KHR_texture_transform.
float2 emissive_uv(float4 uvs, ObjMaterial material)
{
    return transform_uv(slot_uv(uvs, material, UV1_SLOT_EMISSIVE), material.emissive_uv_transform_row0, material.emissive_uv_transform_row1);
}

// Occlusion slot's UV set and KHR_texture_transform.
float2 occlusion_uv(float4 uvs, ObjMaterial material)
{
    return transform_uv(slot_uv(uvs, material, UV1_SLOT_OCCLUSION), material.occlusion_uv_transform_row0, material.occlusion_uv_transform_row1);
}

// OBJ map_d slot: it shares the base colour's UV set and rows, identity for every OBJ material.
float2 alpha_uv(float4 uvs, ObjMaterial material)
{
    return base_color_uv(uvs, material);
}
