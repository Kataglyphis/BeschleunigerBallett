import scene_types;
import ambient;
import brdf;
import material_fetch;
import material_rules;
import base_color;
import cascaded_shadow;
import push_constants;
import raster_geometry;
import material_textures;
import hdr_safe;
import punctual_lights;

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
    float4 tangent : TANGENT,
    float2 tex_coords1 : TEXCOORD1)
{
    return raster_geometry_vs(globalUBO, pc_raster, positions, normal, color, tex_coords, tangent, tex_coords1);
}

[shader("fragment")]
float4 fs_main(RasterVsOut In, uint primitiveID : SV_PrimitiveID, bool isFrontFace : SV_IsFrontFace) : SV_Target
{
    ObjectDescription obj = fetch_object_description(pc_raster.objectIndex);
    ObjMaterial material = fetch_material(obj, primitiveID);

    float3 L = normalize(-sceneUBO.dirLight.direction.xyz);
    // Only doubleSided materials disable culling (MeshDrawRecorder.cpp), so a back face means doubleSided.
    float3 N = resolved_normal(normalize(In.shadingNormal), In.worldTangent, isFrontFace, obj, material, In.texCoords);
    float3 V = normalize(sceneUBO.cam_pos.xyz - In.worldPosition);

    // map_d alpha feeds both branches' alpha_masked_out(), so it is sampled once up front.
    float alphaTexSample = 1.0;
    if (material.alphaTextureID >= 0)
    {
        alphaTexSample = sample_alpha(obj, material, In.texCoords);
    }

    float3 albedo;
    float sampledAlpha = In.fragmentColor.a * alphaTexSample;
    if (material.textureID >= 0)
    {
        int textureId = resolve_texture_slot(obj, material);
        float4 baseSample = textures[NonUniformResourceIndex(textureId)].Sample(textureSamplers[NonUniformResourceIndex(textureId)], base_color_uv(In.texCoords, material));
        sampledAlpha *= baseSample.a;
        albedo = base_color(material, baseSample.xyz);
    }
    else
    {
        albedo = material.diffuse;
    }
    if (alpha_masked_out(material, sampledAlpha)) discard;

    albedo *= In.fragmentColor.rgb;

    // The transparent pass blends BLEND materials by this; every other material covers its pixel.
    float coverage = blend_coverage(material, sampledAlpha);

    // KHR_materials_unlit: skip lighting, shadowing and emissive alike.
    if (material.unlit != 0) return float4(hdr_safe(albedo), coverage);

    float2 metallicRoughness = resolved_metallic_roughness(obj, material, In.texCoords);
    float metallic = metallicRoughness.x;
    float roughness = metallicRoughness.y;
    float3 lightColor = sceneUBO.dirLight.color.rgb;
    float lightIntensity = sceneUBO.dirLight.color.w;

    float3 f0 = lerp(float3(0.04), albedo, metallic);
    // The fit once per fragment, not once per light: the sun, every punctual light and the ambient share it.
    float2 energy = ggx_energy(dot(N, V), roughness);
    float3 color = brdf_direct(N, V, L, albedo, metallic, roughness, f0,
                               lightColor * lightIntensity, energy);

    float shadow = calc_cascaded_shadow(In.worldPosition, N, L, sceneUBO);
    color *= 1.0 - shadow * sceneUBO.cascadedShadowIntensity;

    // The cascades shadow the sun only; the first point and spot lights have their own cubes.
    color += punctual_lighting(In.worldPosition, N, V, albedo, metallic, roughness, f0, energy, sceneUBO.punctualShadowCount);

    // After the sun's shadow: the sky lights what the sun cannot reach.
    color += sceneUBO.dirLight.direction.w
             * analytic_ambient(N, V, albedo, metallic, roughness, f0, energy, L, lightIntensity);

    // Emissive is added after shadowing: emitted light is not shadowed.
    color += resolved_emission(obj, material, In.texCoords);

    return float4(hdr_safe(color), coverage);
}