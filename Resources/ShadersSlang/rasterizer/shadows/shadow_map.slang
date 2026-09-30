import scene_types;
import material_fetch;
import material_rules;
import base_color;
import material_textures;

// Single-pass layered cascades: SV_ViewID is the cascade index.

[vk::binding(1, 1)] ConstantBuffer<float4x4[MAX_CASCADES]> lightSpaceMatrices;

struct ShadowPushConstants
{
    float4x4 model;
    uint objectIndex;
};
[[vk::push_constant]] ShadowPushConstants pc;

struct ShadowVsOut
{
    float4 svPosition : SV_Position;
    float2 fragUV : TEXCOORD0;
    float fragAlpha : TEXCOORD1;
};

[shader("vertex")]
ShadowVsOut shadow_vs_main(
    float3 pos : POSITION,
    [[vk::location(2)]] float4 color : COLOR,
    [[vk::location(3)]] float2 tex_coords : TEXCOORD0,
    uint viewId : SV_ViewID)
{
    ShadowVsOut o;
    uint cascade = min(viewId, uint(MAX_CASCADES - 1));
    o.fragUV = tex_coords;
    o.fragAlpha = color.a;
    o.svPosition = mul(mul(lightSpaceMatrices[cascade], pc.model), float4(pos, 1.0));
    return o;
}

[shader("fragment")]
void shadow_fs_main(ShadowVsOut In, uint primitiveID : SV_PrimitiveID)
{
    ObjectDescription obj = fetch_object_description(pc.objectIndex);
    ObjMaterial material = fetch_material(obj, primitiveID);

    // Only MASK materials alpha-test; OPAQUE/BLEND (alphaCutoff < 0) cast unchanged.
    if (material.alphaCutoff >= 0.0)
    {
        // map_d alpha feeds both branches' alpha_masked_out(), so it is sampled once up front.
        float alphaTexSample = 1.0;
        if (material.alphaTextureID >= 0)
        {
            alphaTexSample = sample_alpha(obj, material, In.fragUV);
        }

        if (material.textureID >= 0)
        {
            int textureId = resolve_texture_slot(obj, material);
            float alpha = textures[textureId].Sample(textureSamplers[textureId], transform_uv(In.fragUV, material)).a;
            if (alpha_masked_out(material, alpha * In.fragAlpha * alphaTexSample)) discard;
        }
        else
        {
            if (alpha_masked_out(material, In.fragAlpha * alphaTexSample)) discard;
        }
    }
}