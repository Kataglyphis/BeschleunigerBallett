import scene_types;
import ambient_maps;
import material_fetch;
import material_rules;
import base_color;
import brdf;
import cascaded_shadow;
import push_constants;
import raster_geometry;
import material_textures;
import fullscreen;
import gbuffer;
import hdr_safe;
import punctual_lights;

// Deferred geometry pass (writes the G-buffer) and lighting pass.

[[vk::push_constant]] PushConstantRasterizer pc_raster;

[vk::binding(globalUBO_BINDING, 0)] ConstantBuffer<GlobalUBO> globalUBO;

// OxidANT's gbuffer.slang texels at their attachments.
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

    // map_d alpha feeds both branches' material_masked_out(), so it is sampled once up front.
    float alphaTexSample = 1.0;
    if (material.alphaTextureID >= 0)
    {
        alphaTexSample = sample_alpha(obj, material, In.texCoords);
    }

    float4 texColor;
    if (material.textureID >= 0)
    {
        int textureId = resolve_texture_slot(obj, material);
        texColor = textures[NonUniformResourceIndex(textureId)].Sample(textureSamplers[NonUniformResourceIndex(textureId)], base_color_uv(In.texCoords, material));
        if (material_masked_out(material, texColor.a * In.fragmentColor.a * alphaTexSample)) discard;
        texColor = float4(base_color(material, texColor.rgb), texColor.a);
    }
    else
    {
        if (material_masked_out(material, In.fragmentColor.a * alphaTexSample)) discard;
        texColor = float4(material.diffuse, 1.0);
    }

    texColor.rgb *= In.fragmentColor.rgb;

    // Only doubleSided materials disable culling; resolved here, since the lighting pass has no SV_IsFrontFace.
    SurfacePoint surface;
    surface.position = In.worldPosition;
    surface.n = resolved_normal(normalize(In.shadingNormal), In.worldTangent, isFrontFace, obj, material, In.texCoords);
    float2 metallicRoughness = resolved_metallic_roughness(obj, material, In.texCoords);
    surface.albedo = texColor.rgb;
    surface.metallic = metallicRoughness.x;
    surface.roughness = metallicRoughness.y;
    surface.occlusion = resolved_occlusion(obj, material, In.texCoords);
    // The float16 format keeps emissive strength above 1.0, short of +Inf.
    surface.emissive = hdr_safe(resolved_emission(obj, material, In.texCoords));
    GBufferTexels t = gbuffer_pack(surface, material.unlit != 0);
    GBufferOut g;
    g.outNormal = t.normalMetallic;
    g.outAlbedo = t.albedoOcclusion;
    g.outMaterial = t.roughnessEmissive;
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

    GBufferTexels t;
    t.normalMetallic = inNormal.Load(texel);
    t.albedoOcclusion = inAlbedo.Load(texel);
    // KHR_materials_unlit: skip lighting, shadowing and emissive alike.
    if (gbuffer_unlit(t.normalMetallic)) return float4(hdr_safe(t.albedoOcclusion.rgb), 1.0);
    t.roughnessEmissive = inMaterial.Load(texel);
    SurfacePoint s = gbuffer_unpack(t, worldPos.xyz);

    float3 L = normalize(-sceneUBO_lighting.dirLight.direction.xyz);
    float3 V = normalize(sceneUBO_lighting.cam_pos.xyz - s.position);
    float3 lightColor = sceneUBO_lighting.dirLight.color.rgb;
    float lightIntensity = sceneUBO_lighting.dirLight.color.w;

    float3 f0 = surface_f0(s.albedo, s.metallic);
    // The fit once per pixel, not once per light: the sun, every punctual light and the ambient share it.
    float2 energy = ggx_energy(dot(s.n, V), s.roughness);
    float3 sunDirect =
        brdf_direct(s.n, V, L, s.albedo, s.metallic, s.roughness, f0, lightColor * lightIntensity, energy);
    float sunVisibility = 1.0 - calc_cascaded_shadow(s.position, s.n, L, sceneUBO_lighting);

    // The cascades shadow the sun only; the first point and spot lights have their own cubes.
    float3 punctual = punctual_lighting(s.position, s.n, V, s.albedo, s.metallic, s.roughness, f0, energy,
                                        sceneUBO_lighting.punctualShadowCount);
    // The sky lights what the sun cannot reach, as far as the surface's occlusion lets it.
    float3 ambient = sceneUBO_lighting.dirLight.direction.w * s.occlusion
                     * scene_sky_ambient(sceneUBO_lighting, s.n, V, s.albedo, s.metallic, s.roughness, f0, energy);

    // Emitted light is not shadowed.
    return float4(surface_lighting(sunDirect, sunVisibility, sceneUBO_lighting.cascadedShadowIntensity, punctual,
                                   ambient, s.emissive),
                  1.0);
}