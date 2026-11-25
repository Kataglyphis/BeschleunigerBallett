import scene_types;
import material_fetch;
import material_rules;
import base_color;
import material_textures;

// Single-pass layered cascades: SV_ViewID is the cascade index.

[vk::binding(1, 1)] ConstantBuffer<float4x4[CASCADE_COUNT]> lightSpaceMatrices;

struct ShadowPushConstants
{
    float4x4 model;
    uint objectIndex;
};
[[vk::push_constant]] ShadowPushConstants pc;

struct ShadowVsOut
{
    float4 svPosition : SV_Position;
    float4 fragUV : TEXCOORD0;
    float fragAlpha : TEXCOORD1;
};

[shader("vertex")]
ShadowVsOut shadow_vs_main(
    float3 pos : POSITION,
    [[vk::location(2)]] float4 color : COLOR,
    [[vk::location(3)]] float2 tex_coords : TEXCOORD0,
    [[vk::location(5)]] float2 tex_coords1 : TEXCOORD1,
    uint viewId : SV_ViewID)
{
    ShadowVsOut o;
    uint cascade = min(viewId, uint(CASCADE_COUNT - 1));
    o.fragUV = uv_sets(tex_coords, tex_coords1);
    o.fragAlpha = color.a;
    o.svPosition = mul(mul(lightSpaceMatrices[cascade], pc.model), float4(pos, 1.0));
    return o;
}

// Punctual-light cubes: SV_ViewID is the face; the dynamic offset picks the light's six face matrices.
[vk::binding(2, 1)] ConstantBuffer<float4x4[6]> cubeFaceMatrices;

[shader("vertex")]
ShadowVsOut cube_shadow_vs_main(
    float3 pos : POSITION,
    [[vk::location(2)]] float4 color : COLOR,
    [[vk::location(3)]] float2 tex_coords : TEXCOORD0,
    [[vk::location(5)]] float2 tex_coords1 : TEXCOORD1,
    uint viewId : SV_ViewID)
{
    ShadowVsOut o;
    o.fragUV = uv_sets(tex_coords, tex_coords1);
    o.fragAlpha = color.a;
    o.svPosition = mul(mul(cubeFaceMatrices[min(viewId, 5u)], pc.model), float4(pos, 1.0));
    return o;
}

[shader("fragment")]
void shadow_fs_main(ShadowVsOut In, uint primitiveID : SV_PrimitiveID)
{
    ObjectDescription obj = fetch_object_description(pc.objectIndex);
    ObjMaterial material = fetch_material(obj, primitiveID);

    // Only MASK materials alpha-test; OPAQUE and BLEND cast unchanged.
    if (material.alphaMode == ALPHA_MASK)
    {
        // map_d alpha feeds both branches' material_masked_out(), so it is sampled once up front.
        float alphaTexSample = 1.0;
        if (material.alphaTextureID >= 0)
        {
            alphaTexSample = sample_alpha(obj, material, In.fragUV);
        }

        if (material.textureID >= 0)
        {
            int textureId = resolve_texture_slot(obj, material);
            float alpha = textures[NonUniformResourceIndex(textureId)].Sample(textureSamplers[NonUniformResourceIndex(textureId)], base_color_uv(In.fragUV, material)).a;
            if (material_masked_out(material, alpha * In.fragAlpha * alphaTexSample)) discard;
        }
        else
        {
            if (material_masked_out(material, In.fragAlpha * alphaTexSample)) discard;
        }
    }
}