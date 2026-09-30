import scene_types;
import brdf;
import material_fetch;
import material_rules;
import base_color;
import cascaded_shadow;
import push_constants;
import raster_geometry;
import material_textures;

// The normal matrix comes precomputed in push constants: Slang has no inverse() for SPIR-V.

[[vk::push_constant]] PushConstantRasterizer pc_raster;

[vk::binding(globalUBO_BINDING, 0)] ConstantBuffer<GlobalUBO> globalUBO;
[vk::binding(sceneUBO_BINDING, 0)] ConstantBuffer<SceneUBO> sceneUBO;

[shader("vertex")]
RasterVsOut vs_main(
    float3 positions : POSITION,
    float3 normal : NORMAL,
    float4 color : COLOR,
    float2 tex_coords : TEXCOORD0,
    float4 tangent : TANGENT)
{
    return raster_geometry_vs(globalUBO, pc_raster, positions, normal, color, tex_coords, tangent);
}

[shader("fragment")]
float4 fs_main(RasterVsOut In, uint primitiveID : SV_PrimitiveID, bool isFrontFace : SV_IsFrontFace) : SV_Target
{
    ObjectDescription obj = fetch_object_description(pc_raster.objectIndex);
    ObjMaterial material = fetch_material(obj, primitiveID);

    float3 L = normalize(-sceneUBO.dirLight.direction.xyz);
    float3 N = normalize(In.shadingNormal);
    // Only doubleSided materials disable culling (MeshDrawRecorder.cpp), so a back face means doubleSided.
    if (!isFrontFace) N = -N;
    N = resolved_normal(N, In.worldTangent, obj, material, In.texCoords);
    float3 V = normalize(sceneUBO.cam_pos.xyz - In.worldPosition);

    // map_d alpha feeds both branches' alpha_masked_out(), so it is sampled once up front.
    float alphaTexSample = 1.0;
    if (material.alphaTextureID >= 0)
    {
        alphaTexSample = sample_alpha(obj, material, In.texCoords);
    }

    float3 albedo;
    if (material.textureID >= 0)
    {
        int textureId = resolve_texture_slot(obj, material);
        float4 baseSample = textures[textureId].Sample(textureSamplers[textureId], transform_uv(In.texCoords, material));
        if (alpha_masked_out(material, baseSample.a * In.fragmentColor.a * alphaTexSample)) discard;
        albedo = base_color(material, baseSample.xyz);
    }
    else
    {
        if (alpha_masked_out(material, In.fragmentColor.a * alphaTexSample)) discard;
        albedo = material.diffuse;
    }

    albedo *= In.fragmentColor.rgb;

    // KHR_materials_unlit: skip lighting, shadowing and emissive alike.
    if (material.unlit != 0) return float4(albedo, 1.0);

    float2 metallicRoughness = resolved_metallic_roughness(obj, material, In.texCoords);
    float metallic = metallicRoughness.x;
    float roughness = metallicRoughness.y;
    float3 lightColor = sceneUBO.dirLight.color.rgb;
    float lightIntensity = sceneUBO.dirLight.color.w;

    float3 f0 = lerp(float3(0.04), albedo, metallic);
    float3 color = brdf_direct(N, V, L, albedo, metallic, roughness, f0,
                               lightColor * lightIntensity);

    float shadow = calc_cascaded_shadow(In.worldPosition, N, L, sceneUBO);
    color *= 1.0 - shadow * sceneUBO.cascadedShadowIntensity;

    // Emissive is added after shadowing: emitted light is not shadowed.
    color += resolved_emission(obj, material, In.texCoords);

    return float4(color, 1.0);
}