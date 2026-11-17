import scene_types;
import material_fetch;
import material_rules;
import base_color;
import brdf;
import cascaded_shadow;
import push_constants;
import raster_geometry;
import material_textures;
import fullscreen;
import hdr_safe;
import punctual_lights;

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
    float4 tangent : TANGENT,
    float2 tex_coords1 : TEXCOORD1)
{
    return raster_geometry_vs(globalUBO, pc_raster, positions, normal, color, tex_coords, tangent, tex_coords1);
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
        texColor = textures[textureId].Sample(textureSamplers[textureId], base_color_uv(In.texCoords, material));
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
    // Only doubleSided materials disable culling; resolved here, since the lighting pass has no SV_IsFrontFace.
    float3 N = resolved_normal(normalize(In.shadingNormal), In.worldTangent, isFrontFace, obj, material, In.texCoords);
    float2 metallicRoughness = resolved_metallic_roughness(obj, material, In.texCoords);
    float metallic = metallicRoughness.x;
    float roughness = metallicRoughness.y;
    // Metallic rides in the normal's unused .w channel instead of its own attachment.
    g.outNormal = float4(N, metallic);
    // .a carries KHR_materials_unlit, not alpha: sRGB encodes RGB only, so 0.0/1.0 round-trip exactly.
    g.outAlbedo = float4(texColor.rgb, material.unlit != 0 ? 1.0 : 0.0);
    // r = roughness, gba = emissive; the float16 format keeps emissive strength above 1.0, short of +Inf.
    g.outMaterial = float4(roughness, hdr_safe(resolved_emission(obj, material, In.texCoords)));
    return g;
}

// --- Deferred lighting pass ---

// The lighting pass reads the geometry pass's globalUBO: a second name on its binding drew Slang's overlap warning.
[vk::binding(sceneUBO_BINDING, 0)] ConstantBuffer<SceneUBO> sceneUBO_lighting;

// Sampled images, texel-fetched at this pixel: the geometry pass is its own vkCmdBeginRendering, not a subpass.
[[vk::binding(GBUFFER_NORMAL_BINDING, 1)]] Texture2D<float4> inNormal;
[[vk::binding(GBUFFER_ALBEDO_BINDING, 1)]] Texture2D<float4> inAlbedo;
[[vk::binding(GBUFFER_MATERIAL_BINDING, 1)]] Texture2D<float4> inMaterial;
[[vk::binding(GBUFFER_DEPTH_BINDING, 1)]] Texture2D<float4> inDepth;

// SV_VulkanVertexID: SV_VertexID subtracts BaseVertex, which needs draw parameters a Raspberry Pi 5's v3dv lacks.
[shader("vertex")]
FullscreenVsOut lighting_vs_main(uint vid : SV_VulkanVertexID)
{
    return fullscreen_vs(vid);
}

[shader("fragment")]
float4 lighting_fs_main(FullscreenVsOut In) : SV_Target
{
    // The lighting pass covers the G-buffer's full extent, so SV_Position names the texel the geometry pass wrote.
    int3 texel = int3(int2(In.svPosition.xy), 0);
    float depth = inDepth.Load(texel).r;
    if (depth >= 1.0) discard;

    float4 clipPos = float4(fullscreen_uv_to_ndc(In.uv), depth, 1.0);
    float4 worldPos = mul(globalUBO.inv_view, mul(globalUBO.inv_projection, clipPos));
    worldPos /= worldPos.w;

    float4 normal = inNormal.Load(texel);
    float4 albedo = inAlbedo.Load(texel);
    float4 material = inMaterial.Load(texel);

    // KHR_materials_unlit (outAlbedo.a): skip lighting, shadowing and emissive alike.
    if (albedo.a > 0.5) return float4(hdr_safe(albedo.rgb), 1.0);

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

    // The cascades shadow the sun only; punctual lights stay unshadowed, as in the Rust raster passes.
    color += punctual_lighting(worldPos.xyz, N, V, albedo.rgb, metallic, roughness, f0);

    // Emissive is added after shadowing: emitted light is not shadowed.
    color += material.gba;

    return float4(hdr_safe(color), 1.0);
}