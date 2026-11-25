import scene_types;
import base_color;
import emission;
import material_rules;
import gltf_material;

// Ray tracing callers must use the `_lod0` helpers. See docs/source/shader-sharing.md § What is wired today.

// Indexed only through NonUniformResourceIndex: triangles of one draw, rays and bounces each pick their own slot.
[vk::binding(TEXTURES_BINDING, 0)] Texture2D<float4> textures[MAX_TEXTURE_COUNT];
[vk::binding(SAMPLER_BINDING, 0)] SamplerState textureSamplers[MAX_TEXTURE_COUNT];

// map_d cut-out slot; callers guard alphaTextureID >= 0 because the miss-case default differs per call site.
float sample_alpha_lod0(ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    int alphaTexId = resolve_texture_slot(obj, material.alphaTextureID);
    return textures[NonUniformResourceIndex(alphaTexId)].SampleLevel(textureSamplers[NonUniformResourceIndex(alphaTexId)], alpha_uv(texCoords, material), 0.0).r;
}

float sample_alpha(ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    int alphaTexId = resolve_texture_slot(obj, material.alphaTextureID);
    return textures[NonUniformResourceIndex(alphaTexId)].Sample(textureSamplers[NonUniformResourceIndex(alphaTexId)], alpha_uv(texCoords, material)).r;
}

float3 sample_normal_lod0(ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    int normalTexId = resolve_texture_slot(obj, material.normalTextureID);
    return textures[NonUniformResourceIndex(normalTexId)].SampleLevel(textureSamplers[NonUniformResourceIndex(normalTexId)], normal_uv(texCoords, material), 0.0).xyz;
}

float3 sample_normal(ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    int normalTexId = resolve_texture_slot(obj, material.normalTextureID);
    return textures[NonUniformResourceIndex(normalTexId)].Sample(textureSamplers[NonUniformResourceIndex(normalTexId)], normal_uv(texCoords, material)).xyz;
}

float4 sample_metallic_roughness_lod0(ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    int metallicRoughnessTexId = resolve_texture_slot(obj, material.metallicRoughnessTextureID);
    return textures[NonUniformResourceIndex(metallicRoughnessTexId)].SampleLevel(textureSamplers[NonUniformResourceIndex(metallicRoughnessTexId)], metallic_roughness_uv(texCoords, material), 0.0);
}

float4 sample_metallic_roughness(ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    int metallicRoughnessTexId = resolve_texture_slot(obj, material.metallicRoughnessTextureID);
    return textures[NonUniformResourceIndex(metallicRoughnessTexId)].Sample(textureSamplers[NonUniformResourceIndex(metallicRoughnessTexId)], metallic_roughness_uv(texCoords, material));
}

float3 sample_emissive_lod0(ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    int emissiveTexId = resolve_texture_slot(obj, material.emissiveTextureID);
    return textures[NonUniformResourceIndex(emissiveTexId)].SampleLevel(textureSamplers[NonUniformResourceIndex(emissiveTexId)], emissive_uv(texCoords, material), 0.0).xyz;
}

float3 sample_emissive(ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    int emissiveTexId = resolve_texture_slot(obj, material.emissiveTextureID);
    return textures[NonUniformResourceIndex(emissiveTexId)].Sample(textureSamplers[NonUniformResourceIndex(emissiveTexId)], emissive_uv(texCoords, material)).xyz;
}

float sample_occlusion_lod0(ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    int occlusionTexId = resolve_texture_slot(obj, material.occlusionTextureID);
    return textures[NonUniformResourceIndex(occlusionTexId)].SampleLevel(textureSamplers[NonUniformResourceIndex(occlusionTexId)], occlusion_uv(texCoords, material), 0.0).r;
}

float sample_occlusion(ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    int occlusionTexId = resolve_texture_slot(obj, material.occlusionTextureID);
    return textures[NonUniformResourceIndex(occlusionTexId)].Sample(textureSamplers[NonUniformResourceIndex(occlusionTexId)], occlusion_uv(texCoords, material)).r;
}

// How much of the sky reaches the point; 1 without an occlusion map.
float resolved_occlusion_lod0(ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    if (material.occlusionTextureID < 0) { return 1.0; }
    return material_occlusion(material.occlusionStrength, sample_occlusion_lod0(obj, material, texCoords));
}

float resolved_occlusion(ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    if (material.occlusionTextureID < 0) { return 1.0; }
    return material_occlusion(material.occlusionStrength, sample_occlusion(obj, material, texCoords));
}

float3 resolved_emission_lod0(ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    float3 sampled = float3(1.0);
    if (material.emissiveTextureID >= 0)
    {
        sampled = sample_emissive_lod0(obj, material, texCoords);
    }
    return material_emission(material, sampled);
}

float3 resolved_emission(ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    float3 sampled = float3(1.0);
    if (material.emissiveTextureID >= 0)
    {
        sampled = sample_emissive(obj, material, texCoords);
    }
    return material_emission(material, sampled);
}

float2 resolved_metallic_roughness_lod0(ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    if (material.metallicRoughnessTextureID < 0)
    {
        return material_metallic_roughness(material, float4(1.0));
    }
    float4 mrSample = sample_metallic_roughness_lod0(obj, material, texCoords);
    return material_metallic_roughness(material, mrSample);
}

float2 resolved_metallic_roughness(ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    if (material.metallicRoughnessTextureID < 0)
    {
        return material_metallic_roughness(material, float4(1.0));
    }
    float4 mrSample = sample_metallic_roughness(obj, material, texCoords);
    return material_metallic_roughness(material, mrSample);
}

// geometricNormal as authored, never pre-flipped; frontFace false turns the result for a doubleSided back face.
float3 resolved_normal_lod0(float3 geometricNormal, float4 tangent, bool frontFace, ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    if (material.normalTextureID < 0) { return frontFace ? geometricNormal : -geometricNormal; }
    float3 sampled = sample_normal_lod0(obj, material, texCoords);
    return apply_normal_map(normalize(geometricNormal), tangent, tangent_space_normal(sampled, material.normalScale), frontFace);
}

float3 resolved_normal(float3 geometricNormal, float4 tangent, bool frontFace, ObjectDescription obj, ObjMaterial material, float4 texCoords)
{
    if (material.normalTextureID < 0) { return frontFace ? geometricNormal : -geometricNormal; }
    float3 sampled = sample_normal(obj, material, texCoords);
    return apply_normal_map(normalize(geometricNormal), tangent, tangent_space_normal(sampled, material.normalScale), frontFace);
}
