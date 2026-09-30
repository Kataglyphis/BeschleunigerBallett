import scene_types;
import material_fetch;
import material_rules;
import base_color;
import brdf;
import cascaded_shadow;
import push_constants;
import raster_geometry;
import material_textures;

// Deferred geometry pass (writes the G-buffer) and lighting pass.

[[vk::push_constant]] PushConstantRasterizer pc_raster;

[vk::binding(globalUBO_BINDING, 0)] ConstantBuffer<GlobalUBO> globalUBO;

struct GBufferOut
{
    float4 outNormal : SV_Target0;
    float4 outAlbedo : SV_Target1;
    float4 outMaterial : SV_Target2;
};

[shader("vertex")]
RasterVsOut geometry_vs_main(
    float3 positions : POSITION,
    float3 normal : NORMAL,
    float4 color : COLOR,
    float2 tex_coords : TEXCOORD0,
    float4 tangent : TANGENT)
{
    return raster_geometry_vs(globalUBO, pc_raster, positions, normal, color, tex_coords, tangent);
}

[shader("fragment")]
GBufferOut geometry_fs_main(RasterVsOut In, uint primitiveID : SV_PrimitiveID, bool isFrontFace : SV_IsFrontFace)
{
    ObjectDescription obj = fetch_object_description(pc_raster.objectIndex);
    ObjMaterial material = fetch_material(obj, primitiveID);

    // map_d alpha feeds both branches' alpha_masked_out(), so it is sampled once up front.
    float alphaTexSample = 1.0;
    if (material.alphaTextureID >= 0)
    {
        alphaTexSample = sample_alpha(obj, material, In.texCoords);
    }

    float4 texColor;
    if (material.textureID >= 0)
    {
        int textureId = resolve_texture_slot(obj, material);
        texColor = textures[textureId].Sample(textureSamplers[textureId], transform_uv(In.texCoords, material));
        if (alpha_masked_out(material, texColor.a * In.fragmentColor.a * alphaTexSample)) discard;
        texColor = float4(base_color(material, texColor.rgb), texColor.a);
    }
    else
    {
        if (alpha_masked_out(material, In.fragmentColor.a * alphaTexSample)) discard;
        texColor = float4(material.diffuse, 1.0);
    }

    texColor.rgb *= In.fragmentColor.rgb;

    GBufferOut g;
    // Only doubleSided materials disable culling; flip here, since the lighting pass has no SV_IsFrontFace.
    float3 N = normalize(In.shadingNormal);
    if (!isFrontFace) N = -N;
    N = resolved_normal(N, In.worldTangent, obj, material, In.texCoords);
    float2 metallicRoughness = resolved_metallic_roughness(obj, material, In.texCoords);
    float metallic = metallicRoughness.x;
    float roughness = metallicRoughness.y;
    // Metallic rides in the normal's unused .w channel instead of its own attachment.
    g.outNormal = float4(N, metallic);
    // .a carries KHR_materials_unlit, not alpha: sRGB encodes RGB only, so 0.0/1.0 round-trip exactly.
    g.outAlbedo = float4(texColor.rgb, material.unlit != 0 ? 1.0 : 0.0);
    // r = roughness, gba = emissive; the float16 format keeps emissive strength above 1.0.
    g.outMaterial = float4(roughness, resolved_emission(obj, material, In.texCoords));
    return g;
}

// --- Deferred lighting pass ---

[vk::binding(globalUBO_BINDING, 0)] ConstantBuffer<GlobalUBO> globalUBO_lighting;
[vk::binding(sceneUBO_BINDING, 0)] ConstantBuffer<SceneUBO> sceneUBO_lighting;

[[vk::binding(GBUFFER_NORMAL_BINDING, 1)]] [[vk::input_attachment_index(GBUFFER_NORMAL_BINDING)]] SubpassInput<float4> inNormal;
[[vk::binding(GBUFFER_ALBEDO_BINDING, 1)]] [[vk::input_attachment_index(GBUFFER_ALBEDO_BINDING)]] SubpassInput<float4> inAlbedo;
[[vk::binding(GBUFFER_MATERIAL_BINDING, 1)]] [[vk::input_attachment_index(GBUFFER_MATERIAL_BINDING)]] SubpassInput<float4> inMaterial;
[[vk::binding(GBUFFER_DEPTH_BINDING, 1)]] [[vk::input_attachment_index(GBUFFER_DEPTH_BINDING)]] SubpassInput<float4> inDepth;

struct LightingVsOut
{
    float4 svPosition : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
LightingVsOut lighting_vs_main(uint vid : SV_VertexID)
{
    LightingVsOut o;
    float x = float(vid / 2) * 4.0 - 1.0;
    float y = float(vid % 2) * 4.0 - 1.0;
    o.svPosition = float4(x, y, 0.0, 1.0);
    o.uv = float2((x + 1.0) * 0.5, 1.0 - (y + 1.0) * 0.5);
    return o;
}

[shader("fragment")]
float4 lighting_fs_main(LightingVsOut In) : SV_Target
{
    float depth = inDepth.SubpassLoad().r;
    if (depth >= 1.0) discard;

    float4 clipPos = float4(In.uv * 2.0 - 1.0, depth, 1.0);
    float4 worldPos = mul(globalUBO_lighting.inv_view, mul(globalUBO_lighting.inv_projection, clipPos));
    worldPos /= worldPos.w;

    float4 normal = inNormal.SubpassLoad();
    float4 albedo = inAlbedo.SubpassLoad();
    float4 material = inMaterial.SubpassLoad();

    // KHR_materials_unlit (outAlbedo.a): skip lighting, shadowing and emissive alike.
    if (albedo.a > 0.5) return float4(albedo.rgb, 1.0);

    float3 N = normalize(normal.xyz);
    float3 L = normalize(-sceneUBO_lighting.dirLight.direction.xyz);
    float3 V = normalize(sceneUBO_lighting.cam_pos.xyz - worldPos.xyz);

    float roughness = material.r;
    float metallic = normal.w;
    float3 lightColor = sceneUBO_lighting.dirLight.color.rgb;
    float lightIntensity = sceneUBO_lighting.dirLight.color.w;

    float3 f0 = lerp(float3(0.04), albedo.rgb, metallic);
    float3 color = brdf_direct(N, V, L, albedo.rgb, metallic, roughness, f0,
                               lightColor * lightIntensity);

    float shadow = calc_cascaded_shadow(worldPos.xyz, N, L, sceneUBO_lighting);
    color *= 1.0 - shadow * sceneUBO_lighting.cascadedShadowIntensity;

    // Emissive is added after shadowing: emitted light is not shadowed.
    color += material.gba;

    return float4(color, 1.0);
}